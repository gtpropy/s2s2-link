#include "cw_rx.h"
#include <string.h>

/* Same character set as the satellite keyer (Get_Morse_Code in satellite_app.c) */
static const struct { const char *code; char ch; } k_morse[] =
{
    {".-",'A'},{"-...",'B'},{"-.-.",'C'},{"-..",'D'},{".",'E'},{"..-.",'F'},
    {"--.",'G'},{"....",'H'},{"..",'I'},{".---",'J'},{"-.-",'K'},{".-..",'L'},
    {"--",'M'},{"-.",'N'},{"---",'O'},{".--.",'P'},{"--.-",'Q'},{".-.",'R'},
    {"...",'S'},{"-",'T'},{"..-",'U'},{"...-",'V'},{".--",'W'},{"-..-",'X'},
    {"-.--",'Y'},{"--..",'Z'},
    {"-----",'0'},{".----",'1'},{"..---",'2'},{"...--",'3'},{"....-",'4'},
    {".....",'5'},{"-....",'6'},{"--...",'7'},{"---..",'8'},{"----.",'9'},
    {".-.-.-",'.'},{"--..--",','},{"..--..",'?'},{"-..-.",'/'},{"-....-",'-'}
};

static CwRx_EventCb s_cb;

/* Level tracking (dBm * 16, integer only - Cortex-M0+ has no FPU) */
static int32_t  s_nf16, s_pk16;
static int16_t  s_hist[3];
static uint8_t  s_hist_n;
static bool     s_sig_present;
static bool     s_active_valid;
static uint32_t s_last_active;

/* Mark / space state machine */
static bool     s_mark;
static uint8_t  s_cand_cnt;
static uint32_t s_cand_t;
static uint32_t s_state_start;   /* start of current mark or space */
static uint32_t s_space_start;   /* start of the last space        */

/* Symbol / line assembly */
static char     s_sym[CW_SYM_MAX + 1];
static uint8_t  s_sym_len;
static bool     s_char_done, s_word_done, s_seg_done;
static char     s_line[CW_LINE_MAX + 1];
static uint8_t  s_line_len;
static uint32_t s_mute_until;
static bool     s_muted;

static void emit(CwEvt_t e, char ch, const char *t, uint32_t v, int16_t aux)
{
    if (s_cb) s_cb(e, ch, t, v, aux);
}

/* Returns pointer just after the callsign in the current line, or NULL */
static const char *call_end(void)
{
    const char *p = strstr(s_line, CW_CALLSIGN);
    if (p) return p + strlen(CW_CALLSIGN);
    return NULL;
}

static void line_flush(void)
{
    while (s_line_len > 0 && s_line[s_line_len - 1] == ' ') s_line_len--;
    s_line[s_line_len] = '\0';
    if (s_line_len > 0)
    {
        bool call = (strstr(s_line, CW_CALLSIGN) != NULL ||
                     strstr(s_line, "S2S2") != NULL);
        bool telem = (s_line_len >= 5 &&
                      (strchr(s_line, 'V') || strchr(s_line, 'C') || strchr(s_line, 'A')));
        if (call || telem)
        {
            emit(CW_EVT_LINE, 0, s_line, call ? 1U : 0U, (int16_t)(s_pk16 / 16));
        }
    }
    s_line_len = 0;
    s_line[0] = '\0';
}

static void line_add(char c)
{
    if (s_line_len >= CW_LINE_MAX) line_flush();
    if (c == ' ' && (s_line_len == 0 || s_line[s_line_len - 1] == ' ')) return;
    s_line[s_line_len++] = c;
    s_line[s_line_len] = '\0';
}

static void finish_char(void)
{
    if (s_sym_len == 0) return;
    s_sym[s_sym_len] = '\0';
    char c = '?';
    for (unsigned i = 0; i < sizeof(k_morse) / sizeof(k_morse[0]); i++)
    {
        if (strcmp(k_morse[i].code, s_sym) == 0) { c = k_morse[i].ch; break; }
    }
    emit(CW_EVT_CHAR, c, s_sym, 0, 0);
    line_add(c);
    s_sym_len = 0;
}

static void segment_break(void)
{
    /* Satellite: CALLSIGN <1.0 s> MESSAGE <1.5 s or more> CALLSIGN ...
     * - callsign present, nothing after it yet -> keep line open (message follows)
     * - callsign present and payload already there -> message finished, flush
     * - no callsign -> junk / standalone, flush                                */
    const char *e = call_end();
    if (e != NULL)
    {
        while (*e == ' ') e++;
        if (*e == '\0') line_add(' ');
        else            line_flush();
    }
    else
    {
        line_flush();
    }
}

static void reset_symbols(void)
{
    s_sym_len = 0;
    s_char_done = s_word_done = s_seg_done = true;
}

void CwRx_Reset(uint32_t now_ms)
{
    s_hist_n = 0;
    s_mark = false;
    s_cand_cnt = 0;
    s_sig_present = false;
    s_active_valid = false;
    s_pk16 = s_nf16;                 /* keep learned noise floor */
    s_state_start = s_space_start = now_ms;
    s_line_len = 0;
    s_line[0] = '\0';
    reset_symbols();
}

void CwRx_Init(CwRx_EventCb cb, uint32_t now_ms)
{
    s_cb = cb;
    s_muted = false;
    s_mute_until = 0;
    s_nf16 = (int32_t)CW_NOISE_INIT_DBM * 16;
    s_pk16 = s_nf16;
    CwRx_Reset(now_ms);
}

void CwRx_Mute(uint32_t until_ms)
{
    s_muted = true;
    s_mute_until = until_ms;
    s_mark = false;
    s_cand_cnt = 0;
    s_hist_n = 0;
    s_sym_len = 0;
}

uint32_t CwRx_GetUnitMs(void)   { return CW_UNIT_DEFAULT_MS; }
int16_t  CwRx_GetNoiseDbm(void) { return (int16_t)(s_nf16 / 16); }
int16_t  CwRx_GetPeakDbm(void)  { return (int16_t)(s_pk16 / 16); }

static void mark_ended(uint32_t t)
{
    uint32_t m = t - s_state_start;

    if (m < CW_GLITCH_MS)          /* noise spike: pretend it never happened */
        return;

    s_space_start = t;
    s_char_done = s_word_done = s_seg_done = false;

    if (m >= CW_TUNE_MIN_MS)       /* tuning carrier (0.5 s), not a dah */
    {
        emit(CW_EVT_CARRIER, 0, 0, m, (int16_t)(s_pk16 / 16));
        s_sym_len = 0;
        return;
    }

    /* dit ~60 ms (40..120), dah ~180 ms (120..300) -- thresholds scale with CW_UNIT_DEFAULT_MS */
    bool is_dah = (m >= CW_DAH_MIN_MS);

    if (s_sym_len < CW_SYM_MAX) s_sym[s_sym_len++] = is_dah ? '-' : '.';
    else                        s_sym_len = CW_SYM_MAX + 1;   /* overflow */
}

static void space_timers(uint32_t t)
{
    uint32_t g = t - s_space_start;

    if (!s_char_done && g >= CW_CHAR_GAP_MS)
    {
        s_char_done = true;
        if (s_sym_len > CW_SYM_MAX) { s_sym_len = 0; line_add('?'); }
        else finish_char();
    }
    if (!s_word_done && g >= CW_WORD_GAP_MS)
    {
        s_word_done = true;
        line_add(' ');
    }
    if (!s_seg_done && g >= CW_SEG_GAP_MS)
    {
        s_seg_done = true;
        segment_break();
    }
    if (g >= CW_LINE_IDLE_MS && s_line_len > 0)
        line_flush();
}

void CwRx_Feed(int16_t rssi, uint32_t t)
{
    if (s_muted)
    {
        if ((int32_t)(t - s_mute_until) < 0) return;
        s_muted = false;
        CwRx_Reset(t);
    }

    /* 3-sample moving average (15 ms) */
    if (s_hist_n < 3) s_hist[s_hist_n++] = rssi;
    else { s_hist[0] = s_hist[1]; s_hist[1] = s_hist[2]; s_hist[2] = rssi; }
    int32_t s16 = 0;
    for (uint8_t i = 0; i < s_hist_n; i++) s16 += s_hist[i];
    s16 = (s16 * 16) / s_hist_n;

    /* Noise floor: follows minima at once, rises slowly only when no carrier */
    if (s16 < s_nf16)                       s_nf16 = s16;
    else if (!s_mark && !s_sig_present)     s_nf16 += (s16 - s_nf16) >> 6;

    /* Peak: fast attack, slow hold/decay */
    if (s16 > s_pk16)
    {
        s_pk16 = s16;
    }
    else if (s_mark)
    {
        if (s16 < s_pk16 - 4 * 16) s_pk16 -= (s_pk16 - s16) >> 6;   /* fading signal */
    }
    else
    {
        int32_t d = s_pk16 - s_nf16;
        if (d > 16 * 16) s_pk16 -= (d >> 8);                        /* very slow hold */
    }

    /* Carrier presence: activity timer with hold (no flapping in gaps) */
    if ((s16 - s_nf16) >= (int32_t)CW_MIN_SNR_DB * 16)
    {
        s_last_active = t;
        s_active_valid = true;
    }
    bool present = s_active_valid && ((int32_t)(t - s_last_active) < (int32_t)CW_LOST_HOLD_MS);

    if (present != s_sig_present)
    {
        s_sig_present = present;
        emit(CW_EVT_SIGNAL, 0, 0, present ? 1U : 0U, (int16_t)(s_pk16 / 16));
        if (!present)
        {
            if (s_mark) { mark_ended(t); s_mark = false; s_state_start = t; }
            s_cand_cnt = 0;
            s_active_valid = false;
            s_pk16 = s_nf16;
        }
    }

    /* Mark / space decision with hysteresis */
    int32_t span16 = s_pk16 - s_nf16;
    int32_t thr16  = s_nf16 + span16 / 2;
    int32_t hyst   = span16 / 8;
    bool want_mark = s_mark;

    if (present && span16 >= (int32_t)CW_MIN_SNR_DB * 16)
    {
        if (!s_mark && s16 > thr16 + hyst) want_mark = true;
        if ( s_mark && s16 < thr16 - hyst) want_mark = false;
    }
    else want_mark = false;

    if (want_mark != s_mark)
    {
        if (s_cand_cnt == 0) s_cand_t = t;
        if (++s_cand_cnt >= CW_DEBOUNCE_SAMPLES)
        {
            if (s_mark) mark_ended(s_cand_t);
            s_mark = want_mark;
            s_state_start = s_cand_t;
            s_cand_cnt = 0;
        }
    }
    else s_cand_cnt = 0;

    /* -----------------------------------------------------------------------
     * Mark-duration watchdog: if we stay in mark for more than
     * CW_MARK_WATCHDOG_MS (e.g. 1600 ms) the adaptive threshold has locked
     * up on a strong unmodulated carrier and will never see a space edge.
     * Force mark_ended() now so the state machine can decode the carrier
     * event and reset properly — prevents the "stuck after CW detect" freeze.
     * --------------------------------------------------------------------- */
    if (s_mark && (uint32_t)(t - s_state_start) >= CW_MARK_WATCHDOG_MS)
    {
        mark_ended(t);
        s_mark = false;
        s_state_start = t;
        s_space_start = t;
        s_cand_cnt = 0;
    }

    if (!s_mark) space_timers(t);
}
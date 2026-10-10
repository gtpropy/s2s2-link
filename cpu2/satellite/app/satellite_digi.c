#include "satellite_digi.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "s2s2_config.h"
#include "ax25.h"
#include "satellite_app.h"
#include "uart_debug.h"
#include "saf_shared.h"

#ifndef SAT_DIGI_TX_GUARD_MS
#define SAT_DIGI_TX_GUARD_MS 300U     /* give the sender time to finish and our PA time to settle; also avoids talking over the ground station's report */
#endif
#define SAT_SAF_TX_GAP_MS    5000UL   /* minimum time between two stored messages going out */
#define SAT_SAF_FIRST_MS     3000UL   /* a message that was not repeated right away goes out this soon */
#define SAT_SAF_RESTORE_MS   30000UL  /* give CPU1 this long to play the journal back */

Digi_t g_digi;
Saf_t  g_saf;
volatile uint32_t g_digiSelfTest = 0;
volatile uint32_t g_digiSelfStep = 0;      /* number of the check that failed (0 = none), for SWD */

static uint8_t s_out[DIGI_MAX_BODY];
static uint32_t s_nextFwd;
static bool s_restoring;
static uint32_t s_restoreT0, s_restored;

#if S2S2_SAF_ENABLE && S2S2_SAF_APPEND_STATS
/* TEST FEATURE: the frame that is transmitted right away gets the state of the store appended to its text, e.g.
 *   "hello | MEMORY OBJECTS: #1 id=1 len=56 hops_left=0 forwarded=0 ttl_left=21598s | STORED 3 msg, 168 B"
 * so a receiver (Dire Wolf, a ground station) sees what the satellite keeps. The copy in the store stays unchanged. If the
 * text does not fit the radio packet it is cut: the frame is never longer than DIGI_MAX_BODY. */
static uint16_t append_stats(uint8_t *body, uint16_t len, int slot, uint32_t now_ms)
{
    char sfx[120];
    uint32_t bytes = 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < SAF_SLOTS; i++)
        if (g_saf.slot[i].used) { n++; bytes += g_saf.slot[i].len; }
    int k = 0;
    if (slot >= 0 && g_saf.slot[slot].used) {
        const SafSlot_t *m = &g_saf.slot[slot];
        k = snprintf(sfx, sizeof(sfx), " | MEMORY OBJECTS: #%u id=%u len=%u hops_left=%u forwarded=%u ttl_left=%lus",
                     (unsigned)(slot + 1), (unsigned)m->id, (unsigned)m->len, (unsigned)m->hops_left, (unsigned)m->fwd_count,
                     (unsigned long)(Saf_TtlLeftMs(&g_saf, slot, now_ms) / 1000UL));
    }
    if (k < 0) k = 0;
    int j = snprintf(sfx + k, sizeof(sfx) - (size_t)k, " | STORED %u msg, %lu B", (unsigned)n, (unsigned long)bytes);
    if (j < 0) j = 0;
    size_t total = (size_t)k + (size_t)j;
    if (total >= sizeof(sfx)) total = sizeof(sfx) - 1U;
    if ((uint32_t)len + total > DIGI_MAX_BODY) total = (DIGI_MAX_BODY > len) ? (size_t)(DIGI_MAX_BODY - len) : 0U;
    memcpy(body + len, sfx, total);
    return (uint16_t)(len + total);
}
#endif

static void build_config(DigiConfig_t *c)
{
    Digi_DefaultConfig(c, S2S2_SAT_CALLSIGN, S2S2_SAT_SSID);
    c->enabled = (S2S2_DIGI_ENABLE != 0);
    if (S2S2_DIGI_ALIAS[0] != '\0') {
        strncpy(c->alias[0], S2S2_DIGI_ALIAS, DIGI_CALL_LEN);
        c->nAlias = 1;
    }
    c->wide_enable = (S2S2_DIGI_WIDE != 0);
    c->wide_max_ssid = S2S2_DIGI_MAX_N;
    c->trace = (S2S2_DIGI_TRACE != 0);
    c->dedupe_ms = S2S2_DIGI_DEDUPE_MS;
    c->rate_per_min = S2S2_DIGI_RATE_PER_MIN;
    c->burst = S2S2_DIGI_BURST;
    c->src_min_interval_ms = S2S2_DIGI_SRC_MIN_MS;
}

/* rules checked on the chip itself with a fixed configuration, independent of the mission settings */
static bool self_test(void)
{
    static Digi_t t;
    static Saf_t sf;
    static uint8_t in[DIGI_MAX_BODY], out[DIGI_MAX_BODY], rep[DIGI_MAX_BODY];
    static char txt[96];
    DigiConfig_t c;
    DigiFrame_t f;
    uint16_t n, ol = 0;

    Digi_DefaultConfig(&c, "TESTDG", 1);
    strcpy(c.alias[0], "ARISS"); c.nAlias = 1;
    c.rate_per_min = 0; c.src_min_interval_ms = 0;
    Digi_Init(&t, &c);

    n = Digi_FromTnc2("A1ABC>APRS,WIDE2-2:hello", in, sizeof(in));
    if (n == 0 || Digi_Process(&t, in, n, 1000, out, sizeof(out), &ol) != DIGI_REPEAT) return (g_digiSelfStep = 1, false);
    if (!Digi_ParseFrame(out, ol, &f)) return (g_digiSelfStep = 2, false);
    memcpy(rep, out, ol);                                                                              /* kept for the loop check */
    uint16_t rl = ol;
    Digi_FormatFrame(&f, txt, sizeof(txt));
    if (strcmp(txt, "A1ABC>APRS,TESTDG-1*,WIDE2-1:hello") != 0) return (g_digiSelfStep = 3, false);     /* trace + decrement */
    if (Digi_HopsLeft(rep, rl) != 1) return (g_digiSelfStep = 10, false);                               /* hop budget left in the path */
    if (Digi_Process(&t, in, n, 2000, out, sizeof(out), &ol) != DIGI_DROP_DUPLICATE) return (g_digiSelfStep = 4, false);     /* same frame again */
    if (Digi_Process(&t, rep, rl, 40000, out, sizeof(out), &ol) != DIGI_DROP_LOOP) return (g_digiSelfStep = 5, false);       /* our own repeat heard back */

    n = Digi_FromTnc2("B2BBB>APRS,ARISS:x", in, sizeof(in));
    if (n == 0 || Digi_Process(&t, in, n, 3000, out, sizeof(out), &ol) != DIGI_REPEAT) return (g_digiSelfStep = 6, false);
    if (!Digi_ParseFrame(out, ol, &f)) return (g_digiSelfStep = 7, false);
    Digi_FormatFrame(&f, txt, sizeof(txt));
    if (strcmp(txt, "B2BBB>APRS,TESTDG-1*:x") != 0) return (g_digiSelfStep = 8, false);                 /* alias replaced by our call */

    n = Digi_FromTnc2("C3CCC>APRS,WIDE1-1:x", in, sizeof(in));
    if (n == 0 || Digi_Process(&t, in, n, 4000, out, sizeof(out), &ol) != DIGI_DROP_NOT_FOR_US) return (g_digiSelfStep = 9, false);   /* ground fill-in tier */
    n = Digi_FromTnc2("TESTDG-1>APRS,WIDE2-2:x", in, sizeof(in));
    if (n == 0 || Digi_Process(&t, in, n, 5000, out, sizeof(out), &ol) != DIGI_DROP_OWN_SOURCE) return (g_digiSelfStep = 11, false);

    /* store and forward: priority by hop budget, forward interval, last send frees the slot, journal bookkeeping */
    SafConfig_t sc = { 2, 1000, 100000 };
    Saf_Init(&sf, &sc);
    int a = Saf_Store(&sf, rep, rl, 1, 0, 0);
    uint16_t l2 = Digi_FromTnc2("D4DDD>APRS,TESTDG-1*:y", in, sizeof(in));
    int b = Saf_Store(&sf, in, l2, 0, 0, 0);
    if (a < 0 || b < 0 || Saf_Store(&sf, rep, rl, 1, 1, 0) != -1) return (g_digiSelfStep = 12, false);   /* second copy refused */
    if (Saf_NextDue(&sf, 0) != a) return (g_digiSelfStep = 13, false);                                    /* hop budget first */
    Saf_Forwarded(&sf, a, 0);
    if (Saf_NextDue(&sf, 500) != b) return (g_digiSelfStep = 14, false);                                  /* a waits for its interval */
    Saf_Forwarded(&sf, a, 1000);                                                                          /* second and last send of a */
    uint16_t gid;
    if (Saf_Count(&sf) != 1 || !Saf_PopGone(&sf, &gid) || gid != sf.slot[a].id) return (g_digiSelfStep = 15, false);
    Saf_Expire(&sf, 100000);
    if (Saf_Count(&sf) != 0) return (g_digiSelfStep = 16, false);
    return true;
}

/* ------------------------------------------------------------------------------------------------------------------
 * mailbox to CPU1 (journal in flash) -- see ipc/saf_shared.h
 * ------------------------------------------------------------------------------------------------------------------ */
#if S2S2_SAF_ENABLE
static void mbox_init(uint32_t now_ms)
{
    struct saf_mbox_s *m = SAF_MBOX;
    m->put_seq = m->put_ack;                    /* nothing in flight */
    m->rst_ack = m->rst_seq;
    m->inject_ack = m->inject_seq;
    m->restore_req = m->restore_ack + 1U;       /* ask CPU1 for the stored messages */
    SAF_BARRIER();
    m->cpu2_magic = SAF_MBOX_MAGIC;
    s_restoring = true;
    s_restoreT0 = now_ms;
    s_restored = 0;
}

static bool cpu1_ready(void) { return SAF_MBOX->cpu1_ready == SAF_CPU1_READY; }

static void mbox_persist(uint32_t now_ms)
{
    struct saf_mbox_s *m = SAF_MBOX;
    if (!cpu1_ready() || m->put_seq != m->put_ack) return;           /* CPU1 still busy with the last record */
    uint16_t id;
    if (Saf_PopGone(&g_saf, &id)) {
        memset((void *)&m->put, 0, offsetof(struct saf_wire_s, body));
        m->put.id = id;
        m->put.op = SAF_OP_DELETE;
    } else {
        int i = Saf_NextDirty(&g_saf);
        if (i < 0) return;
        const SafSlot_t *s = &g_saf.slot[i];
        m->put.id = s->id;
        m->put.op = SAF_OP_STORE;
        m->put.len = s->len;
        m->put.hops_left = s->hops_left;
        m->put.fwd_count = s->fwd_count;
        m->put.reserved = 0;
        m->put.ttl_left_s = Saf_TtlLeftMs(&g_saf, i, now_ms) / 1000UL;
        memcpy((void *)m->put.body, s->body, s->len);
        Saf_ClearDirty(&g_saf, i);
    }
    SAF_BARRIER();
    m->put_seq++;
}

static void mbox_restore(uint32_t now_ms)
{
    struct saf_mbox_s *m = SAF_MBOX;
    if (m->rst_seq != m->rst_ack) {
        SAF_BARRIER();
        if (m->rst.op == SAF_OP_STORE && m->rst.len >= 16U && m->rst.len <= SAF_BODY_MAX) {
            uint8_t body[SAF_BODY_MAX];
            memcpy(body, (const void *)m->rst.body, m->rst.len);
            if (Saf_Restore(&g_saf, m->rst.id, body, m->rst.len, m->rst.hops_left, m->rst.fwd_count,
                            m->rst.ttl_left_s * 1000UL, now_ms)) s_restored++;
        } else if (m->rst.op == SAF_OP_END) {
            s_restoring = false;
            uart1_printf("[SAF] restored %lu stored message(s) from the journal\r\n", (unsigned long)s_restored);
        }
        m->rst_ack = m->rst_seq;
    }
    if (s_restoring && (uint32_t)(now_ms - s_restoreT0) > SAT_SAF_RESTORE_MS) {
        s_restoring = false;
        uart1_puts("[SAF] no journal answer: starting with an empty store\r\n");
    }
}
#endif

/* ------------------------------------------------------------------------------------------------------------------
 * the decision for one frame body (from the air, or injected over the VCP for tests)
 * ------------------------------------------------------------------------------------------------------------------ */
static DigiResult_t digi_body(const uint8_t *body, uint16_t blen, uint32_t now_ms, bool *sent)
{
    char txt[DIGI_MAX_BODY + 40];
    DigiFrame_t f;
    uint16_t olen = 0;
    *sent = false;
    if (Digi_ParseFrame(body, blen, &f)) Digi_FormatFrame(&f, txt, sizeof(txt)); else txt[0] = '\0';
    DigiResult_t r = Digi_Process(&g_digi, body, blen, now_ms, s_out, sizeof(s_out), &olen);
    if (r != DIGI_REPEAT) {
        uart1_printf("[DIGI] not repeated (%s): %s\r\n", Digi_ReasonName(r), txt);
        return r;
    }

    DigiFrame_t g;
    if (Digi_ParseFrame(s_out, olen, &g)) Digi_FormatFrame(&g, txt, sizeof(txt));
#if S2S2_SAF_ENABLE
    uint8_t hops = Digi_HopsLeft(s_out, olen);
    int k = Saf_Store(&g_saf, s_out, olen, hops, now_ms, S2S2_SAF_IMMEDIATE ? S2S2_SAF_INTERVAL_MS : SAT_SAF_FIRST_MS);
#if S2S2_SAF_APPEND_STATS
    if (k < 0) {                                       /* already stored: report that copy */
        for (uint8_t i = 0; i < SAF_SLOTS; i++)
            if (g_saf.slot[i].used && g_saf.slot[i].len == olen && memcmp(g_saf.slot[i].body, s_out, olen) == 0) { k = i; break; }
    }
    olen = append_stats(s_out, olen, k, now_ms);       /* only the copy sent now; the store keeps the original */
#endif
    uart1_printf("[SAF] %s (hops left %u, %u stored)\r\n", (k >= 0) ? "stored" : ((k == -1) ? "already stored" : "not stored"),
                 (unsigned)hops, (unsigned)Saf_Count(&g_saf));
#endif
#if S2S2_SAF_IMMEDIATE || !S2S2_SAF_ENABLE
    uart1_printf("[DIGI] REPEAT (%u B): %s\r\n", olen, txt);
    CPU2_Delay_Ms(SAT_DIGI_TX_GUARD_MS);
    *sent = Satellite_Send_Raw_Frame(s_out, olen);
    if (!*sent) uart1_puts("[DIGI] transmit failed\r\n");
#else
    uart1_printf("[DIGI] accepted, will be sent from the store (%u B): %s\r\n", olen, txt);
#endif
    return r;
}

bool Sat_Digi_Init(void)
{
    DigiConfig_t c;
    build_config(&c);
    Digi_Init(&g_digi, &c);
    SafConfig_t sc = { S2S2_SAF_MAX_FORWARDS, S2S2_SAF_INTERVAL_MS, S2S2_SAF_TTL_MS };
    Saf_Init(&g_saf, &sc);
    bool ok = self_test();
    g_digiSelfTest = ok ? 0xA5A50002UL : 0xDEAD0002UL;
    uart1_printf("[DIGI] %s: %s%s, alias %s, WIDE2-N<=%d, dedupe %lu s, %d repeats/min\r\n",
                 c.enabled ? "ON" : "OFF", S2S2_SAT_CALLSIGN, "", S2S2_DIGI_ALIAS[0] ? S2S2_DIGI_ALIAS : "(none)",
                 (int)S2S2_DIGI_MAX_N, (unsigned long)(S2S2_DIGI_DEDUPE_MS / 1000UL), (int)S2S2_DIGI_RATE_PER_MIN);
#if S2S2_SAF_ENABLE
    uart1_printf("[SAF] store and forward ON: %d sends per message, every %lu s, lifetime %lu min, %s\r\n",
                 (int)S2S2_SAF_MAX_FORWARDS, (unsigned long)(S2S2_SAF_INTERVAL_MS / 1000UL), (unsigned long)(S2S2_SAF_TTL_MS / 60000UL),
                 S2S2_SAF_IMMEDIATE ? "repeat right away too" : "only from the store");
    mbox_init(0);
#endif
    uart1_printf("[DIGI] rule self-test: %s\r\n", ok ? "PASS" : "FAIL");
    return ok;
}

bool Sat_Digi_Handle(const uint8_t *frame, uint16_t len, uint32_t now_ms)
{
    /* frame = 0x7E | body | FCS(2) | 0x7E ; only frames with a good FCS and a digipeater address are ours to judge */
    if (frame == NULL || len < 4U + 16U || frame[0] != AX25_FLAG || frame[len - 1U] != AX25_FLAG) return false;
    uint16_t blen = (uint16_t)(len - 4U);
    DigiFrame_t f;
    if (!Digi_ParseFrame(&frame[1], blen, &f) || f.nvia == 0) return false;
    if (!AX25_VerifyCRC_Method1(frame, len)) {
        uart1_puts("[DIGI] frame with a digipeater path but a bad FCS: ignored\r\n");
        return true;
    }
    bool sent;
    (void)digi_body(&frame[1], blen, now_ms, &sent);
    return true;
}

void Sat_Digi_Service(uint32_t now_ms, bool allow_tx)
{
#if S2S2_SAF_ENABLE
    Saf_Expire(&g_saf, now_ms);
    mbox_restore(now_ms);
    mbox_persist(now_ms);

    /* a frame handed over by CPU1 (VCP test command): treated exactly like one heard on the radio */
    struct saf_mbox_s *m = SAF_MBOX;
    if (m->inject_seq != m->inject_ack) {
        SAF_BARRIER();
        uint8_t body[SAF_BODY_MAX];
        uint8_t n = m->inject.len;
        uint32_t res = 0x1FFU;
        if (n >= 16U && n <= SAF_BODY_MAX) {
            memcpy(body, (const void *)m->inject.body, n);
            bool sent = false;
            res = (uint32_t)digi_body(body, n, now_ms, &sent) | (sent ? 0x100UL : 0UL);
        }
        m->inject_result = res;
        SAF_BARRIER();
        m->inject_ack = m->inject_seq;
    }

    /* send stored messages: one at a time, never while a command is being served, not right after a restore request */
    if (allow_tx && !s_restoring && (int32_t)(now_ms - s_nextFwd) >= 0) {
        int i = Saf_NextDue(&g_saf, now_ms);
        if (i >= 0) {
            char txt[DIGI_MAX_BODY + 40];
            DigiFrame_t f;
            if (Digi_ParseFrame(g_saf.slot[i].body, g_saf.slot[i].len, &f)) Digi_FormatFrame(&f, txt, sizeof(txt)); else txt[0] = '\0';
            uart1_printf("[SAF] forward %u/%u (hops left %u): %s\r\n", (unsigned)(g_saf.slot[i].fwd_count + 1U),
                         (unsigned)S2S2_SAF_MAX_FORWARDS, (unsigned)g_saf.slot[i].hops_left, txt);
            CPU2_Delay_Ms(SAT_DIGI_TX_GUARD_MS);
            Satellite_Send_Raw_Frame(g_saf.slot[i].body, g_saf.slot[i].len);
            Saf_Forwarded(&g_saf, i, now_ms);
            s_nextFwd = now_ms + SAT_SAF_TX_GAP_MS;
        }
    }
#else
    (void)now_ms; (void)allow_tx;
#endif
}

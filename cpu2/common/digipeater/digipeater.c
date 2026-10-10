#include "digipeater.h"

#include <string.h>

/* ------------------------------------------------------------------------------------------------------------------
 * addresses
 * ------------------------------------------------------------------------------------------------------------------ */
static bool decode_addr(const uint8_t *p, DigiAddr_t *a)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < DIGI_CALL_LEN; i++) {
        if (p[i] & 0x01U) return false;                 /* the extension bit is only legal in the SSID byte */
        char c = (char)(p[i] >> 1);
        if (c == ' ') continue;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
        a->call[n++] = c;
    }
    if (n == 0) return false;
    a->call[n] = '\0';
    a->ssid = (uint8_t)((p[6] >> 1) & 0x0FU);
    a->h = (p[6] & 0x80U) != 0;
    a->reserved = (uint8_t)(p[6] & 0x60U);
    return true;
}

static void encode_addr(uint8_t *p, const DigiAddr_t *a, bool last)
{
    size_t n = strlen(a->call);
    for (uint8_t i = 0; i < DIGI_CALL_LEN; i++)
        p[i] = (uint8_t)(((i < n) ? a->call[i] : ' ') << 1);
    p[6] = (uint8_t)((a->h ? 0x80U : 0x00U) | (a->reserved & 0x60U) | ((a->ssid & 0x0FU) << 1) | (last ? 0x01U : 0x00U));
}

static bool addr_is(const DigiAddr_t *a, const char *call, uint8_t ssid)
{
    return a->ssid == ssid && strcmp(a->call, call) == 0;
}

static void set_addr(DigiAddr_t *a, const char *call, uint8_t ssid, bool h)
{
    memset(a, 0, sizeof(*a));
    strncpy(a->call, call, DIGI_CALL_LEN);
    a->ssid = ssid;
    a->h = h;
    a->reserved = 0x60U;
}

/* ------------------------------------------------------------------------------------------------------------------
 * parse / build
 * ------------------------------------------------------------------------------------------------------------------ */
bool Digi_ParseFrame(const uint8_t *body, uint16_t len, DigiFrame_t *f)
{
    if (body == NULL || f == NULL || len < 16U) return false;          /* 2 addresses + control + pid */
    memset(f, 0, sizeof(*f));
    if (!decode_addr(body, &f->dest) || !decode_addr(body + 7, &f->src)) return false;
    if (body[6] & 0x01U) return false;                                  /* the address field cannot end after the destination */

    uint16_t pos = 14;
    bool last = (body[13] & 0x01U) != 0;
    while (!last) {
        if (f->nvia >= DIGI_MAX_VIA || (uint16_t)(pos + 7U + 2U) > len) return false;
        if (!decode_addr(body + pos, &f->via[f->nvia])) return false;
        last = (body[pos + 6] & 0x01U) != 0;
        f->nvia++;
        pos += 7;
    }
    if ((uint16_t)(pos + 2U) > len) return false;
    f->control = body[pos];
    f->pid = body[pos + 1];
    f->info = body + pos + 2;
    f->infoLen = (uint16_t)(len - pos - 2U);
    return true;
}

uint16_t Digi_BuildFrame(const DigiFrame_t *f, uint8_t *out, uint16_t outMax)
{
    if (f == NULL || out == NULL || f->nvia > DIGI_MAX_VIA) return 0;
    uint16_t need = (uint16_t)(14U + 7U * f->nvia + 2U + f->infoLen);
    if (need > outMax) return 0;
    encode_addr(out, &f->dest, false);
    encode_addr(out + 7, &f->src, f->nvia == 0);
    for (uint8_t i = 0; i < f->nvia; i++)
        encode_addr(out + 14 + 7U * i, &f->via[i], i == f->nvia - 1U);
    uint16_t pos = (uint16_t)(14U + 7U * f->nvia);
    out[pos] = f->control;
    out[pos + 1] = f->pid;
    if (f->infoLen) memmove(out + pos + 2, f->info, f->infoLen);      /* memmove: info may live in the same buffer */
    return need;
}

/* ------------------------------------------------------------------------------------------------------------------
 * helpers: hashing, token bucket
 * ------------------------------------------------------------------------------------------------------------------ */
static uint32_t fnv(uint32_t h, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    while (n--) { h ^= *p++; h *= 16777619UL; }
    return h;
}

/* key = source + destination + info, NOT the path (Dire Wolf / aprsdigi). Trailing CR, LF and blanks of the info are
 * ignored so a station that appends a newline is still recognised. */
static uint32_t frame_key(const DigiFrame_t *f)
{
    uint32_t h = 2166136261UL;
    h = fnv(h, f->src.call, strlen(f->src.call)); h = fnv(h, &f->src.ssid, 1);
    h = fnv(h, f->dest.call, strlen(f->dest.call)); h = fnv(h, &f->dest.ssid, 1);
    uint16_t n = f->infoLen;
    while (n && (f->info[n - 1] == '\r' || f->info[n - 1] == '\n' || f->info[n - 1] == ' ')) n--;
    return fnv(h, f->info, n);
}

static uint32_t src_key(const DigiFrame_t *f)
{
    uint32_t h = 2166136261UL;
    h = fnv(h, f->src.call, strlen(f->src.call));
    return fnv(h, &f->src.ssid, 1);
}

static bool age_ok(uint32_t now, uint32_t t, uint32_t window)
{
    return (uint32_t)(now - t) < window;          /* unsigned difference: safe across the 32-bit millisecond wrap */
}

static bool bucket_take(Digi_t *d, uint32_t now_ms)
{
    const DigiConfig_t *c = &d->cfg;
    if (c->rate_per_min == 0) return true;
    uint32_t cap = (uint32_t)(c->burst ? c->burst : 1U) * 1000UL;
    if (!d->tokens_init) { d->tokens_milli = cap; d->tokens_t = now_ms; d->tokens_init = true; }
    uint32_t dt = (uint32_t)(now_ms - d->tokens_t);
    d->tokens_t = now_ms;
    /* rate_per_min repeats per 60 s = rate_per_min*1000/60000 tokens per ms, in milli tokens: rate_per_min / 60 per ms */
    if (dt > 600000UL) dt = 600000UL;                      /* an idle spell longer than 10 min fills the bucket anyway; keeps the math 32-bit */
    d->tokens_milli += (dt * c->rate_per_min) / 60U;
    if (d->tokens_milli > cap) d->tokens_milli = cap;
    if (d->tokens_milli < 1000UL) return false;
    d->tokens_milli -= 1000UL;
    return true;
}

/* ------------------------------------------------------------------------------------------------------------------
 * configuration
 * ------------------------------------------------------------------------------------------------------------------ */
void Digi_DefaultConfig(DigiConfig_t *cfg, const char *mycall, uint8_t myssid)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->mycall, mycall, DIGI_CALL_LEN);
    cfg->myssid = myssid;
    cfg->wide_enable = true;
    cfg->wide_tiers = 0x04U;            /* WIDE2-N: the wide-area tier a satellite belongs to; WIDE1-1 is for ground fill-in digis */
    cfg->wide_max_ssid = 2;
    cfg->trace = true;
    cfg->max_via = DIGI_MAX_VIA;
    cfg->dedupe_ms = 30000UL;
    cfg->rate_per_min = 6;
    cfg->burst = 2;
    cfg->src_min_interval_ms = 10000UL;
    cfg->enabled = true;
}

void Digi_Init(Digi_t *d, const DigiConfig_t *cfg)
{
    memset(d, 0, sizeof(*d));
    d->cfg = *cfg;
    if (d->cfg.max_via == 0 || d->cfg.max_via > DIGI_MAX_VIA) d->cfg.max_via = DIGI_MAX_VIA;
    if (d->cfg.nAlias > DIGI_MAX_ALIASES) d->cfg.nAlias = DIGI_MAX_ALIASES;
    if (d->cfg.nDeny > DIGI_MAX_DENY) d->cfg.nDeny = DIGI_MAX_DENY;
}

void Digi_ResetStats(Digi_t *d) { memset(&d->stats, 0, sizeof(d->stats)); }

const char *Digi_ReasonName(DigiResult_t r)
{
    static const char *const n[DIGI_REASON_COUNT] = {
        "repeat", "disabled", "malformed", "not-ui", "no-path", "not-for-us", "own-source", "loop", "denied",
        "duplicate", "hops", "too-long", "rate", "source-rate" };
    return ((unsigned)r < DIGI_REASON_COUNT) ? n[r] : "?";
}

size_t Digi_FormatFrame(const DigiFrame_t *f, char *out, size_t outMax)
{
    size_t n = 0;
#define PUT(c) do { if (n + 1 < outMax) out[n] = (c); n++; } while (0)
#define PUTS(s) do { for (const char *q_ = (s); *q_; q_++) PUT(*q_); } while (0)
#define PUT_ADDR(a_, star_) do { PUTS((a_)->call); if ((a_)->ssid) { PUT('-'); \
        if ((a_)->ssid >= 10) { PUT('1'); PUT((char)('0' + (a_)->ssid - 10)); } else PUT((char)('0' + (a_)->ssid)); } \
        if (star_) PUT('*'); } while (0)
    PUT_ADDR(&f->src, false);
    PUT('>');
    PUT_ADDR(&f->dest, false);
    /* TNC2 monitor style: one '*' after the LAST used entry */
    for (uint8_t i = 0; i < f->nvia; i++) {
        PUT(',');
        PUT_ADDR(&f->via[i], f->via[i].h && (i + 1U == f->nvia || !f->via[i + 1U].h));
    }
    PUT(':');
    for (uint16_t i = 0; i < f->infoLen; i++) {
        uint8_t c = f->info[i];
        PUT((c >= 32 && c < 127) ? (char)c : '.');
    }
    if (outMax) out[(n < outMax) ? n : outMax - 1U] = '\0';
    return n;
#undef PUT
#undef PUTS
#undef PUT_ADDR
}

/* ------------------------------------------------------------------------------------------------------------------
 * the decision
 * ------------------------------------------------------------------------------------------------------------------ */
static bool is_alias(const DigiConfig_t *c, const DigiAddr_t *a)
{
    for (uint8_t i = 0; i < c->nAlias; i++)
        if (a->ssid == 0 && strcmp(a->call, c->alias[i]) == 0) return true;
    return false;
}

/* WIDEn-N: call "WIDE" + one digit 1..7, SSID N = hops left */
static bool is_wide(const DigiAddr_t *a, uint8_t *tier)
{
    if (strlen(a->call) != 5 || strncmp(a->call, "WIDE", 4) != 0) return false;
    char t = a->call[4];
    if (t < '1' || t > '7') return false;
    *tier = (uint8_t)(t - '0');
    return true;
}

static DigiResult_t drop(Digi_t *d, DigiResult_t r)
{
    d->stats.drop[r]++;
    return r;
}

DigiResult_t Digi_Process(Digi_t *d, const uint8_t *body, uint16_t len, uint32_t now_ms,
                          uint8_t *out, uint16_t outMax, uint16_t *outLen)
{
    const DigiConfig_t *c = &d->cfg;
    DigiFrame_t f;
    d->stats.rx++;
    if (outLen) *outLen = 0;

    if (!c->enabled) return drop(d, DIGI_DROP_DISABLED);
    if (!Digi_ParseFrame(body, len, &f)) return drop(d, DIGI_DROP_MALFORMED);
    if (f.control != 0x03U || (!c->allow_any_pid && f.pid != 0xF0U)) return drop(d, DIGI_DROP_NOT_UI);
    if (f.nvia > c->max_via) return drop(d, DIGI_DROP_HOPS);

    /* first via entry that has not been used yet */
    int r = -1;
    for (uint8_t i = 0; i < f.nvia; i++) if (!f.via[i].h) { r = i; break; }
    if (r < 0) return drop(d, DIGI_DROP_NO_PATH);

    if (addr_is(&f.src, c->mycall, c->myssid)) return drop(d, DIGI_DROP_OWN_SOURCE);
    for (uint8_t i = 0; i < c->nDeny; i++)
        if (strcmp(f.src.call, c->deny[i]) == 0) return drop(d, DIGI_DROP_DENIED);
    /* loop: our call already carries the H bit somewhere in the path -> we repeated this frame before */
    for (uint8_t i = 0; i < f.nvia; i++)
        if (f.via[i].h && addr_is(&f.via[i], c->mycall, c->myssid)) return drop(d, DIGI_DROP_LOOP);

    /* ---- which rule applies, and the rewrite ---- */
    DigiAddr_t me;
    set_addr(&me, c->mycall, c->myssid, true);
    DigiAddr_t newVia[DIGI_MAX_VIA + 1];
    uint8_t n = 0;
    bool matched = false;

    /* via entries that were used already stay in front, unchanged */
    #define KEEP_USED_UP_TO(k)  for (int q_ = 0; q_ < (k); q_++) newVia[n++] = f.via[q_]
    const DigiAddr_t *cur = &f.via[r];
    uint8_t tier = 0;

    if (addr_is(cur, c->mycall, c->myssid) || is_alias(c, cur)) {
        KEEP_USED_UP_TO(r);
        newVia[n++] = me;                                       /* "MYCALL*" replaces the entry */
        for (uint8_t i = (uint8_t)(r + 1); i < f.nvia; i++) newVia[n++] = f.via[i];
        matched = true;
    } else if (c->wide_enable && is_wide(cur, &tier) && ((c->wide_tiers >> tier) & 1U)) {
        if (cur->ssid < 1 || cur->ssid > c->wide_max_ssid) return drop(d, DIGI_DROP_HOPS);
        KEEP_USED_UP_TO(r);
        /* trace needs room: when the path is full the call sign is simply not inserted (Dire Wolf does the same) */
        bool doTrace = c->trace && (uint8_t)(f.nvia + 1U) <= c->max_via;
        if (cur->ssid == 1) {
            if (c->trace) {
                newVia[n++] = me;                               /* WIDEn-1 -> MYCALL*: the path is used up, our call is the trace */
            } else {
                DigiAddr_t w = *cur; w.ssid = 0; w.h = true;    /* WIDEn* */
                newVia[n++] = w;
            }
        } else {
            DigiAddr_t w = *cur; w.ssid--;                      /* WIDEn-N -> WIDEn-(N-1), H stays clear */
            if (doTrace) newVia[n++] = me;
            newVia[n++] = w;
        }
        for (uint8_t i = (uint8_t)(r + 1); i < f.nvia; i++) newVia[n++] = f.via[i];
        matched = true;
    } else if (c->preempt) {
        /* our call or an alias further down: drop the unused entries in front, mark ours as used */
        for (uint8_t i = (uint8_t)(r + 1); i < f.nvia && !matched; i++) {
            if (addr_is(&f.via[i], c->mycall, c->myssid) || is_alias(c, &f.via[i])) {
                n = 0;
                KEEP_USED_UP_TO(r);
                newVia[n++] = me;
                for (uint8_t k = (uint8_t)(i + 1); k < f.nvia; k++) newVia[n++] = f.via[k];
                matched = true;
            }
        }
    }
    if (!matched) return drop(d, DIGI_DROP_NOT_FOR_US);

    /* ---- duplicates, rates (state changes only when we really repeat) ---- */
    uint32_t key = frame_key(&f), skey = src_key(&f);
    if (c->dedupe_ms) {
        for (uint8_t i = 0; i < DIGI_DEDUPE_SLOTS; i++)
            if (d->dedupe[i].used && d->dedupe[i].hash == key && age_ok(now_ms, d->dedupe[i].t, c->dedupe_ms))
                return drop(d, DIGI_DROP_DUPLICATE);
    }
    if (c->src_min_interval_ms) {
        for (uint8_t i = 0; i < DIGI_SOURCE_SLOTS; i++)
            if (d->srcs[i].used && d->srcs[i].hash == skey && age_ok(now_ms, d->srcs[i].t, c->src_min_interval_ms))
                return drop(d, DIGI_DROP_SOURCE_RATE);
    }

    DigiFrame_t g = f;
    memcpy(g.via, newVia, sizeof(DigiAddr_t) * n);
    g.nvia = n;
    uint16_t built = Digi_BuildFrame(&g, out, (outMax < DIGI_MAX_BODY) ? outMax : (uint16_t)DIGI_MAX_BODY);
    if (built == 0) return drop(d, DIGI_DROP_TOO_LONG);

    if (!bucket_take(d, now_ms)) return drop(d, DIGI_DROP_RATE);

    if (c->dedupe_ms) {
        d->dedupe[d->dedupeNext].hash = key; d->dedupe[d->dedupeNext].t = now_ms; d->dedupe[d->dedupeNext].used = true;
        d->dedupeNext = (uint8_t)((d->dedupeNext + 1U) % DIGI_DEDUPE_SLOTS);
    }
    if (c->src_min_interval_ms) {
        uint8_t slot = d->srcNext;
        for (uint8_t i = 0; i < DIGI_SOURCE_SLOTS; i++) if (d->srcs[i].used && d->srcs[i].hash == skey) { slot = i; break; }
        d->srcs[slot].hash = skey; d->srcs[slot].t = now_ms; d->srcs[slot].used = true;
        if (slot == d->srcNext) d->srcNext = (uint8_t)((d->srcNext + 1U) % DIGI_SOURCE_SLOTS);
    }
    d->stats.repeated++;
    d->stats.last_repeat_ms = now_ms;
    if (outLen) *outLen = built;
    return DIGI_REPEAT;
}

/* ------------------------------------------------------------------------------------------------------------------
 * TNC2 text -> frame
 * ------------------------------------------------------------------------------------------------------------------ */
static bool tnc2_addr(const char **pp, DigiAddr_t *a)
{
    const char *p = *pp;
    char call[DIGI_CALL_LEN + 1];
    uint8_t n = 0, ssid = 0;
    while (*p && *p != '>' && *p != ',' && *p != ':' && *p != '*' && *p != '-') {
        char c = *p++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (n >= DIGI_CALL_LEN || !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
        call[n++] = c;
    }
    if (n == 0) return false;
    call[n] = '\0';
    if (*p == '-') {
        p++;
        if (*p < '0' || *p > '9') return false;
        ssid = (uint8_t)(*p++ - '0');
        if (*p >= '0' && *p <= '9') ssid = (uint8_t)(ssid * 10U + (uint8_t)(*p++ - '0'));
        if (ssid > 15) return false;
    }
    set_addr(a, call, ssid, false);
    *pp = p;
    return true;
}

uint16_t Digi_FromTnc2(const char *text, uint8_t *body, uint16_t bodyMax)
{
    DigiFrame_t f;
    memset(&f, 0, sizeof(f));
    const char *p = text;
    if (!tnc2_addr(&p, &f.src) || *p != '>') return 0;
    p++;
    if (!tnc2_addr(&p, &f.dest)) return 0;
    int star = -1;
    while (*p == ',') {
        if (f.nvia >= DIGI_MAX_VIA) return 0;
        p++;
        if (!tnc2_addr(&p, &f.via[f.nvia])) return 0;
        if (*p == '*') { star = f.nvia; p++; }
        f.nvia++;
    }
    if (*p != ':') return 0;
    for (int i = 0; i <= star; i++) f.via[i].h = true;
    p++;
    f.control = 0x03;
    f.pid = 0xF0;
    f.info = (const uint8_t *)p;
    f.infoLen = (uint16_t)strlen(p);
    return Digi_BuildFrame(&f, body, bodyMax);
}

uint8_t Digi_HopsLeft(const uint8_t *body, uint16_t len)
{
    DigiFrame_t f;
    uint8_t tier;
    if (!Digi_ParseFrame(body, len, &f)) return 0;
    for (uint8_t i = 0; i < f.nvia; i++)
        if (!f.via[i].h) return is_wide(&f.via[i], &tier) ? f.via[i].ssid : 0;
    return 0;
}

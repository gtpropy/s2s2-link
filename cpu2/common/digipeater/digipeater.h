#ifndef DIGIPEATER_H
#define DIGIPEATER_H

/*
 * APRS / AX.25 UI digipeater engine -- pure logic, no hardware, no radio, no RTOS.
 *
 * It takes one received frame (as bytes), decides whether this station has to repeat it and, if so, produces the
 * rewritten frame. The caller owns the radio: receive -> Digi_Process() -> transmit the result when it says REPEAT.
 * Works with any modem (here: the 4800 bit/s GMSK path through the SX126x, or a host test harness). See docs/DIGIPEATER.md for the architecture and the rules with their sources.
 *
 * Frame layout used everywhere in this API ("body"): the AX.25 frame WITHOUT flags and WITHOUT FCS:
 *     destination(7) source(7) [via(7) x 0..8] control(1) pid(1) info(0..n)
 *
 * Rules (New-N paradigm, as Dire Wolf's digipeat_match and the AX.25 v2.2 digipeater description):
 *   1. only UI frames (control 0x03) with PID 0xF0 (or any PID when allow_any_pid)
 *   2. look at the FIRST via entry whose H ("has been repeated") bit is clear
 *   3. repeat when that entry is   our call sign        -> replaced by "MYCALL*"
 *                                  an alias (ARISS...)  -> replaced by "MYCALL*"       (trapped once)
 *                                  WIDEn-N, n allowed   -> trace: insert "MYCALL*" before it and decrement N
 *                                                          (WIDEn-1 becomes MYCALL*, WIDEn-0* never printed); without trace
 *                                                          only N is decremented and H is set when N reaches 0
 *   4. never repeat: our own source, a path where our call already has H set (loop), denied stations, duplicates
 *      (same source + destination + info inside dedupe_ms), over-long results, paths that would exceed max_via, and
 *      anything above the rate limits (global token bucket and a minimum interval per source)
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIGI_MAX_VIA        8U      /* AX.25 v2.0 limit used by APRS */
#define DIGI_CALL_LEN       6U
#define DIGI_MAX_ALIASES    4U
#define DIGI_MAX_DENY       4U
#define DIGI_DEDUPE_SLOTS   32U
#define DIGI_SOURCE_SLOTS   8U
#define DIGI_MAX_BODY       160U    /* longest frame body we re-emit: must fit the fixed 200-byte radio packet */

typedef struct {
    char    call[DIGI_CALL_LEN + 1];   /* upper case, trailing blanks removed */
    uint8_t ssid;                      /* 0..15 */
    bool    h;                         /* via entries: has-been-repeated bit; dest/source: the C bit */
    uint8_t reserved;                  /* bits 6..5 of the SSID byte as received (normally 0x60) */
} DigiAddr_t;

typedef struct {
    DigiAddr_t dest, src, via[DIGI_MAX_VIA];
    uint8_t    nvia;
    uint8_t    control, pid;
    const uint8_t *info;
    uint16_t   infoLen;
} DigiFrame_t;

typedef enum {
    DIGI_REPEAT = 0,        /* out/outLen hold the frame to transmit */
    DIGI_DROP_DISABLED,
    DIGI_DROP_MALFORMED,
    DIGI_DROP_NOT_UI,
    DIGI_DROP_NO_PATH,      /* no unused via entry: heard direct or fully repeated */
    DIGI_DROP_NOT_FOR_US,   /* next unused entry is neither our call, an alias nor an allowed WIDEn-N */
    DIGI_DROP_OWN_SOURCE,
    DIGI_DROP_LOOP,
    DIGI_DROP_DENIED,
    DIGI_DROP_DUPLICATE,
    DIGI_DROP_HOPS,         /* WIDEn-N with N above the limit, or the path would get too long */
    DIGI_DROP_TOO_LONG,
    DIGI_DROP_RATE,         /* global token bucket empty */
    DIGI_DROP_SOURCE_RATE,  /* same source again inside src_min_interval_ms */
    DIGI_REASON_COUNT
} DigiResult_t;

typedef struct {
    char     mycall[DIGI_CALL_LEN + 1];
    uint8_t  myssid;
    char     alias[DIGI_MAX_ALIASES][DIGI_CALL_LEN + 1];   /* "" = unused, e.g. "ARISS" */
    uint8_t  nAlias;
    bool     wide_enable;               /* answer WIDEn-N paths */
    uint8_t  wide_tiers;                /* bit n set = WIDEn answered; satellites normally answer WIDE2 only: 0x04 */
    uint8_t  wide_max_ssid;             /* highest N accepted (flooding control), 1..7, usually 2 */
    bool     trace;                     /* insert our call sign into WIDEn-N paths (New-N trace) */
    bool     preempt;                   /* also repeat when our call / an alias is further down the path */
    bool     allow_any_pid;             /* APRS = 0xF0; set for other protocols on the same channel */
    uint8_t  max_via;                  /* path length cap, 1..DIGI_MAX_VIA */
    uint32_t dedupe_ms;                 /* 0 = off; Dire Wolf 30 s, aprsdigi 28 s, PACSAT 28 s */
    uint16_t rate_per_min;              /* sustained repeats per minute, 0 = unlimited */
    uint8_t  burst;                     /* token bucket size */
    uint32_t src_min_interval_ms;       /* 0 = off */
    char     deny[DIGI_MAX_DENY][DIGI_CALL_LEN + 1];       /* source call signs never repeated */
    uint8_t  nDeny;
    bool     enabled;
} DigiConfig_t;

typedef struct {
    uint32_t rx;                        /* frames offered to Digi_Process */
    uint32_t repeated;
    uint32_t drop[DIGI_REASON_COUNT];   /* indexed by DigiResult_t (REPEAT slot unused) */
    uint32_t last_repeat_ms;
} DigiStats_t;

typedef struct {
    DigiConfig_t cfg;
    DigiStats_t  stats;
    struct { uint32_t hash, t; bool used; } dedupe[DIGI_DEDUPE_SLOTS];
    uint8_t  dedupeNext;
    struct { uint32_t hash, t; bool used; } srcs[DIGI_SOURCE_SLOTS];
    uint8_t  srcNext;
    uint32_t tokens_milli;              /* token bucket, 1000 = one repeat */
    uint32_t tokens_t;
    bool     tokens_init;
} Digi_t;

/* defaults for a satellite: own call + alias, WIDE2-N (N<=2) with trace, 30 s dedupe, 6 repeats/min, 2 burst */
void Digi_DefaultConfig(DigiConfig_t *cfg, const char *mycall, uint8_t myssid);
void Digi_Init(Digi_t *d, const DigiConfig_t *cfg);
void Digi_ResetStats(Digi_t *d);

/* parse / build the body layout above; addresses are validated (printable characters, last-address bit) */
bool     Digi_ParseFrame(const uint8_t *body, uint16_t len, DigiFrame_t *f);
uint16_t Digi_BuildFrame(const DigiFrame_t *f, uint8_t *out, uint16_t outMax);   /* 0 on error */

/* the whole decision. out receives the rewritten body when the result is DIGI_REPEAT. now_ms = monotonic milliseconds. */
DigiResult_t Digi_Process(Digi_t *d, const uint8_t *body, uint16_t len, uint32_t now_ms,
                          uint8_t *out, uint16_t outMax, uint16_t *outLen);

const char *Digi_ReasonName(DigiResult_t r);

/* hop budget of a frame: the N of its first unused WIDEn-N entry (hops other digipeaters may still make), 0 if there is none */
uint8_t Digi_HopsLeft(const uint8_t *body, uint16_t len);

/* "SRC>DEST,VIA1*,VIA2:info" text of a frame (for logs and tests); returns the length */
size_t Digi_FormatFrame(const DigiFrame_t *f, char *out, size_t outMax);

/* the opposite: TNC2 monitor text -> frame body (UI frame, PID 0xF0). '*' marks the last used via entry, entries before it
 * count as used. Returns the body length or 0 on a syntax error. Used by the ground-station console and the host tests. */
uint16_t Digi_FromTnc2(const char *text, uint8_t *body, uint16_t bodyMax);

#ifdef __cplusplus
}
#endif

#endif /* DIGIPEATER_H */

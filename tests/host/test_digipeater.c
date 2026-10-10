/* Host tests of cpu2/common/digipeater: golden cases of the New-N rules, loop / duplicate / rate handling, round trips. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "digipeater.h"
#include "tnc2.h"

static int fails, checks;
#define CHK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint16_t tnc2(const char *txt, uint8_t *body, uint8_t control, uint8_t pid) { return tnc2_to_body(txt, body, 255, control, pid); }
static void show(const uint8_t *body, uint16_t len, char *out, size_t max)
{
    DigiFrame_t f; if (!Digi_ParseFrame(body, len, &f)) { strcpy(out, "<bad>"); return; }
    Digi_FormatFrame(&f, out, max);
}

static Digi_t D;
static DigiConfig_t cfg(void)
{
    DigiConfig_t c; Digi_DefaultConfig(&c, "9N2SJ", 1);
    strcpy(c.alias[0], "ARISS"); c.nAlias = 1;
    c.rate_per_min = 0; c.src_min_interval_ms = 0;        /* individual tests switch these on */
    return c;
}
static DigiResult_t run(const char *in, uint32_t t, char *res)
{
    uint8_t b[255], o[255]; uint16_t olen = 0;
    uint16_t n = tnc2(in, b, 0x03, 0xF0);
    DigiResult_t r = Digi_Process(&D, b, n, t, o, sizeof o, &olen);
    if (res) { if (r == DIGI_REPEAT) show(o, olen, res, 300); else strcpy(res, Digi_ReasonName(r)); }
    return r;
}
static void expect(const char *name, const char *in, const char *want)
{
    char got[300]; Digi_Init(&D, &(DigiConfig_t){0}); DigiConfig_t c = cfg(); Digi_Init(&D, &c);
    run(in, 1000, got);
    checks++;
    if (strcmp(got, want)) { printf("FAIL %-22s in : %s\n%-27s want: %s\n%-27s got : %s\n", name, in, "", want, "", got); fails++; }
}

int main(void)
{
    /* ---- the rules ---- */
    expect("own call",        "A1ABC>APRS,9N2SJ-1:hello",                 "A1ABC>APRS,9N2SJ-1*:hello");
    expect("alias",           "A1ABC>APRS,ARISS:x",                       "A1ABC>APRS,9N2SJ-1*:x");
    expect("wide2-2 trace",   "A1ABC>APRS,WIDE2-2:x",                     "A1ABC>APRS,9N2SJ-1*,WIDE2-1:x");
    expect("wide2-1 trace",   "A1ABC>APRS,WIDE2-1:x",                     "A1ABC>APRS,9N2SJ-1*:x");
    expect("wide1-1 fill-in", "A1ABC>APRS,WIDE1-1:x",                     "not-for-us");
    expect("fill-in then us", "A1ABC>APRS,K1FILL*,WIDE2-1:x",             "A1ABC>APRS,K1FILL,9N2SJ-1*:x");
    expect("fill-in not done","A1ABC>APRS,WIDE1-1,WIDE2-1:x",             "not-for-us");
    expect("wide2-3 too big", "A1ABC>APRS,WIDE2-3:x",                     "hops");
    expect("wide2-0",         "A1ABC>APRS,WIDE2:x",                       "hops");
    expect("wide3-3 tier",    "A1ABC>APRS,WIDE3-3:x",                     "not-for-us");
    expect("other digi",      "A1ABC>APRS,K1XYZ:x",                       "not-for-us");
    expect("other ssid",      "A1ABC>APRS,9N2SJ-2:x",                     "not-for-us");
    expect("direct",          "A1ABC>APRS:x",                             "no-path");
    expect("all used",        "A1ABC>APRS,K1XYZ*:x",                      "no-path");
    expect("own source",      "9N2SJ-1>APRS,WIDE2-2:x",                   "own-source");
    expect("loop",            "A1ABC>APRS,9N2SJ-1*,WIDE2-1:x",            "loop");
    expect("loop used mid",   "A1ABC>APRS,K1XYZ,9N2SJ-1*,WIDE2-2:x",      "loop");
    expect("ssid15 of ours",  "A1ABC>APRS,9N2SJ-1:\r",                    "A1ABC>APRS,9N2SJ-1*:.");
    expect("long info",       "A1ABC>APRS,ARISS:Hi there, this is a test",  "A1ABC>APRS,9N2SJ-1*:Hi there, this is a test");

    /* ---- configuration variants ---- */
    { char g[300]; DigiConfig_t c = cfg(); c.trace = false; Digi_Init(&D, &c);
      run("A1ABC>APRS,WIDE2-2:x", 1000, g); CHK(!strcmp(g, "A1ABC>APRS,WIDE2-1:x"));
      run("B2BBB>APRS,WIDE2-1:y", 2000, g); CHK(!strcmp(g, "B2BBB>APRS,WIDE2*:y")); }
    { char g[300]; DigiConfig_t c = cfg(); c.preempt = true; Digi_Init(&D, &c);
      run("A1ABC>APRS,FOO,9N2SJ-1:x", 1000, g); CHK(!strcmp(g, "A1ABC>APRS,9N2SJ-1*:x"));
      run("A1ABC>APRS,FOO,ARISS,BAR:y", 2000, g); CHK(!strcmp(g, "A1ABC>APRS,9N2SJ-1*,BAR:y")); }
    { char g[300]; DigiConfig_t c = cfg(); c.preempt = false; Digi_Init(&D, &c);
      run("A1ABC>APRS,FOO,9N2SJ-1:x", 1000, g); CHK(!strcmp(g, "not-for-us")); }
    { char g[300]; DigiConfig_t c = cfg(); c.wide_enable = false; Digi_Init(&D, &c);
      run("A1ABC>APRS,WIDE2-2:x", 1000, g); CHK(!strcmp(g, "not-for-us")); }
    { char g[300]; DigiConfig_t c = cfg(); c.wide_tiers = 0x06; c.wide_max_ssid = 3; Digi_Init(&D, &c);          /* WIDE1 and WIDE2 */
      run("A1ABC>APRS,WIDE1-1:x", 1000, g); CHK(!strcmp(g, "A1ABC>APRS,9N2SJ-1*:x"));
      run("B2BBB>APRS,WIDE2-3:x", 2000, g); CHK(!strcmp(g, "B2BBB>APRS,9N2SJ-1*,WIDE2-2:x")); }
    { char g[300]; DigiConfig_t c = cfg(); c.enabled = false; Digi_Init(&D, &c);
      run("A1ABC>APRS,ARISS:x", 1000, g); CHK(!strcmp(g, "disabled")); }
    { char g[300]; DigiConfig_t c = cfg(); strcpy(c.deny[0], "BADGUY"); c.nDeny = 1; Digi_Init(&D, &c);
      run("BADGUY>APRS,ARISS:x", 1000, g); CHK(!strcmp(g, "denied"));
      run("GOODGU>APRS,ARISS:x", 1000, g); CHK(!strcmp(g, "GOODGU>APRS,9N2SJ-1*:x")); }
    { char g[300]; DigiConfig_t c = cfg(); c.max_via = 4; Digi_Init(&D, &c);
      run("A>B,K1*,K2*,K3*,WIDE2-2:x", 1000, g);                           /* 4 entries: no room for the trace call */
      CHK(!strcmp(g, "A>B,K1,K2,K3,WIDE2-1:x") || !strcmp(g, "A>B,K1,K2,K3*,WIDE2-1:x"));
      run("A>B,K1*,K2*,K3*,K4*,WIDE2-2:x", 2000, g); CHK(!strcmp(g, "hops")); }          /* longer than the cap */
    { char g[300]; Digi_Init(&D, &(DigiConfig_t){0}); DigiConfig_t c = cfg(); Digi_Init(&D, &c);
      run("A>B,U1*,U2*,U3*,U4*,U5*,U6*,U7*,WIDE2-2:x", 1000, g);          /* 8 entries: AX.25 limit, no trace insertion */
      CHK(!strcmp(g, "A>B,U1,U2,U3,U4,U5,U6,U7*,WIDE2-1:x") || !strcmp(g, "A>B,U1,U2,U3,U4,U5,U6,U7,WIDE2-1:x")); }

    /* ---- duplicates ---- */
    { char g[300]; DigiConfig_t c = cfg(); Digi_Init(&D, &c);
      CHK(run("A1ABC>APRS,ARISS:same", 1000, g) == DIGI_REPEAT);
      CHK(run("A1ABC>APRS,ARISS:same", 20000, g) == DIGI_DROP_DUPLICATE);
      CHK(run("A1ABC>APRS,WIDE2-2:same", 25000, g) == DIGI_DROP_DUPLICATE);            /* the path is not part of the key */
      CHK(run("A1ABC>APRS,ARISS:same\r", 26000, g) == DIGI_DROP_DUPLICATE);            /* trailing CR ignored */
      CHK(run("A1ABC>APRS,ARISS:other", 27000, g) == DIGI_REPEAT);                      /* different info */
      CHK(run("A1ABD>APRS,ARISS:same", 28000, g) == DIGI_REPEAT);                       /* different source */
      CHK(run("A1ABC>APRS,ARISS:same", 31001, g) == DIGI_REPEAT);                       /* window over */
      CHK(D.stats.drop[DIGI_DROP_DUPLICATE] == 3); }
    { char g[300]; DigiConfig_t c = cfg(); Digi_Init(&D, &c);                           /* millisecond counter wrap */
      CHK(run("A1ABC>APRS,ARISS:w", 0xFFFFFF00UL, g) == DIGI_REPEAT);
      CHK(run("A1ABC>APRS,ARISS:w", 0x00000100UL, g) == DIGI_DROP_DUPLICATE);            /* 512 ms later */
      CHK(run("A1ABC>APRS,ARISS:w", 0x00007800UL, g) == DIGI_REPEAT); }                  /* 30.7 s later */
    { char g[300]; DigiConfig_t c = cfg(); c.dedupe_ms = 0; Digi_Init(&D, &c);
      CHK(run("A1ABC>APRS,ARISS:s", 1000, g) == DIGI_REPEAT);
      CHK(run("A1ABC>APRS,ARISS:s", 1001, g) == DIGI_REPEAT); }

    /* ---- rate limits ---- */
    { char g[300]; DigiConfig_t c = cfg(); c.rate_per_min = 6; c.burst = 2; Digi_Init(&D, &c);
      CHK(run("A1>APRS,ARISS:1", 0, g) == DIGI_REPEAT);
      CHK(run("A2>APRS,ARISS:2", 100, g) == DIGI_REPEAT);
      CHK(run("A3>APRS,ARISS:3", 200, g) == DIGI_DROP_RATE);
      CHK(run("A4>APRS,ARISS:4", 9000, g) == DIGI_DROP_RATE);                            /* 6/min = one token per 10 s */
      CHK(run("A5>APRS,ARISS:5", 10300, g) == DIGI_REPEAT);
      CHK(run("A6>APRS,ARISS:6", 10400, g) == DIGI_DROP_RATE);
      CHK(run("A7>APRS,ARISS:7", 700000, g) == DIGI_REPEAT);                             /* long idle refills only up to the burst size */
      CHK(run("A8>APRS,ARISS:8", 700001, g) == DIGI_REPEAT);
      CHK(run("A9>APRS,ARISS:9", 700002, g) == DIGI_DROP_RATE); }
    { char g[300]; DigiConfig_t c = cfg(); c.src_min_interval_ms = 10000; Digi_Init(&D, &c);
      CHK(run("A1>APRS,ARISS:1", 0, g) == DIGI_REPEAT);
      CHK(run("A1>APRS,ARISS:2", 5000, g) == DIGI_DROP_SOURCE_RATE);
      CHK(run("A2>APRS,ARISS:3", 5000, g) == DIGI_REPEAT);
      CHK(run("A1>APRS,ARISS:4", 10001, g) == DIGI_REPEAT); }

    /* ---- not UI / malformed / too long ---- */
    { uint8_t b[255], o[255]; uint16_t n, ol; DigiConfig_t c = cfg(); Digi_Init(&D, &c);
      n = tnc2("A1>APRS,ARISS:x", b, 0x13, 0xF0); CHK(Digi_Process(&D, b, n, 1, o, sizeof o, &ol) == DIGI_DROP_NOT_UI);
      n = tnc2("A1>APRS,ARISS:x", b, 0x03, 0xCF); CHK(Digi_Process(&D, b, n, 1, o, sizeof o, &ol) == DIGI_DROP_NOT_UI);
      CHK(Digi_Process(&D, b, 10, 1, o, sizeof o, &ol) == DIGI_DROP_MALFORMED);
      n = tnc2("A1>APRS,ARISS:x", b, 0x03, 0xF0); b[2] |= 1;                              /* extension bit inside a call sign */
      CHK(Digi_Process(&D, b, n, 1, o, sizeof o, &ol) == DIGI_DROP_MALFORMED);
      n = tnc2("A1>APRS,ARISS:x", b, 0x03, 0xF0); b[5] = 0x01 << 1 | 0x00; b[5] = '!' << 1;   /* illegal character */
      CHK(Digi_Process(&D, b, n, 1, o, sizeof o, &ol) == DIGI_DROP_MALFORMED);
      n = tnc2("A1>APRS,ARISS:x", b, 0x03, 0xF0); b[13] &= (uint8_t)~1;                    /* address field never ends */
      CHK(Digi_Process(&D, b, 16, 1, o, sizeof o, &ol) == DIGI_DROP_MALFORMED);
      { char big[200]; memset(big, 'z', 150); big[150] = 0; char in[300]; snprintf(in, sizeof in, "A1>APRS,WIDE2-2:%s", big);
        n = tnc2(in, b, 0x03, 0xF0);
        CHK(Digi_Process(&D, b, n, 2, o, sizeof o, &ol) == DIGI_DROP_TOO_LONG); }
      { char big[200]; memset(big, 'y', 120); big[120] = 0; char in[300]; snprintf(in, sizeof in, "A2>APRS,WIDE2-2:%s", big);
        n = tnc2(in, b, 0x03, 0xF0);
        CHK(Digi_Process(&D, b, n, 3, o, sizeof o, &ol) == DIGI_REPEAT && ol == n + 7); } }

    /* ---- parse / build round trip keeps every bit ---- */
    srand(7);
    for (int it = 0; it < 2000; it++) {
        uint8_t b[200], o[200]; DigiFrame_t f; uint16_t len;
        int nv = rand() % 9;
        len = 14 + 7 * nv + 2 + (rand() % 40);
        for (int i = 0; i < len; i++) b[i] = (uint8_t)(rand());
        for (int a = 0; a < 2 + nv; a++) {                 /* make addresses legal: A-Z0-9 or blank, extension bit only on the last */
            for (int k = 0; k < 6; k++) { int v = rand() % 37; char ch = (v < 26) ? 'A' + v : (v < 36 ? '0' + v - 26 : ' '); if (k == 0 && ch == ' ') ch = 'Q'; b[a * 7 + k] = (uint8_t)(ch << 1); }
            b[a * 7 + 6] = (uint8_t)((b[a * 7 + 6] & 0xFE) | ((a == 1 + nv) ? 1 : 0));
        }
        /* a call sign may only have blanks at the end */
        for (int a = 0; a < 2 + nv; a++) { int blank = 0; for (int k = 0; k < 6; k++) { if ((b[a*7+k] >> 1) == ' ') blank = 1; else if (blank) b[a*7+k] = (uint8_t)(' ' << 1); } }
        if (!Digi_ParseFrame(b, len, &f)) { CHK(0); continue; }
        CHK(f.nvia == nv);
        uint16_t m = Digi_BuildFrame(&f, o, sizeof o);
        if (!(m == len && memcmp(b, o, len) == 0) && fails < 5) { printf("rt it=%d nv=%d len=%d m=%d\n", it, nv, len, m); for (int i = 0; i < len && i < 40; i++) printf("%02x", b[i]); printf("\n"); for (int i = 0; i < m && i < 40; i++) printf("%02x", o[i]); printf("\n"); }
        CHK(m == len && memcmp(b, o, len) == 0);
    }

    printf(fails ? "digipeater FAILED %d of %d checks\n" : "digipeater OK (%d checks)\n", fails ? fails : checks, checks);
    return fails != 0;
}

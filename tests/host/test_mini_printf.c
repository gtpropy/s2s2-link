/* Compares mini_printf.c (renamed) with the host C library over random formats and arguments. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
int mini_vsnprintf(char *, size_t, const char *, va_list);
int mini_snprintf(char *, size_t, const char *, ...);

static unsigned long long rs = 88172645463325252ULL;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 11); }

int main(void)
{
    static const char *fmts[] = {
        "%d", "%u", "%lu", "%ld", "%02X", "%04X", "%08lX", "%02d", "%03lu", "%+d", "%2d", "%c", "%s", "%5s", "%-6s|",
        "%x", "%X", "%lx", "%#x", "%-5d|", "%05d", "%.3d", "%8.3d", "%+05d", "% d", "%.2s", "%10s", "%%", "%5c|",
        "RX=%d %s #%lu [%02X %02X %02X]", "%u/%u", "%08lX%04lX", "%3d%%",
    };
    long bad = 0, total = 0;
    for (int it = 0; it < 200000; it++) {
        const char *f = fmts[rnd() % (sizeof fmts / sizeof *fmts)];
        int32_t a = (int32_t)rnd(); uint32_t b = rnd();
        if (rnd() % 4 == 0) a >>= rnd() % 31;
        if (rnd() % 5 == 0) b >>= rnd() % 31;
        char s1[64], s2[64], s3[200], s4[200]; for (int i = 0; i < 6; i++) s1[i] = 'a' + (char)(rnd() % 26); s1[rnd() % 7] = 0;
        char r1[512], r2[512]; int n1, n2;
        if (!strstr(f, "RX=") && (strstr(f, "%s") || strstr(f, "%5s") || strstr(f, "%-6s") || strstr(f, "%10s") || strstr(f, "%.2s"))) {
            n1 = snprintf(r1, sizeof r1, f, s1); n2 = mini_snprintf(r2, sizeof r2, f, s1);
        } else if (strstr(f, "RX=")) {
            n1 = snprintf(r1, sizeof r1, f, a, s1, (unsigned long)b, b & 255, (b >> 8) & 255, (b >> 16) & 255);
            n2 = mini_snprintf(r2, sizeof r2, f, a, s1, (unsigned long)b, b & 255, (b >> 8) & 255, (b >> 16) & 255);
        } else if (strstr(f, "%u/%u")) {
            n1 = snprintf(r1, sizeof r1, f, b, b ^ a); n2 = mini_snprintf(r2, sizeof r2, f, b, b ^ a);
        } else if (strstr(f, "%08lX%04lX")) {
            n1 = snprintf(r1, sizeof r1, f, (unsigned long)b, (unsigned long)(b ^ a)); n2 = mini_snprintf(r2, sizeof r2, f, (unsigned long)b, (unsigned long)(b ^ a));
        } else if (strchr(f, 'c')) {
            n1 = snprintf(r1, sizeof r1, f, 'A' + (int)(b % 26)); n2 = mini_snprintf(r2, sizeof r2, f, 'A' + (int)(b % 26));
        } else if (strchr(f, 'u') || strchr(f, 'x') || strchr(f, 'X')) {
            if (strchr(f, 'l')) { n1 = snprintf(r1, sizeof r1, f, (unsigned long)b); n2 = mini_snprintf(r2, sizeof r2, f, (unsigned long)b); }
            else { n1 = snprintf(r1, sizeof r1, f, b); n2 = mini_snprintf(r2, sizeof r2, f, b); }
        } else if (strstr(f, "%%") && !strchr(f, 'd')) {
            n1 = snprintf(r1, sizeof r1, f); n2 = mini_snprintf(r2, sizeof r2, f);
        } else if (strchr(f, 'l')) {
            n1 = snprintf(r1, sizeof r1, f, (long)a); n2 = mini_snprintf(r2, sizeof r2, f, (long)a);
        } else {
            n1 = snprintf(r1, sizeof r1, f, a); n2 = mini_snprintf(r2, sizeof r2, f, a);
        }
        total++;
        if (n1 != n2 || strcmp(r1, r2)) { if (bad++ < 8) printf("MISMATCH fmt=\"%s\" a=%d b=%u\n  libc: [%s] (%d)\n  mini: [%s] (%d)\n", f, a, b, r1, n1, r2, n2); }
        /* truncation: small buffers must give the same text and return value */
        char t1[9], t2[9];
        int m1 = snprintf(t1, sizeof t1, "%08lX-%s", (unsigned long)b, "tail");
        int m2 = mini_snprintf(t2, sizeof t2, "%08lX-%s", (unsigned long)b, "tail");
        if (m1 != m2 || strcmp(t1, t2)) { if (bad++ < 8) printf("TRUNC MISMATCH [%s]/%d vs [%s]/%d\n", t1, m1, t2, m2); }
    }
    printf("%ld format cases compared with libc, mismatches: %ld\n", total, bad);
    return bad != 0;
}

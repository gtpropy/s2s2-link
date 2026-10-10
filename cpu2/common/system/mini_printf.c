/*
 * mini_printf.c -- compact vsnprintf/snprintf for the CPU2 firmware.
 *
 * Defining these here keeps newlib's vfprintf (and the malloc/stdio machinery
 * it drags in) out of the image. Supported: %d %i %u %x %X %c %s %p %%,
 * flags - + space 0 #, width (number or *), precision (number or *), length
 * modifiers h l z (all 32-bit on this target). %f/%e/%g print fixed point
 * with integer math when MINI_PRINTF_FLOAT is defined, "?" otherwise.
 * C99 return value: the length the full output would have had.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct { char *buf; size_t size; size_t len; } Out;

static void put(Out *o, char c)
{
    if (o->size && o->len + 1 < o->size) o->buf[o->len] = c;
    o->len++;
}

static void pad(Out *o, char c, int n) { while (n-- > 0) put(o, c); }

static int utoa_rev(char *tmp, uint32_t v, uint32_t base, int upper)
{
    int n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = dig[v % base]; v /= base; }
    return n;
}

static void emit_num(Out *o, uint32_t v, uint32_t base, int upper, int neg,
                     int plus, int space, int left, int zero, int alt,
                     int width, int prec)
{
    char tmp[12];
    int n = utoa_rev(tmp, v, base, upper);
    if (prec == 0 && v == 0) n = 0;
    int digits = n > prec ? n : prec;
    char sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
    int pre = (sign ? 1 : 0) + ((alt && base == 16 && v) ? 2 : 0);
    int total = pre + digits;
    if (!left && !(zero && prec < 0)) pad(o, ' ', width - total);
    if (sign) put(o, sign);
    if (alt && base == 16 && v) { put(o, '0'); put(o, upper ? 'X' : 'x'); }
    if (!left && zero && prec < 0) pad(o, '0', width - total);
    pad(o, '0', prec - n);
    while (n) put(o, tmp[--n]);
    if (left) pad(o, ' ', width - total);
}

#ifdef MINI_PRINTF_FLOAT
/*
 * Exact fixed-point conversion from the IEEE-754 double bits, integer math only
 * (values below 2^32; larger ones clamp). Digit-by-digit like libc, rounding the
 * exact binary value (ties to even).
 */
static void emit_float(Out *o, double d, int prec, int plus, int left, int zero, int width)
{
    union { double d; uint64_t u; } c = { d };
    int neg = (int)(c.u >> 63);
    int e = (int)((c.u >> 52) & 0x7FF);
    uint64_t m = c.u & 0xFFFFFFFFFFFFFULL;
    char tmp[40]; int n = 0;
    if (prec < 0) prec = 6;
    if (prec > 9) prec = 9;
    if (e == 0x7FF) { tmp[n++] = m ? 'n' : 'i'; tmp[n++] = m ? 'a' : 'n'; tmp[n++] = m ? 'n' : 'f'; goto out; }

    uint32_t ip = 0;
    uint8_t dig[10] = {0};
    if (e != 0) {
        m |= 1ULL << 52;
        int sh = e - 1075;                           /* value = m * 2^sh */
        if (sh >= 0) {
            ip = (sh > 31) ? 0xFFFFFFFFU : (uint32_t)(m << sh);
        } else {
            int s = -sh;
            uint64_t mm = m;
            if (s > 60) {                            /* keep 60 fractional bits + a sticky bit */
                int drop = s - 60;
                if (drop >= 64) mm = (m != 0);
                else mm = (m >> drop) | ((m & ((1ULL << drop) - 1U)) != 0);
                s = 60;
            }
            uint64_t fr;
            {
                ip = (s >= 53) ? 0 : (uint32_t)(mm >> s);
                fr = (s >= 53) ? mm : (mm & ((1ULL << s) - 1U));
                for (int i = 0; i < prec; i++) {     /* fr < 2^s <= 2^60, so fr * 10 fits */
                    fr *= 10U;
                    dig[i] = (uint8_t)(fr >> s);
                    fr &= (1ULL << s) - 1U;
                }
                uint64_t half = 1ULL << (s - 1);
                int up = fr > half || (fr == half && ((prec ? dig[prec - 1] : (int)(ip & 1U)) & 1));
                if (up) {
                    int i = prec - 1;
                    while (i >= 0 && dig[i] == 9) dig[i--] = 0;
                    if (i >= 0) dig[i]++; else ip++;
                }
            }
        }
    }
    { char t2[12]; int k = utoa_rev(t2, ip, 10, 0); while (k) tmp[n++] = t2[--k]; }
    if (prec) { tmp[n++] = '.'; for (int i = 0; i < prec; i++) tmp[n++] = (char)('0' + dig[i]); }
out:;
    char sign = neg ? '-' : plus ? '+' : 0;
    int total = n + (sign ? 1 : 0);
    if (!left && !zero) pad(o, ' ', width - total);
    if (sign) put(o, sign);
    if (!left && zero) pad(o, '0', width - total);
    for (int i = 0; i < n; i++) put(o, tmp[i]);
    if (left) pad(o, ' ', width - total);
}
#endif

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    Out o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        int left = 0, plus = 0, space = 0, zero = 0, alt = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1; else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1; else if (*fmt == '0') zero = 1;
            else if (*fmt == '#') alt = 1; else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 't') fmt++;
        switch (*fmt) {
        case 'd': case 'i': {
            int32_t v = va_arg(ap, int32_t);
            emit_num(&o, v < 0 ? (uint32_t)0 - (uint32_t)v : (uint32_t)v, 10, 0, v < 0, plus, space, left, zero, 0, width, prec);
            break; }
        case 'u': emit_num(&o, va_arg(ap, uint32_t), 10, 0, 0, 0, 0, left, zero, 0, width, prec); break;
        case 'x': emit_num(&o, va_arg(ap, uint32_t), 16, 0, 0, 0, 0, left, zero, alt, width, prec); break;
        case 'X': emit_num(&o, va_arg(ap, uint32_t), 16, 1, 0, 0, 0, left, zero, alt, width, prec); break;
        case 'p': emit_num(&o, (uint32_t)(uintptr_t)va_arg(ap, void *), 16, 0, 0, 0, 0, left, 0, 1, width, prec); break;
        case 'c': { char c = (char)va_arg(ap, int); if (!left) pad(&o, ' ', width - 1); put(&o, c); if (left) pad(&o, ' ', width - 1); break; }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int n = 0; while (s[n] && (prec < 0 || n < prec)) n++;
            if (!left) pad(&o, ' ', width - n);
            for (int i = 0; i < n; i++) put(&o, s[i]);
            if (left) pad(&o, ' ', width - n);
            break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double d = va_arg(ap, double);
#ifdef MINI_PRINTF_FLOAT
            emit_float(&o, d, prec, plus, left, zero, width);
#else
            (void)d; put(&o, '?');
#endif
            break; }
        case '%': put(&o, '%'); break;
        case 0: fmt--; break;
        default: put(&o, '%'); put(&o, *fmt); break;
        }
    }
    if (size) buf[o.len < size ? o.len : size - 1] = '\0';
    return (int)o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

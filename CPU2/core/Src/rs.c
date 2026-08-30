/*
 * rs.c -- Reed-Solomon RS(255,223) over GF(256), primitive polynomial 0x11D.
 *
 * Systematic encode via generator-polynomial division (LFSR-style byte loop).
 * Decode via syndromes -> Berlekamp-Massey (Massey's LFSR synthesis) ->
 * Chien search -> Forney error-value formula. Bare C, no malloc.
 */
#include <string.h>
#include "s2s2com.h"

static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static uint8_t gen[RS_PARITY + 1];

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) return 0;
    return gf_exp[(unsigned)gf_log[a] + (unsigned)gf_log[b]];
}

static uint8_t gf_div(uint8_t a, uint8_t b)
{
    if (a == 0) return 0;
    int d = (int)gf_log[a] - (int)gf_log[b];
    if (d < 0) d += 255;
    return gf_exp[d];
}

static uint8_t gf_pow(uint8_t a, int n)
{
    if (n == 0) return 1;
    if (a == 0) return 0;
    long e = ((long)gf_log[a] * (long)n) % 255;
    if (e < 0) e += 255;
    return gf_exp[e];
}

static uint8_t gf_inverse(uint8_t a)
{
    return gf_exp[255 - gf_log[a]];
}

/* All poly helpers use HIGH-to-LOW coefficient order: poly[0] is the
   highest-degree coefficient, matching msg/codeword byte order. */

static int gf_poly_mul(const uint8_t *a, int la, const uint8_t *b, int lb, uint8_t *out)
{
    int lo = la + lb - 1;
    memset(out, 0, (size_t)lo);
    for (int i = 0; i < la; i++) {
        if (!a[i]) continue;
        for (int j = 0; j < lb; j++) {
            if (b[j]) out[i + j] ^= gf_mul(a[i], b[j]);
        }
    }
    return lo;
}

static void gf_poly_scale(const uint8_t *a, int la, uint8_t scalar, uint8_t *out)
{
    for (int i = 0; i < la; i++) out[i] = gf_mul(a[i], scalar);
}

/* Right-aligned XOR add (both operands high-to-low, differing lengths ok). */
static int gf_poly_add(const uint8_t *a, int la, const uint8_t *b, int lb, uint8_t *out)
{
    int lo = (la > lb) ? la : lb;
    memset(out, 0, (size_t)lo);
    for (int i = 0; i < la; i++) out[i + lo - la] = a[i];
    for (int i = 0; i < lb; i++) out[i + lo - lb] ^= b[i];
    return lo;
}

static uint8_t gf_poly_eval_hl(const uint8_t *poly, int len, uint8_t x)
{
    uint8_t y = poly[0];
    for (int i = 1; i < len; i++) y = (uint8_t)(gf_mul(y, x) ^ poly[i]);
    return y;
}

/* Low-to-high Horner: poly[0] is the constant term. */
static uint8_t gf_poly_eval_lh(const uint8_t *poly, int len, uint8_t x)
{
    uint8_t y = poly[len - 1];
    for (int i = len - 2; i >= 0; i--) y = (uint8_t)(gf_mul(y, x) ^ poly[i]);
    return y;
}

void rs_init(void)
{
    int x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[(uint8_t)x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    for (int i = 255; i < 512; i++) gf_exp[i] = gf_exp[i - 255];
    gf_log[0] = 0;

    /* Generator polynomial, monic, roots alpha^1 .. alpha^RS_PARITY. */
    gen[0] = 1;
    int glen = 1;
    for (int i = 0; i < RS_PARITY; i++) {
        uint8_t factor[2] = { 1, gf_pow(2, i + 1) };
        uint8_t newgen[RS_PARITY + 1];
        int newlen = gf_poly_mul(gen, glen, factor, 2, newgen);
        memcpy(gen, newgen, (size_t)newlen);
        glen = newlen;
    }
}

void rs_encode(const uint8_t msg[RS_K], uint8_t codeword[RS_N])
{
    uint8_t data[RS_N];
    memcpy(data, msg, RS_K);
    memset(data + RS_K, 0, RS_PARITY);

    for (int i = 0; i < RS_K; i++) {
        uint8_t coef = data[i];
        if (coef) {
            for (int j = 1; j <= RS_PARITY; j++) {
                if (gen[j]) data[i + j] = (uint8_t)(data[i + j] ^ gf_mul(gen[j], coef));
            }
        }
    }

    memcpy(codeword, msg, RS_K);
    memcpy(codeword + RS_K, data + RS_K, RS_PARITY);
}

int rs_decode(uint8_t codeword[RS_N])
{
    uint8_t synd[RS_PARITY]; /* synd[i] = S_{i+1} = codeword(alpha^{i+1}) */
    int has_error = 0;
    for (int i = 0; i < RS_PARITY; i++) {
        synd[i] = gf_poly_eval_hl(codeword, RS_N, gf_pow(2, i + 1));
        if (synd[i]) has_error = 1;
    }
    if (!has_error) return 0;

    /* --- Berlekamp-Massey (Massey's LFSR synthesis) --- */
    uint8_t err_loc[RS_PARITY + 2], old_loc[RS_PARITY + 2];
    int le = 1, lo = 1;
    err_loc[0] = 1;
    old_loc[0] = 1;

    for (int i = 0; i < RS_PARITY; i++) {
        uint8_t delta = synd[i];
        for (int j = 1; j < le; j++) {
            int idx = i - j;
            if (idx >= 0) delta = (uint8_t)(delta ^ gf_mul(err_loc[le - 1 - j], synd[idx]));
        }

        /* old_loc *= x (append a zero low-order term) */
        old_loc[lo] = 0;
        lo += 1;

        if (delta != 0) {
            if (lo > le) {
                int le_before = le, lo_before = lo;
                uint8_t new_loc[RS_PARITY + 2], new_old[RS_PARITY + 2];
                gf_poly_scale(old_loc, lo_before, delta, new_loc);
                uint8_t dinv = gf_inverse(delta);
                gf_poly_scale(err_loc, le_before, dinv, new_old);
                memcpy(old_loc, new_old, (size_t)le_before);
                lo = le_before;
                memcpy(err_loc, new_loc, (size_t)lo_before);
                le = lo_before;
            }
            uint8_t scaled[RS_PARITY + 2];
            gf_poly_scale(old_loc, lo, delta, scaled);
            uint8_t sum[RS_PARITY + 2];
            int lsum = gf_poly_add(err_loc, le, scaled, lo, sum);
            memcpy(err_loc, sum, (size_t)lsum);
            le = lsum;
        }
    }

    /* Strip leading zero coefficients. */
    int start = 0;
    while (start < le - 1 && err_loc[start] == 0) start++;
    int L = le - start;
    if (start) memmove(err_loc, err_loc + start, (size_t)L);
    int errs = L - 1;
    if (errs < 1 || errs > RS_T) return -1;

    /* --- Chien search: roots of err_loc give error location numbers X_k = alpha^i --- */
    int err_pos[RS_T];
    int nerr = 0;
    for (int i = 0; i < RS_N; i++) {
        if (gf_poly_eval_hl(err_loc, L, gf_pow(2, i)) == 0) {
            if (nerr >= RS_T) return -1;
            /* Roots of err_loc(x) occur at X_k^{-1} = alpha^i; the error's
               array position j satisfies X_k = alpha^{N-1-j}, i.e.
               alpha^i = alpha^{-(N-1-j)} = alpha^{j+1} (mod N), so j = i-1. */
            err_pos[nerr++] = (i == 0) ? RS_N - 1 : i - 1;
        }
    }
    if (nerr != errs) return -1;

    /* --- Forney: error evaluator Omega(x) = S(x)*Lambda(x) mod x^(2t), low-to-high --- */
    uint8_t Lam[RS_T + 1]; /* low-to-high error locator */
    for (int k = 0; k <= errs; k++) Lam[k] = err_loc[L - 1 - k];

    uint8_t Somega[RS_PARITY];
    for (int k = 0; k < RS_PARITY; k++) {
        uint8_t acc = 0;
        for (int j = 0; j <= errs; j++) {
            int a = k - j;
            if (a >= 0 && a < RS_PARITY) acc = (uint8_t)(acc ^ gf_mul(synd[a], Lam[j]));
        }
        Somega[k] = acc;
    }

    /* Lambda'(x), low-to-high: coefficient of x^t is Lam[t+1] when (t+1) is odd. */
    uint8_t Lamp[RS_T];
    for (int t = 0; t < errs; t++) {
        Lamp[t] = ((t + 1) & 1) ? Lam[t + 1] : 0;
    }

    for (int k = 0; k < nerr; k++) {
        int p = err_pos[k];
        int m = RS_N - 1 - p;
        uint8_t Xk = gf_pow(2, m);
        uint8_t Xk_inv = gf_inverse(Xk);
        uint8_t omega_val = gf_poly_eval_lh(Somega, RS_PARITY, Xk_inv);
        uint8_t lamp_val = gf_poly_eval_lh(Lamp, errs, Xk_inv);
        if (lamp_val == 0) return -1;
        uint8_t mag = gf_div(omega_val, lamp_val);
        codeword[p] = (uint8_t)(codeword[p] ^ mag);
    }

    /* Defensive re-check: corrected codeword must now have all-zero syndromes. */
    for (int i = 0; i < RS_PARITY; i++) {
        if (gf_poly_eval_hl(codeword, RS_N, gf_pow(2, i + 1)) != 0) return -1;
    }

    return errs;
}

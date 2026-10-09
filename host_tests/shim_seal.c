/* Host-only C stand-in for seal_core.S so protocol.c / seal_wrap.c build on a PC.
 * Same algorithm as fec_asm_tests/fec_model.py (the assembly is tested against it). */
#include <stdint.h>
#include <string.h>

static uint8_t E[512], L[256], G[33];
static int ready;

static uint8_t mul(uint8_t a, uint8_t b) { return (a && b) ? E[L[a] + L[b]] : 0; }

static void init(void)
{
    int x = 1;
    for (int i = 0; i < 255; i++) { E[i] = (uint8_t)x; L[x] = (uint8_t)i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
    for (int i = 255; i < 512; i++) E[i] = E[i - 255];
    memset(G, 0, sizeof G); G[0] = 1;
    for (int j = 0; j < 32; j++) {
        uint8_t ng[33]; memset(ng, 0, sizeof ng);
        for (int k = 0; k <= j; k++) { ng[k] ^= G[k]; ng[k + 1] ^= mul(G[k], E[j]); }
        memcpy(G, ng, sizeof G);
    }
    ready = 1;
}

void seal_encode(const uint8_t *data, uint8_t *chk)
{
    if (!ready) init();
    uint8_t p[32] = {0};
    for (int i = 0; i < 120; i++) {
        uint8_t fb = data[i] ^ p[0];
        for (int k = 0; k < 31; k++) p[k] = p[k + 1] ^ mul(fb, G[k + 1]);
        p[31] = mul(fb, G[32]);
    }
    memcpy(chk, p, 32);
}

uint32_t seal_syndromes(const uint8_t *cw, uint8_t *syn) { (void)cw; memset(syn, 0, 32); return 0; }
/* Host decode: clean-codeword check only (correction is covered by the emulator tests). */
int32_t seal_decode(uint8_t *cw)
{
    uint8_t chk[33];
    seal_encode(cw, chk);
    return memcmp(chk, cw + 120, 32) == 0 ? 0 : -1;
}

/* Host stand-in for the assembly seal_tag (SipHash-2-4, same as test_seal_tag.py's model). */
static uint64_t rl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
void seal_tag(const uint8_t key[16], const uint8_t *d, uint32_t len, uint8_t out[8])
{
    uint64_t k0, k1; memcpy(&k0, key, 8); memcpy(&k1, key + 8, 8);
    uint64_t v0 = k0 ^ 0x736f6d6570736575ULL, v1 = k1 ^ 0x646f72616e646f6dULL;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ULL, v3 = k1 ^ 0x7465646279746573ULL;
#define R do { v0 += v1; v1 = rl(v1,13); v1 ^= v0; v0 = rl(v0,32); v2 += v3; v3 = rl(v3,16); v3 ^= v2; \
               v0 += v3; v3 = rl(v3,21); v3 ^= v0; v2 += v1; v1 = rl(v1,17); v1 ^= v2; v2 = rl(v2,32); } while (0)
    uint64_t b = (uint64_t)len << 56, m;
    uint32_t n = len & ~7U;
    for (uint32_t i = 0; i < n; i += 8) { memcpy(&m, d + i, 8); v3 ^= m; R; R; v0 ^= m; }
    for (uint32_t i = 0; i < (len & 7U); i++) b |= (uint64_t)d[n + i] << (8 * i);
    v3 ^= b; R; R; v0 ^= b; v2 ^= 0xff; R; R; R; R;
    uint64_t t = v0 ^ v1 ^ v2 ^ v3; memcpy(out, &t, 8);
}

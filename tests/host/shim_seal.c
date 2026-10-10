/* Host-only C stand-in for seal_tag.S so protocol.c / seal_wrap.c build on a PC. */
#include <stdint.h>
#include <string.h>

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

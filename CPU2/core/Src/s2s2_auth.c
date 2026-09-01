/*
 * s2s2_auth.c
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-31
 */

#include "s2s2_auth.h"
#include <string.h>

/* ---- SipHash-2-4 reference implementation (public-domain algorithm,
   Aumasson/Bernstein, https://131002.net/siphash/) -- 2 compression
   rounds per input block, 4 finalization rounds, 64-bit output, 128-bit
   key. Ported straight from the reference C, no external dependency. */

static uint64_t rotl(uint64_t x, int b) {
    return (x << b) | (x >> (64 - b));
}

static uint64_t get_u64_le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

#define SIPROUND(v0, v1, v2, v3) do { \
    v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32); \
    v2 += v3; v3 = rotl(v3, 16); v3 ^= v2; \
    v0 += v3; v3 = rotl(v3, 21); v3 ^= v0; \
    v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32); \
} while (0)

uint64_t siphash24(const uint8_t key[S2S2_AUTH_KEY_LEN], const uint8_t *data, size_t len) {
    uint64_t k0 = get_u64_le(&key[0]);
    uint64_t k1 = get_u64_le(&key[8]);

    uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
    uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
    uint64_t v3 = k1 ^ 0x7465646279746573ULL;

    const uint8_t *end = data + (len - (len % 8));
    uint64_t b = ((uint64_t)len) << 56;

    for (; data != end; data += 8) {
        uint64_t m = get_u64_le(data);
        v3 ^= m;
        SIPROUND(v0, v1, v2, v3);
        SIPROUND(v0, v1, v2, v3);
        v0 ^= m;
    }

    size_t left = len & 7;
    for (size_t i = 0; i < left; i++) {
        b |= ((uint64_t)data[i]) << (8 * i);
    }

    v3 ^= b;
    SIPROUND(v0, v1, v2, v3);
    SIPROUND(v0, v1, v2, v3);
    v0 ^= b;

    v2 ^= 0xff;
    SIPROUND(v0, v1, v2, v3);
    SIPROUND(v0, v1, v2, v3);
    SIPROUND(v0, v1, v2, v3);
    SIPROUND(v0, v1, v2, v3);

    return v0 ^ v1 ^ v2 ^ v3;
}

/* ---- S2S2 command authentication ---- */

void s2s2_auth_compute_mac(uint8_t src_id, uint8_t dst_id,
                            const uint8_t *signed_bytes, uint16_t signed_len,
                            uint8_t mac_out[S2S2_AUTH_MAC_LEN]) {
    /* Bind src_id/dst_id into the hashed message rather than passing them
       as extra siphash "context" (SipHash only takes one key + one
       message) -- 2-byte header in front of the actual command bytes. */
    uint8_t buf[2 + 32]; /* 32 is generous headroom over today's 15-byte signed_len */
    buf[0] = src_id;
    buf[1] = dst_id;
    if (signed_len > sizeof(buf) - 2) {
        /* Should never happen for any real command; fail closed rather
           than silently truncating what gets authenticated. */
        memset(mac_out, 0, S2S2_AUTH_MAC_LEN);
        return;
    }
    memcpy(&buf[2], signed_bytes, signed_len);

    uint64_t tag = siphash24(s2s2_auth_key, buf, (size_t)(2 + signed_len));
    for (int i = 0; i < S2S2_AUTH_MAC_LEN; i++) {
        mac_out[i] = (uint8_t)(tag >> (8 * i));
    }
}

static uint32_t s_next_counter = 1; /* ground-station side */
static uint32_t s_last_accepted_counter = 0; /* satellite side, floor */

uint32_t s2s2_auth_next_counter(void) {
    return s_next_counter++;
}

bool s2s2_auth_check_and_update_counter(uint32_t counter) {
    if (counter <= s_last_accepted_counter) {
        return false;
    }
    s_last_accepted_counter = counter;
    return true;
}

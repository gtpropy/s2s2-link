#include <string.h>

#include "seal_wrap.h"

/* make SEAL_TEST=TRUE: also check the tag vector, a damaged round trip and replay rejection at boot */
#ifndef SEAL_FULL_SELFTEST
#define SEAL_FULL_SELFTEST 0
#endif

/* ---- shared key (16 bytes) ---- */
static const uint8_t s_sealKey[16] __attribute__((aligned(4))) = {
    0xEC, 0xC5, 0xE1, 0x09, 0x55, 0x6F, 0x1A, 0xFD,
    0x11, 0xC5, 0x63, 0x8A, 0xC0, 0x21, 0x2C, 0x7D
};

#if AUTH_ENABLE
static uint32_t s_sealTxCounter = 1;
static uint32_t s_sealRxFloor = 0;

/* keyed 64-bit digest (assembly, seal_core.S); key and data must be word aligned */
void seal_tag(const uint8_t key[16], const uint8_t *data, uint32_t len, uint8_t out[8]);

static uint64_t sealTag(const uint8_t key[16], const uint8_t *data, uint32_t len) {
    uint32_t o[2];
    seal_tag(key, data, len, (uint8_t *)o);
    return ((uint64_t)o[1] << 32) | o[0];
}
#endif /* AUTH_ENABLE */

static uint8_t s_sealData[SEAL_DATA_LEN] __attribute__((aligned(4)));
static uint8_t s_sealChk[SEAL_CHECK_LEN + 4] __attribute__((aligned(4)));
static uint8_t s_sealCw[SEAL_WIRE_LEN + 4] __attribute__((aligned(4)));

uint16_t Seal_Wrap(const uint8_t *payload, uint16_t len, uint8_t *out)
{
    if (payload == NULL || out == NULL || len > SEAL_MAX_PAYLOAD) return 0;

    memset(s_sealData, 0, sizeof(s_sealData));
#if AUTH_ENABLE
    uint32_t c = s_sealTxCounter;
    s_sealData[0] = (uint8_t)(c >> 24);
    s_sealData[1] = (uint8_t)(c >> 16);
    s_sealData[2] = (uint8_t)(c >> 8);
    s_sealData[3] = (uint8_t)(c);
#endif
    s_sealData[4] = (uint8_t)len;
    memcpy(&s_sealData[SEAL_HDR_LEN], payload, len);

#if AUTH_ENABLE
    uint64_t tag = sealTag(s_sealKey, s_sealData, SEAL_BLOCK_LEN);
    for (uint32_t i = 0; i < SEAL_TAG_LEN; i++)
        s_sealData[SEAL_BLOCK_LEN + i] = (uint8_t)(tag >> (8U * i));
    s_sealTxCounter++;
#endif

    seal_encode(s_sealData, s_sealChk);
    memcpy(out, s_sealData, SEAL_DATA_LEN);
    memcpy(&out[SEAL_DATA_LEN], s_sealChk, SEAL_CHECK_LEN);
    return SEAL_WIRE_LEN;
}

bool Seal_Unwrap(const uint8_t *wire, uint16_t wireLen,
                 uint8_t *payloadOut, uint16_t payloadOutMax, uint16_t *payloadLenOut)
{
    if (wire == NULL || payloadOut == NULL || payloadLenOut == NULL) return false;
    if (wireLen != SEAL_WIRE_LEN) return false;

    memcpy(s_sealCw, wire, SEAL_WIRE_LEN);
    if (seal_decode(s_sealCw) < 0) return false;

#if AUTH_ENABLE
    uint64_t expect = sealTag(s_sealKey, s_sealCw, SEAL_BLOCK_LEN);
    uint8_t diff = 0;
    for (uint32_t i = 0; i < SEAL_TAG_LEN; i++)
        diff |= (uint8_t)(s_sealCw[SEAL_BLOCK_LEN + i] ^ (uint8_t)(expect >> (8U * i)));
    if (diff != 0) return false;

    uint32_t counter = ((uint32_t)s_sealCw[0] << 24) | ((uint32_t)s_sealCw[1] << 16) |
                       ((uint32_t)s_sealCw[2] << 8)  |  (uint32_t)s_sealCw[3];
    if (counter <= s_sealRxFloor) return false;
    s_sealRxFloor = counter;
#endif

    uint8_t realLen = s_sealCw[4];
    if (realLen > SEAL_MAX_PAYLOAD || realLen > payloadOutMax) return false;
    memcpy(payloadOut, &s_sealCw[SEAL_HDR_LEN], realLen);
    *payloadLenOut = realLen;
    return true;
}

/* 0xA5A50001 = pass, 0xDEAD0000 = fail, 0 = not run (readable with a debugger) */
volatile uint32_t g_sealSelfTest = 0;

static bool sealSelfTestRun(void);

bool Seal_SelfTest(void)
{
    bool ok = sealSelfTestRun();
    g_sealSelfTest = ok ? 0xA5A50001UL : 0xDEAD0000UL;
    return ok;
}

static bool sealSelfTestRun(void)
{
    static uint8_t data[SEAL_DATA_LEN] __attribute__((aligned(4)));
    static uint8_t chk[SEAL_CHECK_LEN + 4] __attribute__((aligned(4)));
    static uint8_t cw[SEAL_WIRE_LEN + 4] __attribute__((aligned(4)));

    for (uint32_t i = 0; i < SEAL_DATA_LEN; i++) data[i] = (uint8_t)(i * 37U + 11U);
    seal_encode(data, chk);
    memcpy(cw, data, SEAL_DATA_LEN);
    memcpy(&cw[SEAL_DATA_LEN], chk, SEAL_CHECK_LEN);

    uint8_t good[SEAL_WIRE_LEN];
    memcpy(good, cw, SEAL_WIRE_LEN);

    static const uint8_t pos[6] = { 0, 17, 63, 119, 120, 151 };
    for (uint32_t i = 0; i < 6; i++) cw[pos[i]] ^= (uint8_t)(0x21U + i * 0x1DU);

    if (seal_decode(cw) != 6) return false;
    if (memcmp(cw, good, SEAL_WIRE_LEN) != 0) return false;

#if SEAL_FULL_SELFTEST
#if AUTH_ENABLE
    /* tag function against the official SipHash-2-4 vector (key 00..0f, msg 00..0e) */
    static uint8_t kKey[16] __attribute__((aligned(4))), kMsg[16] __attribute__((aligned(4)));
    for (uint32_t i = 0; i < 16; i++) kKey[i] = (uint8_t)i;
    for (uint32_t i = 0; i < 15; i++) kMsg[i] = (uint8_t)i;
    if (sealTag(kKey, kMsg, 15) != 0xa129ca6149be45e5ULL) return false;
#endif

    /* full wrap -> damage 5 bytes -> unwrap round trip */
    static const uint8_t cmd[13] = { 0x53, 0x04, 0xCC, 0x5E, 0xBD, 0, 0, 0, 0, 0, 0, 0, 0 };
    static uint8_t wire[SEAL_WIRE_LEN], dmg[SEAL_WIRE_LEN], outp[SEAL_MAX_PAYLOAD];
    uint16_t olen = 0;
    if (Seal_Wrap(cmd, sizeof(cmd), wire) != SEAL_WIRE_LEN) return false;
    memcpy(dmg, wire, SEAL_WIRE_LEN);
    dmg[3] ^= 0x5A; dmg[40] ^= 0x11; dmg[118] ^= 0xFF; dmg[125] ^= 0x01; dmg[150] ^= 0x80;
    if (!Seal_Unwrap(dmg, SEAL_WIRE_LEN, outp, sizeof(outp), &olen)) return false;
    if (olen != sizeof(cmd) || memcmp(outp, cmd, sizeof(cmd)) != 0) return false;
#if AUTH_ENABLE
    if (Seal_Unwrap(wire, SEAL_WIRE_LEN, outp, sizeof(outp), &olen)) return false; /* replay must fail */
    s_sealTxCounter = 1;                                                           /* leave counters fresh */
    s_sealRxFloor = 0;
#endif
#endif /* SEAL_FULL_SELFTEST */
    return true;
}

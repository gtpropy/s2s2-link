#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "protocol.h"
#include "ax25.h"
#include "g3ruh.h"

/*
 * ============================================================
 * PACKET HARDENING
 *
 * Every Protocol_CreatePacket payload picks up a small sequence/length
 * header and a short integrity tag before AX.25 framing, then gets
 * spread across a fixed-size block with some trailing check bytes so a
 * handful of flipped bits from a weak pass don't cost the whole packet
 * -- 152 bytes total, comfortably inside this frame's measured
 * ~156-byte real capacity at RADIO_FIXED_PACKET_LEN=200.
 * Protocol_DecodePayload() is the inverse, called after AX25_
 * VerifyFrame's own CRC passes (that FCS covers the wire-exact bytes,
 * so unwrapping must happen after it, not before). Table-driven byte
 * math throughout -- builds its tables once, no per-call derivation.
 * ============================================================
 */

#define PROTO_AUTH_KEY_LEN      16U
#define PROTO_WRAP_HDR_LEN      5U    /* counter(4) + len(1) */
#define PROTO_WRAP_BLOCK_LEN    112U  /* hdr + real payload + zero pad */
#define PROTO_TAG_LEN           8U
#define PROTO_MAX_PAYLOAD_LEN   (PROTO_WRAP_BLOCK_LEN - PROTO_WRAP_HDR_LEN) /* 107 */

#define PROTO_BLOCK_K              223
#define PROTO_BLOCK_PARITY         32
#define PROTO_BLOCK_N              255
#define PROTO_BLOCK_T              16
#define PROTO_SEALED_LEN       (PROTO_WRAP_BLOCK_LEN + PROTO_TAG_LEN) /* 120 */
#define PROTO_WIRE_LEN          (PROTO_SEALED_LEN + PROTO_BLOCK_PARITY)  /* 152 */

static const uint8_t s_protoKey[PROTO_AUTH_KEY_LEN] = {
    0xEC, 0xC5, 0xE1, 0x09, 0x55, 0x6F, 0x1A, 0xFD,
    0x11, 0xC5, 0x63, 0x8A, 0xC0, 0x21, 0x2C, 0x7D
};

static uint32_t s_protoTxCounter = 1;
static uint32_t s_protoRxFloor = 0;

static uint64_t rotl64(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

static uint64_t getU64LE(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

#define PROTO_MIXROUND(v0, v1, v2, v3) do { \
    v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0; v0 = rotl64(v0, 32); \
    v2 += v3; v3 = rotl64(v3, 16); v3 ^= v2; \
    v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0; \
    v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2; v2 = rotl64(v2, 32); \
} while (0)

/* Short keyed digest over a byte run -- 64-bit tag, 128-bit key. */
static uint64_t computeTag(const uint8_t key[PROTO_AUTH_KEY_LEN], const uint8_t *data, uint32_t len) {
    uint64_t k0 = getU64LE(&key[0]);
    uint64_t k1 = getU64LE(&key[8]);

    uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
    uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
    uint64_t v3 = k1 ^ 0x7465646279746573ULL;

    const uint8_t *end = data + (len - (len % 8U));
    uint64_t b = ((uint64_t)len) << 56;

    for (; data != end; data += 8) {
        uint64_t m = getU64LE(data);
        v3 ^= m;
        PROTO_MIXROUND(v0, v1, v2, v3);
        PROTO_MIXROUND(v0, v1, v2, v3);
        v0 ^= m;
    }

    uint32_t left = len & 7U;
    for (uint32_t i = 0; i < left; i++) b |= ((uint64_t)data[i]) << (8U * i);

    v3 ^= b;
    PROTO_MIXROUND(v0, v1, v2, v3);
    PROTO_MIXROUND(v0, v1, v2, v3);
    v0 ^= b;

    v2 ^= 0xff;
    PROTO_MIXROUND(v0, v1, v2, v3);
    PROTO_MIXROUND(v0, v1, v2, v3);
    PROTO_MIXROUND(v0, v1, v2, v3);
    PROTO_MIXROUND(v0, v1, v2, v3);

    return v0 ^ v1 ^ v2 ^ v3;
}

/* ---- Fixed-rate error-correcting block code, table-driven. ---- */

static uint8_t s_mixExp[512];
static uint8_t s_mixLog[256];
static uint8_t s_blockGen[PROTO_BLOCK_PARITY + 1];
static bool s_blockReady = false;

static inline uint8_t mixMul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return s_mixExp[(uint32_t)s_mixLog[a] + (uint32_t)s_mixLog[b]];
}

static inline uint8_t mixDiv(uint8_t a, uint8_t b) {
    if (a == 0) return 0;
    int32_t d = (int32_t)s_mixLog[a] - (int32_t)s_mixLog[b];
    if (d < 0) d += 255;
    return s_mixExp[d];
}

static inline uint8_t mixPow(uint8_t a, int32_t n) {
    if (n == 0) return 1;
    if (a == 0) return 0;
    int32_t e = ((int32_t)s_mixLog[a] * n) % 255;
    if (e < 0) e += 255;
    return s_mixExp[e];
}

static inline uint8_t mixInv(uint8_t a) { return s_mixExp[255 - s_mixLog[a]]; }

static int32_t polyMul(const uint8_t *a, int32_t la, const uint8_t *b, int32_t lb, uint8_t *out) {
    int32_t lo = la + lb - 1;
    memset(out, 0, (size_t)lo);
    for (int32_t i = 0; i < la; i++) {
        if (!a[i]) continue;
        for (int32_t j = 0; j < lb; j++) {
            if (b[j]) out[i + j] ^= mixMul(a[i], b[j]);
        }
    }
    return lo;
}

static void polyScale(const uint8_t *a, int32_t la, uint8_t scalar, uint8_t *out) {
    for (int32_t i = 0; i < la; i++) out[i] = mixMul(a[i], scalar);
}

static int32_t polyAdd(const uint8_t *a, int32_t la, const uint8_t *b, int32_t lb, uint8_t *out) {
    int32_t lo = (la > lb) ? la : lb;
    memset(out, 0, (size_t)lo);
    for (int32_t i = 0; i < la; i++) out[i + lo - la] = a[i];
    for (int32_t i = 0; i < lb; i++) out[i + lo - lb] ^= b[i];
    return lo;
}

static uint8_t polyEvalHL(const uint8_t *poly, int32_t len, uint8_t x) {
    uint8_t y = poly[0];
    for (int32_t i = 1; i < len; i++) y = (uint8_t)(mixMul(y, x) ^ poly[i]);
    return y;
}

static uint8_t polyEvalLH(const uint8_t *poly, int32_t len, uint8_t x) {
    uint8_t y = poly[len - 1];
    for (int32_t i = len - 2; i >= 0; i--) y = (uint8_t)(mixMul(y, x) ^ poly[i]);
    return y;
}

static void sealInit(void) {
    if (s_blockReady) return;
    int32_t x = 1;
    for (int32_t i = 0; i < 255; i++) {
        s_mixExp[i] = (uint8_t)x;
        s_mixLog[(uint8_t)x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    for (int32_t i = 255; i < 512; i++) s_mixExp[i] = s_mixExp[i - 255];
    s_mixLog[0] = 0;

    s_blockGen[0] = 1;
    int32_t glen = 1;
    for (int32_t i = 0; i < PROTO_BLOCK_PARITY; i++) {
        uint8_t factor[2] = { 1, mixPow(2, i + 1) };
        uint8_t newgen[PROTO_BLOCK_PARITY + 1];
        int32_t newlen = polyMul(s_blockGen, glen, factor, 2, newgen);
        memcpy(s_blockGen, newgen, (size_t)newlen);
        glen = newlen;
    }
    s_blockReady = true;
}

/* Systematic encode: msg[PROTO_BLOCK_K] (caller zero-pads) -> parityOut[PROTO_BLOCK_PARITY]
   only -- the caller already has msg itself, so there's no need to hand
   back a full K+PARITY codeword just to have it copy the first K bytes
   straight back out again. */
__attribute__((optimize("O3")))
static void sealEncode(const uint8_t msg[PROTO_BLOCK_K], uint8_t parityOut[PROTO_BLOCK_PARITY]) {
    uint8_t data[PROTO_BLOCK_N];
    memcpy(data, msg, PROTO_BLOCK_K);
    memset(data + PROTO_BLOCK_K, 0, PROTO_BLOCK_PARITY);

    for (int32_t i = 0; i < PROTO_BLOCK_K; i++) {
        uint8_t coef = data[i];
        if (coef) {
            for (int32_t j = 1; j <= PROTO_BLOCK_PARITY; j++) {
                if (s_blockGen[j]) data[i + j] = (uint8_t)(data[i + j] ^ mixMul(s_blockGen[j], coef));
            }
        }
    }
    memcpy(parityOut, data + PROTO_BLOCK_K, PROTO_BLOCK_PARITY);
}

/* Decode in place; returns corrected-symbol count (>=0) or -1 if uncorrectable. */
__attribute__((optimize("O3")))
static int32_t sealDecode(uint8_t codeword[PROTO_BLOCK_N]) {
    uint8_t synd[PROTO_BLOCK_PARITY];
    int32_t hasError = 0;
    for (int32_t i = 0; i < PROTO_BLOCK_PARITY; i++) {
        synd[i] = polyEvalHL(codeword, PROTO_BLOCK_N, mixPow(2, i + 1));
        if (synd[i]) hasError = 1;
    }
    if (!hasError) return 0;

    uint8_t errLoc[PROTO_BLOCK_PARITY + 2], oldLoc[PROTO_BLOCK_PARITY + 2];
    int32_t le = 1, lo = 1;
    errLoc[0] = 1;
    oldLoc[0] = 1;

    for (int32_t i = 0; i < PROTO_BLOCK_PARITY; i++) {
        uint8_t delta = synd[i];
        for (int32_t j = 1; j < le; j++) {
            int32_t idx = i - j;
            if (idx >= 0) delta = (uint8_t)(delta ^ mixMul(errLoc[le - 1 - j], synd[idx]));
        }
        oldLoc[lo] = 0;
        lo += 1;

        if (delta != 0) {
            if (lo > le) {
                int32_t leBefore = le, loBefore = lo;
                uint8_t newLoc[PROTO_BLOCK_PARITY + 2], newOld[PROTO_BLOCK_PARITY + 2];
                polyScale(oldLoc, loBefore, delta, newLoc);
                uint8_t dinv = mixInv(delta);
                polyScale(errLoc, leBefore, dinv, newOld);
                memcpy(oldLoc, newOld, (size_t)leBefore);
                lo = leBefore;
                memcpy(errLoc, newLoc, (size_t)loBefore);
                le = loBefore;
            }
            uint8_t scaled[PROTO_BLOCK_PARITY + 2];
            polyScale(oldLoc, lo, delta, scaled);
            uint8_t sum[PROTO_BLOCK_PARITY + 2];
            int32_t lsum = polyAdd(errLoc, le, scaled, lo, sum);
            memcpy(errLoc, sum, (size_t)lsum);
            le = lsum;
        }
    }

    int32_t start = 0;
    while (start < le - 1 && errLoc[start] == 0) start++;
    int32_t L = le - start;
    if (start) memmove(errLoc, errLoc + start, (size_t)L);
    int32_t errs = L - 1;
    if (errs < 1 || errs > PROTO_BLOCK_T) return -1;

    int32_t errPos[PROTO_BLOCK_T];
    int32_t nerr = 0;
    for (int32_t i = 0; i < PROTO_BLOCK_N; i++) {
        if (polyEvalHL(errLoc, L, mixPow(2, i)) == 0) {
            if (nerr >= PROTO_BLOCK_T) return -1;
            errPos[nerr++] = (i == 0) ? PROTO_BLOCK_N - 1 : i - 1;
        }
    }
    if (nerr != errs) return -1;

    uint8_t lam[PROTO_BLOCK_T + 1];
    for (int32_t k = 0; k <= errs; k++) lam[k] = errLoc[L - 1 - k];

    uint8_t somega[PROTO_BLOCK_PARITY];
    for (int32_t k = 0; k < PROTO_BLOCK_PARITY; k++) {
        uint8_t acc = 0;
        for (int32_t j = 0; j <= errs; j++) {
            int32_t a = k - j;
            if (a >= 0 && a < PROTO_BLOCK_PARITY) acc = (uint8_t)(acc ^ mixMul(synd[a], lam[j]));
        }
        somega[k] = acc;
    }

    uint8_t lamp[PROTO_BLOCK_T];
    for (int32_t t = 0; t < errs; t++) lamp[t] = ((t + 1) & 1) ? lam[t + 1] : 0;

    for (int32_t k = 0; k < nerr; k++) {
        int32_t p = errPos[k];
        int32_t m = PROTO_BLOCK_N - 1 - p;
        uint8_t xk = mixPow(2, m);
        uint8_t xkInv = mixInv(xk);
        uint8_t omegaVal = polyEvalLH(somega, PROTO_BLOCK_PARITY, xkInv);
        uint8_t lampVal = polyEvalLH(lamp, errs, xkInv);
        if (lampVal == 0) return -1;
        uint8_t mag = mixDiv(omegaVal, lampVal);
        codeword[p] = (uint8_t)(codeword[p] ^ mag);
    }

    for (int32_t i = 0; i < PROTO_BLOCK_PARITY; i++) {
        if (polyEvalHL(codeword, PROTO_BLOCK_N, mixPow(2, i + 1)) != 0) return -1;
    }
    return errs;
}

/*
 * ============================================================
 * BITSTREAM HELPER
 * ============================================================
 */

typedef struct
{
    uint8_t *buffer;
    uint32_t capacity;
    uint32_t count;
} BitWriter;

static bool BitWriter_Put(
    BitWriter *w,
    uint8_t bit)
{
    if (w == NULL)
        return false;

    if (w->count >= w->capacity)
        return false;

    w->buffer[w->count++] =
        bit & 1U;

    return true;
}

static bool BitWriter_PutFlag(
    BitWriter *w)
{
    /*
     * AX.25 flag:
     *
     * 01111110
     *
     * transmitted LSB-first:
     *
     * 0 1 1 1 1 1 1 0
     */

    static const uint8_t flagBits[8] =
    {
        0,1,1,1,1,1,1,0
    };

    for (uint8_t i = 0; i < 8; i++)
    {
        if (!BitWriter_Put(
                w,
                flagBits[i]))
        {
            return false;
        }
    }

    return true;
}

/*
 * ============================================================
 * PUT NORMAL AX.25 OCTET
 *
 * All normal AX.25 fields are transmitted LSB-first.
 * ============================================================
 */

static bool BitWriter_PutByteLSB(
    BitWriter *w,
    uint8_t value)
{
    for (uint8_t b = 0; b < 8; b++)
    {
        if (!BitWriter_Put(
                w,
                (value >> b) & 1U))
        {
            return false;
        }
    }

    return true;
}

/*
 * ============================================================
 * PUT FCS BYTE
 *
 * AX.25 FCS is transmitted MSB-first.
 *
 * The software frame contains:
 *
 *     FCS low byte
 *     FCS high byte
 *
 * But the physical bitstream transmits the 16-bit FCS
 * MSB-first.
 *
 * Therefore:
 *
 *     high-byte bit7 -> first
 *     ...
 *     high-byte bit0
 *     low-byte bit7
 *     ...
 *     low-byte bit0
 * ============================================================
 */

static bool BitWriter_PutFCS(
    BitWriter *w,
    uint8_t fcsLow,
    uint8_t fcsHigh)
{
    for (int b = 7; b >= 0; b--)
    {
        if (!BitWriter_Put(
                w,
                (fcsHigh >> b) & 1U))
        {
            return false;
        }
    }

    for (int b = 7; b >= 0; b--)
    {
        if (!BitWriter_Put(
                w,
                (fcsLow >> b) & 1U))
        {
            return false;
        }
    }

    return true;
}

/*
 * ============================================================
 * BITSTUFF AX.25 DATA
 *
 * Input:
 *
 *     frame[1 ... frameLength-3]
 *
 * This includes:
 *
 *     destination
 *     source
 *     control
 *     PID
 *     information
 *
 * Then FCS is separately serialized MSB-first.
 *
 * Bit stuffing occurs continuously across DATA + FCS.
 * ============================================================
 */

static bool PutStuffedBit(
    BitWriter *w,
    uint8_t bit,
    uint8_t *ones)
{
    if (!BitWriter_Put(w, bit))
        return false;

    if (bit)
    {
        (*ones)++;

        if (*ones == 5)
        {
            /*
             * Insert stuffed zero.
             */
            if (!BitWriter_Put(w, 0))
                return false;

            *ones = 0;
        }
    }
    else
    {
        *ones = 0;
    }

    return true;
}

/*
 * ============================================================
 * CREATE TX PACKET
 *
 * Physical chain:
 *
 * AX.25
 *   ↓
 * HDLC bit stuffing
 *   ↓
 * NRZI
 *   ↓
 * G3RUH scrambling
 *   ↓
 * FIFO byte packing
 *
 * IMPORTANT:
 *
 * The G3RUH scrambler works on the NRZI stream.
 * ============================================================
 */

uint16_t Protocol_CreatePacket(
    uint8_t *output,
    const uint8_t *payload,
    uint16_t payloadLength,
    const RadioConfig_t *cfg)
{
    if (output == NULL ||
        cfg == NULL)
    {
        return 0;
    }

    if (payload == NULL ||
        payloadLength > PROTO_MAX_PAYLOAD_LEN)
    {
        return 0;
    }

    sealInit();

    /*
     * Wrap: [counter(4)][len(1)][payload][zero pad -> 112][tag(8)],
     * then run through the block coder (padded wider for the math,
     * never transmitted) to add the trailing check bytes.
     * See the PACKET HARDENING block at the top of this file.
     */
    static uint8_t s_rsReal[PROTO_BLOCK_K];
    memset(s_rsReal, 0, sizeof(s_rsReal));
    s_rsReal[0] = (uint8_t)(s_protoTxCounter >> 24);
    s_rsReal[1] = (uint8_t)(s_protoTxCounter >> 16);
    s_rsReal[2] = (uint8_t)(s_protoTxCounter >> 8);
    s_rsReal[3] = (uint8_t)(s_protoTxCounter);
    s_rsReal[4] = (uint8_t)payloadLength;
    memcpy(&s_rsReal[PROTO_WRAP_HDR_LEN], payload, payloadLength);

#if AUTH_ENABLE
    uint64_t mac = computeTag(s_protoKey, s_rsReal, PROTO_WRAP_BLOCK_LEN);
    for (uint32_t i = 0; i < PROTO_TAG_LEN; i++) {
        s_rsReal[PROTO_WRAP_BLOCK_LEN + i] = (uint8_t)(mac >> (8U * i));
    }
#endif

    static uint8_t s_wirePayload[PROTO_WIRE_LEN];
    memcpy(s_wirePayload, s_rsReal, PROTO_SEALED_LEN);
    sealEncode(s_rsReal, &s_wirePayload[PROTO_SEALED_LEN]);

    s_protoTxCounter++;

    /*
     * ========================================================
     * STEP 1
     *
     * Build ordinary AX.25 frame into static buffer to prevent stack overflow.
     * ========================================================
     */

    static uint8_t s_clean[AX25_MAX_FRAME_SIZE];

    uint16_t cleanLen = 0;

    if (AX25_BuildFrame(
            s_clean,
            sizeof(s_clean),
            &cleanLen,
            cfg->destCallsign,
            cfg->destSSID,
            cfg->sourceCallsign,
            cfg->sourceSSID,
            s_wirePayload,
            PROTO_WIRE_LEN) != AX25_OK)
    {
        return 0;
    }

    if (cleanLen <
        AX25_MinFrameLength())
    {
        return 0;
    }

    /*
     * ========================================================
     * STEP 2
     *
     * Generate the complete HDLC bitstream.
     *
     * The actual physical packet length is fixed.
     * ========================================================
     */

    static uint8_t s_hdlcBits[
        RADIO_FIXED_PACKET_LEN * 8U
    ];

    memset(
        s_hdlcBits,
        0,
        sizeof(s_hdlcBits));

    BitWriter writer =
    {
        .buffer = s_hdlcBits,
        .capacity =
            RADIO_FIXED_PACKET_LEN * 8U,
        .count = 0
    };

    /*
     * Leading flags.
     */

    for (uint32_t f = 0;
         f < PROTOCOL_LEADING_FLAG_COUNT;
         f++)
    {
        if (!BitWriter_PutFlag(&writer))
            return 0;
    }

    /*
     * Opening frame flag.
     */

    if (!BitWriter_PutFlag(&writer))
        return 0;

    /*
     * ========================================================
     * DATA
     *
     * All AX.25 fields are transmitted LSB-first.
     *
     * clean[1] ... clean[cleanLen-4]
     * ========================================================
     */

    uint8_t ones = 0;

    uint16_t dataEnd =
        cleanLen - 4;

    for (uint16_t i = 1;
         i <= dataEnd;
         i++)
    {
        uint8_t value = s_clean[i];

        for (uint8_t b = 0;
             b < 8;
             b++)
        {
            uint8_t bit =
                (value >> b) & 1U;

            if (!PutStuffedBit(
                    &writer,
                    bit,
                    &ones))
            {
                return 0;
            }
        }
    }

    /*
     * ========================================================
     * FCS
     *
     * s_clean[cleanLen-3] = FCS low
     * s_clean[cleanLen-2] = FCS high
     *
     * Standard AX.25 / HDLC transmits both FCS bytes LSB-first:
     * low byte first (bit 0..7), then high byte (bit 0..7).
     * Bit stuffing applies across the FCS field.
     * ========================================================
     */

    uint8_t fcsLow =
        s_clean[cleanLen - 3];

    uint8_t fcsHigh =
        s_clean[cleanLen - 2];

    for (uint8_t b = 0;
         b < 8;
         b++)
    {
        uint8_t bit =
            (fcsLow >> b) & 1U;

        if (!PutStuffedBit(
                &writer,
                bit,
                &ones))
        {
            return 0;
        }
    }

    for (uint8_t b = 0;
         b < 8;
         b++)
    {
        uint8_t bit =
            (fcsHigh >> b) & 1U;

        if (!PutStuffedBit(
                &writer,
                bit,
                &ones))
        {
            return 0;
        }
    }

    /*
     * ========================================================
     * CLOSING FLAG
     *
     * The closing flag starts immediately after the FCS.
     *
     * No byte padding is inserted before it.
     * ========================================================
     */

    if (!BitWriter_PutFlag(&writer))
        return 0;

    /*
     * Fill remainder of the fixed packet buffer with continuous flag bits (0x7E).
     */
    static const uint8_t padFlagBits[8] =
    {
        0, 1, 1, 1, 1, 1, 1, 0
    };

    uint8_t padIdx = 0;

    while (writer.count <
           writer.capacity)
    {
        if (!BitWriter_Put(
                &writer,
                padFlagBits[padIdx]))
        {
            break;
        }

        padIdx = (uint8_t)((padIdx + 1U) % 8U);
    }

    /*
     * ========================================================
     * STEP 3
     *
     * NRZI encode the COMPLETE continuous bitstream.
     *
     * ========================================================
     */

    static uint8_t s_nrziBits[
        RADIO_FIXED_PACKET_LEN * 8U
    ];

    uint8_t nrziState = 1;

    for (uint32_t i = 0;
         i < writer.count;
         i++)
    {
        uint8_t bit =
            s_hdlcBits[i];

        if (bit == 0)
            nrziState =
                (uint8_t)!nrziState;

        s_nrziBits[i] =
            nrziState;
    }

    /*
     * ========================================================
     * STEP 4
     *
     * G3RUH SCRAMBLE
     *
     * NRZI -> G3RUH
     * ========================================================
     */

    static uint8_t s_scrambledBits[
        RADIO_FIXED_PACKET_LEN * 8U
    ];

    uint32_t sr =
        G3RUH_INITIAL_STATE;

    for (uint32_t i = 0;
         i < writer.count;
         i++)
    {
        uint8_t inputBit =
            s_nrziBits[i];

        uint8_t feedback =
            ((sr >> G3RUH_POLY_TAP1) ^
             (sr >> G3RUH_POLY_TAP2)) & 1U;

        uint8_t outputBit =
            inputBit ^ feedback;

        /*
         * Self-synchronizing feedback.
         */
        sr =
            ((sr << 1) | outputBit) &
            G3RUH_REGISTER_MASK;

        s_scrambledBits[i] =
            outputBit;
    }

    /*
     * ========================================================
     * STEP 5
     *
     * Pack physical bits into bytes.
     *
     * Our bitstream array uses:
     *
     *     bit 0 = first physical bit
     *
     * The SX126x FIFO representation used by your current
     * radio configuration expects the first transmitted bit
     * in the MSB position.
     * ========================================================
     */

    for (uint16_t byteIndex = 0;
         byteIndex < RADIO_FIXED_PACKET_LEN;
         byteIndex++)
    {
        uint8_t value = 0;

        for (uint8_t b = 0;
             b < 8;
             b++)
        {
            uint32_t bitIndex =
                ((uint32_t)byteIndex * 8U) + b;

            if (s_scrambledBits[bitIndex])
            {
                value |=
                    (uint8_t)(1U << (7U - b));
            }
        }

        output[byteIndex] = value;
    }

    return RADIO_FIXED_PACKET_LEN;
}

/*
 * ============================================================
 * RX BITSTREAM
 * ============================================================
 */

typedef struct
{
    uint8_t *buffer;
    uint32_t capacity;
    uint32_t count;
} BitReaderOutput;

static bool FrameBitAppend(
    BitReaderOutput *out,
    uint8_t bit)
{
    if (out->count >= out->capacity)
        return false;

    out->buffer[out->count++] =
        bit & 1U;

    return true;
}

/*
 * ============================================================
 * EXTRACT AX.25 FRAME
 *
 * RX chain:
 *
 * RF FIFO
 *   ↓
 * G3RUH descramble
 *   ↓
 * NRZI decode
 *   ↓
 * HDLC flag detection
 *   ↓
 * HDLC bit unstuff
 *   ↓
 * AX.25
 *   ↓
 * CRC
 * ============================================================
 */

bool Protocol_ExtractFrame(
    const uint8_t *rxBuffer,
    uint16_t rxBufferLen,
    uint8_t *frameOut,
    uint16_t frameOutMax,
    uint16_t *frameOutLen)
{
    if (rxBuffer == NULL ||
        frameOut == NULL ||
        frameOutLen == NULL ||
        rxBufferLen == 0)
    {
        return false;
    }

    /*
     * G3RUH state.
     */
    uint32_t sr =
        G3RUH_INITIAL_STATE;

    /*
     * NRZI state.
     */
    uint8_t lastNrzi =
        1;

    /*
     * Sliding AX.25 flag detector.
     *
     * Incoming logical flag bits:
     *
     *     0 1 1 1 1 1 1 0
     *
     * This shift arrangement produces 0x7E.
     */
    uint8_t flagShift = 0;

    bool inFrame = false;

    /*
     * Maximum stuffed bit storage (static to avoid stack overflow).
     */
    static uint8_t s_rx_frameBits[
        AX25_MAX_FRAME_SIZE * 10U
    ];

    uint32_t frameBitCount = 0;

    for (uint16_t byteIndex = 0;
         byteIndex < rxBufferLen;
         byteIndex++)
    {
        uint8_t value =
            rxBuffer[byteIndex];

        /*
         * Physical FIFO bit order:
         *
         * MSB first.
         */
        for (int b = 7;
             b >= 0;
             b--)
        {
            uint8_t scrambledBit =
                (value >> b) & 1U;

            /*
             * =================================================
             * 1. G3RUH DESCRAMBLE
             * =================================================
             */

            uint8_t feedback =
                ((sr >> G3RUH_POLY_TAP1) ^
                 (sr >> G3RUH_POLY_TAP2)) & 1U;

            uint8_t nrziBit =
                scrambledBit ^ feedback;

            /*
             * Feed received scrambled bit back.
             */
            sr =
                ((sr << 1) | scrambledBit) &
                G3RUH_REGISTER_MASK;

            /*
             * =================================================
             * 2. NRZI DECODE
             * =================================================
             */

            uint8_t dataBit =
                (nrziBit == lastNrzi) ?
                1U : 0U;

            lastNrzi =
                nrziBit;

            /*
             * =================================================
             * 3. HDLC FLAG DETECTION
             * =================================================
             */

            flagShift =
                (uint8_t)(
                    (flagShift >> 1) |
                    (dataBit << 7));

            if (flagShift == AX25_FLAG)
            {
                /*
                 * A flag contains:
                 *
                 *     0 111111 0
                 *
                 * The first seven bits of the flag have
                 * already been appended to s_rx_frameBits.
                 *
                 * Remove those seven bits.
                 */

                if (inFrame)
                {
                    if (frameBitCount >= 7U)
                    {
                        uint32_t contentBits =
                            frameBitCount - 7U;

                        static uint8_t s_rx_unstuffed[
                            AX25_MAX_FRAME_SIZE
                        ];

                        memset(
                            s_rx_unstuffed,
                            0,
                            sizeof(s_rx_unstuffed));

                        uint32_t outBitCount = 0;

                        uint8_t ones = 0;

                        bool violation = false;

                        /*
                         * =====================================
                         * HDLC UNSTUFF
                         * =====================================
                         */

                        for (uint32_t k = 0;
                             k < contentBits;
                             k++)
                        {
                            uint8_t bit =
                                s_rx_frameBits[k];

                            /*
                             * Stuffed zero.
                             */
                            if (ones == 5U)
                            {
                                if (bit != 0U)
                                {
                                    violation =
                                        true;
                                    break;
                                }

                                ones = 0;
                                continue;
                            }

                            if (outBitCount >=
                                ((uint32_t)
                                 sizeof(s_rx_unstuffed) *
                                 8U))
                            {
                                violation =
                                    true;
                                break;
                            }

                            if (bit)
                            {
                                s_rx_unstuffed[
                                    outBitCount / 8U
                                ] |=
                                    (uint8_t)(
                                        1U <<
                                        (outBitCount % 8U));

                                ones++;
                            }
                            else
                            {
                                ones = 0;
                            }

                            outBitCount++;
                        }

                        /*
                         * AX.25 frame must be byte aligned.
                         */
                        if (!violation &&
                            (outBitCount % 8U) == 0U)
                        {
                            uint16_t byteLen =
                                (uint16_t)(
                                    outBitCount / 8U);

                            /*
                             * Need:
                             *
                             * opening flag
                             * AX25 bytes
                             * closing flag
                             */
                            if (byteLen >= 18U &&
                                ((uint32_t)byteLen + 2U)
                                <= frameOutMax)
                            {
                                frameOut[0] =
                                    AX25_FLAG;

                                memcpy(
                                    &frameOut[1],
                                    s_rx_unstuffed,
                                    byteLen);

                                frameOut[
                                    byteLen + 1U
                                ] =
                                    AX25_FLAG;

                                *frameOutLen =
                                    (uint16_t)(
                                        byteLen + 2U);

                                /*
                                 * Verify CRC with either method 1 or method 2.
                                 */
                                if (AX25_VerifyCRC_Method1(
                                        frameOut,
                                        *frameOutLen) ||
                                    AX25_VerifyCRC_Method2(
                                        frameOut,
                                        *frameOutLen,
                                        NULL, NULL, NULL))
                                {
                                    return true;
                                }
                            }
                        }
                    }

                    /*
                     * Current frame failed.
                     * Start searching for another one.
                     */
                    frameBitCount = 0;
                }
                else
                {
                    /*
                     * First flag.
                     */
                    inFrame = true;
                    frameBitCount = 0;
                }

                continue;
            }

            /*
             * =================================================
             * Store non-flag data bits.
             * =================================================
             */

            if (inFrame)
            {
                if (frameBitCount <
                    sizeof(s_rx_frameBits))
                {
                    s_rx_frameBits[
                        frameBitCount++
                    ] = dataBit;
                }
                else
                {
                    /*
                     * Frame too large.
                     */
                    frameBitCount = 0;
                    inFrame = false;
                }
            }
        }
    }

    return false;
}

/*
 * ============================================================
 * PACKET VERIFICATION
 * ============================================================
 */

bool Protocol_VerifyPacket(
    const uint8_t *frame,
    uint16_t frameLength)
{
    return AX25_VerifyFrame(
        frame,
        frameLength);
}

/*
 * ============================================================
 * PAYLOAD DECODE (correct + verify)
 *
 * Call this on the AX.25 Information field bytes AFTER Protocol_
 * VerifyPacket/AX25_VerifyFrame's own FCS check has already passed --
 * the FCS covers the wire-exact wrapped+RS-coded bytes, so unwrapping
 * earlier would make a correctly-FCS'd frame look tampered. Returns
 * false on RS-uncorrectable, bad MAC, or a replayed/stale counter.
 * ============================================================
 */
bool Protocol_DecodePayload(
    const uint8_t *infoField,
    uint16_t infoFieldLen,
    uint8_t *payloadOut,
    uint16_t payloadOutMax,
    uint16_t *payloadLenOut)
{
    if (infoField == NULL || payloadOut == NULL || payloadLenOut == NULL) return false;
    if (infoFieldLen != PROTO_WIRE_LEN) return false;

    sealInit();

    static uint8_t s_rxBlock[PROTO_BLOCK_N];
    memset(s_rxBlock, 0, sizeof(s_rxBlock));
    memcpy(s_rxBlock, infoField, PROTO_SEALED_LEN);
    memcpy(&s_rxBlock[PROTO_BLOCK_K], &infoField[PROTO_SEALED_LEN], PROTO_BLOCK_PARITY);

    if (sealDecode(s_rxBlock) < 0) return false;

    const uint8_t *wrapped = s_rxBlock;

#if AUTH_ENABLE
    const uint8_t *mac = &s_rxBlock[PROTO_WRAP_BLOCK_LEN];
    uint64_t expectedTag = computeTag(s_protoKey, wrapped, PROTO_WRAP_BLOCK_LEN);
    for (uint32_t i = 0; i < PROTO_TAG_LEN; i++) {
        if (mac[i] != (uint8_t)(expectedTag >> (8U * i))) return false;
    }

    uint32_t counter =
        ((uint32_t)wrapped[0] << 24) | ((uint32_t)wrapped[1] << 16) |
        ((uint32_t)wrapped[2] << 8) | (uint32_t)wrapped[3];
    if (counter <= s_protoRxFloor) return false;
    s_protoRxFloor = counter;
#endif

    uint8_t realLen = wrapped[4];
    if (realLen > PROTO_MAX_PAYLOAD_LEN || realLen > payloadOutMax) return false;

    memcpy(payloadOut, &wrapped[PROTO_WRAP_HDR_LEN], realLen);
    *payloadLenOut = realLen;
    return true;
}
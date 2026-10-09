#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "protocol.h"
#include "ax25.h"
#include "g3ruh.h"
#include "seal_wrap.h"

/*
 * ============================================================
 * BITSTREAM HELPER
 * ============================================================
 */

typedef struct
{
    uint8_t *buffer;       /* packed output bytes, first transmitted bit in the MSB */
    uint32_t capacity;     /* in bits */
    uint32_t count;        /* bits written */
    uint32_t sr;           /* G3RUH scrambler state */
    uint8_t nrzi;          /* NRZI line level */
} BitWriter;

/*
 * One HDLC-level bit in -> NRZI -> G3RUH scramble -> packed into the output.
 * Streaming: no intermediate one-byte-per-bit arrays.
 */
static bool BitWriter_Put(
    BitWriter *w,
    uint8_t bit)
{
    if (w == NULL)
        return false;

    if (w->count >= w->capacity)
        return false;

    if ((bit & 1U) == 0U)
        w->nrzi ^= 1U;

    uint8_t feedback =
        (uint8_t)(((w->sr >> G3RUH_POLY_TAP1) ^
                   (w->sr >> G3RUH_POLY_TAP2)) & 1U);

    uint8_t out = (uint8_t)(w->nrzi ^ feedback);

    w->sr = ((w->sr << 1) | out) & G3RUH_REGISTER_MASK;

    if (out)
        w->buffer[w->count >> 3] |=
            (uint8_t)(0x80U >> (w->count & 7U));

    w->count++;
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

static uint8_t s_clean[AX25_MAX_FRAME_SIZE];
static uint16_t s_cleanLen = 0;

uint16_t Protocol_GetLastTxAx25Frame(uint8_t *outFrame, uint16_t maxLen)
{
    if (outFrame == NULL || s_cleanLen == 0)
        return 0;

    uint16_t len = (s_cleanLen < maxLen) ? s_cleanLen : maxLen;
    memcpy(outFrame, s_clean, len);
    return len;
}

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

    /*
     * Short payloads (<= SEAL_MAX_PAYLOAD) go out as a 152-byte checked
     * block: counter + length + tag, plus 32 repair bytes (seal_core.S).
     * Longer payloads are sent as-is, exactly as before.
     */
    static uint8_t s_sealWire[SEAL_WIRE_LEN];
    if (payload != NULL && payloadLength <= SEAL_MAX_PAYLOAD)
    {
        uint16_t w = Seal_Wrap(payload, payloadLength, s_sealWire);
        if (w == 0)
        {
            return 0;
        }
        payload = s_sealWire;
        payloadLength = w;
    }

    /*
     * ========================================================
     * STEP 1
     *
     * Build ordinary AX.25 frame into static buffer to prevent stack overflow.
     * ========================================================
     */

    uint16_t cleanLen = 0;

    if (AX25_BuildFrame(
            s_clean,
            sizeof(s_clean),
            &cleanLen,
            cfg->destCallsign,
            cfg->destSSID,
            cfg->sourceCallsign,
            cfg->sourceSSID,
            payload,
            payloadLength) != AX25_OK)
    {
        return 0;
    }

    if (cleanLen <
        AX25_MinFrameLength())
    {
        return 0;
    }

    s_cleanLen = cleanLen;

    /*
     * ========================================================
     * STEP 2
     *
     * Generate the complete HDLC bitstream.
     *
     * The actual physical packet length is fixed.
     * ========================================================
     */

    memset(
        output,
        0,
        RADIO_FIXED_PACKET_LEN);

    BitWriter writer =
    {
        .buffer = output,
        .capacity =
            RADIO_FIXED_PACKET_LEN * 8U,
        .count = 0,
        .sr = G3RUH_INITIAL_STATE,
        .nrzi = 1
    };

    /*
     * Leading flags.
     */

    /*
     * Leading flags.
     * Large payloads produce ~1300 to ~1450 bits after HDLC bit stuffing and FCS.
     * With a fixed 200-byte (1600 bits) radio buffer, using 12 leading flags (96 bits)
     * guarantees ample headroom for bit stuffing and frame termination.
     */
    uint32_t leadingFlags = PROTOCOL_LEADING_FLAG_COUNT;
    if (payloadLength > 100)
    {
        /* CHANGED: 20 leading flags (the Packet Format sheets show 20 start-flag bytes), was 12. A camera fragment (138 B)
         * then needs about 181 of the 200 bytes before bit stuffing. */
        leadingFlags = 20;
    }
    /* Wrapped (152 B) frames: the satellite's RX window starts at preamble detect (no sync),
     * losing ~18 of the 200 bytes, so 12 flags would push the closing flag out of it. */
    if (payloadLength == SEAL_WIRE_LEN)
    {
        leadingFlags = 4;
    }

    for (uint32_t f = 0;
         f < leadingFlags;
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
     * CLOSING FLAGS: Up to 5 bytes of 0x7E as defined in Packet Format.xlsx
     *
     * AX.25/HDLC strictly requires at least 1 closing flag (0x7E).
     * Flags 2..5 are optional trailing flags to fill buffer; if buffer
     * runs out of space, break gracefully rather than failing the packet.
     * ========================================================
     */
    for (uint32_t f = 0; f < 5; f++)
    {
        if (!BitWriter_PutFlag(&writer))
        {
            if (f == 0)
                return 0; /* Minimum 1 closing flag must fit */
            break;
        }
    }

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
    enum { RX_FRAME_BITS = AX25_MAX_FRAME_SIZE * 10U };

    static uint8_t s_rx_frameBits[
        (RX_FRAME_BITS + 7U) / 8U
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
                                (uint8_t)((s_rx_frameBits[k >> 3] >>
                                           (k & 7U)) & 1U);

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
                if (frameBitCount < RX_FRAME_BITS)
                {
                    if (dataBit)
                        s_rx_frameBits[frameBitCount >> 3] |=
                            (uint8_t)(1U << (frameBitCount & 7U));
                    else
                        s_rx_frameBits[frameBitCount >> 3] &=
                            (uint8_t)~(1U << (frameBitCount & 7U));
                    frameBitCount++;
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
 * PAYLOAD UNWRAP
 *
 * Call on the information field after the AX.25 FCS has passed.
 * Returns true and the original payload when the field is a valid
 * 152-byte checked block (repairing up to 16 bad bytes).
 * ============================================================
 */

bool Protocol_DecodePayload(
    const uint8_t *infoField,
    uint16_t infoFieldLen,
    uint8_t *payloadOut,
    uint16_t payloadOutMax,
    uint16_t *payloadLenOut)
{
    return Seal_Unwrap(infoField, infoFieldLen, payloadOut, payloadOutMax, payloadLenOut);
}

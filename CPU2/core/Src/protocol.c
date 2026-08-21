#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "protocol.h"
#include "ax25.h"
#include "g3ruh.h"

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

    /*
     * ========================================================
     * STEP 1
     *
     * Build ordinary AX.25 frame.
     * ========================================================
     */

    uint8_t clean[AX25_MAX_FRAME_SIZE];

    uint16_t cleanLen = 0;

    if (AX25_BuildFrame(
            clean,
            sizeof(clean),
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

    /*
     * ========================================================
     * STEP 2
     *
     * Generate the complete HDLC bitstream.
     *
     * The actual physical packet length is fixed.
     * ========================================================
     */

    uint8_t hdlcBits[
        RADIO_FIXED_PACKET_LEN * 8U
    ];

    memset(
        hdlcBits,
        0,
        sizeof(hdlcBits));

    BitWriter writer =
    {
        .buffer = hdlcBits,
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
        uint8_t value = clean[i];

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
     * clean[cleanLen-3] = FCS low
     * clean[cleanLen-2] = FCS high
     *
     * Standard AX.25 / HDLC transmits both FCS bytes LSB-first:
     * low byte first (bit 0..7), then high byte (bit 0..7).
     * Bit stuffing applies across the FCS field.
     * ========================================================
     */

    uint8_t fcsLow =
        clean[cleanLen - 3];

    uint8_t fcsHigh =
        clean[cleanLen - 2];

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

    uint8_t nrziBits[
        RADIO_FIXED_PACKET_LEN * 8U
    ];

    uint8_t nrziState = 1;

    for (uint32_t i = 0;
         i < writer.count;
         i++)
    {
        uint8_t bit =
            hdlcBits[i];

        if (bit == 0)
            nrziState =
                (uint8_t)!nrziState;

        nrziBits[i] =
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

    uint8_t scrambledBits[
        RADIO_FIXED_PACKET_LEN * 8U
    ];

    uint32_t sr =
        G3RUH_INITIAL_STATE;

    for (uint32_t i = 0;
         i < writer.count;
         i++)
    {
        uint8_t inputBit =
            nrziBits[i];

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

        scrambledBits[i] =
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

            if (scrambledBits[bitIndex])
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
     * Maximum stuffed bit storage.
     */
    uint8_t frameBits[
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
                 * already been appended to frameBits.
                 *
                 * Remove those seven bits.
                 */

                if (inFrame)
                {
                    if (frameBitCount >= 7U)
                    {
                        uint32_t contentBits =
                            frameBitCount - 7U;

                        uint8_t unstuffed[
                            AX25_MAX_FRAME_SIZE
                        ];

                        memset(
                            unstuffed,
                            0,
                            sizeof(unstuffed));

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
                                frameBits[k];

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
                                 sizeof(unstuffed) *
                                 8U))
                            {
                                violation =
                                    true;
                                break;
                            }

                            if (bit)
                            {
                                unstuffed[
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
                                    unstuffed,
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
                    sizeof(frameBits))
                {
                    frameBits[
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
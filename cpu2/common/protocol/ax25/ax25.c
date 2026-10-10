#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

#include "ax25.h"

/*
 * ============================================================
 * CRC-16/X.25
 *
 * Polynomial:
 *     x^16 + x^12 + x^5 + 1
 *
 * Reflected implementation:
 *     0x8408
 *
 * Initial value:
 *     0xFFFF
 *
 * Final XOR:
 *     0xFFFF
 *
 * The function below returns the RAW CRC before final inversion.
 * AX25_BuildFrame() performs the final inversion and stores:
 *
 *     FCS low byte
 *     FCS high byte
 *
 * The physical AX.25 serializer later transmits the FCS
 * MSB-first as required by AX.25.
 * ============================================================
 */

/* CRC-16/X.25 (reflected polynomial 0x8408), nibble-wise: a 16-entry table instead of 256 (saves 480 bytes of flash) */
static const uint16_t crc16_x25_nibble[16] =
{
    0x0000, 0x1081, 0x2102, 0x3183, 0x4204, 0x5285, 0x6306, 0x7387, 0x8408, 0x9489, 0xA50A, 0xB58B, 0xC60C, 0xD68D, 0xE70E, 0xF78F
};

uint16_t AX25_CalculateCRC(const uint8_t *data, uint16_t length)
{
    if (data == NULL)
        return 0;

    uint16_t crc = AX25_CRC_INIT;

    for (uint16_t i = 0; i < length; i++)
    {
        crc ^= data[i];
        crc = (uint16_t)((crc >> 4) ^ crc16_x25_nibble[crc & 0x0F]);
        crc = (uint16_t)((crc >> 4) ^ crc16_x25_nibble[crc & 0x0F]);
    }

    return crc;
}

/*
 * ============================================================
 * AX.25 ADDRESS ENCODE
 * ============================================================
 */

AX25_Status_t AX25_EncodeAddress(
    uint8_t *address,
    const char *callsign,
    uint8_t ssid,
    bool last)
{
    if (address == NULL || callsign == NULL)
        return AX25_ERROR_NULL_POINTER;

    if (ssid > AX25_MAX_SSID)
        return AX25_ERROR_SSID;

    char cs[7] =
    {
        ' ', ' ', ' ', ' ', ' ', ' ', '\0'
    };

    size_t len = strlen(callsign);

    if (len > 6)
        len = 6;

    for (size_t i = 0; i < len; i++)
    {
        cs[i] = (char)toupper((unsigned char)callsign[i]);
    }

    for (int i = 0; i < 6; i++)
    {
        address[i] = ((uint8_t)cs[i]) << 1;
    }

    /*
     * SSID byte:
     *
     * bit 0 = address extension
     * bits 1..4 = SSID
     * bits 5..6 = reserved = 1
     * bit 7 = command/response convention
     *
     * For a normal two-address frame:
     * destination last = 0
     * source last      = 1
     */

    address[6] =
        0x60 |
        ((uint8_t)(ssid & 0x0F) << 1);

    if (last)
        address[6] |= 0x01;

    return AX25_OK;
}

/*
 * ============================================================
 * AX.25 ADDRESS DECODE
 * ============================================================
 */

AX25_Status_t AX25_DecodeAddress(
    const uint8_t *address,
    char *callsign)
{
    if (address == NULL || callsign == NULL)
        return AX25_ERROR_NULL_POINTER;

    for (int i = 0; i < 6; i++)
    {
        char c = (char)(address[i] >> 1);

        if (c == ' ')
        {
            callsign[i] = '\0';
            break;
        }

        callsign[i] = c;

        if (i == 5)
            callsign[6] = '\0';
    }

    callsign[6] = '\0';

    return AX25_OK;
}

uint8_t AX25_DecodeSSID(const uint8_t *address)
{
    if (address == NULL)
        return 0;

    return (address[6] >> 1) & 0x0F;
}

bool AX25_DecodeIsLast(const uint8_t *address)
{
    if (address == NULL)
        return false;

    return (address[6] & 0x01U) != 0;
}

/*
 * ============================================================
 * BUILD AX.25 UI FRAME
 *
 * Software representation:
 *
 *   7E
 *   DEST 7 bytes
 *   SRC  7 bytes
 *   03
 *   F0
 *   PAYLOAD
 *   FCS low
 *   FCS high
 *   7E
 *
 * Physical bit serialization is performed later by protocol.c.
 * ============================================================
 */

AX25_Status_t AX25_BuildFrame(
    uint8_t *frameBuffer,
    uint16_t frameBufferSize,
    uint16_t *frameLength,
    const char *destination,
    uint8_t destinationSSID,
    const char *source,
    uint8_t sourceSSID,
    const uint8_t *payload,
    uint16_t payloadLength)
{
    if (frameBuffer == NULL ||
        frameLength == NULL ||
        destination == NULL ||
        source == NULL)
    {
        return AX25_ERROR_NULL_POINTER;
    }

    if (payloadLength > AX25_MAX_PAYLOAD_LEN)
        return AX25_ERROR_PAYLOAD;

    uint16_t needed =
        AX25_FLAG_LEN +
        AX25_ADDRESS_FIELDS +
        AX25_CONTROL_PID_LEN +
        payloadLength +
        AX25_FCS_LEN;

    if (frameBufferSize < needed)
        return AX25_ERROR_BUFFER;

    uint16_t idx = 0;

    /*
     * Opening flag
     */
    frameBuffer[idx++] = AX25_FLAG;

    /*
     * Destination
     */
    if (AX25_EncodeAddress(
            &frameBuffer[idx],
            destination,
            destinationSSID,
            false) != AX25_OK)
    {
        return AX25_ERROR_CALLSIGN;
    }

    idx += AX25_ADDRESS_LEN;

    /*
     * Source
     */
    if (AX25_EncodeAddress(
            &frameBuffer[idx],
            source,
            sourceSSID,
            true) != AX25_OK)
    {
        return AX25_ERROR_CALLSIGN;
    }

    idx += AX25_ADDRESS_LEN;

    /*
     * UI frame
     */
    frameBuffer[idx++] = AX25_CONTROL_UI;
    frameBuffer[idx++] = AX25_PID_NO_LAYER3;

    /*
     * Information field
     */
    if (payload != NULL && payloadLength != 0)
    {
        memcpy(
            &frameBuffer[idx],
            payload,
            payloadLength);

        idx += payloadLength;
    }

    /*
     * CRC covers:
     *
     * DEST + SRC + CONTROL + PID + INFO
     *
     * excluding both flags.
     */

    uint16_t crc =
        AX25_CalculateCRC(
            &frameBuffer[1],
            idx - 1);

    crc = (uint16_t)~crc;

    /*
     * AX.25 software representation keeps FCS
     * as low byte followed by high byte.
     *
     * protocol.c performs the required physical
     * MSB-first serialization.
     */

    frameBuffer[idx++] =
        (uint8_t)(crc & 0xFFU);

    frameBuffer[idx++] =
        (uint8_t)((crc >> 8) & 0xFFU);

    /*
     * Closing flag
     */
    frameBuffer[idx++] = AX25_FLAG;

    *frameLength = idx;

    return AX25_OK;
}

/*
 * ============================================================
 * CRC METHOD 1
 *
 * Residue check:
 *
 *     CRC(data + received FCS) == AX25_CRC_RESIDUE
 *
 * ============================================================
 */

bool AX25_VerifyCRC_Method1(
    const uint8_t *frame,
    uint16_t frameLength)
{
    if (frame == NULL ||
        frameLength < AX25_MinFrameLength())
    {
        return false;
    }

    uint16_t crc =
        AX25_CalculateCRC(
            &frame[1],
            frameLength - 2);

    return (crc == AX25_CRC_RESIDUE);
}

/*
 * ============================================================
 * CRC METHOD 2
 *
 * Calculate CRC over frame payload and compare with
 * received FCS.
 * ============================================================
 */

bool AX25_VerifyCRC_Method2(
    const uint8_t *frame,
    uint16_t frameLength,
    uint16_t *computedCRC,
    uint16_t *receivedLE,
    uint16_t *receivedBE)
{
    if (frame == NULL ||
        frameLength < AX25_MinFrameLength())
    {
        return false;
    }

    /*
     * Remove:
     *
     * opening flag = 1
     * FCS           = 2
     * closing flag  = 1
     *
     * therefore data length = frameLength - 4
     */

    uint16_t dataLength =
        frameLength - 4;

    uint16_t crc =
        AX25_CalculateCRC(
            &frame[1],
            dataLength);

    crc = (uint16_t)~crc;

    uint16_t recvLE =
        (uint16_t)frame[frameLength - 3] |
        ((uint16_t)frame[frameLength - 2] << 8);

    uint16_t recvBE =
        (uint16_t)frame[frameLength - 2] |
        ((uint16_t)frame[frameLength - 3] << 8);

    if (computedCRC)
        *computedCRC = crc;

    if (receivedLE)
        *receivedLE = recvLE;

    if (receivedBE)
        *receivedBE = recvBE;

    return (crc == recvLE);
}

/*
 * ============================================================
 * COMPLETE FRAME VALIDATION
 * ============================================================
 */

bool AX25_VerifyFrame(
    const uint8_t *frame,
    uint16_t frameLength)
{
    if (frame == NULL ||
        frameLength < AX25_MinFrameLength())
    {
        return false;
    }

    if (frame[0] != AX25_FLAG)
        return false;

    if (frame[frameLength - 1] != AX25_FLAG)
        return false;

    return AX25_VerifyCRC_Method1(
        frame,
        frameLength);
}

/*
 * ============================================================
 * HDLC BIT STUFF
 *
 * Bits are processed LSB-first.
 * ============================================================
 */

uint16_t HDLC_BitStuff(
    const uint8_t *input,
    uint16_t input_len,
    uint8_t *output,
    uint16_t output_max)
{
    if (input == NULL ||
        output == NULL ||
        output_max == 0)
    {
        return 0;
    }

    memset(output, 0, output_max);

    uint32_t out_bit = 0;
    uint8_t ones = 0;

    for (uint16_t i = 0; i < input_len; i++)
    {
        for (uint8_t b = 0; b < 8; b++)
        {
            uint8_t bit =
                (input[i] >> b) & 1U;

            if (out_bit >=
                ((uint32_t)output_max * 8U))
            {
                return 0;
            }

            if (bit)
            {
                output[out_bit / 8U] |=
                    (uint8_t)(1U << (out_bit % 8U));

                out_bit++;
                ones++;

                if (ones == 5)
                {
                    if (out_bit >=
                        ((uint32_t)output_max * 8U))
                    {
                        return 0;
                    }

                    /*
                     * Stuffed zero.
                     *
                     * output is already zero.
                     */
                    out_bit++;
                    ones = 0;
                }
            }
            else
            {
                out_bit++;
                ones = 0;
            }
        }
    }

    return (uint16_t)((out_bit + 7U) / 8U);
}

/*
 * ============================================================
 * HDLC BIT UNSTUFF
 * ============================================================
 */

uint16_t HDLC_BitUnstuff(
    const uint8_t *input,
    uint16_t input_len,
    uint8_t *output,
    uint16_t output_max)
{
    if (input == NULL ||
        output == NULL ||
        output_max == 0)
    {
        return 0;
    }

    memset(output, 0, output_max);

    uint32_t out_bit = 0;
    uint8_t ones = 0;

    for (uint16_t i = 0; i < input_len; i++)
    {
        for (uint8_t b = 0; b < 8; b++)
        {
            uint8_t bit =
                (input[i] >> b) & 1U;

            /*
             * A zero immediately after five ones
             * is the stuffed bit.
             */
            if (ones == 5)
            {
                if (bit != 0)
                    return 0;

                ones = 0;
                continue;
            }

            if (out_bit >=
                ((uint32_t)output_max * 8U))
            {
                return 0;
            }

            if (bit)
            {
                output[out_bit / 8U] |=
                    (uint8_t)(1U << (out_bit % 8U));

                ones++;
            }
            else
            {
                ones = 0;
            }

            out_bit++;
        }
    }

    if ((out_bit & 7U) != 0)
        return 0;

    return (uint16_t)(out_bit / 8U);
}
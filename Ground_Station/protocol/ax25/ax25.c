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

static const uint16_t crc16_x25_table[256] =
{
    0x0000,0x1189,0x2312,0x329B,0x4624,0x57AD,0x6536,0x74BF,
    0x8C48,0x9DC1,0xAF5A,0xBED3,0xCA6C,0xDBE5,0xE97E,0xF8F7,
    0x1081,0x0108,0x3393,0x221A,0x56A5,0x472C,0x75B7,0x643E,
    0x9CC9,0x8D40,0xBFDB,0xAE52,0xDAED,0xCB64,0xF9FF,0xE876,
    0x2102,0x308B,0x0210,0x1399,0x6726,0x76AF,0x4434,0x55BD,
    0xAD4A,0xBCC3,0x8E58,0x9FD1,0xEB6E,0xFAE7,0xC87C,0xD9F5,
    0x3183,0x200A,0x1291,0x0318,0x77A7,0x662E,0x54B5,0x453C,
    0xBDCB,0xAC42,0x9ED9,0x8F50,0xFBEF,0xEA66,0xD8FD,0xC974,
    0x4204,0x538D,0x6116,0x709F,0x0420,0x15A9,0x2732,0x36BB,
    0xCE4C,0xDFC5,0xED5E,0xFCD7,0x8868,0x99E1,0xAB7A,0xBAF3,
    0x5285,0x430C,0x7197,0x601E,0x14A1,0x0528,0x37B3,0x263A,
    0xDECD,0xCF44,0xFDDF,0xEC56,0x98E9,0x8960,0xBBFB,0xAA72,
    0x6306,0x728F,0x4014,0x519D,0x2522,0x34AB,0x0630,0x17B9,
    0xEF4E,0xFEC7,0xCC5C,0xDDD5,0xA96A,0xB8E3,0x8A78,0x9BF1,
    0x7387,0x620E,0x5095,0x411C,0x35A3,0x242A,0x16B1,0x0738,
    0xFFCF,0xEE46,0xDCDD,0xCD54,0xB9EB,0xA862,0x9AF9,0x8B70,
    0x8408,0x9581,0xA71A,0xB693,0xC22C,0xD3A5,0xE13E,0xF0B7,
    0x0840,0x19C9,0x2B52,0x3ADB,0x4E64,0x5FED,0x6D76,0x7CFF,
    0x9489,0x8500,0xB79B,0xA612,0xD2AD,0xC324,0xF1BF,0xE036,
    0x18C1,0x0948,0x3BD3,0x2A5A,0x5EE5,0x4F6C,0x7DF7,0x6C7E,
    0xA50A,0xB483,0x8618,0x9791,0xE32E,0xF2A7,0xC03C,0xD1B5,
    0x2942,0x38CB,0x0A50,0x1BD9,0x6F66,0x7EEF,0x4C74,0x5DFD,
    0xB58B,0xA402,0x9699,0x8710,0xF3AF,0xE226,0xD0BD,0xC134,
    0x39C3,0x284A,0x1AD1,0x0B58,0x7FE7,0x6E6E,0x5CF5,0x4D7C,
    0xC60C,0xD785,0xE51E,0xF497,0x8028,0x91A1,0xA33A,0xB2B3,
    0x4A44,0x5BCD,0x6956,0x78DF,0x0C60,0x1DE9,0x2F72,0x3EFB,
    0xD68D,0xC704,0xF59F,0xE416,0x90A9,0x8120,0xB3BB,0xA232,
    0x5AC5,0x4B4C,0x79D7,0x685E,0x1CE1,0x0D68,0x3FF3,0x2E7A,
    0xE70E,0xF687,0xC41C,0xD595,0xA12A,0xB0A3,0x8238,0x93B1,
    0x6B46,0x7ACF,0x4854,0x59DD,0x2D62,0x3CEB,0x0E70,0x1FF9,
    0xF78F,0xE606,0xD49D,0xC514,0xB1AB,0xA022,0x92B9,0x8330,
    0x7BC7,0x6A4E,0x58D5,0x495C,0x3DE3,0x2C6A,0x1EF1,0x0F78
};

uint16_t AX25_CalculateCRC(const uint8_t *data, uint16_t length)
{
    if (data == NULL)
        return 0;

    uint16_t crc = AX25_CRC_INIT;

    for (uint16_t i = 0; i < length; i++)
    {
        crc =
            (crc >> 8) ^
            crc16_x25_table[(crc ^ data[i]) & 0xFF];
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
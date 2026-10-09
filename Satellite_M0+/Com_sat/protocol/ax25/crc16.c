#include <stdint.h>

#include <stddef.h>
#include "crc16.h"

/*
 * ============================================================
 * CRC-16/X.25
 *
 * This implementation is retained only for generic CRC tests.
 *
 * AX.25 production code should use:
 *
 *     AX25_CalculateCRC()
 *
 * from ax25.c
 * ============================================================
 */

uint16_t CRC16_X25(
    const uint8_t *data,
    uint16_t length)
{
    if (data == NULL)
        return 0;

    uint16_t crc =
        CRC16_X25_INIT;

    while (length--)
    {
        crc ^= *data++;

        for (uint8_t bit = 0;
             bit < 8;
             bit++)
        {
            if (crc & 1U)
            {
                crc =
                    (crc >> 1) ^
                    CRC16_X25_POLYNOMIAL;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return (uint16_t)~crc;
}

uint8_t CRC16_X25_Verify(
    const uint8_t *data,
    uint16_t length)
{
    if (data == NULL)
        return 0;

    uint16_t crc =
        CRC16_X25_INIT;

    while (length--)
    {
        crc ^= *data++;

        for (uint8_t bit = 0;
             bit < 8;
             bit++)
        {
            if (crc & 1U)
            {
                crc =
                    (crc >> 1) ^
                    CRC16_X25_POLYNOMIAL;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return
        (crc == CRC16_X25_RESIDUE);
}
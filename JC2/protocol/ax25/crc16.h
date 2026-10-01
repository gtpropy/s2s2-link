#ifndef CRC16_H
#define CRC16_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*==========================================================
    CRC-16/X.25

    Polynomial : 0x1021

    Reflected  : 0x8408

    Initial    : 0xFFFF

    Final XOR  : 0xFFFF
==========================================================*/

#define CRC16_X25_INIT          0xFFFFU
#define CRC16_X25_POLYNOMIAL    0x8408U
#define CRC16_X25_RESIDUE       0xF0B8U

/*
 * Calculate CRC-16/X.25
 */
uint16_t CRC16_X25(
    const uint8_t *data,
    uint16_t length);

/*
 * Verify CRC residue.
 *
 * Pass the entire message including the received
 * CRC bytes.
 *
 * Returns true if CRC is valid.
 */
uint8_t CRC16_X25_Verify(
    const uint8_t *data,
    uint16_t length);

#ifdef __cplusplus
}
#endif

#endif /* CRC16_H */
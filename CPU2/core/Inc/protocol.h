#ifndef PROTOCOL_H
#define PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#include "config.h"
#include "ax25.h"
#include "g3ruh.h"

/*==========================================================
  Protocol API - Fixed-length, standard-compliant framing

  Fixed-length mode means the SX126x no longer inserts its own
  length byte on air - the exact same byte count is always sent,
  padded with literal 0x7E flags. This matches how a real 9600
  baud G3RUH radio/SDR expects a continuous HDLC bitstream, with
  no vendor-specific framing mixed in.
==========================================================*/

#ifndef PROTOCOL_LEADING_FLAG_COUNT
#define PROTOCOL_LEADING_FLAG_COUNT   16U  /* 16 preamble flags (128 bits) to give
                                               SDR clock recovery PLL & descrambler
                                               ample time to settle */
#endif

#ifndef RADIO_FIXED_PACKET_LEN
#define RADIO_FIXED_PACKET_LEN        AX25_MAX_FRAME_SIZE  /* must fit inside
                                               the SX126x's 128-byte RX FIFO
                                               half (RX base 0x80-0xFF) */
#endif

#define PROTOCOL_STUFFED_BUF_SIZE     (AX25_MAX_FRAME_SIZE + 40U)

/*
 * Build a complete, fixed-length TX packet:
 *
 *   payload
 *     -> AX.25 frame (with FCS)
 *     -> Bit stuffing
 *     -> Leading flags + start flag + data + end flag + pad flags
 *        (padded to exactly RADIO_FIXED_PACKET_LEN bytes)
 *     -> NRZ-I encode
 *     -> G3RUH scramble
 *
 * Always returns RADIO_FIXED_PACKET_LEN on success, or 0 if the
 * payload doesn't fit in the fixed slot (shrink payloadLength).
 */
uint16_t Protocol_CreatePacket(
    uint8_t *output,
    const uint8_t *payload,
    uint16_t payloadLength,
    const RadioConfig_t *cfg);

/*
 * Take a raw, fixed-length received buffer (still G3RUH-scrambled
 * and NRZ-I-encoded), undo both, locate the frame between two
 * literal 0x7E flags, bit-unstuff it, and copy out a clean
 * flag-delimited AX.25 frame ready for the existing CRC/address
 * checks.
 *
 * Returns false if no valid flag-delimited frame was found.
 */
bool Protocol_ExtractFrame(
    const uint8_t *rxBuffer,
    uint16_t rxBufferLen,
    uint8_t *frameOut,
    uint16_t frameOutMax,
    uint16_t *frameOutLen);

bool Protocol_VerifyPacket(
    const uint8_t *frame,
    uint16_t frameLength);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_H */
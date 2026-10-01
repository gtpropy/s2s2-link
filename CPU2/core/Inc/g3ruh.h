/*
 * g3ruh.h -- G3RUH self-synchronizing scrambler + NRZI, bit-exact with
 * Direwolf's own reference implementation (github.com/wb2osz/direwolf,
 * src/gen_tone.c's tone_gen_put_bit() for TX, src/demod_9600.h's
 * descramble() + src/hdlc_rec.c's dbit-from-descram compare for RX), so
 * a real Direwolf (`-B 4800 -g`), UZ7HO Soundmodem, or any other G3RUH-
 * capable AX.25/FX.25 receiver can decode this signal.
 *
 * Two extra things beyond the scrambler itself, both needed because the
 * SX126x's native GFSK packet engine -- unlike a sound-card TNC bit-
 * banging a UART -- shifts each payload byte onto the air MSB-first
 * (confirmed against real captured air samples: gmsk_demod.py's sync-
 * word correlation only locks with bits_of_byte(..., "msb"), never
 * "lsb"), while standard AX.25/G3RUH is defined LSB-first per octet
 * (matches fx25.c's own internal bit-stuffing convention, get_bit()/
 * put_bit(), which is already LSB-first and spec-correct):
 *
 *   1. The scrambler/NRZI operate on a LSB-first bitstream built from
 *      the frame bytes (matching AX.25/fx25.c convention, and Direwolf).
 *   2. Each output byte is then bit-reversed before being handed to
 *      SUBGRF_SetPayload, so that after the SX126x's own MSB-first
 *      shift-out, the bits that actually leave the antenna are in the
 *      correct standard order. RX undoes this first, before descrambling.
 *
 * Both the scrambler's LFSR and the NRZI reference bit reset to a fixed
 * value at the start of every call -- one call scrambles/descrambles one
 * complete, self-contained on-air frame (this project sends one FX.25
 * frame per transmission burst, not a continuous back-to-back stream).
 * This is safe: a self-synchronizing scrambler's correctness never
 * depends on any *shared* initial state between TX and RX -- only on
 * TX and RX each using a *fixed, internally consistent* one, which the
 * 0-init below is.
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-09-30
 */
#ifndef G3RUH_H
#define G3RUH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SX126x packet-engine FIFO limit -- callers never pass more than this. */
#define G3RUH_MAX_LEN 255U

/* NRZI-encodes then G3RUH-scrambles `len` bytes of `in` (LSB-first bit
   order per byte, matching AX.25/fx25.c), then bit-reverses each output
   byte to compensate the SX126x's native MSB-first shift-out. `out` may
   alias `in`. Frames longer than G3RUH_MAX_LEN are left untouched. */
void g3ruh_scramble_frame(const uint8_t *in, uint8_t *out, uint16_t len);

/* Exact inverse of g3ruh_scramble_frame(): undoes the SX126x bit
   reversal, then descrambles and NRZI-decodes back to `out` (LSB-first
   bit order per byte). `out` may alias `in`. Frames longer than
   G3RUH_MAX_LEN are left untouched. */
void g3ruh_descramble_frame(const uint8_t *in, uint8_t *out, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* G3RUH_H */

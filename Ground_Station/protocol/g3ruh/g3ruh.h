#ifndef G3RUH_H
#define G3RUH_H

#include <stdint.h>

/*==========================================================
  G3RUH self-synchronizing scrambler

  Polynomial: x^17 + x^12 + 1
  Taps at bit index 11 and bit index 16 (0-indexed), matching
  the confirmed-working reference G3RUH.c.
==========================================================*/
#define G3RUH_POLY_TAP1      11u        /* x^12 term */
#define G3RUH_POLY_TAP2      16u        /* x^17 term */
#define G3RUH_REGISTER_MASK  0x1FFFFu   /* 17-bit register */

#define G3RUH_INITIAL_STATE  0x1FFFFu

typedef struct
{
    uint32_t shiftRegister;
} G3RUH_Context_t;

void G3RUH_Init(G3RUH_Context_t *ctx);
/* Add this function declaration */
void G3RUH_Reset(G3RUH_Context_t *ctx);
void G3RUH_ScrambleBuffer(G3RUH_Context_t *ctx, uint8_t *buffer, uint16_t length);
void G3RUH_DescrambleBuffer(G3RUH_Context_t *ctx, uint8_t *buffer, uint16_t length);

void NRZI_Encode(uint8_t *buffer, uint16_t length);
void NRZI_Decode(uint8_t *buffer, uint16_t length);

/*==========================================================
  FIX (added): per-byte bit reflection.

  The SX126x FIFO always shifts bytes out MSB-first over the
  air. The reference G3RUH.c compensates for this by explicitly
  re-packing each logically LSB-first AX.25 bit into the MSB
  slot of its output byte (see Build_G3RUH_TX_Frame:
  `txByte |= (bit << (7 - txBitPos))`).

  This modular pipeline (HDLC_BitStuff -> NRZI_Encode ->
  G3RUH_ScrambleBuffer) keeps every bit at the same bit
  position throughout, so without this reflect step the whole
  on-air byte stream comes out bit-reversed relative to
  standard AX.25/G3RUH bit order. Two units both running this
  code talk to each other fine (the reversal cancels out), but
  they will NOT interoperate with the reference station or with
  UZ7HO/Direwolf/gr-satellites.

  Call ReverseBitsPerByte() once, at the TX/RX boundary only:
    TX: after G3RUH_ScrambleBuffer(), just before handing the
        buffer to SUBGRF_SetPayload().
    RX: first thing, before G3RUH_DescrambleBuffer().
==========================================================*/
void ReverseBitsPerByte(uint8_t *buffer, uint16_t length);

#endif /* G3RUH_H */
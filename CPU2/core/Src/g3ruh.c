/*
 * g3ruh.c
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-09-30
 */

#include "g3ruh.h"

static uint8_t reverse8(uint8_t v)
{
    v = (uint8_t)((v & 0xF0U) >> 4 | (v & 0x0FU) << 4);
    v = (uint8_t)((v & 0xCCU) >> 2 | (v & 0x33U) << 2);
    v = (uint8_t)((v & 0xAAU) >> 1 | (v & 0x55U) << 1);
    return v;
}

/* NRZI reference bit both sides start from -- see g3ruh.h. Any fixed
   value works; only TX/RX using the *same* one matters. */
#define NRZI_INITIAL_REF 1

void g3ruh_scramble_frame(const uint8_t *in, uint8_t *out, uint16_t len)
{
    if (len > G3RUH_MAX_LEN) return;

    uint8_t scratch[G3RUH_MAX_LEN];
    for (uint16_t i = 0; i < len; i++) scratch[i] = 0;

    uint32_t lfsr = 0;
    int nrzi_prev = NRZI_INITIAL_REF;

    for (uint16_t bitpos = 0; bitpos < (uint16_t)(len * 8U); bitpos++) {
        uint16_t byte_idx = bitpos >> 3;
        uint8_t bit_idx = (uint8_t)(bitpos & 7U);
        int data_bit = (in[byte_idx] >> bit_idx) & 1;

        int nrzi_bit = data_bit ? nrzi_prev : !nrzi_prev;
        nrzi_prev = nrzi_bit;

        int x = (nrzi_bit ^ (int)(lfsr >> 16) ^ (int)(lfsr >> 11)) & 1;
        lfsr = (lfsr << 1) | (uint32_t)x;

        if (x) scratch[byte_idx] = (uint8_t)(scratch[byte_idx] | (uint8_t)(1U << bit_idx));
    }

    for (uint16_t i = 0; i < len; i++) out[i] = reverse8(scratch[i]);
}

void g3ruh_descramble_frame(const uint8_t *in, uint8_t *out, uint16_t len)
{
    if (len > G3RUH_MAX_LEN) return;

    uint8_t scratch[G3RUH_MAX_LEN];
    for (uint16_t i = 0; i < len; i++) scratch[i] = reverse8(in[i]);

    uint32_t lfsr = 0;
    int prev_descram = NRZI_INITIAL_REF;

    uint8_t result[G3RUH_MAX_LEN];
    for (uint16_t i = 0; i < len; i++) result[i] = 0;

    for (uint16_t bitpos = 0; bitpos < (uint16_t)(len * 8U); bitpos++) {
        uint16_t byte_idx = bitpos >> 3;
        uint8_t bit_idx = (uint8_t)(bitpos & 7U);
        int raw = (scratch[byte_idx] >> bit_idx) & 1;

        int descram = (raw ^ (int)(lfsr >> 16) ^ (int)(lfsr >> 11)) & 1;
        lfsr = (lfsr << 1) | (uint32_t)raw;

        int dbit = (descram == prev_descram) ? 1 : 0;
        prev_descram = descram;

        if (dbit) result[byte_idx] = (uint8_t)(result[byte_idx] | (uint8_t)(1U << bit_idx));
    }

    for (uint16_t i = 0; i < len; i++) out[i] = result[i];
}

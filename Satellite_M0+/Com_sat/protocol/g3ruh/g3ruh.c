#include <stdint.h>
#include <stddef.h>
#include "g3ruh.h"

/*
 * ============================================================
 * G3RUH 9600 SCRAMBLER
 *
 * Polynomial:
 *
 *     1 + x^12 + x^17
 *
 * The input to the scrambler is the NRZI stream.
 *
 * TX:
 *
 *     NRZI bit
 *        |
 *        v
 *     G3RUH
 *        |
 *        v
 *     RF
 *
 * RX:
 *
 *     RF detected bit
 *        |
 *        v
 *     G3RUH descrambler
 *        |
 *        v
 *     NRZI decoder
 *
 * This matches the conventional G3RUH processing chain.
 * ============================================================
 */

void G3RUH_Init(G3RUH_Context_t *ctx)
{
    if (ctx == NULL)
        return;

    ctx->shiftRegister =
        G3RUH_INITIAL_STATE;
}

/*
 * ============================================================
 * SCRAMBLER
 *
 * IMPORTANT:
 *
 * Feedback uses the previously transmitted scrambled bits.
 * ============================================================
 */

void G3RUH_ScrambleBuffer(
    G3RUH_Context_t *ctx,
    uint8_t *buffer,
    uint16_t length)
{
    if (ctx == NULL || buffer == NULL)
        return;

    uint32_t sr =
        ctx->shiftRegister;

    for (uint16_t byte = 0;
         byte < length;
         byte++)
    {
        uint8_t input =
            buffer[byte];

        uint8_t output = 0;

        for (uint8_t bit = 0;
             bit < 8;
             bit++)
        {
            uint8_t data =
                (input >> bit) & 1U;

            uint8_t feedback =
                ((sr >> G3RUH_POLY_TAP1) ^
                 (sr >> G3RUH_POLY_TAP2)) & 1U;

            uint8_t scrambled =
                data ^ feedback;

            sr =
                ((sr << 1) | scrambled) &
                G3RUH_REGISTER_MASK;

            output |=
                (uint8_t)(scrambled << bit);
        }

        buffer[byte] = output;
    }

    ctx->shiftRegister = sr;
}

/*
 * ============================================================
 * DESCRAMBLER
 * ============================================================
 */

void G3RUH_DescrambleBuffer(
    G3RUH_Context_t *ctx,
    uint8_t *buffer,
    uint16_t length)
{
    if (ctx == NULL || buffer == NULL)
        return;

    uint32_t sr =
        ctx->shiftRegister;

    for (uint16_t byte = 0;
         byte < length;
         byte++)
    {
        uint8_t input =
            buffer[byte];

        uint8_t output = 0;

        for (uint8_t bit = 0;
             bit < 8;
             bit++)
        {
            uint8_t scrambled =
                (input >> bit) & 1U;

            uint8_t feedback =
                ((sr >> G3RUH_POLY_TAP1) ^
                 (sr >> G3RUH_POLY_TAP2)) & 1U;

            uint8_t data =
                scrambled ^ feedback;

            /*
             * Feed the RECEIVED scrambled bit back
             * into the descrambler.
             */
            sr =
                ((sr << 1) | scrambled) &
                G3RUH_REGISTER_MASK;

            output |=
                (uint8_t)(data << bit);
        }

        buffer[byte] = output;
    }

    ctx->shiftRegister = sr;
}

/*
 * ============================================================
 * NRZI
 *
 * 0 = transition
 * 1 = no transition
 * ============================================================
 */

void NRZI_Encode(
    uint8_t *buffer,
    uint16_t length)
{
    if (buffer == NULL)
        return;

    uint8_t last = 1;

    for (uint16_t i = 0;
         i < length;
         i++)
    {
        uint8_t output = 0;

        for (uint8_t b = 0;
             b < 8;
             b++)
        {
            uint8_t bit =
                (buffer[i] >> b) & 1U;

            if (bit == 0)
                last = (uint8_t)!last;

            if (last)
            {
                output |=
                    (uint8_t)(1U << b);
            }
        }

        buffer[i] = output;
    }
}

void NRZI_Decode(
    uint8_t *buffer,
    uint16_t length)
{
    if (buffer == NULL)
        return;

    uint8_t last = 1;

    for (uint16_t i = 0;
         i < length;
         i++)
    {
        uint8_t output = 0;

        for (uint8_t b = 0;
             b < 8;
             b++)
        {
            uint8_t received =
                (buffer[i] >> b) & 1U;

            uint8_t data =
                (received == last) ? 1U : 0U;

            last = received;

            if (data)
            {
                output |=
                    (uint8_t)(1U << b);
            }
        }

        buffer[i] = output;
    }
}

/*
 * ============================================================
 * BIT REVERSE
 * ============================================================
 */

void ReverseBitsPerByte(
    uint8_t *buffer,
    uint16_t length)
{
    if (buffer == NULL)
        return;

    for (uint16_t i = 0;
         i < length;
         i++)
    {
        uint8_t b = buffer[i];

        b =
            (uint8_t)(
                ((b & 0xF0U) >> 4) |
                ((b & 0x0FU) << 4));

        b =
            (uint8_t)(
                ((b & 0xCCU) >> 2) |
                ((b & 0x33U) << 2));

        b =
            (uint8_t)(
                ((b & 0xAAU) >> 1) |
                ((b & 0x55U) << 1));

        buffer[i] = b;
    }
}
/*
 * board_pins.h -- RF front-end GPIO pin mapping, selected at compile time
 * by the Makefile's BOARD variable (BOARD_NUCLEO_WL55JC / BOARD_GS_CUSTOM_PCB
 * / BOARD_SAT_CUSTOM_PCB). Consumed by target/radio_board_if.c.
 *
 * FE_CTRL1/2/3 (the SX126x RF switch itself) sit on the same physical pins
 * on every board variant confirmed so far -- ST's Nucleo reference mapping,
 * verified identical on the custom ground-station PCB via ../../gs_config.json's
 * pin table. The "extra" pins below are NOT the same across boards: on the
 * Nucleo bench setup they're a spare external PA/LNA enable pair; on the
 * custom GS PCB the same physical pin (PA8) is committed to the external
 * SX1278's DIO2 IRQ input, so driving it as a push-pull output there would
 * fight that chip. That mismatch is exactly why this needs to be a per-board
 * table instead of one hardcoded set of literals.
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-30
*/
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#include "stm32wlxx_hal.h"

/* ---- RF switch (identical across all board variants seen so far) ---- */
#define BOARD_FE_CTRL_GPIO_PORT   GPIOC
#define BOARD_FE_CTRL1_PIN        GPIO_PIN_4
#define BOARD_FE_CTRL2_PIN        GPIO_PIN_5
#define BOARD_FE_CTRL3_PIN        GPIO_PIN_3
#define BOARD_FE_CTRL_ALL_PINS    (BOARD_FE_CTRL1_PIN | BOARD_FE_CTRL2_PIN | BOARD_FE_CTRL3_PIN)

#if defined(BOARD_GS_CUSTOM_PCB)

/*
 * Custom ground-station PCB (../../gs_config.json). PA8 is wired to the
 * external SX1278's DIO2 (RxTimeout/FhssChangeChannel IRQ, an INPUT) and
 * PA1 is unconnected -- neither is a spare PA/LNA pair here. The external
 * RF power amplifier on this board is instead gated by two dedicated rail
 * enables: EN_AMP_3V3 (PA11) and EN_AMP_5V0 (PA12).
 */
#define BOARD_HAS_EXTRA_FRONTEND_PINS  1
#define BOARD_EXTRA_GPIO_PORT          GPIOA
#define BOARD_EXTRA_PIN_A              GPIO_PIN_11  /* EN_AMP_3V3 */
#define BOARD_EXTRA_PIN_B              GPIO_PIN_12  /* EN_AMP_5V0 */

#elif defined(BOARD_SAT_CUSTOM_PCB)

#error "BOARD_SAT_CUSTOM_PCB: fill in target/board_pins.h with the real \
satellite PCB pin mapping before building for this board. No satellite \
schematic export exists in this repo yet (see ../../gs_config.json for the \
format a future sat_config.json should follow) -- guessing pin numbers for \
real hardware here would be actively unsafe, not just wrong."

#else /* BOARD_NUCLEO_WL55JC (default) */

/*
 * Nucleo-WL55JC2 bench setup: external PA/LNA enable pair wired to PA8/PA1
 * for RF testing (not part of ST's stock Nucleo reference design).
 */
#define BOARD_HAS_EXTRA_FRONTEND_PINS  1
#define BOARD_EXTRA_GPIO_PORT          GPIOA
#define BOARD_EXTRA_PIN_A              GPIO_PIN_8   /* External PA enable */
#define BOARD_EXTRA_PIN_B              GPIO_PIN_1   /* External LNA enable */

#endif

#endif /* BOARD_PINS_H */

#ifndef OBC_LED_H
#define OBC_LED_H

/*
 * Nucleo-WL55JC LEDs (PB15 blue LD1, PB9 green LD2, PB11 red LD3, active high). Both cores drive
 * them on the image path: CPU1 shows blue = writing flash, green = reading flash; CPU2 shows
 * yellow (green+red) while it transmits over RF. The colour stays until OBC_LED_IDLE_MS passes.
 */

#include <stdint.h>

#define OBC_LED_BLUE      1U
#define OBC_LED_GREEN     2U
#define OBC_LED_RED       4U
#define OBC_LED_YELLOW    (OBC_LED_GREEN | OBC_LED_RED)
#define OBC_LED_IDLE_MS   1500U

void Obc_Led(uint8_t mask);      /* 0 = all off */
void Obc_Led_Tick(void);         /* switches the LEDs off after OBC_LED_IDLE_MS */

#endif

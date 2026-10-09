#ifndef LL_PORT_H
#define LL_PORT_H

/*
 * Small LL / direct-register replacements for the HAL core, GPIO, cortex and PWR modules
 * (HAL_Init, HAL_GPIO_Init, HAL_NVIC_*, HAL_SYSTICK_Config ...). See core/ll_port.c.
 */

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Replaces HAL_Init(): SystemCoreClock from the CPU2 clock tree + lowest SysTick priority. */
void LL_Init_CPU2(void);

/* Push-pull, no pull, very high speed outputs for every pin set in `pins` (GPIO_PIN_x mask). */
void LL_GPIO_OutputInit(GPIO_TypeDef *port, uint32_t pins);

#ifdef __cplusplus
}
#endif

#endif /* LL_PORT_H */

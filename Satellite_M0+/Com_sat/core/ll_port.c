/*
 * LL / direct-register replacements for the HAL modules this firmware used only in
 * trivial ways: stm32wlxx_hal.c (init + 1 ms tick counter), _hal_gpio.c (pin init,
 * read, write), _hal_cortex.c (NVIC / SysTick, now CMSIS NVIC_* at the call sites)
 * and _hal_pwr*.c (no longer referenced).
 */

#include "ll_port.h"

/* ---- 1 ms tick: SysTick_Handler() calls HAL_IncTick() ------------------------------ */
__IO uint32_t uwTick;
uint32_t uwTickPrio = (1UL << __NVIC_PRIO_BITS);   /* invalid until LL_Init_CPU2 */
HAL_TickFreqTypeDef uwTickFreq = HAL_TICK_FREQ_DEFAULT;

void HAL_IncTick(void)
{
    uwTick += (uint32_t)uwTickFreq;
}

uint32_t HAL_GetTick(void)
{
    return uwTick;
}

/*
 * What HAL_Init() amounted to on CPU2 with INSTRUCTION_CACHE_ENABLE=1, PREFETCH_ENABLE=0:
 * SystemCoreClock = HCLK2, SysTick at the lowest priority (the SysTick reload itself is
 * done afterwards by SysTick_Init_CPU2()).
 */
void LL_Init_CPU2(void)
{
    SystemCoreClock = HAL_RCC_GetHCLK2Freq();
    uwTickPrio = TICK_INT_PRIORITY;
    NVIC_SetPriority(SysTick_IRQn, TICK_INT_PRIORITY);
}

/* ---- GPIO --------------------------------------------------------------------------- */
void LL_GPIO_OutputInit(GPIO_TypeDef *port, uint32_t pins)
{
    for (uint32_t i = 0U; i < 16U; i++)
    {
        if ((pins & (1UL << i)) == 0U) continue;
        port->MODER   = (port->MODER   & ~(3UL << (2U * i))) | (1UL << (2U * i));   /* output     */
        port->OTYPER &= ~(1UL << i);                                                /* push-pull  */
        port->OSPEEDR |= (3UL << (2U * i));                                         /* very high  */
        port->PUPDR   &= ~(3UL << (2U * i));                                        /* no pull    */
    }
}

void HAL_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState)
{
    GPIOx->BSRR = (PinState != GPIO_PIN_RESET) ? (uint32_t)GPIO_Pin : ((uint32_t)GPIO_Pin << 16U);
}

GPIO_PinState HAL_GPIO_ReadPin(const GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
    return ((GPIOx->IDR & GPIO_Pin) != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

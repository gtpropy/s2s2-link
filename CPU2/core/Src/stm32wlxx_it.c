/**
 * stm32wlxx_it.c
 *
 * Minimal interrupt handler file for CPU2 (M0+).
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-21
*/

#include "stm32wlxx_hal.h"

volatile uint32_t g_system_tick_ms = 0;

void SysTick_Handler(void)
{
    g_system_tick_ms++;
    HAL_IncTick();
}

void SysTick_Init_CPU2(uint32_t sys_freq_hz)
{
    /* Configure SysTick to generate 1ms interrupt */
    SysTick->LOAD = (sys_freq_hz / 1000UL) - 1UL;
    SysTick->VAL  = 0UL;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |
                    SysTick_CTRL_TICKINT_Msk   |
                    SysTick_CTRL_ENABLE_Msk;
}

void CPU2_Delay_Ms(uint32_t ms)
{
    while (ms--)
    {
        for (volatile uint32_t i = 0; i < 6000; i++)
        {
            __NOP();
        }
    }
}
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
    /* Sleep-wait on the real 1ms SysTick count instead of spin-counting
     * NOPs. __WFI() halts the M0+ core (Sleep mode, not Stop/Standby --
     * no clock/peripheral reinit needed, SysTick keeps ticking) until
     * the next interrupt; SysTick_Handler fires every 1ms regardless,
     * so this wakes on schedule even with nothing else going on, and
     * wakes early (then just re-checks and goes back to sleep) on any
     * other interrupt -- e.g. SUBGHZ_Radio_IRQn, which is a real NVIC
     * interrupt on this build (see ipcc_m0plus_main.c's
     * HAL_NVIC_EnableIRQ(SUBGHZ_Radio_IRQn)), so a radio event during a
     * delay (CW keying gaps, inter-burst pacing) still lands the same
     * instant it would have with the old busy-wait. Unsigned
     * subtraction makes this correct across the eventual g_system_tick_ms
     * wraparound at ~49.7 days uptime. Power: this is the difference
     * between full active-mode current and Sleep-mode current for
     * every millisecond this firmware ever spends waiting -- on a
     * power-constrained CubeSat, that was the single biggest fixable
     * power cost in this file. */
    uint32_t start = g_system_tick_ms;
    while ((g_system_tick_ms - start) < ms)
    {
        __WFI();
    }
}
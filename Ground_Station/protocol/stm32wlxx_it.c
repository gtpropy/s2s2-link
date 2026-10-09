/**
 * stm32wlxx_it.c
 *
 * Minimal interrupt handler file for CPU2 (M0+).
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

__attribute__((weak)) void CPU2_Poll_IPC(void)
{
}

void CPU2_Delay_Ms(uint32_t ms)
{
    if ((SysTick->CTRL & SysTick_CTRL_ENABLE_Msk) == 0)
    {
        while (ms--)
        {
            for (volatile uint32_t i = 0; i < 6000; i++)
            {
                __NOP();
            }
        }
        return;
    }

    uint32_t start = g_system_tick_ms;
    while ((g_system_tick_ms - start) < ms)
    {
        CPU2_Poll_IPC();
        __NOP();
    }
}

extern SUBGHZ_HandleTypeDef hsubghz;

/**
 * @brief Sub-GHz Radio Interrupt Handler for Cortex-M0+ (CPU2)
 *        Dispatches TX_DONE, RX_DONE, CRC_ERROR, TIMEOUT to radio HAL driver.
 */
void SUBGHZ_Radio_IRQHandler(void)
{
    HAL_SUBGHZ_IRQHandler(&hsubghz);
}

void NMI_Handler(void)
{
    while (1)
    {
    }
}

void HardFault_Handler_C(uint32_t *stack_frame)
{
    (void)stack_frame;
    while (1)
    {
        __NOP();
    }
}

__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "movs r0, #4\n"
        "mov  r1, lr\n"
        "tst  r0, r1\n"
        "beq  1f\n"
        "mrs  r0, psp\n"
        "b    2f\n"
        "1:\n"
        "mrs  r0, msp\n"
        "2:\n"
        "b    HardFault_Handler_C\n"
    );
}

void SVC_Handler(void)
{
}

void PendSV_Handler(void)
{
}

void IPCC_C2_RX_IRQHandler(void)
{
    /* Clear and acknowledge any CPU2 IPCC channel flags */
    (*(volatile uint32_t *)0x58000C1CUL) = 0x003F003FUL;
}

__attribute__((weak)) void IPCC_C2_RX_C2_TX_IRQHandler(void)
{
    IPCC_C2_RX_IRQHandler();
}
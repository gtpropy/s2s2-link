/**
  ******************************************************************************
  * @file    stm32wlxx_it.c
  * @brief   Interrupt Service Routines for CPU2 (Cortex-M0+).
  ******************************************************************************
  */

#include "main.h"
#include "stm32wlxx_hal.h"
#include "stm32wlxx_it.h"

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

void NMI_Handler(void)
{
    while (1)
    {
    }
}

static void uart1_direct_puts(const char *s)
{
    while (*s)
    {
        while (!((*(volatile uint32_t *)0x4001381CUL) & (1UL << 7)));
        (*(volatile uint32_t *)0x40013828UL) = (uint8_t)(*s++);
    }
}

static void uart1_direct_hex32(uint32_t val)
{
    const char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
    {
        while (!((*(volatile uint32_t *)0x4001381CUL) & (1UL << 7)));
        (*(volatile uint32_t *)0x40013828UL) = hex[(val >> i) & 0x0F];
    }
}

void HardFault_Handler_C(uint32_t *stack_frame)
{
    (*(volatile uint32_t *)0x58000160UL) |= (1UL << 14); /* RCC_C2APB2ENR USART1 */
    (*(volatile uint32_t *)0x40013820UL) = 0xFFFFFFFFUL; /* USART1_ICR */
    (*(volatile uint32_t *)0x40013800UL) |= (1UL << 0) | (1UL << 3); /* UE | TE */

    uart1_direct_puts("\r\n\r\n[CPU2 HARDFAULT TRAPPED!]\r\n");
    if (stack_frame != NULL)
    {
        uart1_direct_puts("  PC  : 0x"); uart1_direct_hex32(stack_frame[6]); uart1_direct_puts("\r\n");
        uart1_direct_puts("  LR  : 0x"); uart1_direct_hex32(stack_frame[5]); uart1_direct_puts("\r\n");
        uart1_direct_puts("  PSR : 0x"); uart1_direct_hex32(stack_frame[7]); uart1_direct_puts("\r\n");
        uart1_direct_puts("  R0  : 0x"); uart1_direct_hex32(stack_frame[0]); uart1_direct_puts("\r\n");
        uart1_direct_puts("  R1  : 0x"); uart1_direct_hex32(stack_frame[1]); uart1_direct_puts("\r\n");
        uart1_direct_puts("  R2  : 0x"); uart1_direct_hex32(stack_frame[2]); uart1_direct_puts("\r\n");
        uart1_direct_puts("  R3  : 0x"); uart1_direct_hex32(stack_frame[3]); uart1_direct_puts("\r\n");
        uart1_direct_puts("  R12 : 0x"); uart1_direct_hex32(stack_frame[4]); uart1_direct_puts("\r\n");
    }
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

extern RTC_HandleTypeDef hrtc;
void RTC_LSECSS_IRQHandler(void)
{
    HAL_RTC_AlarmIRQHandler(&hrtc);
    HAL_RTCEx_SSRUIRQHandler(&hrtc);
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

extern SUBGHZ_HandleTypeDef hsubghz;
void SUBGHZ_Radio_IRQHandler(void)
{
    HAL_SUBGHZ_IRQHandler(&hsubghz);
}

/****************************************************************************
 * boards/arm/stm32wl5/nucleo-wl55jc/CPU2/startup_cm0p.s
 *
 * STM32WL55 Cortex-M0+ CPU2 startup
 ****************************************************************************/

.syntax unified
.cpu cortex-m0plus
.thumb


.global Reset_Handler
.global Default_Handler

.global IPCC_C2_RX_IRQHandler
.global SUBGHZ_Radio_IRQHandler



/* ==========================================================
   VECTOR TABLE
   Cortex-M0+ 
   IRQ numbering from stm32wl55xx.h
   ========================================================== */

.section .isr_vector,"a",%progbits
.type g_vectors,%object


g_vectors:


/* Core exceptions 0-15 */

.word _estack

.word Reset_Handler

.word NMI_Handler

.word HardFault_Handler

.word 0
.word 0
.word 0
.word 0
.word 0
.word 0
.word 0

.word SVC_Handler

.word 0
.word 0

.word PendSV_Handler

.word SysTick_Handler



/* ==========================================================
   External IRQ0-31
   ========================================================== */


/* IRQ0  TZIC */
.word Default_Handler

/* IRQ1  PVD_PVM */
.word Default_Handler


/* IRQ2 RTC */
.word Default_Handler


/* IRQ3 RCC_FLASH */
.word Default_Handler


/* IRQ4 EXTI1_0 */
.word Default_Handler


/* IRQ5 EXTI3_2 */
.word Default_Handler


/* IRQ6 EXTI15_4 */
.word Default_Handler


/* IRQ7 ADC_COMP_DAC */
.word Default_Handler


/* IRQ8 DMA1 CH1-3 */
.word Default_Handler


/* IRQ9 DMA1 CH4-7 */
.word Default_Handler


/* IRQ10 DMA2 DMAMUX */
.word Default_Handler


/* IRQ11 LPTIM1 */
.word Default_Handler


/* IRQ12 LPTIM2 */
.word Default_Handler


/* IRQ13 LPTIM3 */
.word Default_Handler


/* IRQ14 TIM1 */
.word Default_Handler


/* IRQ15 TIM2 */
.word Default_Handler


/* IRQ16 TIM16 */
.word Default_Handler


/* IRQ17 TIM17 */
.word Default_Handler


/* IRQ18 IPCC_C2_RX_C2_TX */
.word IPCC_C2_RX_IRQHandler


/* IRQ19 HSEM */
.word Default_Handler


/* IRQ20 RNG */
.word Default_Handler


/* IRQ21 AES PKA */
.word Default_Handler


/* IRQ22 I2C1 */
.word Default_Handler


/* IRQ23 I2C2 */
.word Default_Handler


/* IRQ24 I2C3 */
.word Default_Handler


/* IRQ25 SPI1 */
.word Default_Handler


/* IRQ26 SPI2 */
.word Default_Handler


/* IRQ27 USART1 */
.word Default_Handler


/* IRQ28 USART2 */
.word Default_Handler


/* IRQ29 LPUART1 */
.word Default_Handler


/* IRQ30 SUBGHZSPI */
.word Default_Handler


/* IRQ31 SUBGHZ RADIO  <<< IMPORTANT */
.word SUBGHZ_Radio_IRQHandler



.size g_vectors,.-g_vectors





/* ==========================================================
   RESET HANDLER
   ========================================================== */


.section .text.Reset_Handler,"ax",%progbits
.type Reset_Handler,%function


Reset_Handler:

/* Disable interrupts during early startup */
cpsid i

/* 1. Point Cortex-M0+ vector table to CPU2 flash at 0x08032000 */
ldr r0, =0xE000ED08
ldr r1, =0x08032000
str r1, [r0]
dsb
isb

/* Copy DATA */

ldr r0,=_sdata
ldr r1,=_edata
ldr r2,=_sidata

movs r3,#0


CopyDataLoop:


adds r4,r0,r3

cmp r4,r1

bcs CopyDataDone


ldr r4,[r2,r3]

str r4,[r0,r3]


adds r3,r3,#4

b CopyDataLoop



CopyDataDone:





/* Clear BSS */


ldr r0,=_sbss

ldr r1,=_ebss


movs r2,#0



ClearBssLoop:


cmp r0,r1

bcs ClearBssDone


str r2,[r0]

adds r0,r0,#4


b ClearBssLoop



ClearBssDone:



/* Call main */

bl main



Infinite:

b Infinite



.size Reset_Handler,.-Reset_Handler





/* ==========================================================
   DEFAULT HANDLER
   ========================================================== */


.section .text.Default_Handler,"ax",%progbits


Default_Handler:


b Default_Handler



.size Default_Handler,.-Default_Handler





/* ==========================================================
   WEAK INTERRUPTS
   ========================================================== */


.weak NMI_Handler
.thumb_set NMI_Handler,Default_Handler


.weak HardFault_Handler
.thumb_set HardFault_Handler,Default_Handler


.weak SVC_Handler
.thumb_set SVC_Handler,Default_Handler


.weak PendSV_Handler
.thumb_set PendSV_Handler,Default_Handler


.weak SysTick_Handler
.thumb_set SysTick_Handler,Default_Handler



.weak IPCC_C2_RX_IRQHandler
.thumb_set IPCC_C2_RX_IRQHandler,Default_Handler



.weak SUBGHZ_Radio_IRQHandler
.thumb_set SUBGHZ_Radio_IRQHandler,Default_Handler

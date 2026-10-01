/****************************************************************************
 * uart_debug.c
 *
 * STM32WL55 CPU2 / Cortex-M0+
 *
 * USART1 debug driver for STM32WL55:
 *   - PA9  (AF7) = USART1_TX (Morpho header CN10 pin 21 / Arduino SCL)
 *   - PA10 (AF7) = USART1_RX (Morpho header CN10 pin 33 / Arduino SDA)
 *
 * Configured as AF7, High-Speed Push-Pull with Pull-Up.
 *
 * Dual-core clock gating:
 *   Configures both CPU1 (M4) and CPU2 (M0+) clock enable and sleep-mode
 *   enable registers (RCC_APB2ENR, RCC_C2APB2ENR, RCC_APB2SMENR, RCC_C2APB2SMENR)
 *   to ensure USART1 operates continuously even when CPU1 is sleeping.
 *
 ****************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#include "stm32wlxx.h"
#include "stm32wlxx_hal.h"
#include "uart_debug.h"

/*
 * ==========================================================================
 * RCC REGISTERS (Dual-core map: CPU1 and CPU2)
 * Base: 0x58000000UL
 * ==========================================================================
 */

/* CPU1 Clock Control & Reset */
#define RCC_CR            (*(volatile uint32_t *)0x58000000UL)
#define RCC_AHB2ENR       (*(volatile uint32_t *)0x5800004CUL)
#define RCC_APB2ENR       (*(volatile uint32_t *)0x58000060UL)
#define RCC_AHB2SMENR     (*(volatile uint32_t *)0x5800006CUL)
#define RCC_APB2SMENR     (*(volatile uint32_t *)0x58000080UL)
#define RCC_CCIPR         (*(volatile uint32_t *)0x58000088UL)

/* CPU2 (Cortex-M0+) Clock & Sleep Enable */
#define RCC_C2AHB2ENR     (*(volatile uint32_t *)0x5800014CUL)
#define RCC_C2APB2ENR     (*(volatile uint32_t *)0x58000160UL)
#define RCC_C2AHB2SMENR   (*(volatile uint32_t *)0x5800016CUL)
#define RCC_C2APB2SMENR   (*(volatile uint32_t *)0x58000180UL)

/*
 * ==========================================================================
 * GPIOB REGISTERS (PB6 = USART1_TX Alternate)
 * GPIOB Base: 0x48000400UL
 * ==========================================================================
 */

#define GPIOB_MODER       (*(volatile uint32_t *)0x48000400UL)
#define GPIOB_OTYPER      (*(volatile uint32_t *)0x48000404UL)
#define GPIOB_OSPEEDR     (*(volatile uint32_t *)0x48000408UL)
#define GPIOB_PUPDR       (*(volatile uint32_t *)0x4800040CUL)
#define GPIOB_AFRL        (*(volatile uint32_t *)0x48000420UL)

/*
 * ==========================================================================
 * GPIOA REGISTERS
 * GPIOA Base: 0x48000000UL
 * ==========================================================================
 */

#define GPIOA_MODER       (*(volatile uint32_t *)0x48000000UL)
#define GPIOA_OTYPER      (*(volatile uint32_t *)0x48000004UL)
#define GPIOA_OSPEEDR     (*(volatile uint32_t *)0x48000008UL)
#define GPIOA_PUPDR       (*(volatile uint32_t *)0x4800000CUL)
#define GPIOA_AFRH        (*(volatile uint32_t *)0x48000024UL)

/*
 * ==========================================================================
 * USART1 REGISTERS
 * Base: 0x40013800UL (APB2 Peripheral)
 * ==========================================================================
 */

#ifndef USART1_BASE
#define USART1_BASE       0x40013800UL
#endif

#define USART1_CR1        (*(volatile uint32_t *)(USART1_BASE + 0x00UL))
#define USART1_CR2        (*(volatile uint32_t *)(USART1_BASE + 0x04UL))
#define USART1_CR3        (*(volatile uint32_t *)(USART1_BASE + 0x08UL))
#define USART1_BRR        (*(volatile uint32_t *)(USART1_BASE + 0x0CUL))
#define USART1_ISR        (*(volatile uint32_t *)(USART1_BASE + 0x1CUL))
#define USART1_ICR        (*(volatile uint32_t *)(USART1_BASE + 0x20UL))
#define USART1_RDR        (*(volatile uint32_t *)(USART1_BASE + 0x24UL))
#define USART1_TDR        (*(volatile uint32_t *)(USART1_BASE + 0x28UL))

/* Control / Status Bits */
#ifndef USART_CR1_UE
#define USART_CR1_UE      (1UL << 0)
#endif
#ifndef USART_CR1_RE
#define USART_CR1_RE      (1UL << 2)
#endif
#ifndef USART_CR1_TE
#define USART_CR1_TE      (1UL << 3)
#endif
#ifndef USART_ISR_TC
#define USART_ISR_TC      (1UL << 6)
#endif
#ifndef USART_ISR_RXNE
#define USART_ISR_RXNE    (1UL << 5)
#endif
#ifndef USART_ISR_TXE
#define USART_ISR_TXE     (1UL << 7)
#endif
#ifndef USART_ISR_TEACK
#define USART_ISR_TEACK   (1UL << 21)
#endif

/*
 * Baud rate calculation:
 * SYSCLK = 48 MHz (selected via CCIPR USART1SEL = 01b)
 * Desired baud = 115200 (16x oversampling)
 * BRR = 48,000,000 / 115,200 ≈ 417 (0x01A1)
 */
#define USART1_BRR_48MHZ_115200   417UL

/*
 * ==========================================================================
 * LPUART1 REGISTERS (PA2/PA3 ST-Link VCP)
 * Base: 0x40008000UL (APB1 Peripheral)
 * ==========================================================================
 */

#ifndef LPUART1_BASE
#define LPUART1_BASE      0x40008000UL
#endif

#define LPUART1_CR1       (*(volatile uint32_t *)(LPUART1_BASE + 0x00UL))
#define LPUART1_BRR       (*(volatile uint32_t *)(LPUART1_BASE + 0x0CUL))
#define LPUART1_ISR       (*(volatile uint32_t *)(LPUART1_BASE + 0x1CUL))
#define LPUART1_ICR       (*(volatile uint32_t *)(LPUART1_BASE + 0x20UL))
#define LPUART1_TDR       (*(volatile uint32_t *)(LPUART1_BASE + 0x28UL))

#define LPUART1_BRR_48MHZ_115200  106667UL

/* CPU1 & CPU2 APB1ENR2 / APB1SMENR2 registers for LPUART1 */
#define RCC_APB1ENR2      (*(volatile uint32_t *)0x5800005CUL)
#define RCC_C2APB1ENR2    (*(volatile uint32_t *)0x5800015CUL)
#define RCC_APB1SMENR2    (*(volatile uint32_t *)0x5800007CUL)
#define RCC_C2APB1SMENR2  (*(volatile uint32_t *)0x5800017CUL)
#define GPIOA_AFRL        (*(volatile uint32_t *)0x48000020UL)

/*
 * ==========================================================================
 * uart1_init
 * ==========================================================================
 */

void uart1_init(void)
{
  /*
   * 1. Enable GPIOA and GPIOB clocks for BOTH CPU1 and CPU2 (Active & Sleep)
   *    Bit 0 = GPIOA, Bit 1 = GPIOB in AHB2
   */
  RCC_AHB2ENR     |= (1UL << 0) | (1UL << 1);
  RCC_C2AHB2ENR   |= (1UL << 0) | (1UL << 1);
  RCC_AHB2SMENR   |= (1UL << 0) | (1UL << 1);
  RCC_C2AHB2SMENR |= (1UL << 0) | (1UL << 1);

  /*
   * 2. Configure GPIOA pins:
   *    - PA9  = USART1_TX (AF7)
   *    - PA10 = USART1_RX (AF7)
   *    - PA2  = LPUART1_TX (AF8)
   *    - PA3  = LPUART1_RX (AF8)
   */

  /* MODER: Alternate Function (10b) for PA2, PA3, PA9, PA10 */
  GPIOA_MODER &= ~((3UL << 4) | (3UL << 6) | (3UL << 18) | (3UL << 20));
  GPIOA_MODER |=  ((2UL << 4) | (2UL << 6) | (2UL << 18) | (2UL << 20));

  /* OTYPER: Clear bits 2, 3, 9, 10 to ensure Push-Pull */
  GPIOA_OTYPER &= ~((1UL << 2) | (1UL << 3) | (1UL << 9) | (1UL << 10));

  /* OSPEEDR: Very High Speed for all */
  GPIOA_OSPEEDR |= ((3UL << 4) | (3UL << 6) | (3UL << 18) | (3UL << 20));

  /* PUPDR: Pull-up on TX and RX lines */
  GPIOA_PUPDR &= ~((3UL << 4) | (3UL << 6) | (3UL << 18) | (3UL << 20));
  GPIOA_PUPDR |=  ((1UL << 4) | (1UL << 6) | (1UL << 18) | (1UL << 20));

  /* AFRL: PA2 (AF8 = LPUART1_TX, bits 11:8), PA3 (AF8 = LPUART1_RX, bits 15:12) */
  GPIOA_AFRL &= ~((0xFUL << 8) | (0xFUL << 12));
  GPIOA_AFRL |=  ((8UL << 8)   | (8UL << 12));

  /* AFRH: PA9 (AF7 = USART1_TX, bits 7:4), PA10 (AF7 = USART1_RX, bits 11:8) */
  GPIOA_AFRH &= ~((0xFUL << 4) | (0xFUL << 8));
  GPIOA_AFRH |=  ((7UL << 4)   | (7UL << 8));

  /*
   * 3. Also configure PB6 as USART1_TX (AF7)
   */
  GPIOB_MODER &= ~(3UL << 12);
  GPIOB_MODER |=  (2UL << 12);
  GPIOB_OTYPER &= ~(1UL << 6);
  GPIOB_OSPEEDR |= (3UL << 12);
  GPIOB_PUPDR &= ~(3UL << 12);
  GPIOB_PUPDR |=  (1UL << 12);
  GPIOB_AFRL &= ~(0xFUL << 24);
  GPIOB_AFRL |=  (7UL << 24);

  /*
   * 4. Enable USART1 (APB2 bit 14) and LPUART1 (APB1 bit 0) clocks
   */
  RCC_APB2ENR     |= (1UL << 14);
  RCC_C2APB2ENR   |= (1UL << 14);
  RCC_APB2SMENR   |= (1UL << 14);
  RCC_C2APB2SMENR |= (1UL << 14);

  RCC_APB1ENR2     |= (1UL << 0);
  RCC_C2APB1ENR2   |= (1UL << 0);
  RCC_APB1SMENR2   |= (1UL << 0);
  RCC_C2APB1SMENR2 |= (1UL << 0);

  /*
   * 5. Initialize USART1 (PA9 / PB6 TX, PA10 RX)
   *    If CPU1 (NuttX) has already initialized USART1, do not smash its configuration.
   *    If not yet initialized, configure for 48 MHz PCLK2 at 115200 baud (BRR = 417).
   */
  if ((USART1_CR1 & USART_CR1_UE) == 0)
    {
      USART1_CR1 = 0;
      USART1_CR2 = 0;
      USART1_CR3 = 0;
      USART1_BRR = 417UL; /* 48,000,000 / 115,200 ≈ 417 */
      USART1_ICR = 0xFFFFFFFFUL;
      USART1_CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
    }
  else
    {
      /* Ensure transmitter and receiver remain active and clear any errors */
      USART1_ICR = 0xFFFFFFFFUL;
      USART1_CR1 |= (USART_CR1_TE | USART_CR1_UE);
    }

  /*
   * 6. Initialize LPUART1 (PA2/PA3)
   *    If CPU1 (NuttX) has already initialized LPUART1, do not smash its configuration.
   *    If not yet initialized, configure for 48 MHz SYSCLK at 115200 baud (BRR = 106667).
   */
  if ((LPUART1_CR1 & USART_CR1_UE) == 0)
    {
      LPUART1_CR1 = 0;
      LPUART1_BRR = 106667UL; /* (256 * 48,000,000) / 115,200 */
      LPUART1_ICR = 0xFFFFFFFFUL;
      LPUART1_CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
    }
  else
    {
      LPUART1_ICR = 0xFFFFFFFFUL;
      LPUART1_CR1 |= (USART_CR1_TE | USART_CR1_UE);
    }
}

/*
 * ==========================================================================
 * uart1_putc - Emits to BOTH USART1 (PA9) and LPUART1 (PA2) safely
 * ==========================================================================
 */

void uart1_putc(char c)
{
  /* Ensure CPU2 clock gates remain active */
  RCC_C2APB2ENR  |= (1UL << 14);
  RCC_C2APB1ENR2 |= (1UL << 0);

  /* Ensure USART1 is enabled */
  if ((USART1_CR1 & (USART_CR1_UE | USART_CR1_TE)) != (USART_CR1_UE | USART_CR1_TE))
    {
      USART1_CR1 |= (USART_CR1_UE | USART_CR1_TE);
    }

  /* Clear any error flags */
  if (USART1_ISR & 0x1FUL) { USART1_ICR = 0x1FUL; }
  if (LPUART1_ISR & 0x1FUL) { LPUART1_ICR = 0x1FUL; }

  /* Wait for TXE on USART1 with safe non-hanging timeout */
  uint32_t timeout_u1 = 50000UL;
  while (((USART1_ISR & USART_ISR_TXE) == 0) && (timeout_u1 > 0))
    {
      timeout_u1--;
    }
  if (timeout_u1 > 0)
    {
      USART1_TDR = (uint32_t)(uint8_t)c;
    }

  /* Dual-emit to LPUART1 (PA2 / Arduino D1) if transmitter is ready */
  if (LPUART1_CR1 & USART_CR1_TE)
    {
      uint32_t timeout_lpu = 50000UL;
      while (((LPUART1_ISR & USART_ISR_TXE) == 0) && (timeout_lpu > 0))
        {
          timeout_lpu--;
        }
      if (timeout_lpu > 0)
        {
          LPUART1_TDR = (uint32_t)(uint8_t)c;
        }
    }
}

/*
 * ==========================================================================
 * uart1_puts
 * ==========================================================================
 */

void uart1_puts(const char *s)
{
  if (s == NULL)
    {
      return;
    }

  while (*s != '\0')
    {
      if (*s == '\n')
        {
          uart1_putc('\r');
        }
      uart1_putc(*s);
      s++;
    }
}

/*
 * ==========================================================================
 * uart1_printf
 * ==========================================================================
 */

void uart1_printf(const char *fmt, ...)
{
  char buf[360];
  va_list args;

  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  uart1_puts(buf);
}

/*
 * ==========================================================================
 * uart1_try_getc
 * ==========================================================================
 */

int uart1_try_getc(uint8_t *out_byte)
{
  if (out_byte == NULL)
    {
      return 0;
    }

  if (USART1_ISR & USART_ISR_RXNE)
    {
      *out_byte = (uint8_t)(USART1_RDR & 0xFFUL);
      return 1;
    }

  return 0;
}

/*
 * ==========================================================================
 * Backward-Compatibility Wrappers (uart2_* -> uart1_*)
 * ==========================================================================
 */

void uart2_init(void)
{
  uart1_init();
}

void uart2_putc(char c)
{
  uart1_putc(c);
}

void uart2_puts(const char *s)
{
  uart1_puts(s);
}

void uart2_printf(const char *fmt, ...)
{
  char buf[360];
  va_list args;

  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  uart1_puts(buf);
}

int uart2_try_getc(uint8_t *out_byte)
{
  return uart1_try_getc(out_byte);
}
/****************************************************************************
 * uart_debug.c
 *
 * STM32WL55 CPU2 / Cortex-M0+
 *
 * LPUART1 debug driver for NUCLEO-WL55JC
 *
 * PA2 = LPUART1_TX
 * PA3 = LPUART1_RX
 *
 ****************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#include "uart_debug.h"


/*
 * ==========================================================================
 * RCC REGISTERS
 * ==========================================================================
 */

/* AHB2 peripheral clock enable */
#define RCC_AHB2ENR \
  (*(volatile uint32_t *)0x5800004CUL)

/* APB1 peripheral clock enable 2 */
#define RCC_APB1ENR2 \
  (*(volatile uint32_t *)0x5800005CUL)

/* APB1 peripheral reset 2 */
#define RCC_APB1RSTR2 \
  (*(volatile uint32_t *)0x5800003CUL)


/*
 * ==========================================================================
 * GPIOA REGISTERS
 * ==========================================================================
 */

#define GPIOA_MODER \
  (*(volatile uint32_t *)0x48000000UL)

#define GPIOA_OSPEEDR \
  (*(volatile uint32_t *)0x48000008UL)

#define GPIOA_PUPDR \
  (*(volatile uint32_t *)0x4800000CUL)

#define GPIOA_AFRL \
  (*(volatile uint32_t *)0x48000020UL)


/*
 * ==========================================================================
 * LPUART1 REGISTERS
 * ==========================================================================
 */

#define LPUART1_BASE    0x40008000UL

#define LPUART1_CR1 \
  (*(volatile uint32_t *)(LPUART1_BASE + 0x00UL))

#define LPUART1_BRR \
  (*(volatile uint32_t *)(LPUART1_BASE + 0x0CUL))

#define LPUART1_ISR \
  (*(volatile uint32_t *)(LPUART1_BASE + 0x1CUL))

#define LPUART1_RDR \
  (*(volatile uint32_t *)(LPUART1_BASE + 0x24UL))

#define LPUART1_TDR \
  (*(volatile uint32_t *)(LPUART1_BASE + 0x28UL))


/*
 * ==========================================================================
 * USART/LPUART CONTROL BITS
 * ==========================================================================
 */

#define USART_CR1_UE    (1UL << 0)
#define USART_CR1_RE    (1UL << 2)
#define USART_CR1_TE    (1UL << 3)

#define USART_ISR_RXNE  (1UL << 5)
#define USART_ISR_TXE   (1UL << 7)


/*
 * ==========================================================================
 * CLOCK / BAUD CONFIGURATION
 * ==========================================================================
 *
 * Assumption:
 *
 *   LPUART1 kernel clock = 48 MHz
 *   Baud rate            = 115200
 *
 * LPUART BRR:
 *
 *   BRR = 256 * fck / baud
 *
 *   = 256 * 48,000,000 / 115,200
 *   ≈ 106667
 *
 * ==========================================================================
 */

#define LPUART1_BRR_48MHZ_115200  106667UL


/*
 * ==========================================================================
 * uart2_init
 * ==========================================================================
 *
 * Despite the function name uart2_init(), this initializes LPUART1.
 *
 * ==========================================================================
 */

void uart2_init(void)
{
  /*
   * ----------------------------------------------------------
   * 1. Enable GPIOA clock
   * ----------------------------------------------------------
   */

  RCC_AHB2ENR |= (1UL << 0);


  /*
   * ----------------------------------------------------------
   * 2. Enable LPUART1 clock
   * ----------------------------------------------------------
   */

  RCC_APB1ENR2 |= (1UL << 0);


  /*
   * ----------------------------------------------------------
   * 3. Reset LPUART1
   * ----------------------------------------------------------
   */

  RCC_APB1RSTR2 |= (1UL << 0);

  RCC_APB1RSTR2 &= ~(1UL << 0);


  /*
   * ----------------------------------------------------------
   * 4. Configure PA2 and PA3 as alternate function
   * ----------------------------------------------------------
   *
   * PA2 = LPUART1_TX
   * PA3 = LPUART1_RX
   *
   * MODER:
   *
   *   10 = Alternate Function
   */

  GPIOA_MODER &= ~((3UL << 4) | (3UL << 6));

  GPIOA_MODER |= ((2UL << 4) | (2UL << 6));


  /*
   * ----------------------------------------------------------
   * 5. Select Alternate Function 8
   * ----------------------------------------------------------
   *
   * PA2 AF8 = LPUART1_TX
   * PA3 AF8 = LPUART1_RX
   */

  GPIOA_AFRL &= ~((0xFUL << 8) | (0xFUL << 12));

  GPIOA_AFRL |= ((8UL << 8) | (8UL << 12));


  /*
   * ----------------------------------------------------------
   * 6. GPIO speed
   * ----------------------------------------------------------
   */

  GPIOA_OSPEEDR |= ((3UL << 4) | (3UL << 6));


  /*
   * ----------------------------------------------------------
   * 7. GPIO pull configuration
   * ----------------------------------------------------------
   *
   * RX should be pulled high when idle.
   */

  GPIOA_PUPDR &= ~((3UL << 4) | (3UL << 6));

  GPIOA_PUPDR |= (1UL << 6);


  /*
   * ----------------------------------------------------------
   * 8. Configure LPUART baud rate
   * ----------------------------------------------------------
   */

  LPUART1_BRR = LPUART1_BRR_48MHZ_115200;


  /*
   * ----------------------------------------------------------
   * 9. Enable transmitter and receiver
   * ----------------------------------------------------------
   */

  LPUART1_CR1 = USART_CR1_TE | USART_CR1_RE;


  /*
   * ----------------------------------------------------------
   * 10. Enable LPUART1
   * ----------------------------------------------------------
   */

  LPUART1_CR1 |= USART_CR1_UE;
}


/*
 * ==========================================================================
 * uart2_putc
 * ==========================================================================
 */

void uart2_putc(char c)
{
  /*
   * Wait until transmit data register is empty.
   */

  while ((LPUART1_ISR & USART_ISR_TXE) == 0)
    {
    }

  LPUART1_TDR = (uint32_t)c;
}


/*
 * ==========================================================================
 * uart2_puts
 * ==========================================================================
 */

void uart2_puts(const char *s)
{
  if (s == NULL)
    {
      return;
    }

  while (*s != '\0')
    {
      /*
       * Convert LF to CR/LF.
       */

      if (*s == '\n')
        {
          uart2_putc('\r');
        }

      uart2_putc(*s);

      s++;
    }
}


/*
 * ==========================================================================
 * uart2_printf
 * ==========================================================================
 */

void uart2_printf(const char *fmt, ...)
{
  char buf[360];

  va_list args;

  va_start(args, fmt);

  vsnprintf(buf, sizeof(buf), fmt, args);

  va_end(args);

  uart2_puts(buf);
}


/*
 * ==========================================================================
 * uart2_try_getc
 * ==========================================================================
 *
 * Non-blocking receive.
 *
 * ==========================================================================
 */

int uart2_try_getc(uint8_t *out_byte)
{
  if (out_byte == NULL)
    {
      return 0;
    }

  if (LPUART1_ISR & USART_ISR_RXNE)
    {
      *out_byte = (uint8_t)(LPUART1_RDR & 0xFFUL);

      return 1;
    }

  return 0;
}
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
 ***************************************************************************
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-21
*/

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

#define LPUART1_ICR \
  (*(volatile uint32_t *)(LPUART1_BASE + 0x20UL))

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
#define USART_ISR_ORE   (1UL << 3)
#define USART_ICR_ORECF (1UL << 3)
#define USART_CR1_RXNEIE (1UL << 5)


/*
 * ==========================================================================
 * RX RING BUFFER (interrupt-driven)
 * ==========================================================================
 *
 * Real bug found and fixed 2026-09-28: uart2_try_getc() used to poll
 * LPUART1_ISR/RDR directly, once per main-loop iteration. This LPUART
 * has a single-byte RDR with no hardware FIFO, so any byte that arrived
 * while the main loop was busy elsewhere -- most notably during a CW
 * Morse beacon TX, which blocks for the whole ~15s message duration
 * (cw_beacon_send() has no background/async variant) -- got silently
 * overrun and lost, with no error indication. A UART command sent
 * during or shortly before a CW beacon was transmitting would never be
 * received at all. This ring buffer, filled from the real LPUART1 RX
 * interrupt instead of polling, decouples reception from however busy
 * the main loop happens to be: the ISR drains RDR the instant each byte
 * arrives, regardless of what else is running. 128 bytes is generous
 * for a UART running well under CMD_LINE_MAX-length interactive lines.
 */
#define UART2_RX_RING_SIZE 128U  /* power of 2 -- cheap wraparound via & */

static volatile uint8_t  s_rx_ring[UART2_RX_RING_SIZE];
static volatile uint16_t s_rx_head = 0;  /* next slot the ISR will fill */
static volatile uint16_t s_rx_tail = 0;  /* next slot uart2_try_getc() will read */


/*
 * ==========================================================================
 * CLOCK / BAUD CONFIGURATION
 * ==========================================================================
 *
 * Assumption: LPUART1 kernel clock = 48 MHz.
 *
 * LPUART BRR = 256 * fck / baud
 *
 *   115200: 256 * 48,000,000 / 115,200   ≈ 106667  (default -- matches the
 *           architecture README's documented serial monitor setting)
 *     9600: 256 * 48,000,000 /   9,600   = 1280000  (define UART_DEBUG_BAUD_9600
 *           at build time, e.g. `make CFLAGS+=-DUART_DEBUG_BAUD_9600`, to use
 *           this instead -- the host GUI's baud selection must match whichever
 *           one is actually flashed)
 *
 * ==========================================================================
 */

#define LPUART1_BRR_48MHZ_115200  106667UL
#define LPUART1_BRR_48MHZ_9600    1280000UL

#ifdef UART_DEBUG_BAUD_9600
#define LPUART1_BRR_VALUE  LPUART1_BRR_48MHZ_9600
#else
#define LPUART1_BRR_VALUE  LPUART1_BRR_48MHZ_115200
#endif


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

  LPUART1_BRR = LPUART1_BRR_VALUE;


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

  /*
   * ----------------------------------------------------------
   * 11. Enable RX-not-empty interrupt -- see the RX ring buffer
   *     block above for why this replaced pure polling.
   * ----------------------------------------------------------
   */

  LPUART1_CR1 |= USART_CR1_RXNEIE;
}


/*
 * ==========================================================================
 * LPUART1_IRQHandler
 * ==========================================================================
 *
 * Real ISR, wired into the vector table in startup_cm0p.s (IRQ29 used to
 * hardcode Default_Handler -- see that file's comment). Runs the
 * instant a byte arrives, regardless of what the main loop is doing;
 * this is the actual fix, not just the ring buffer -- a ring buffer
 * fed by polling would have the exact same loss problem the old code
 * had.
 */
void LPUART1_IRQHandler(void)
{
  if (LPUART1_ISR & USART_ISR_ORE)
    {
      LPUART1_ICR = USART_ICR_ORECF;
    }

  if (LPUART1_ISR & USART_ISR_RXNE)
    {
      uint8_t c = (uint8_t)(LPUART1_RDR & 0xFFUL);
      uint16_t next_head = (uint16_t)((s_rx_head + 1U) & (UART2_RX_RING_SIZE - 1U));

      /* Drop the byte on a full ring rather than overwrite unread data
         -- a dropped byte breaks the current line same as before (no
         worse than the old behavior), but never corrupts a DIFFERENT,
         already-buffered line by clobbering it. */
      if (next_head != s_rx_tail)
        {
          s_rx_ring[s_rx_head] = c;
          s_rx_head = next_head;
        }
    }
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

  /* Pops from the interrupt-filled ring buffer (see the block above
   * uart2_init()/LPUART1_IRQHandler) instead of polling hardware
   * directly -- see that comment for the real bug this replaced (bytes
   * silently lost whenever the main loop was busy elsewhere, e.g. for
   * the ~15s of a blocking CW beacon TX). s_rx_tail is only ever
   * written here (single reader, the main loop), s_rx_head only in the
   * ISR (single writer) -- no lock needed for this producer/consumer
   * pattern on a single core.
   */
  if (s_rx_tail == s_rx_head)
    {
      return 0;  /* ring empty */
    }

  *out_byte = s_rx_ring[s_rx_tail];
  s_rx_tail = (uint16_t)((s_rx_tail + 1U) & (UART2_RX_RING_SIZE - 1U));
  return 1;
}
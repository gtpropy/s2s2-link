/****************************************************************************
 * uart_debug.h
 *
 * M0+ USART1 debug interface
 *
 * STM32WL55 (Dual-Core):
 *   USART1 on APB2:
 *     TX: PA9  (Morpho CN10 pin 21 / Arduino SCL) -> AF7 (Push-Pull)
 *     RX: PA10 (Morpho CN10 pin 33 / Arduino SDA) -> AF7 (Pull-Up)
 *   Baud rate: 115200 8N1 (SYSCLK @ 48 MHz)
 *
 ****************************************************************************/

#ifndef UART_DEBUG_H
#define UART_DEBUG_H

#include <stdint.h>

/*
 * Initialize USART1 (115200 baud, 8N1) on PA9 (TX) and PA10 (RX).
 * Enables clock gates for both CPU1 and CPU2 (active + sleep).
 */
void uart1_init(void);

/*
 * Send one character over USART1.
 */
void uart1_putc(char c);

/*
 * Send a string over USART1 (converts '\n' to "\r\n").
 */
void uart1_puts(const char *s);

/*
 * printf-style formatted output over USART1.
 */
void uart1_printf(const char *fmt, ...);

/*
 * Non-blocking receive from USART1.
 *
 * Returns:
 *   1 = byte received
 *   0 = no byte available
 */
int uart1_try_getc(uint8_t *out_byte);

/*
 * Backward-compatibility wrappers:
 * Forward legacy uart2_* calls directly to USART1 so all modules
 * output to USART1 without breaking existing builds.
 */
void uart2_init(void);
void uart2_putc(char c);
void uart2_puts(const char *s);
void uart2_printf(const char *fmt, ...);
int uart2_try_getc(uint8_t *out_byte);

#endif /* UART_DEBUG_H */
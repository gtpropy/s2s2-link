/****************************************************************************
 * uart_debug.h
 *
 * M0+ UART debug interface
 *
 * NUCLEO-WL55JC:
 *
 *   PA2 -> LPUART1_TX
 *   PA3 -> LPUART1_RX
 *
 * The ST-Link Virtual COM Port is connected to LPUART1.
 *
 * Function names are kept as uart2_* for compatibility with
 * the existing M0+ application.
 *
 ****************************************************************************/

#ifndef UART_DEBUG_H
#define UART_DEBUG_H

#include <stdint.h>

/*
 * Initialize LPUART1.
 */
void uart2_init(void);

/*
 * Send one character.
 */
void uart2_putc(char c);

/*
 * Send a string.
 */
void uart2_puts(const char *s);

/*
 * printf-style UART output.
 */
void uart2_printf(const char *fmt, ...);

/*
 * Non-blocking receive.
 *
 * Returns:
 *
 *   1 = byte received
 *   0 = no byte available
 */
int uart2_try_getc(uint8_t *out_byte);

#endif /* UART_DEBUG_H */
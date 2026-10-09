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
/*
 * LOG=1 (make default) / LOG=0 build switch for the satellite console log.
 *
 * LOG=0 defines SAT_LOG_QUIET (Makefile): uart1_puts() / uart1_printf() become ((void)0), so the
 * ~14-15 KB of log strings and the time spent printing them (a 200 B hex dump at 115200 baud is
 * ~50 ms) leave the image. UART1 command input (uart1_try_getc) still works.
 *
 * !! LOG=0 IS NOT USABLE YET (open bug): with it the satellite's downlink frames arrive corrupted
 * !! (extra bytes inside the frame). Bisected to the prints in satellite_app.c, cause unknown.
 * !! Leading suspects, none confirmed: (1) the log calls were acting as timing delays between
 * !! building a frame and keying the PA; (2) the macro does not evaluate its arguments, so any call
 * !! with side effects inside a print is dropped (one such case, Seal_SelfTest(), is fixed).
 * !! Keep LOG=1 for anything that goes on air.
 */
#if defined(SAT_LOG_QUIET) && !defined(UART_DEBUG_IMPL)
#define uart1_puts(s)       ((void)0)
#define uart1_printf(...)   ((void)0)
#else
void uart1_puts(const char *s);
#endif

/*
 * printf-style formatted output over USART1.
 */
#if !(defined(SAT_LOG_QUIET) && !defined(UART_DEBUG_IMPL))
void uart1_printf(const char *fmt, ...);
#endif

/*
 * Non-blocking receive from USART1.
 *
 * Returns:
 *   1 = byte received
 *   0 = no byte available
 */
int uart1_try_getc(uint8_t *out_byte);

void uart1_rx_pump(void);

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
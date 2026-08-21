#ifndef UART_F_H
#define UART_F_H

#include "main.h"
#include <stdint.h>
#include <stdarg.h>

/* ============================================================
 * UART1 (Serial)
 * ============================================================ */

/* ---------- Initialization ---------- */
void Serial_begin(uint32_t baud);

/* ---------- Transmit functions ---------- */
void Serial_write(uint8_t data);                      // single byte
void Serial_writeBuffer(uint8_t *data, uint16_t len); // multiple bytes
void Serial_print(const char *fmt, ...);              // formatted text
void Serial_println(const char *fmt, ...);            // formatted text + newline

/* ---------- Receive functions ---------- */
int Serial_available(void);  // bytes available
int Serial_read(void);       // read one byte
void Serial_flush(void);
void Serial_printFloat(const char *label, float value, uint8_t decimals);
void Serial_printFloatAuto(const char *label, float value);

/* ============================================================
 * UART2 (Serial2)
 * ============================================================ */

/* ---------- Initialization ---------- */
void Serial2_begin(uint32_t baud);

/* ---------- Transmit functions ---------- */
void Serial2_write(uint8_t data);                      // single byte
void Serial2_writeBuffer(uint8_t *data, uint16_t len); // multiple bytes
void Serial2_print(const char *fmt, ...);              // formatted text
void Serial2_println(const char *fmt, ...);            // formatted text + newline

/* ---------- Receive functions ---------- */
int Serial2_available(void);  // bytes available
int Serial2_read(void);       // read one byte
void Serial2_flush(void);

#endif /* SERIAL_UTILS_H */

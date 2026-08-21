//#include <string.h>
//#include <stdio.h>
//#include <stdarg.h>
//#include <UART_F.h>
//
///* ---------- UART handles ---------- */
//extern UART_HandleTypeDef huart1;
//extern UART_HandleTypeDef huart2;
//
///* ============================================================
// * UART1 (Serial)
// * ============================================================ */
//
///* ---------- Internal helper ---------- */
//static void uart1_send(const char *s)
//{
//  HAL_UART_Transmit(&huart1, (uint8_t*)s, strlen(s), HAL_MAX_DELAY);
//}
//
///* ---------- Print / println ---------- */
//void Serial_print(const char *fmt, ...)
//{
//    char buf[128];
//    va_list args;
//    va_start(args, fmt);
//    vsnprintf(buf, sizeof(buf), fmt, args);
//    va_end(args);
//    uart1_send(buf);
//}
//
//void Serial_println(const char *fmt, ...)
//{
//    char buf[128];
//    va_list args;
//    va_start(args, fmt);
//    vsnprintf(buf, sizeof(buf), fmt, args);
//    va_end(args);
//    uart1_send(buf);
//    uart1_send("\r\n");
//}
//
///* ---------- Write bytes ---------- */
//void Serial_write(uint8_t data)
//{
//    HAL_UART_Transmit(&huart1, &data, 1, HAL_MAX_DELAY);
//}
//
//void Serial_writeBuffer(uint8_t *data, uint16_t len)
//{
//    HAL_UART_Transmit(&huart1, data, len, HAL_MAX_DELAY);
//}
//
//
//
///* ---------- Ring buffer for UART1 RX ---------- */
//#define RX1_BUF_SIZE 64
//static uint8_t rx1_buffer[RX1_BUF_SIZE];
//static volatile uint16_t rx1_head = 0;
//static volatile uint16_t rx1_tail = 0;
//static uint8_t rx1_byte;
//
///* ============================================================
// * UART2 (Serial2)
// * ============================================================ */
//
///* ---------- Internal helper ---------- */
//static void uart2_send(const char *s)
//{
//    HAL_UART_Transmit(&huart2, (uint8_t*)s, strlen(s), HAL_MAX_DELAY);
//}
//
///* ---------- Print / println ---------- */
//void Serial2_print(const char *fmt, ...)
//{
//    char buf[128];
//    va_list args;
//    va_start(args, fmt);
//    vsnprintf(buf, sizeof(buf), fmt, args);
//    va_end(args);
//    uart2_send(buf);
//}
//
//void Serial2_println(const char *fmt, ...)
//{
//    char buf[128];
//    va_list args;
//    va_start(args, fmt);
//    vsnprintf(buf, sizeof(buf), fmt, args);
//    va_end(args);
//    uart2_send(buf);
//    uart2_send("\r\n");
//}
//
///* ---------- Write bytes ---------- */
//void Serial2_write(uint8_t data)
//{
//    HAL_UART_Transmit(&huart2, &data, 1, HAL_MAX_DELAY);
//}
//
//void Serial2_writeBuffer(uint8_t *data, uint16_t len)
//{
//    HAL_UART_Transmit(&huart2, data, len, HAL_MAX_DELAY);
//}
//
//void Serial_printFloatAuto(const char *label, float value)
//{
//    int whole = (int)value;
//
//    Serial_print("%s %d", label, whole);
//
//    float frac = value - whole;
//
//    if(frac > 0.000001f)
//    {
//        Serial_print(".");
//
//        /* rounding helper */
//        frac += 0.0000005f;
//
//        for(int i=0; i<6; i++)   // max 6 decimals
//        {
//            frac *= 10;
//
//            int digit = (int)frac;
//
//            Serial_print("%d", digit);
//
//            frac -= digit;
//
//            if(frac < 0.000001f) break;
//        }
//    }
//
//    Serial_println("");
//}
//
//void Serial_printFloat(const char *label, float value, uint8_t decimals)
//{
//    int whole;
//    int fraction;
//    int scale = 1;
//
//    for(int i=0; i<decimals; i++) scale *= 10;
//
//    whole = (int)value;
//
//    fraction = (int)((value - whole) * scale);
//
//    if(fraction < 0) fraction = -fraction;
//
//    Serial_print("%s %d.", label, whole);
//
//    Serial_print("%0*d", decimals, fraction);
//
//    Serial_println("");
//}
//
//
///* ---------- Ring buffer for UART2 RX ---------- */
//#define RX2_BUF_SIZE 255
//static uint8_t rx2_buffer[RX2_BUF_SIZE];
//static volatile uint16_t rx2_head = 0;
//static volatile uint16_t rx2_tail = 0;
//static uint8_t rx2_byte;
//
///* ============================================================
// * Init
// * ============================================================ */
//
//void Serial_begin(uint32_t baud)
//{
//    huart1.Init.BaudRate = baud;
//    HAL_UART_Init(&huart1);
//    HAL_UART_Receive_IT(&huart1, &rx1_byte, 1);
//}
//
//void Serial2_begin(uint32_t baud)
//{
//    huart2.Init.BaudRate = baud;
//    HAL_UART_Init(&huart2);
//    HAL_UART_Receive_IT(&huart2, &rx2_byte, 1);
//}
//
///* ============================================================
// * RX interrupt callback (shared)
// * ============================================================ */
//
//void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
//{
//    /* ---------- UART1 ---------- */
//    if (huart->Instance == USART1)
//    {
//        uint16_t next = (rx1_head + 1) % RX1_BUF_SIZE;
//        if (next != rx1_tail)
//        {
//            rx1_buffer[rx1_head] = rx1_byte;
//            rx1_head = next;
//        }
//        HAL_UART_Receive_IT(&huart1, &rx1_byte, 1);
//    }
//
//    /* ---------- UART2 ---------- */
//    else if (huart->Instance == USART2)
//    {
//        uint16_t next = (rx2_head + 1) % RX2_BUF_SIZE;
//        if (next != rx2_tail)
//        {
//            rx2_buffer[rx2_head] = rx2_byte;
//            rx2_head = next;
//        }
//        HAL_UART_Receive_IT(&huart2, &rx2_byte, 1);
//    }
//}
//
//
//
///* ============================================================
// * Receive API
// * ============================================================ */
//
//int Serial_available(void)
//{
//    if (rx1_head >= rx1_tail) return rx1_head - rx1_tail;
//    else return RX1_BUF_SIZE - (rx1_tail - rx1_head);
//}
//
//int Serial_read(void)
//{
//    if (rx1_head == rx1_tail) return -1;
//    uint8_t val = rx1_buffer[rx1_tail];
//    rx1_tail = (rx1_tail + 1) % RX1_BUF_SIZE;
//    return val;
//}
//
//void Serial_flush(void)
//{
//  while(Serial_available()) Serial_read();
//}
//
//int Serial2_available(void)
//{
//    if (rx2_head >= rx2_tail) return rx2_head - rx2_tail;
//    else return RX2_BUF_SIZE - (rx2_tail - rx2_head);
//}
//
//int Serial2_read(void)
//{
//    if (rx2_head == rx2_tail) return -1;
//    uint8_t val = rx2_buffer[rx2_tail];
//    rx2_tail = (rx2_tail + 1) % RX2_BUF_SIZE;
//    return val;
//}
//
//void Serial2_flush(void)
//{
//  while(Serial2_available()) Serial2_read();
//}

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "UART_F.h"

/* ---------- UART handles ---------- */
extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;

/* ============================================================
 * UART1 (Serial)
 * ============================================================ */

#define RX1_BUF_SIZE 64

static uint8_t rx1_buffer[RX1_BUF_SIZE];
static volatile uint16_t rx1_head = 0;
static volatile uint16_t rx1_tail = 0;
static uint8_t rx1_byte;

/* ============================================================
 * UART2 (Serial2)
 * ============================================================ */

#define RX2_BUF_SIZE 256

static uint8_t rx2_buffer[RX2_BUF_SIZE];
static volatile uint16_t rx2_head = 0;
static volatile uint16_t rx2_tail = 0;
static uint8_t rx2_byte;

/* ============================================================
 * TX helpers
 * ============================================================ */

static void uart1_send(const char *s)
{
    HAL_UART_Transmit(&huart1,
                      (uint8_t*)s,
                      strlen(s),
                      HAL_MAX_DELAY);
}

static void uart2_send(const char *s)
{
    HAL_UART_Transmit(&huart2,
                      (uint8_t*)s,
                      strlen(s),
                      HAL_MAX_DELAY);
}

/* ============================================================
 * PRINT FUNCTIONS
 * ============================================================ */

void Serial_print(const char *fmt, ...)
{
    char buf[128];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    uart1_send(buf);
}

void Serial_println(const char *fmt, ...)
{
    char buf[128];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    uart1_send(buf);
    uart1_send("\r\n");
}

void Serial2_print(const char *fmt, ...)
{
    char buf[128];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    uart2_send(buf);
}

void Serial2_println(const char *fmt, ...)
{
    char buf[128];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    uart2_send(buf);
    uart2_send("\r\n");
}

/* ============================================================
 * BYTE WRITE
 * ============================================================ */

void Serial_write(uint8_t data)
{
    HAL_UART_Transmit(&huart1,
                      &data,
                      1,
                      HAL_MAX_DELAY);
}

void Serial2_write(uint8_t data)
{
    HAL_UART_Transmit(&huart2,
                      &data,
                      1,
                      HAL_MAX_DELAY);
}

/* ============================================================
 * INIT
 * ============================================================ */

void Serial_begin(uint32_t baud)
{
    /* UART already initialized by CubeMX */

    HAL_UART_Receive_IT(&huart1,
                        &rx1_byte,
                        1);
}

void Serial2_begin(uint32_t baud)
{
    /* UART already initialized by CubeMX */

    HAL_UART_Receive_IT(&huart2,
                        &rx2_byte,
                        1);
}

/* ============================================================
 * RX CALLBACK
 * ============================================================ */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        uint16_t next =
            (rx1_head + 1) % RX1_BUF_SIZE;

        if (next != rx1_tail)
        {
            rx1_buffer[rx1_head] =
                rx1_byte;

            rx1_head = next;
        }

        HAL_UART_Receive_IT(huart,
                            &rx1_byte,
                            1);
    }

    else if (huart->Instance == USART2)
    {
        uint16_t next =
            (rx2_head + 1) % RX2_BUF_SIZE;

        if (next != rx2_tail)
        {
            rx2_buffer[rx2_head] =
                rx2_byte;

            rx2_head = next;
        }

        HAL_UART_Receive_IT(huart,
                            &rx2_byte,
                            1);
    }
}

/* ============================================================
 * ERROR CALLBACK (VERY IMPORTANT)
 * ============================================================ */

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        __HAL_UART_CLEAR_OREFLAG(huart);

        HAL_UART_Receive_IT(huart,
                            &rx2_byte,
                            1);
    }

    if (huart->Instance == USART1)
    {
        __HAL_UART_CLEAR_OREFLAG(huart);

        HAL_UART_Receive_IT(huart,
                            &rx1_byte,
                            1);
    }
}

/* ============================================================
 * RECEIVE API
 * ============================================================ */

int Serial2_available(void)
{
    if (rx2_head >= rx2_tail)
        return rx2_head - rx2_tail;

    else
        return RX2_BUF_SIZE -
               (rx2_tail - rx2_head);
}

int Serial2_read(void)
{
    if (rx2_head == rx2_tail)
        return -1;

    uint8_t val =
        rx2_buffer[rx2_tail];

    rx2_tail =
        (rx2_tail + 1) % RX2_BUF_SIZE;

    return val;
}

int Serial_available(void)
{
    if (rx1_head >= rx1_tail)
        return rx1_head - rx1_tail;

    else
        return RX1_BUF_SIZE -
               (rx1_tail - rx1_head);
}

int Serial_read(void)
{
    if (rx1_head == rx1_tail)
        return -1;

    uint8_t val =
        rx1_buffer[rx1_tail];

    rx1_tail =
        (rx1_tail + 1) % RX1_BUF_SIZE;

    return val;
}

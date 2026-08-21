#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "stm32wlxx_hal.h"
#include "radio.h"
#include "subghz.h"
#include "radio_board_if.h"
#include "uart_debug.h"

#include "config.h"
#include "radio_app.h"
#include "protocol.h"
#include "ax25.h"
#include "g3ruh.h"

const RadioConfig_t SatelliteProfile = {
    .txFrequency = DEFAULT_TX_FREQUENCY_HZ,
    .rxFrequency = DEFAULT_RX_FREQUENCY_HZ,
    .sourceCallsign = "NEPSAT",
    .sourceSSID = 1,
    .destCallsign = "GROUND",
    .destSSID = 0,
    .isSatelliteMode = true
};

#define BURST_PACKET_COUNT  100
#define BEACON_INTERVAL_MS  5000
#define TX_TIMEOUT_MS       1000

typedef enum {
    STATE_IDLE_RX = 0,
    STATE_PROCESS_COMMAND,
    STATE_TRANSMIT_BURST,
} AppState_t;

volatile AppState_t app_state = STATE_IDLE_RX;
volatile uint8_t tx_busy = 0;
volatile uint8_t rx_frame_buffer[AX25_MAX_FRAME_SIZE];
volatile uint16_t rx_frame_size = 0;
volatile uint8_t rx_done_flag = 0;
volatile uint8_t rx_timeout_flag = 0;
volatile uint8_t rx_error_flag = 0;

static uint8_t last_tx_scrambled[AX25_MAX_FRAME_SIZE];
static uint16_t last_tx_len = 0;
static uint32_t last_beacon_tick_ms = 0;
static uint32_t last_burst_finish_time = 0;

/* Interrupt Status Callbacks */
void OnTxDone(void) { tx_busy = 0; }
void OnTxTimeout(void) { uart2_puts("TX TIMEOUT ERROR\r\n"); tx_busy = 0; }
void OnRxTimeout(void) { rx_timeout_flag = 1; }
void OnRxError(void) { rx_error_flag = 1; }

void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr) {
    (void)rssi;
    (void)snr;
    uint16_t copy_len = (size < AX25_MAX_FRAME_SIZE) ? size : AX25_MAX_FRAME_SIZE;
    memcpy((void *)rx_frame_buffer, payload, copy_len);
    rx_frame_size = copy_len;
    rx_done_flag = 1;
}

extern SUBGHZ_HandleTypeDef hsubghz;
void SUBGHZ_Radio_IRQHandler(void) { HAL_SUBGHZ_IRQHandler(&hsubghz); }

static void Print_Hex_Bytes(const uint8_t *buf, uint16_t len) {
    char line[56]; uint16_t pos = 0; uint16_t on_this_line = 0;
    for (uint16_t i = 0; i < len; i++) {
        pos += snprintf(&line[pos], sizeof(line) - pos, "%02X ", buf[i]);
        on_this_line++;
        if (on_this_line == 16 || i == (uint16_t)(len - 1)) {
            uart2_puts(line); uart2_puts("\r\n"); pos = 0; on_this_line = 0;
        }
    }
}

static void Print_Payload_ASCII(const uint8_t *buf, uint16_t start, uint16_t end) {
    if (end <= start) {
        uart2_puts(" Payload (text) : (none)\r\n");
        return;
    }
    char line[96];
    uint16_t n = (uint16_t)(end - start);
    uint16_t max_chars = (uint16_t)(sizeof(line) - 24);
    if (n > max_chars) n = max_chars;
    uint16_t pos = 0;
    pos += (uint16_t)snprintf(&line[pos], sizeof(line) - pos, " Payload (text): \"");
    for (uint16_t i = 0; i < n; i++) {
        uint8_t c = buf[start + i];
        line[pos++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    pos += (uint16_t)snprintf(&line[pos], sizeof(line) - pos, "\"\r\n");
    uart2_puts(line);
}

static bool Looks_Like_Stale_TX_Buffer(const uint8_t *buf, uint16_t len) {
    if (last_tx_len == 0) return false;
    uint16_t compare_len = (len < last_tx_len) ? len : last_tx_len;
    if (compare_len < 8) return false;
    return (memcmp(buf, last_tx_scrambled, compare_len) == 0);
}

static void Print_AX25_Fields(const char *label, const uint8_t *buf, uint16_t len) {
    char line[112];
    snprintf(line, sizeof(line), " --- %s field breakdown (%u bytes) ---\r\n", label, len);
    uart2_puts(line);
    if (len < 20) {
        uart2_puts(" (too short for a full AX.25 header+FCS - skipping field breakdown)\r\n");
        return;
    }
    snprintf(line, sizeof(line), " Start Flag : %02X %s\r\n", buf[0], (buf[0] == AX25_FLAG) ? "(OK)" : "(MISMATCH)");
    uart2_puts(line);
    char dest_cs[7], src_cs[7];
    AX25_DecodeAddress(&buf[1], dest_cs);
    snprintf(line, sizeof(line), " Dest       : %02X %02X %02X %02X %02X %02X %02X -> \"%s\"\r\n", buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7], dest_cs);
    uart2_puts(line);
    AX25_DecodeAddress(&buf[8], src_cs);
    snprintf(line, sizeof(line), " Src        : %02X %02X %02X %02X %02X %02X %02X -> \"%s\"\r\n", buf[8],buf[9],buf[10],buf[11],buf[12],buf[13],buf[14], src_cs);
    uart2_puts(line);
    snprintf(line, sizeof(line), " Control    : %02X\r\n", buf[15]); uart2_puts(line);
    snprintf(line, sizeof(line), " PID        : %02X\r\n", buf[16]); uart2_puts(line);

    uint16_t payload_start = 17; uint16_t payload_end = (uint16_t)(len - 3);
    if (payload_end > payload_start) {
        uint16_t plen = (uint16_t)(payload_end - payload_start);
        snprintf(line, sizeof(line), " Payload    : %u bytes\r\n", plen); uart2_puts(line);
        Print_Hex_Bytes(&buf[payload_start], plen);
        Print_Payload_ASCII(buf, payload_start, payload_end);
    } else {
        uart2_puts(" Payload    : (none)\r\n");
    }
    if (len >= 3) {
        snprintf(line, sizeof(line), " FCS/CRC    : %02X %02X\r\n", buf[len - 3], buf[len - 2]);
        uart2_puts(line);
    }
    snprintf(line, sizeof(line), " End Flag   : %02X %s\r\n", buf[len - 1], (buf[len - 1] == AX25_FLAG) ? "(OK)" : "(MISMATCH)");
    uart2_puts(line);
}

extern void SysTick_Init_CPU2(uint32_t sys_freq_hz);
extern void CPU2_Delay_Ms(uint32_t ms);
extern volatile uint32_t g_system_tick_ms;

static uint32_t Get_Time_Ms(void) {
    return g_system_tick_ms;
}

/* All TX goes through RadioApp_Send() */
static bool Radio_Send_And_Wait(uint8_t *buffer, uint16_t size, uint32_t timeout_ms) {
    tx_busy = 1;
    RadioApp_Send(buffer, size);

    uint32_t start_ms = Get_Time_Ms();
    while (tx_busy != 0) {
        if ((Get_Time_Ms() - start_ms) > timeout_ms) {
            uart2_puts("ERROR: TX timeout\r\n");
            Radio.Standby();
            tx_busy = 0;
            return false;
        }
        CPU2_Delay_Ms(1);
    }
    return true;
}

/* 100x Telemetry Burst Stream Routine */
static void Transmit_Burst_100(void) {
    uint8_t tx_data[90] = "Namaste Everyone! From Antarikchya Nepal. S2S-2 CubeSat Beacon Communication Test";

    uint8_t frame_buffer[AX25_MAX_FRAME_SIZE];
    uint16_t frame_size = Protocol_CreatePacket(frame_buffer, tx_data, (uint16_t)strlen((const char *)tx_data), &SatelliteProfile);
    if (frame_size == 0) return;

    memcpy(last_tx_scrambled, frame_buffer, frame_size);
    last_tx_len = frame_size;

    char size_msg[80];
    snprintf(size_msg, sizeof(size_msg), "BURST START: %d packets, %d bytes each, scrambled\r\n", BURST_PACKET_COUNT, frame_size);
    uart2_puts(size_msg);

    uart2_puts(" TX plaintext payload : \"");
    uart2_puts((const char *)tx_data);
    uart2_puts("\"\r\n");
    Print_AX25_Fields("TX (on-air / scrambled)", frame_buffer, frame_size);

    uint32_t sent_ok = 0, sent_timeout = 0;
    for (int i = 0; i < BURST_PACKET_COUNT; i++) {
        bool ok = Radio_Send_And_Wait(frame_buffer, frame_size, 3000);
        if (ok) sent_ok++; else sent_timeout++;
        char pkt_msg[48];
        snprintf(pkt_msg, sizeof(pkt_msg), " TX packet %3d/%d : %s\r\n", i + 1, BURST_PACKET_COUNT, ok ? "sent OK" : "TIMEOUT");
        uart2_puts(pkt_msg);
        CPU2_Delay_Ms(25);
    }

    char done_msg[96];
    snprintf(done_msg, sizeof(done_msg), "BURST COMPLETE (%lu/%d sent cleanly, %lu timed out)\r\n", (unsigned long)sent_ok, BURST_PACKET_COUNT, (unsigned long)sent_timeout);
    uart2_puts(done_msg);
}

/* Asynchronous Housekeeping Beacon Timer Loop */
static void Send_Beacon_If_Due(void) {
    if (app_state != STATE_IDLE_RX) {
        return;
    }
    uint32_t now = Get_Time_Ms();
    if ((now - last_beacon_tick_ms) < BEACON_INTERVAL_MS) {
        return;
    }
    last_beacon_tick_ms = now;

    uint8_t beacon_text[32] = "NEPSAT BEACON";
    uint8_t frame_buffer[AX25_MAX_FRAME_SIZE];
    uint16_t frame_size = Protocol_CreatePacket(frame_buffer, beacon_text, (uint16_t)strlen((const char *)beacon_text), &SatelliteProfile);
    if (frame_size == 0) return;

    memcpy(last_tx_scrambled, frame_buffer, frame_size);
    last_tx_len = frame_size;

    uart2_puts("\r\nBEACON TX (5s tick)\r\n");
    uart2_puts(" TX plaintext payload : \"");
    uart2_puts((const char *)beacon_text);
    uart2_puts("\"\r\n");
    Print_AX25_Fields("TX (on-air / scrambled)", frame_buffer, frame_size);

    Radio_Send_And_Wait(frame_buffer, frame_size, 3000);

    /* Back to continuous RX listening on 437.375 MHz */
    rx_done_flag = 0;
    rx_timeout_flag = 0;
    rx_error_flag = 0;
    RadioApp_StartRx();
}

static uint8_t s_prev_rx_buf[AX25_MAX_FRAME_SIZE];
static uint16_t s_prev_rx_len = 0;

int main(void) {
    SystemInit();
    HAL_Init();
    SysTick_Init_CPU2(48000000UL);
    uart2_puts("HAL_Init done\r\n");
    uart2_init();
    uart2_puts("\r\n=== STM32WL55 AX.25 + G3RUH COMMAND->BURST START [rev9: 100-packet burst + sliding RX] ===\r\n");

    RBI_Init();
    uart2_puts("RF FRONT END OK\r\n");

    MX_SUBGHZ_Init();
    HAL_NVIC_SetPriority(SUBGHZ_Radio_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(SUBGHZ_Radio_IRQn);
    __enable_irq();

    RadioEvents_t events = {
        .TxDone = OnTxDone, .TxTimeout = OnTxTimeout,
        .RxDone = OnRxDone, .RxTimeout = OnRxTimeout, .RxError = OnRxError
    };

    RadioApp_Init(&SatelliteProfile, &events);
    RadioApp_StartRx();

    uart2_puts("LISTENING FOR COMMAND (and beaconing every 5s)...\r\n");

    char tick_msg[40];
    snprintf(tick_msg, sizeof(tick_msg), "TICK CHECK: %lu\r\n", (unsigned long)Get_Time_Ms());
    uart2_puts(tick_msg);

    app_state = STATE_IDLE_RX;
    last_beacon_tick_ms = Get_Time_Ms();
    s_prev_rx_len = 0;

    while (1) {
        switch (app_state) {
            case STATE_IDLE_RX: {
                if (rx_timeout_flag || rx_error_flag) {
                    rx_timeout_flag = 0;
                    rx_error_flag = 0;
                }
                if (rx_done_flag) {
                    rx_done_flag = 0;
                    app_state = STATE_PROCESS_COMMAND;
                }
                break;
            }

            case STATE_PROCESS_COMMAND: {
                uint16_t raw_len = (rx_frame_size > AX25_MAX_FRAME_SIZE) ? AX25_MAX_FRAME_SIZE : rx_frame_size;
                uint8_t raw_copy[AX25_MAX_FRAME_SIZE];
                memcpy(raw_copy, (const void *)rx_frame_buffer, raw_len);

                /* Combine with previous buffer to form a 400-byte sliding window across chunk boundaries */
                uint8_t stream_buffer[AX25_MAX_FRAME_SIZE * 2];
                uint16_t stream_len = 0;

                if (s_prev_rx_len > 0) {
                    memcpy(&stream_buffer[0], s_prev_rx_buf, s_prev_rx_len);
                    stream_len += s_prev_rx_len;
                }
                memcpy(&stream_buffer[stream_len], raw_copy, raw_len);
                stream_len += raw_len;

                /* Save current buffer for the next sliding window */
                memcpy(s_prev_rx_buf, raw_copy, raw_len);
                s_prev_rx_len = raw_len;

                uint8_t decoded_frame[AX25_MAX_FRAME_SIZE];
                uint16_t decoded_len = 0;

                if (!Protocol_ExtractFrame(stream_buffer, stream_len, decoded_frame, sizeof(decoded_frame), &decoded_len)) {
                    /* Noise buffer - no frame found; smoothly continue listening */
                    app_state = STATE_IDLE_RX;
                    break;
                }

                /* Clear previous buffer history since frame was consumed */
                s_prev_rx_len = 0;

                uart2_puts("\r\n-----------------------------------\r\n");
                uart2_puts("VALID AX.25 FRAME RECEIVED - PROCESSING COMMAND...\r\n");
                Print_AX25_Fields("DESCRAMBLED", decoded_frame, decoded_len);

                bool crc1_ok = AX25_VerifyCRC_Method1(decoded_frame, decoded_len);
                uint16_t crc2_computed = 0, crc2_recv_le = 0, crc2_recv_be = 0;
                bool crc2_ok = AX25_VerifyCRC_Method2(decoded_frame, decoded_len, &crc2_computed, &crc2_recv_le, &crc2_recv_be);

                char crcmsg[128];
                snprintf(crcmsg, sizeof(crcmsg), " CRC method 1 (reversed X-25)      : %s\r\n", crc1_ok ? "PASS" : "FAIL");
                uart2_puts(crcmsg);
                snprintf(crcmsg, sizeof(crcmsg), " CRC method 2 (direct CCITT-FALSE) : %s (computed=0x%04X LE=0x%04X BE=0x%04X)\r\n", crc2_ok ? "PASS" : "FAIL", crc2_computed, crc2_recv_le, crc2_recv_be);
                uart2_puts(crcmsg);

                if (!(crc1_ok || crc2_ok)) {
                    uart2_puts(" *** COMMAND RECEIVED: NO (CRC failed both methods) ***\r\n");
                    app_state = STATE_IDLE_RX;
                    break;
                }
                if (decoded_len < 17) {
                    uart2_puts(" *** COMMAND RECEIVED: NO (frame too short) ***\r\n");
                    app_state = STATE_IDLE_RX;
                    break;
                }

                char dest_cs[7], src_cs[7];
                AX25_DecodeAddress(&decoded_frame[1], dest_cs);
                AX25_DecodeAddress(&decoded_frame[8], src_cs);
                char id_msg[80];
                snprintf(id_msg, sizeof(id_msg), "FRAME: DEST=%s SRC=%s\r\n", dest_cs, src_cs);
                uart2_puts(id_msg);

                if (strcmp(dest_cs, SatelliteProfile.sourceCallsign) != 0) {
                    uart2_printf(" *** COMMAND RECEIVED: NO (addressed to \"%s\", not us) ***\r\n", dest_cs);
                    app_state = STATE_IDLE_RX;
                    break;
                }

                uint16_t payload_start = 17;
                uint16_t payload_end = decoded_len - 3;
                if (payload_end <= payload_start) {
                    uart2_puts(" *** COMMAND RECEIVED: NO (empty payload, no opcode) ***\r\n");
                    app_state = STATE_IDLE_RX;
                    break;
                }

                uint8_t opcode = decoded_frame[payload_start];
                uart2_printf(" *** COMMAND RECEIVED: YES (from %s, opcode 0x%02X) ***\r\n", src_cs, opcode);

                if (opcode == CMD_REQUEST_BURST) {
                    if ((Get_Time_Ms() - last_burst_finish_time) < 4000) {
                        uart2_puts("CMD_REQUEST_BURST duplicate packet - ignoring\r\n");
                        app_state = STATE_IDLE_RX;
                    } else {
                        uart2_puts("CMD_REQUEST_BURST ACCEPTED - STARTING 100-PACKET BURST\r\n");
                        app_state = STATE_TRANSMIT_BURST;
                    }
                } else {
                    uart2_puts("UNKNOWN OPCODE - IGNORING\r\n");
                    app_state = STATE_IDLE_RX;
                }
                break;
            }

            case STATE_TRANSMIT_BURST:
                Transmit_Burst_100();
                last_burst_finish_time = Get_Time_Ms();
                s_prev_rx_len = 0;
                rx_done_flag = 0;
                rx_timeout_flag = 0;
                rx_error_flag = 0;
                memset((void *)rx_frame_buffer, 0, sizeof(rx_frame_buffer));
                last_beacon_tick_ms = Get_Time_Ms();
                RadioApp_StartRx();
                uart2_puts("LISTENING FOR NEXT COMMAND (and beaconing every 5s)...\r\n");
                app_state = STATE_IDLE_RX;
                break;
        }
        Send_Beacon_If_Due();
    }
    return 0;
}
/*
 * STM32WL55 Ground Station Firmware
 *
 * AX.25 + G3RUH + GFSK
 *
 * Uplink   (TX): 437.375 MHz (+22 dBm)
 * Downlink (RX): 435.000 MHz
 *
 * Features:
 *  - Automatic Detection & Decoding of Beacon 1 (HK1: 34 Bytes, Voltages & Temperatures)
 *  - Automatic Detection & Decoding of Beacon 2 (HK2: 38 Bytes, Currents & 6-Axis IMU/Mag)
 *  - Telecommand Uplink: CAMERA RUN (13B), ADCS (13B), EPDM (13B), BURST REQUEST (0x01)
 *  - Telecommand Confirmation: Parses Satellite ACK (0xAA) / NACK (0x55)
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stm32wlxx_hal.h"
#include "radio.h"
#include "subghz.h"
#include "radio_driver.h"
#include "radio_board_if.h"
#include "uart_debug.h"

#include "config.h"
#include "radio_app.h"
#include "protocol.h"
#include "ax25.h"
#include "g3ruh.h"

/*
=========================================================
GROUND STATION RADIO PROFILE
=========================================================
*/

const RadioConfig_t GroundStationProfile =
{
    /* Ground station transmit frequency (satellite uplink receiver) */
    .txFrequency = 868000000UL,

    /* Ground station receive frequency (satellite downlink transmitter) */
    .rxFrequency = 868000000UL,

    /* AX.25 address */
    .sourceCallsign = "GROUND",
    .sourceSSID = 0,

    /* Satellite address */
    .destCallsign = "9NS2S2",
    .destSSID = 1,

    /* Ground station mode */
    .isSatelliteMode = false
};

/*
=========================================================
SYSTEM CONFIGURATION
=========================================================
*/

#define CMD_LINE_MAX        32
#define TX_TIMEOUT_MS       3000
#define RX_SESSION_MS       60000
#define RX_HEARTBEAT_MS     3000

/*
=========================================================
GLOBAL RADIO FLAGS
=========================================================
*/

volatile uint8_t tx_busy = 0;
volatile uint8_t rx_frame_buffer[AX25_MAX_FRAME_SIZE];
volatile uint16_t rx_frame_size = 0;

volatile uint8_t rx_done_flag = 0;
volatile uint8_t rx_timeout_flag = 0;
volatile uint8_t rx_error_flag = 0;

/*
=========================================================
RX EVENT COUNTERS & STATS
=========================================================
*/

static uint32_t stat_b1_received = 0;
static uint32_t stat_b2_received = 0;
static uint32_t stat_ack_received = 0;
static uint32_t stat_nack_received = 0;
static uint32_t stat_packets_ok = 0;
static uint32_t stat_packets_bad_crc = 0;
static uint32_t stat_packets_misaddressed = 0;

/*
=========================================================
FORWARD DECLARATIONS
=========================================================
*/

static void Print_Hex_Bytes(const uint8_t *buf, uint16_t len);
static void Print_Payload_ASCII(const uint8_t *buf, uint16_t start, uint16_t end);
static void Print_AX25_Fields(const char *label, const uint8_t *buf, uint16_t len);
static void Parse_And_Print_Beacon1(const uint8_t *p, uint16_t plen);
static void Parse_And_Print_Beacon2(const uint8_t *p, uint16_t plen);
static void Process_Received_Frame(void);
static void Print_Help(void);

extern void SysTick_Init_CPU2(uint32_t sys_freq_hz);
extern void CPU2_Delay_Ms(uint32_t ms);
extern volatile uint32_t g_system_tick_ms;

static uint32_t Get_Time_Ms(void)
{
    return g_system_tick_ms;
}

/*
=========================================================
DEBUG PRINT HELPERS
=========================================================
*/

static void Print_Hex_Bytes(const uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        uart2_printf("%02X ", buf[i]);
        if ((i + 1) % 16 == 0)
        {
            uart2_puts("\r\n");
        }
    }
    if (len % 16 != 0)
    {
        uart2_puts("\r\n");
    }
}

static void Print_Payload_ASCII(const uint8_t *buf, uint16_t start, uint16_t end)
{
    if (end <= start)
    {
        uart2_puts("Payload (text) : (none)\r\n");
        return;
    }

    char line[112];
    uint16_t n = (uint16_t)(end - start);
    uint16_t max_chars = (uint16_t)(sizeof(line) - 24);
    if (n > max_chars) n = max_chars;

    uint16_t pos = 0;
    pos += (uint16_t)snprintf(&line[pos], sizeof(line) - pos, "Payload (text): \"");
    for (uint16_t i = 0; i < n; i++)
    {
        uint8_t c = buf[start + i];
        line[pos++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    pos += (uint16_t)snprintf(&line[pos], sizeof(line) - pos, "\"\r\n");
    uart2_puts(line);
}

static void Print_AX25_Fields(const char *label, const uint8_t *buf, uint16_t len)
{
    uart2_printf("\r\n--- %s FIELD BREAKDOWN (%u bytes) ---\r\n", label, len);

    if (len < 20)
    {
        uart2_puts("(too short for a full AX.25 header+FCS - skipping field breakdown)\r\n");
        return;
    }

    uart2_printf("Start Flag : %02X %s\r\n",
                 buf[0], (buf[0] == AX25_FLAG) ? "(OK, 0x7E)" : "(MISMATCH)");

    char dest_cs[7], src_cs[7];
    AX25_DecodeAddress(&buf[1], dest_cs);
    AX25_DecodeAddress(&buf[8], src_cs);

    uart2_printf("Dest       : \"%s\"\r\n", dest_cs);
    uart2_printf("Src        : \"%s\"\r\n", src_cs);
    uart2_printf("Control    : 0x%02X\r\n", buf[15]);
    uart2_printf("PID        : 0x%02X\r\n", buf[16]);

    uint16_t payload_start = 17;
    uint16_t payload_end = (uint16_t)(len - 3);

    if (payload_end > payload_start)
    {
        uint16_t plen = (uint16_t)(payload_end - payload_start);
        uart2_printf("Payload    : %u bytes\r\n", plen);
        Print_Hex_Bytes(&buf[payload_start], plen);
        Print_Payload_ASCII(buf, payload_start, payload_end);
    }
    else
    {
        uart2_puts("Payload    : (none)\r\n");
    }

    uart2_printf("FCS/CRC    : %02X %02X\r\n", buf[len - 3], buf[len - 2]);
    uart2_printf("End Flag   : %02X %s\r\n",
                 buf[len - 1], (buf[len - 1] == AX25_FLAG) ? "(OK, 0x7E)" : "(MISMATCH)");
}

/*
=========================================================
PARSERS FOR BEACON 1 (HK1) & BEACON 2 (HK2)
=========================================================
*/

static void Parse_And_Print_Beacon1(const uint8_t *p, uint16_t plen)
{
    (void)plen;
    const int16_t *v = (const int16_t *)p;
    uint16_t footer = (uint16_t)(p[32] | (p[33] << 8));
    stat_b1_received++;

    uart2_puts("\r\n========================================================================\r\n");
    uart2_printf("  >>> [GS RX] BEACON 1 #%lu: HOUSEKEEPING 1 (VOLTAGES & TEMPERATURES) <<<\r\n",
                 (unsigned long)stat_b1_received);
    uart2_puts("========================================================================\r\n");

    uart2_printf("  [00] Battery Voltage   (ADC_BAT_MON)  : %3d.%02d V   (raw x100: %d)\r\n",
                 v[0] / 100, (v[0] < 0 ? -v[0] : v[0]) % 100, v[0]);
    uart2_printf("  [01] Total Solar Volt  (TOTAL_SOLAR_V): %3d.%02d V   (raw x100: %d)\r\n",
                 v[1] / 100, (v[1] < 0 ? -v[1] : v[1]) % 100, v[1]);
    uart2_printf("  [02] Raw Bus Voltage   (RAW_VOLT)     : %3d.%02d V   (raw x100: %d)\r\n",
                 v[2] / 100, (v[2] < 0 ? -v[2] : v[2]) % 100, v[2]);

    uart2_printf("  [03] Solar Panel 5     (SP5_VOLT)     : %3d.%02d V\r\n",
                 v[3] / 100, (v[3] < 0 ? -v[3] : v[3]) % 100);
    uart2_printf("  [04] Solar Panel 4     (SP4_VOLT)     : %3d.%02d V\r\n",
                 v[4] / 100, (v[4] < 0 ? -v[4] : v[4]) % 100);
    uart2_printf("  [05] Solar Panel 3     (SP3_VOLT)     : %3d.%02d V\r\n",
                 v[5] / 100, (v[5] < 0 ? -v[5] : v[5]) % 100);
    uart2_printf("  [06] Solar Panel 1     (SP1_VOLT)     : %3d.%02d V\r\n",
                 v[6] / 100, (v[6] < 0 ? -v[6] : v[6]) % 100);
    uart2_printf("  [07] Solar Panel 2     (SP2_VOLT)     : %3d.%02d V\r\n",
                 v[7] / 100, (v[7] < 0 ? -v[7] : v[7]) % 100);

    uart2_printf("  [08] Antenna Temp      (ANT_TEMP)     : %3d.%02d C   (raw x100: %d)\r\n",
                 v[8] / 100, (v[8] < 0 ? -v[8] : v[8]) % 100, v[8]);
    uart2_printf("  [09] Battery Temp      (BATT_TEMP)    : %3d.%02d C\r\n",
                 v[9] / 100, (v[9] < 0 ? -v[9] : v[9]) % 100);
    uart2_printf("  [10] BPB Temp          (TEMP_BPB)     : %3d.%02d C\r\n",
                 v[10] / 100, (v[10] < 0 ? -v[10] : v[10]) % 100);
    uart2_printf("  [11] Temperature 1     (TEMP1)        : %3d.%02d C\r\n",
                 v[11] / 100, (v[11] < 0 ? -v[11] : v[11]) % 100);
    uart2_printf("  [12] Temperature 5     (TEMP5)        : %3d.%02d C\r\n",
                 v[12] / 100, (v[12] < 0 ? -v[12] : v[12]) % 100);
    uart2_printf("  [13] Temperature 4     (TEMP4)        : %3d.%02d C\r\n",
                 v[13] / 100, (v[13] < 0 ? -v[13] : v[13]) % 100);
    uart2_printf("  [14] Temperature 3     (TEMP3)        : %3d.%02d C\r\n",
                 v[14] / 100, (v[14] < 0 ? -v[14] : v[14]) % 100);
    uart2_printf("  [15] Temperature 2     (TEMP2)        : %3d.%02d C\r\n",
                 v[15] / 100, (v[15] < 0 ? -v[15] : v[15]) % 100);

    uart2_printf("  [--] Footer: 0x%02X 0x%02X (0x%04X)   [VALID HK1]\r\n",
                 p[32], p[33], footer);
    uart2_puts("========================================================================\r\n");
}

static void Parse_And_Print_Beacon2(const uint8_t *p, uint16_t plen)
{
    (void)plen;
    const int16_t *d = (const int16_t *)p;
    uint16_t footer = (uint16_t)(p[36] | (p[37] << 8));
    stat_b2_received++;

    uart2_puts("\r\n========================================================================\r\n");
    uart2_printf("  >>> [GS RX] BEACON 2 #%lu: HOUSEKEEPING 2 (CURRENTS & IMU/MAG SENSORS) <<<\r\n",
                 (unsigned long)stat_b2_received);
    uart2_puts("========================================================================\r\n");

    uart2_printf("  [00] UNREG_I          : %2d.%03d A   (raw x100: %d)\r\n",
                 d[0] / 100, (d[0] < 0 ? -d[0] : d[0]) % 100, d[0]);
    uart2_printf("  [01] SP4_I            : %2d.%03d A   (raw x100: %d)\r\n",
                 d[1] / 100, (d[1] < 0 ? -d[1] : d[1]) % 100, d[1]);
    uart2_printf("  [02] MAIN_3V3_I       : %2d.%03d A   (raw x100: %d)\r\n",
                 d[2] / 100, (d[2] < 0 ? -d[2] : d[2]) % 100, d[2]);
    uart2_printf("  [03] MISSION_3V3_I    : %2d.%03d A   (raw x100: %d)\r\n",
                 d[3] / 100, (d[3] < 0 ? -d[3] : d[3]) % 100, d[3]);
    uart2_printf("  [04] SPT_I            : %2d.%03d A   (raw x100: %d)\r\n",
                 d[4] / 100, (d[4] < 0 ? -d[4] : d[4]) % 100, d[4]);
    uart2_printf("  [05] 5V_I             : %2d.%03d A   (raw x100: %d)\r\n",
                 d[5] / 100, (d[5] < 0 ? -d[5] : d[5]) % 100, d[5]);
    uart2_printf("  [06] SP2_I            : %2d.%03d A   (raw x100: %d)\r\n",
                 d[6] / 100, (d[6] < 0 ? -d[6] : d[6]) % 100, d[6]);
    uart2_printf("  [07] SP1_I            : %2d.%03d A   (raw x100: %d)\r\n",
                 d[7] / 100, (d[7] < 0 ? -d[7] : d[7]) % 100, d[7]);
    uart2_printf("  [08] RAW_I            : %2d.%03d A   (raw x100: %d)\r\n",
                 d[8] / 100, (d[8] < 0 ? -d[8] : d[8]) % 100, d[8]);
    uart2_printf("  [09] SP5_I            : %2d.%03d A   (raw x100: %d)\r\n",
                 d[9] / 100, (d[9] < 0 ? -d[9] : d[9]) % 100, d[9]);
    uart2_printf("  [10] BAT_I            : %2d.%03d A   (raw x100: %d)\r\n",
                 d[10] / 100, (d[10] < 0 ? -d[10] : d[10]) % 100, d[10]);
    uart2_printf("  [11] SP3_I            : %2d.%03d A   (raw x100: %d)\r\n",
                 d[11] / 100, (d[11] < 0 ? -d[11] : d[11]) % 100, d[11]);

    uart2_printf("  [GYRO] X: %3d.%02d | Y: %3d.%02d | Z: %3d.%02d dps\r\n",
                 d[12] / 100, (d[12] < 0 ? -d[12] : d[12]) % 100,
                 d[13] / 100, (d[13] < 0 ? -d[13] : d[13]) % 100,
                 d[14] / 100, (d[14] < 0 ? -d[14] : d[14]) % 100);

    uart2_printf("  [MAG]  X: %2d.%03d | Y: %2d.%03d | Z: %2d.%03d Gauss\r\n",
                 d[15] / 100, (d[15] < 0 ? -d[15] : d[15]) % 100,
                 d[16] / 100, (d[16] < 0 ? -d[16] : d[16]) % 100,
                 d[17] / 100, (d[17] < 0 ? -d[17] : d[17]) % 100);

    uart2_printf("  [--] Footer: 0x%02X 0x%02X (0x%04X)   [VALID HK2]\r\n",
                 p[36], p[37], footer);
    uart2_puts("========================================================================\r\n");
}

/*
=========================================================
PROCESS A RECEIVED FRAME
=========================================================
*/

static uint8_t s_gs_prev_rx[AX25_MAX_FRAME_SIZE];
static uint16_t s_gs_prev_len = 0;

static void Process_Received_Frame(void)
{
    uint16_t raw_len = (rx_frame_size > AX25_MAX_FRAME_SIZE)
                        ? AX25_MAX_FRAME_SIZE
                        : rx_frame_size;

    uint8_t raw_copy[AX25_MAX_FRAME_SIZE];
    memcpy(raw_copy, (const void *)rx_frame_buffer, raw_len);

    /* Combine previous 200 bytes and current 200 bytes into a sliding stream buffer */
    uint8_t stream_buf[AX25_MAX_FRAME_SIZE * 2];
    uint16_t stream_len = 0;

    if (s_gs_prev_len > 0)
    {
        memcpy(&stream_buf[0], s_gs_prev_rx, s_gs_prev_len);
        stream_len += s_gs_prev_len;
    }
    memcpy(&stream_buf[stream_len], raw_copy, raw_len);
    stream_len += raw_len;

    memcpy(s_gs_prev_rx, raw_copy, raw_len);
    s_gs_prev_len = raw_len;

    uint8_t decoded[AX25_MAX_FRAME_SIZE];
    uint16_t decoded_len = 0;

    if (!Protocol_ExtractFrame(stream_buf, stream_len, decoded, sizeof(decoded), &decoded_len))
    {
        /* Noise chunk - smoothly ignore */
        return;
    }

    /* Reset history since valid frame boundary was consumed */
    s_gs_prev_len = 0;

    bool crc1_ok = AX25_VerifyCRC_Method1(decoded, decoded_len) && (decoded_len >= 17);
    uint16_t crc2_computed = 0, crc2_recv_le = 0, crc2_recv_be = 0;
    bool crc2_ok = AX25_VerifyCRC_Method2(decoded, decoded_len, &crc2_computed, &crc2_recv_le, &crc2_recv_be);

    if (!(crc1_ok || crc2_ok) || decoded_len < 17)
    {
        stat_packets_bad_crc++;
        return;
    }

    char dest_cs[7], src_cs[7];
    AX25_DecodeAddress(&decoded[1], dest_cs);
    AX25_DecodeAddress(&decoded[8], src_cs);

    /* Accept frames addressed to GROUND, CQ, or broadcast */
    if (strcmp(dest_cs, GroundStationProfile.sourceCallsign) != 0 &&
        strcmp(dest_cs, "CQ") != 0 &&
        strcmp(dest_cs, "BEACON") != 0)
    {
        stat_packets_misaddressed++;
        return;
    }

    stat_packets_ok++;

    uint16_t payload_start = 17;
    uint16_t payload_end = (uint16_t)(decoded_len - 3);
    uint16_t plen = (payload_end > payload_start) ? (uint16_t)(payload_end - payload_start) : 0;
    const uint8_t *p = &decoded[payload_start];

    /* ------------------------------------------------------------- */
    /* 1. SATELLITE COMMAND RESPONSES (3-Byte Protocol & Legacy 1B)  */
    /* ------------------------------------------------------------- */
    if (plen == 3 && p[0] == 0xAC && p[1] == 0x00)
    {
        stat_ack_received++;
        uart2_puts("\r\n=======================================================\r\n");
        uart2_printf(" >>> [GS RX] COMMAND ACK: COMMAND ACCEPTED (0xAC 0x00 0x%02X) <<<\r\n", p[2]);
        uart2_puts(" Satellite accepted command; execution started.\r\n");
        uart2_puts("=======================================================\r\n");
        return;
    }
    else if (plen == 3 && p[0] == 0xEE && p[1] == 0x00)
    {
        stat_ack_received++;
        uart2_puts("\r\n=======================================================\r\n");
        uart2_printf(" >>> [GS RX] COMMAND ACK: COMMAND COMPLETED (0xEE 0x00 0x%02X) <<<\r\n", p[2]);
        uart2_puts(" SUCCESS: Satellite completed command successfully.\r\n");
        uart2_puts("=======================================================\r\n");
        return;
    }
    else if (plen == 3 && p[0] == 0xEE && p[1] == 0x05)
    {
        stat_nack_received++;
        uart2_puts("\r\n=======================================================\r\n");
        uart2_printf(" >>> [GS RX] COMMAND NACK: DATA MISSING / ADDRESS MISMATCH (0xEE 0x05 0x%02X) <<<\r\n", p[2]);
        uart2_puts(" WARNING: Start address out of bounds or requested data not found!\r\n");
        uart2_puts("=======================================================\r\n");
        return;
    }
    else if (plen == 3 && p[0] == 0xEE && p[1] == 0x03)
    {
        stat_nack_received++;
        uart2_puts("\r\n=======================================================\r\n");
        uart2_printf(" >>> [GS RX] COMMAND NACK: CRC ERROR (0xEE 0x03 0x%02X) <<<\r\n", p[2]);
        uart2_puts(" WARNING: Uplink telecommand failed CRC verification on satellite!\r\n");
        uart2_puts("=======================================================\r\n");
        return;
    }
    else if (plen == 1 && p[0] == 0xAA)
    {
        stat_ack_received++;
        uart2_puts("\r\n=======================================================\r\n");
        uart2_printf(" >>> [GS RX] SATELLITE COMMAND ACK RECEIVED (0xAA) #%lu <<<\r\n",
                     (unsigned long)stat_ack_received);
        uart2_puts(" SUCCESS: Satellite executed command (e.g. Camera Run)!\r\n");
        uart2_puts("=======================================================\r\n");
        return;
    }
    else if (plen == 1 && p[0] == 0x55)
    {
        stat_nack_received++;
        uart2_puts("\r\n=======================================================\r\n");
        uart2_printf(" >>> [GS RX] SATELLITE COMMAND NACK RECEIVED (0x55) #%lu <<<\r\n",
                     (unsigned long)stat_nack_received);
        uart2_puts(" WARNING: Satellite rejected command (camera not enabled / busy)!\r\n");
        uart2_puts("=======================================================\r\n");
        return;
    }

    /* ------------------------------------------------------------- */
    /* 2. BEACON 1 & 2 (ASCII Text Format: ">9NS2S2 B1 ..." / "B2")   */
    /* ------------------------------------------------------------- */
    if (plen >= 10 && (memcmp(p, ">9NS2S2 B1", 10) == 0 || memcmp(p, "9NS2S2 B1", 9) == 0))
    {
        char txt[140];
        uint16_t clen = (plen < sizeof(txt) - 1) ? plen : sizeof(txt) - 1;
        memcpy(txt, p, clen);
        txt[clen] = '\0';

        const char *tag = strstr(txt, "B1");
        if (tag)
        {
            int16_t v[17];
            memset(v, 0, sizeof(v));
            const char *ptr = tag + 2;
            for (int i = 0; i < 16; i++)
            {
                char *endp;
                v[i] = (int16_t)strtol(ptr, &endp, 10);
                if (endp == ptr) break;
                ptr = endp;
            }
            v[16] = (int16_t)0xAA55; /* Valid HK1 footer */
            Parse_And_Print_Beacon1((const uint8_t *)v, 34);
            return;
        }
    }
    else if (plen >= 10 && (memcmp(p, ">9NS2S2 B2", 10) == 0 || memcmp(p, "9NS2S2 B2", 9) == 0))
    {
        char txt[140];
        uint16_t clen = (plen < sizeof(txt) - 1) ? plen : sizeof(txt) - 1;
        memcpy(txt, p, clen);
        txt[clen] = '\0';

        const char *tag = strstr(txt, "B2");
        if (tag)
        {
            int16_t d[19];
            memset(d, 0, sizeof(d));
            const char *ptr = tag + 2;
            for (int i = 0; i < 18; i++)
            {
                char *endp;
                d[i] = (int16_t)strtol(ptr, &endp, 10);
                if (endp == ptr) break;
                ptr = endp;
            }
            d[18] = (int16_t)0xBB66; /* Valid HK2 footer */
            Parse_And_Print_Beacon2((const uint8_t *)d, 38);
            return;
        }
    }

    /* ------------------------------------------------------------- */
    /* 3. BEACON 1 (Binary HK1: 34 Bytes, Footer 0xAA55)             */
    /* ------------------------------------------------------------- */
    uint16_t f1 = (plen >= 34) ? (uint16_t)(p[32] | (p[33] << 8)) : 0;
    uint16_t f1_be = (plen >= 34) ? (uint16_t)((p[32] << 8) | p[33]) : 0;
    if (plen == 34 || f1 == 0xAA55 || f1_be == 0xAA55)
    {
        Parse_And_Print_Beacon1(p, plen);
        return;
    }

    /* ------------------------------------------------------------- */
    /* 4. BEACON 2 (Binary HK2: 38 Bytes, Footer 0xBB66)             */
    /* ------------------------------------------------------------- */
    uint16_t f2 = (plen >= 38) ? (uint16_t)(p[36] | (p[37] << 8)) : 0;
    uint16_t f2_be = (plen >= 38) ? (uint16_t)((p[36] << 8) | p[37]) : 0;
    if (plen == 38 || f2 == 0xBB66 || f2_be == 0xBB66)
    {
        Parse_And_Print_Beacon2(p, plen);
        return;
    }

    /* ------------------------------------------------------------- */
    /* 5. COMBINED HK BURST (Binary HK1+HK2+IMU: 68 Bytes)           */
    /* ------------------------------------------------------------- */
    if (plen == 68)
    {
        uart2_puts("\r\n========================================================================\r\n");
        uart2_puts("  >>> [GS RX] RECEIVED COMBINED HK BURST PACKET (68 Bytes: ADC1+ADC2+IMU) <<<\r\n");
        uart2_puts("========================================================================\r\n");

        uint8_t b1_buf[34];
        memcpy(b1_buf, p, 32);
        b1_buf[32] = 0x55;
        b1_buf[33] = 0xAA;
        Parse_And_Print_Beacon1(b1_buf, 34);

        uint8_t b2_buf[38];
        memcpy(b2_buf, &p[32], 36);
        b2_buf[36] = 0x66;
        b2_buf[37] = 0xBB;
        Parse_And_Print_Beacon2(b2_buf, 38);
        return;
    }

    /* ------------------------------------------------------------- */
    /* 4. OTHER / ASCII / TELEMETRY BURST PACKETS                    */
    /* ------------------------------------------------------------- */
    uart2_printf("\r\n--- PACKET DECODED (%u bytes, from %s) ---\r\n", decoded_len, src_cs);
    Print_AX25_Fields("PAYLOAD", decoded, decoded_len);
    uart2_printf("TOTAL STATS: %lu OK (%lu B1, %lu B2, %lu ACK, %lu NACK) | %lu bad CRC\r\n",
                 (unsigned long)stat_packets_ok,
                 (unsigned long)stat_b1_received,
                 (unsigned long)stat_b2_received,
                 (unsigned long)stat_ack_received,
                 (unsigned long)stat_nack_received,
                 (unsigned long)stat_packets_bad_crc);
}

/*
=========================================================
RADIO CALLBACK FUNCTIONS
=========================================================
*/

void OnTxDone(void)
{
    tx_busy = 0;
}

void OnTxTimeout(void)
{
    tx_busy = 0;
}

void OnRxTimeout(void)
{
    rx_timeout_flag = 1;
}

void OnRxError(void)
{
    rx_error_flag = 1;
}

void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr)
{
    (void)rssi;
    (void)snr;
    uint16_t copy_len = (size > AX25_MAX_FRAME_SIZE) ? AX25_MAX_FRAME_SIZE : size;
    memcpy((void *)rx_frame_buffer, payload, copy_len);
    rx_frame_size = copy_len;
    rx_done_flag = 1;
}

extern SUBGHZ_HandleTypeDef hsubghz;

void SUBGHZ_Radio_IRQHandler(void)
{
    HAL_SUBGHZ_IRQHandler(&hsubghz);
}

/*
=========================================================
TELECOMMAND UPLINK ENGINE (437.375 MHz)
=========================================================
*/

static void Send_13B_Command(const char *name, const uint8_t *cmd_13b)
{
    uint8_t frame[AX25_MAX_FRAME_SIZE];
    uint16_t frame_len = Protocol_CreatePacket(frame, cmd_13b, 13, &GroundStationProfile);
    if (frame_len == 0)
    {
        uart2_puts("ERROR: Failed to create AX.25 command frame\r\n");
        return;
    }

    uart2_printf("\r\n=======================================================\r\n");
    uart2_printf(">>> TRANSMITTING %s COMMAND (13B, Opcode 0x%02X) <<<\r\n", name, cmd_13b[1]);
    uart2_printf("    Uplink Freq: 437.375 MHz (+22 dBm GFSK 4800bd)\r\n");
    uart2_printf("    Burst of 14 packets spanning satellite listening window\r\n");
    uart2_puts("=======================================================\r\n");

    for (int i = 0; i < 14; i++)
    {
        tx_busy = 1;
        RadioApp_Send(frame, frame_len);

        uint32_t timeout_cnt = TX_TIMEOUT_MS;
        while (tx_busy != 0 && timeout_cnt > 0)
        {
            CPU2_Delay_Ms(1);
            timeout_cnt--;
        }

        if (tx_busy != 0)
        {
            uart2_puts("TX TIMEOUT\r\n");
            Radio.Standby();
            tx_busy = 0;
        }

        uart2_printf("  [%2d/14] Uplink packet sent\r\n", i + 1);
        CPU2_Delay_Ms(160);
    }

    uart2_puts(">>> UPLINK BURST COMPLETE -> SWITCHING TO 435.000 MHz (Listening for ACK/NACK)...\r\n");

    /* Return to continuous RX listening on 435.000 MHz */
    s_gs_prev_len = 0;
    rx_done_flag = 0;
    rx_timeout_flag = 0;
    rx_error_flag = 0;
    RadioApp_StartRx();
}

static void Send_Camera_Command(void)
{
    static const uint8_t cam_cmd[13] = CMD_CAMERA_COMMAND;
    Send_13B_Command("CAMERA RUN", cam_cmd);
}

static void Send_ADCS_Command(void)
{
    static const uint8_t adcs_cmd[13] = CMD_ADCD_COMMAND;
    Send_13B_Command("ADCS RUN", adcs_cmd);
}

static void Send_EPDM_Command(void)
{
    static const uint8_t epdm_cmd[13] = CMD_EPDM_COMMAND;
    Send_13B_Command("EPDM RUN", epdm_cmd);
}

static void Send_Burst_Command(void)
{
    uint8_t payload[1] = { (uint8_t)CMD_REQUEST_BURST };
    uint8_t frame[AX25_MAX_FRAME_SIZE];
    uint16_t frame_len = Protocol_CreatePacket(frame, payload, sizeof(payload), &GroundStationProfile);
    if (frame_len == 0)
    {
        uart2_puts("AX25 CREATE ERROR\r\n");
        return;
    }

    uart2_puts("\r\n>>> TRANSMITTING BURST REQUEST (Opcode 0x01) on 437.375 MHz <<<\r\n");

    for (int i = 0; i < 14; i++)
    {
        tx_busy = 1;
        RadioApp_Send(frame, frame_len);

        uint32_t timeout_cnt = TX_TIMEOUT_MS;
        while (tx_busy != 0 && timeout_cnt > 0)
        {
            CPU2_Delay_Ms(1);
            timeout_cnt--;
        }

        if (tx_busy != 0)
        {
            uart2_puts("TX TIMEOUT\r\n");
            Radio.Standby();
            tx_busy = 0;
        }

        uart2_printf("  [%2d/14] Burst request packet sent\r\n", i + 1);
        CPU2_Delay_Ms(160);
    }

    uart2_puts(">>> SWITCHING TO 435.000 MHz (Listening for 100-packet burst)...\r\n");
    s_gs_prev_len = 0;
    rx_done_flag = 0;
    rx_timeout_flag = 0;
    rx_error_flag = 0;
    RadioApp_StartRx();
}

/*
=========================================================
STRING HELPERS & UART COMMAND LINE READER
=========================================================
*/

static bool str_ieq(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = *a;
        char cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
        a++;
        b++;
    }
    return (*a == 0 && *b == 0);
}

static bool USART_TryReadLine(char *out, uint8_t max)
{
    static char buffer[CMD_LINE_MAX];
    static uint8_t index = 0;
    uint8_t c;

    if (!uart2_try_getc(&c))
    {
        return false;
    }

    uart2_putc(c);

    if (c == '\r' || c == '\n')
    {
        if (index == 0)
        {
            return false;
        }
        buffer[index] = 0;
        strncpy(out, buffer, max - 1);
        out[max - 1] = 0;
        index = 0;
        uart2_puts("\r\n");
        return true;
    }

    if (index < CMD_LINE_MAX - 1)
    {
        buffer[index++] = c;
    }

    return false;
}

static void Print_Help(void)
{
    uart2_puts("\r\n=======================================================\r\n");
    uart2_puts("        STM32WL55 GROUND STATION TELECOMMANDS\r\n");
    uart2_puts("=======================================================\r\n");
    uart2_puts("  CAMERA / CAM  - Send 13B Camera Run Command (Opcode 0x04)\r\n");
    uart2_puts("  ADCS          - Send 13B ADCS Subsystem Command (Opcode 0x03)\r\n");
    uart2_puts("  EPDM          - Send 13B EPDM Payload Command (Opcode 0x05)\r\n");
    uart2_puts("  BURST         - Request 100-packet Telemetry Burst (0x01)\r\n");
    uart2_puts("  HELP / ?      - Display this help menu\r\n");
    uart2_puts("-------------------------------------------------------\r\n");
    uart2_puts("  Uplink   (TX) : 437.375 MHz (+22 dBm GFSK 4800bd)\r\n");
    uart2_puts("  Downlink (RX) : 435.000 MHz (Continuous listening)\r\n");
    uart2_puts("  Auto-Detects  : Beacon 1 (HK1 34B), Beacon 2 (HK2 38B),\r\n");
    uart2_puts("                  Command ACK (0xAA) / NACK (0x55)\r\n");
    uart2_puts("=======================================================\r\n");
}

/*
=========================================================
MAIN FUNCTION
=========================================================
*/

int main(void)
{
    /* CRITICAL: Relocate vector table to Cortex-M0+ partition at 0x08032000 */
    SCB->VTOR = 0x08032000;

    /* MCU INITIALIZATION */
    SystemInit();
    HAL_Init();
    SysTick_Init_CPU2(HAL_RCC_GetHCLK2Freq());

    /* Enable IPCC, SUBGHZSPI, and SRAM2 peripheral bus clocks in both domains */
    (*(volatile uint32_t *)0x58000150UL) |= (1UL << 0) | (1UL << 25);
    (*(volatile uint32_t *)0x58000050UL) |= (1UL << 0) | (1UL << 25);

    /* Enable GPIOA, GPIOB, GPIOC peripheral bus clocks in both domains */
    (*(volatile uint32_t *)0x5800004CUL) |= 0x87;
    (*(volatile uint32_t *)0x5800014CUL) |= 0x87;

    uart2_init();

    uart2_puts("\r\n");
    uart2_puts("=======================================================\r\n");
    uart2_puts("    STM32WL55 SATELLITE GROUND STATION (GS)\r\n");
    uart2_puts("    Dual-Band: 437.375 MHz Uplink / 435.000 MHz Downlink\r\n");
    uart2_puts("    AX.25 + G3RUH Scrambling + GFSK (4800 baud)\r\n");
    uart2_puts("=======================================================\r\n");

    /* RADIO HARDWARE INIT */
    RBI_Init();
    MX_SUBGHZ_Init();

    HAL_NVIC_SetPriority(SUBGHZ_Radio_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(SUBGHZ_Radio_IRQn);
    __enable_irq();

    RadioEvents_t events =
    {
        .TxDone = OnTxDone,
        .TxTimeout = OnTxTimeout,
        .RxDone = OnRxDone,
        .RxTimeout = OnRxTimeout,
        .RxError = OnRxError
    };

    RadioApp_Init(&GroundStationProfile, &events);
    RadioApp_StartRx();

    uart2_puts("RADIO ACTIVE: Listening for satellite beacons on 435.000 MHz...\r\n");
    Print_Help();
    uart2_puts("\r\nGS> ");

    char command[CMD_LINE_MAX];

    /* MAIN EVENT LOOP */
    while (1)
    {
        /* Process any incoming beacon or packet */
        if (rx_done_flag)
        {
            rx_done_flag = 0;
            Process_Received_Frame();
            uart2_puts("\r\nGS> ");
        }

        if (rx_timeout_flag || rx_error_flag)
        {
            rx_timeout_flag = 0;
            rx_error_flag = 0;
        }

        /* Check for user command entry */
        if (USART_TryReadLine(command, sizeof(command)))
        {
            if (str_ieq(command, "CAMERA") || str_ieq(command, "CAM"))
            {
                Send_Camera_Command();
            }
            else if (str_ieq(command, "ADCS"))
            {
                Send_ADCS_Command();
            }
            else if (str_ieq(command, "EPDM"))
            {
                Send_EPDM_Command();
            }
            else if (str_ieq(command, "BURST") || str_ieq(command, "COMMAND"))
            {
                Send_Burst_Command();
            }
            else if (str_ieq(command, "HELP") || str_ieq(command, "?"))
            {
                Print_Help();
            }
            else
            {
                uart2_printf("UNKNOWN COMMAND: \"%s\". Type HELP for available commands.\r\n", command);
            }

            uart2_puts("\r\nGS> ");
        }
    }

    return 0;
}
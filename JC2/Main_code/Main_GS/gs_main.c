/*
 * STM32WL55 Ground Station
 *
 * AX.25 + G3RUH + GFSK
 *
 * TX:
 * 437.375 MHz
 *
 * RX:
 * 435.000 MHz
 *
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
    /* Ground station transmit frequency, satellite uplink receiver --
       437.375 MHz per Mission/config.h's documented frequency plan.
       Was hardcoded to 868000000UL (EU ISM band, not this project's
       UHF ham band) -- a real bug that would have put the GS on the
       wrong frequency entirely for any real-RF test. */
    .txFrequency = DEFAULT_TX_FREQUENCY_HZ,

    /* Ground station receive frequency, satellite downlink transmitter --
       435.000 MHz per Mission/config.h's documented frequency plan.
       Same 868 MHz bug as txFrequency above. */
    .rxFrequency = DEFAULT_RX_FREQUENCY_HZ,

    /* AX25 address */
    .sourceCallsign = "GROUND",
    /* FIX: aligned to 0 to match the SSID the satellite side uses when
       addressing frames to "GROUND" (SatelliteProfile.destSSID = 0).
       AX25_DecodeAddress() doesn't compare SSID today so this wasn't
       breaking the link, but it was inconsistent data - both ends
       should agree on GROUND's SSID. */
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
#define RX_WATCHDOG_MS      5000
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
RX EVENT COUNTERS
=========================================================
*/

static uint32_t stat_rx_timeouts = 0;
static uint32_t stat_rx_errors = 0;
static uint32_t stat_rx_crc_errors = 0;
static uint32_t stat_rx_header_errors = 0;

/* captured inside SUBGHZ_Radio_IRQHandler() BEFORE HAL_SUBGHZ_IRQHandler()
   clears the IRQ register, so OnRxError() can read a real value instead of
   the stale 0x0000 it got from calling SUBGRF_GetIrqStatus() too late. */
volatile uint16_t g_last_irq_status = 0;



/*
=========================================================
TX HISTORY BUFFER
=========================================================
*/

static uint8_t last_tx_frame[AX25_MAX_FRAME_SIZE];
static uint16_t last_tx_len = 0;



/*
=========================================================
RX SESSION STATS
=========================================================
*/

static uint32_t stat_packets_ok = 0;
static uint32_t stat_packets_bad_crc = 0;
static uint32_t stat_packets_misaddressed = 0;
static uint32_t stat_packets_stale_tx = 0;



/*
=========================================================
FORWARD DECLARATIONS
=========================================================
*/

static void Print_Hex_Bytes(const uint8_t *buf, uint16_t len);
static void Print_Payload_ASCII(const uint8_t *buf, uint16_t start, uint16_t end);
static void Print_Decoded_Sheet_Payload(const uint8_t *buf, uint16_t len);
static bool Looks_Like_Stale_TX_Buffer(const uint8_t *buf, uint16_t len);
static void Print_AX25_Fields(const char *label, const uint8_t *buf, uint16_t len);
static void Process_Received_Frame(void);



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



/* CRC-8, poly 0x07, init 0x00 -- mirrors satellite_app.c's Beacon_Crc8
   (not shared across targets -- GS build doesn't compile satellite_app.c);
   used here only to verify Beacon1/Beacon2 CHK_CRC on RX. */
static uint8_t GS_Beacon_Crc8(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
        {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static const char *GS_Ack_Status_Name(uint8_t status)
{
    switch (status)
    {
        case ACK_STATUS_ACCEPTED:         return "ACCEPTED";
        case ACK_STATUS_COMPLETED:        return "COMPLETED";
        case ACK_STATUS_PACKET_ERROR:     return "PACKET_ERROR";
        case ACK_STATUS_CRC_ERROR:        return "CRC_ERROR";
        case ACK_STATUS_INCOMPLETE:       return "INCOMPLETE";
        case ACK_STATUS_DATA_MISSING:     return "DATA_MISSING";
        case ACK_STATUS_DATA_UNAVAILABLE: return "DATA_UNAVAILABLE";
        case ACK_STATUS_BUSY:             return "SATELLITE_BUSY";
        case ACK_STATUS_MISSION_INACTIVE: return "MISSION_INACTIVE";
        case ACK_STATUS_INVALID_COMMAND:  return "INVALID_COMMAND";
        case ACK_STATUS_MEMORY_ERROR:     return "MEMORY_ERROR";
        case ACK_STATUS_TX_FAILED:        return "TX_FAILED";
        default:                          return "UNKNOWN";
    }
}

/* Interprets a decoded payload against the Packet Format sheet's Beacon1/
   Beacon2/ACK-NACK/Command layouts (Mission/config.h) instead of dumping
   it as raw ASCII -- those are binary structs, not text, so the old
   ASCII-only dump just printed dots for every real telemetry byte. Falls
   back to Print_Payload_ASCII for anything that doesn't match a known
   header/length (e.g. the free-form 81/128-byte test messages). */
static void Print_Decoded_Sheet_Payload(const uint8_t *buf, uint16_t len)
{
    if (len >= BEACON1_PAYLOAD_LEN && buf[0] == BEACON1_HEAD)
    {
        uint8_t crc = GS_Beacon_Crc8(buf, BEACON1_PAYLOAD_LEN - 1);
        uart2_printf("BEACON1 (HEAD=0xB1, %u bytes) CHK_CRC: %s\r\n",
                     len, (crc == buf[31]) ? "OK" : "MISMATCH");
        uart2_printf("  TYPE=%u TIM_DAY=%u TIM_HOUR=%u\r\n",
                     buf[1], (uint16_t)((buf[2] << 8) | buf[3]), buf[4]);
        uart2_printf("  BAT_V=%u BAT_I=%u BAT_TEMP=%u RAW_V=%u RAW_I=%u\r\n",
                     (uint16_t)((buf[5] << 8) | buf[6]),
                     (uint16_t)((buf[7] << 8) | buf[8]),
                     buf[9], buf[10], buf[11]);
        uart2_printf("  SOL_TOT_V=%u SOL_TOT_I=%u UNREG_I=%u MAIN_3V3_I=%u 3V3_MSN_I=%u 5V_I=%u ADC_3V32_I=%u\r\n",
                     (uint16_t)((buf[12] << 8) | buf[13]),
                     buf[14], buf[15], buf[16], buf[17], buf[18], buf[19]);
        uart2_printf("  TEMP1-5=%u,%u,%u,%u,%u BPB_T=%u ANT_P_T=%u OBC/COM_T=%u\r\n",
                     buf[20], buf[21], buf[22], buf[23], buf[24], buf[25], buf[26], buf[27]);
        uart2_printf("  SYSTEM_STAT=%u MCU_STAT=%u ADC_STAT=%u\r\n", buf[28], buf[29], buf[30]);
        return;
    }

    if (len >= BEACON2_PAYLOAD_LEN && buf[0] == BEACON2_HEAD)
    {
        uint8_t crc = GS_Beacon_Crc8(buf, BEACON2_PAYLOAD_LEN - 1);
        uart2_printf("BEACON2 (HEAD=0xB2, %u bytes) CHK_CRC: %s\r\n",
                     len, (crc == buf[21]) ? "OK" : "MISMATCH");
        uart2_printf("  TYPE=%u TIM_DAY=%u\r\n", buf[1], (uint16_t)((buf[2] << 8) | buf[3]));
        uart2_printf("  SOL_P1-5_V=%u,%u,%u,%u,%u\r\n", buf[4], buf[5], buf[6], buf[7], buf[8]);
        uart2_printf("  SOL_P1-5_I=%u,%u,%u,%u,%u\r\n", buf[9], buf[10], buf[11], buf[12], buf[13]);
        uart2_printf("  GYRO_X/Y/Z=%u,%u,%u MAG_X/Y/Z=%u,%u,%u\r\n",
                     buf[14], buf[15], buf[16], buf[17], buf[18], buf[19]);
        uart2_printf("  POWER_STAT=0x%02X\r\n", buf[20]);
        return;
    }

    if (len == ACK_PAYLOAD_LEN &&
        (buf[0] == ACK_RESPONSE_TYPE_ACK || buf[0] == ACK_RESPONSE_TYPE_NACK))
    {
        uart2_printf("%s: status=%s (0x%02X) data_id=0x%02X\r\n",
                     (buf[0] == ACK_RESPONSE_TYPE_ACK) ? "ACK" : "NACK",
                     GS_Ack_Status_Name(buf[1]), buf[1], buf[2]);
        return;
    }

    if (len == COMMAND_PAYLOAD_LEN && buf[0] == SAT_ID_S2S2)
    {
        uart2_printf("COMMAND ECHO: SAT-ID=0x%02X MCU-ID=0x%02X OPCODE=%02X %02X %02X "
                     "DATA_ID=0x%04X ADDRESS=0x%08lX NUM_PACKETS=%u\r\n",
                     buf[0], buf[1], buf[2], buf[3], buf[4],
                     (unsigned)((buf[5] << 8) | buf[6]),
                     (unsigned long)((buf[7] << 24) | (buf[8] << 16) | (buf[9] << 8) | buf[10]),
                     (unsigned)((buf[11] << 8) | buf[12]));
        return;
    }

    Print_Payload_ASCII(buf, 0, len);
}



static bool __attribute__((unused)) Looks_Like_Stale_TX_Buffer(const uint8_t *buf, uint16_t len)
{
    if (last_tx_len == 0)
    {
        return false;
    }

    uint16_t compare_len = (len < last_tx_len) ? len : last_tx_len;

    if (compare_len < 8)
    {
        return false;
    }

    return (memcmp(buf, last_tx_frame, compare_len) == 0);
}



static void Print_AX25_Fields(const char *label, const uint8_t *buf, uint16_t len)
{
    uart2_printf("\r\n--- %s FIELD BREAKDOWN (%u bytes) ---\r\n", label, len);

    if (len < 20)
    {
        uart2_puts("(too short for a full AX.25 header+FCS - skipping field breakdown)\r\n");
        return;
    }

    uart2_printf
    (
        "Start Flag : %02X %s\r\n",
        buf[0],
        (buf[0] == AX25_FLAG) ? "(OK, matches 0x7E)" : "(MISMATCH)"
    );

    char dest_cs[7];
    char src_cs[7];

    AX25_DecodeAddress(&buf[1], dest_cs);
    AX25_DecodeAddress(&buf[8], src_cs);

    uart2_printf
    (
        "Dest       : %02X %02X %02X %02X %02X %02X %02X -> \"%s\"\r\n",
        buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
        dest_cs
    );

    uart2_printf
    (
        "Src        : %02X %02X %02X %02X %02X %02X %02X -> \"%s\"\r\n",
        buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14],
        src_cs
    );

    uart2_printf("Control    : %02X\r\n", buf[15]);
    uart2_printf("PID        : %02X\r\n", buf[16]);

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

    uart2_printf
    (
        "FCS/CRC    : %02X %02X (as sent: low-byte-first)\r\n",
        buf[len - 3],
        buf[len - 2]
    );

    uart2_printf
    (
        "End Flag   : %02X %s\r\n",
        buf[len - 1],
        (buf[len - 1] == AX25_FLAG) ? "(OK, matches 0x7E)" : "(MISMATCH)"
    );
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

    /* Combine previous 200 bytes and current 200 bytes into a 400-byte stream */
    uint8_t stream_buf[AX25_MAX_FRAME_SIZE * 2];
    uint16_t stream_len = 0;

    if (s_gs_prev_len > 0)
    {
        memcpy(&stream_buf[0], s_gs_prev_rx, s_gs_prev_len);
        stream_len += s_gs_prev_len;
    }
    memcpy(&stream_buf[stream_len], raw_copy, raw_len);
    stream_len += raw_len;

    /* Save current buffer for next sliding window */
    memcpy(s_gs_prev_rx, raw_copy, raw_len);
    s_gs_prev_len = raw_len;

    uint8_t decoded[AX25_MAX_FRAME_SIZE];
    uint16_t decoded_len = 0;

    if (!Protocol_ExtractFrame(stream_buf, stream_len, decoded, sizeof(decoded), &decoded_len))
    {
        /* Noise chunk - no valid frame found; return silently */
        return;
    }

    /* Valid frame found - reset history */
    s_gs_prev_len = 0;

    uart2_printf("\r\n========================================\r\n");
    uart2_printf("BURST / TELEMETRY PACKET DECODED (%u bytes)\r\n", decoded_len);
    uart2_printf("========================================\r\n");

    Print_AX25_Fields("DESCRAMBLED", decoded, decoded_len);

    bool crc1_ok = AX25_VerifyCRC_Method1(decoded, decoded_len) && (decoded_len >= 17);

    uint16_t crc2_computed = 0;
    uint16_t crc2_recv_le = 0;
    uint16_t crc2_recv_be = 0;

    bool crc2_ok = AX25_VerifyCRC_Method2(
        decoded, decoded_len, &crc2_computed, &crc2_recv_le, &crc2_recv_be);

    uart2_printf(
        "CRC method 1 (reversed X-25, magic 0xF0B8) : %s\r\n",
        crc1_ok ? "PASS" : "FAIL");

    uart2_printf(
        "CRC method 2 (direct CCITT-FALSE)         : %s (computed=0x%04X LE=0x%04X BE=0x%04X)\r\n",
        crc2_ok ? "PASS" : "FAIL", crc2_computed, crc2_recv_le, crc2_recv_be);

    if (!(crc1_ok || crc2_ok))
    {
        stat_packets_bad_crc++;
        uart2_puts("CRC CHECK FAILED (both methods) - DISCARDING FRAME\r\n");
        return;
    }

    if (decoded_len < 17)
    {
        stat_packets_bad_crc++;
        uart2_puts("FRAME TOO SHORT - DISCARDING\r\n");
        return;
    }

    char dest_cs[7];
    char src_cs[7];

    AX25_DecodeAddress(&decoded[1], dest_cs);
    AX25_DecodeAddress(&decoded[8], src_cs);

    uart2_printf("FRAME: DEST=%s SRC=%s\r\n", dest_cs, src_cs);

    if (strcmp(dest_cs, GroundStationProfile.sourceCallsign) != 0)
    {
        stat_packets_misaddressed++;
        uart2_puts("NOT ADDRESSED TO US - IGNORING\r\n");
        return;
    }

    stat_packets_ok++;

    uint16_t payload_start = 17;
    uint16_t payload_end = (uint16_t)(decoded_len - 3);

    if (payload_end > payload_start)
    {
        uint16_t wire_len = (uint16_t)(payload_end - payload_start);
        uint8_t real_payload[128];
        uint16_t real_len = 0;

        if (Protocol_DecodePayload(&decoded[payload_start], wire_len,
                                    real_payload, sizeof(real_payload), &real_len))
        {
            Print_Decoded_Sheet_Payload(real_payload, real_len);
        }
        else
        {
            uart2_puts("PAYLOAD: FEC/MAC check failed - discarding\r\n");
        }
    }

    uart2_printf(
        "BURST STATS: %lu OK | %lu bad CRC | %lu wrong addr | Total events: %lu\r\n",
        (unsigned long)stat_packets_ok, (unsigned long)stat_packets_bad_crc,
        (unsigned long)stat_packets_misaddressed,
        (unsigned long)(stat_packets_ok + stat_packets_bad_crc + stat_packets_misaddressed));
}

/*
=========================================================
RADIO CALLBACK FUNCTIONS
=========================================================
*/

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

/*
=========================================================
STM32WL SUBGHZ INTERRUPT HANDLER
=========================================================
*/

/* SUBGHZ_Radio_IRQHandler is centrally handled in protocol/stm32wlxx_it.c */

/*
=========================================================
SEND COMMAND TO SATELLITE
=========================================================
*/

extern void SysTick_Init_CPU2(uint32_t sys_freq_hz);
extern void CPU2_Delay_Ms(uint32_t ms);
extern volatile uint32_t g_system_tick_ms;

static uint32_t Get_Time_Ms(void)
{
    return g_system_tick_ms;
}

/* Packs the full 13-byte sheet-compliant command (SAT-ID + MCU-ID + 3
   opcode bytes + DATA_ID(2) + ADDRESS(4) + NUM_PACKETS(2)) and sends it
   through Protocol_CreatePacket -- the light FEC wrap + (if AUTH_ENABLE)
   keyed tag/replay counter are applied transparently there, same as any
   other payload, so this struct carries no counter/MAC of its own.
   address/num_packets are only meaningful for CMD_DOWNLOAD_FILE; none of
   the fixed console commands below supply a real target yet, so they
   stay 0 (TODO once a real file table exists on the satellite side). */
static void command_pack(const CommandPacket_t *cmd, uint8_t out[COMMAND_PAYLOAD_LEN])
{
    out[0] = SAT_ID_S2S2;
    out[1] = cmd->mcu_id;
    out[2] = cmd->opcode_b1;
    if (cmd->opcode_b1 == CMD_RUN_CAMERA) {
        out[3] = 0x5E;
        out[4] = 0xBD;
    } else if (cmd->opcode_b1 == CMD_DOWNLOAD_FILE) {
        out[3] = 0xD1;
        out[4] = cmd->opcode_b3;
    } else {
        out[3] = cmd->opcode_b2;
        out[4] = cmd->opcode_b3;
    }
    out[5] = (uint8_t)(cmd->data_id >> 8); out[6] = (uint8_t)cmd->data_id;
    out[7] = (uint8_t)(cmd->address >> 24); out[8] = (uint8_t)(cmd->address >> 16);
    out[9] = (uint8_t)(cmd->address >> 8);  out[10] = (uint8_t)cmd->address;
    out[11] = (uint8_t)(cmd->num_packets >> 8); out[12] = (uint8_t)cmd->num_packets;
}

static void Send_Command_Ex(uint8_t opcode, uint8_t subsystem, uint8_t data_id)
{
    CommandPacket_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.mcu_id = subsystem;
    cmd.opcode_b1 = opcode;
    cmd.data_id = data_id;

    uint8_t payload[COMMAND_PAYLOAD_LEN];
    command_pack(&cmd, payload);

    uint8_t frame[AX25_MAX_FRAME_SIZE];

    uint16_t frame_len = Protocol_CreatePacket(frame, payload, sizeof(payload), &GroundStationProfile);
    if (frame_len == 0)
    {
        uart2_puts("AX25 CREATE ERROR\r\n");
        return;
    }

    uart2_printf("\r\nTX COMMAND sat=0x%02X mcu=0x%02X opcode=0x%02X 0x%02X 0x%02X data=0x%04X (burst of 3 packets)\r\n",
                 payload[0], payload[1], payload[2], payload[3], payload[4], cmd.data_id);

    for (int i = 0; i < 3; i++)
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
        CPU2_Delay_Ms(15);
    }
    uart2_puts("COMMAND BURST COMPLETE - SWITCHING TO RX\r\n");
}

/* "48656C6C6F" -> {0x48,0x65,...}. Returns byte count, or -1 on bad input
   (odd length or a non-hex character). */
static int hex_decode(const char *hex, uint8_t *out, int out_max)
{
    int len = (int)strlen(hex);
    while (len > 0 && (hex[len - 1] == '\r' || hex[len - 1] == '\n' || hex[len - 1] == ' ')) len--;
    if (len == 0 || (len % 2) != 0 || (len / 2) > out_max) return -1;

    for (int i = 0; i < len; i += 2)
    {
        int hi = -1, lo = -1;
        char ch = hex[i], cl = hex[i + 1];
        if (ch >= '0' && ch <= '9') hi = ch - '0';
        else if (ch >= 'a' && ch <= 'f') hi = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') hi = ch - 'A' + 10;
        if (cl >= '0' && cl <= '9') lo = cl - '0';
        else if (cl >= 'a' && cl <= 'f') lo = cl - 'a' + 10;
        else if (cl >= 'A' && cl <= 'F') lo = cl - 'A' + 10;
        if (hi < 0 || lo < 0) return -1;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return len / 2;
}

static void Send_Command(CommandOpcode_t cmd)
{
    uint8_t payload[1];
    payload[0] = (uint8_t)cmd;

    uint8_t frame[AX25_MAX_FRAME_SIZE];

    uint16_t frame_len = Protocol_CreatePacket
    (
        frame,
        payload,
        sizeof(payload),
        &GroundStationProfile
    );

    if (frame_len == 0)
    {
        uart2_puts("AX25 CREATE ERROR\r\n");
        return;
    }

    uart2_printf("\r\nTX COMMAND 0x%02X (burst of 3 packets)\r\n", cmd);

    for (int i = 0; i < 3; i++)
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

        uart2_printf(" Command packet %d/3 sent\r\n", i + 1);
        CPU2_Delay_Ms(15);
    }

    uart2_puts("COMMAND BURST COMPLETE - SWITCHING TO RX\r\n");
}

/*
=========================================================
CASE INSENSITIVE STRING COMPARE
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

        if (ca != cb)
        {
            return false;
        }

        a++;
        b++;
    }

    return (*a == 0 && *b == 0);
}

/* Case-insensitive prefix match -- "TX " matches "tx 48656c6c6f". */
static bool str_starts_ieq(const char *s, const char *prefix)
{
    while (*prefix)
    {
        char cs = *s, cp = *prefix;
        if (cs >= 'a' && cs <= 'z') cs -= 32;
        if (cp >= 'a' && cp <= 'z') cp -= 32;
        if (cs != cp) return false;
        s++;
        prefix++;
    }
    return true;
}

/*
=========================================================
UART COMMAND LINE READER
=========================================================
*/

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

        snprintf(out, max, "%s", buffer);
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

/*
=========================================================
MAIN FUNCTION
=========================================================
*/

int main(void)
{
    /* MCU INITIALIZATION */

    SystemInit();
    HAL_Init();
    SysTick_Init_CPU2(48000000UL);
    uart2_init();

    uart2_puts("\r\n");
    uart2_puts("================================\r\n");
    uart2_puts(" STM32WL55 GROUND STATION\r\n");
    uart2_puts(" AX25 + G3RUH + GFSK\r\n");
    uart2_puts(" [rev4: fast burst TX + sliding RX]\r\n");
    uart2_puts(" UPLINK   : 437.375 MHz\r\n");
    uart2_puts(" DOWNLINK : 435.000 MHz\r\n");
    uart2_puts("================================\r\n");

    /* RADIO HARDWARE INIT */

    RBI_Init();
    MX_SUBGHZ_Init();

    /* ENABLE SUBGHZ INTERRUPT */

    HAL_NVIC_SetPriority(SUBGHZ_Radio_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(SUBGHZ_Radio_IRQn);
    __enable_irq();

    /* RADIO CALLBACK TABLE */

    RadioEvents_t events =
    {
        .TxDone = OnTxDone,
        .TxTimeout = OnTxTimeout,
        .RxDone = OnRxDone,
        .RxTimeout = OnRxTimeout,
        .RxError = OnRxError
    };

    /* RADIO APPLICATION INIT */

    RadioApp_Init(&GroundStationProfile, &events);
    RadioApp_StartRx();

    uart2_puts("\r\nRADIO READY - Listening for satellite beacons on 435.000 MHz\r\n");
    uart2_puts("Type COMMAND to request 100-packet burst from satellite\r\n");
    uart2_puts("> ");

    char command[CMD_LINE_MAX];

    /* MAIN LOOP */

    while (1)
    {
        /* Process any incoming beacon packets while waiting for command */
        if (rx_done_flag)
        {
            rx_done_flag = 0;
            Process_Received_Frame();
        }
        if (rx_timeout_flag || rx_error_flag)
        {
            rx_timeout_flag = 0;
            rx_error_flag = 0;
        }

        if (USART_TryReadLine(command, sizeof(command)))
        {
            if (str_ieq(command, "COMMAND"))
            {
                uart2_puts("\r\nCOMMAND ACCEPTED\r\n");

                stat_packets_ok = 0;
                stat_packets_bad_crc = 0;
                stat_packets_misaddressed = 0;
                stat_packets_stale_tx = 0;
                stat_rx_timeouts = 0;
                stat_rx_errors = 0;
                stat_rx_crc_errors = 0;
                stat_rx_header_errors = 0;
                s_gs_prev_len = 0;

                Send_Command(CMD_REQUEST_BURST);

                uart2_puts("\r\nLISTENING 435 MHz FOR 100-PACKET BURST DATA...\r\n");

                rx_done_flag = 0;
                rx_timeout_flag = 0;
                rx_error_flag = 0;

                RadioApp_StartRx();

                uint32_t start = Get_Time_Ms();
                uint32_t last_packet_time = start;
                uint32_t last_heartbeat = start;

                while ((Get_Time_Ms() - start) < RX_SESSION_MS)
                {
                    if (rx_done_flag)
                    {
                        rx_done_flag = 0;
                        last_packet_time = Get_Time_Ms();
                        last_heartbeat = last_packet_time;
                        Process_Received_Frame();

                        /* If all 100 packets have arrived, finish burst reception immediately */
                        if (stat_packets_ok >= 100)
                        {
                            break;
                        }
                    }

                    if (rx_timeout_flag || rx_error_flag)
                    {
                        rx_timeout_flag = 0;
                        rx_error_flag = 0;
                    }

                    /* If burst has started and no new packets have arrived for 4 seconds, finish */
                    if (stat_packets_ok > 0 && (Get_Time_Ms() - last_packet_time) > 4000)
                    {
                        break;
                    }

                    if ((Get_Time_Ms() - last_heartbeat) > RX_HEARTBEAT_MS)
                    {
                        last_heartbeat = Get_Time_Ms();

                        uart2_printf
                        (
                            "... listening (%lus elapsed) [Received %lu/100 packets]\r\n",
                            (unsigned long)((Get_Time_Ms() - start) / 1000UL),
                            (unsigned long)stat_packets_ok
                        );
                    }
                }

                uart2_printf
                (
                    "\r\n========================================\r\n"
                    "BURST RX COMPLETE: %lu/100 packets received cleanly!\r\n"
                    "========================================\r\n",
                    (unsigned long)stat_packets_ok
                );

                /* Return to continuous listening on 435 MHz for beacons and next command */
                s_gs_prev_len = 0;
                RadioApp_StartRx();
            }
            else if (str_ieq(command, "HEARTBEAT"))
            {
                Send_Command(CMD_HEARTBEAT);
            }
            else if (str_ieq(command, "HEALTH"))
            {
                Send_Command_Ex(CMD_GET_HEALTH, SUBSYS_OBC, DATA_ID_HOUSEKEEPING);
            }
            else if (str_ieq(command, "HOUSEKEEPING"))
            {
                Send_Command_Ex(CMD_GET_HOUSEKEEPING, SUBSYS_OBC, DATA_ID_HOUSEKEEPING);
            }
            else if (str_ieq(command, "BEACON1"))
            {
                Send_Command_Ex(CMD_GET_BEACON1, SUBSYS_OBC, DATA_ID_BEACON1);
            }
            else if (str_ieq(command, "BEACON2"))
            {
                Send_Command_Ex(CMD_GET_BEACON2, SUBSYS_OBC, DATA_ID_BEACON2);
            }
            else if (str_ieq(command, "ADCS"))
            {
                Send_Command_Ex(CMD_GET_ADCS, SUBSYS_ADCS, DATA_ID_ADCS);
            }
            else if (str_ieq(command, "CAMERA"))
            {
                Send_Command_Ex(CMD_RUN_CAMERA, SUBSYS_CAM, DATA_ID_CAMERA_RGB);
            }
            else if (str_ieq(command, "DOWNLOAD"))
            {
                Send_Command_Ex(CMD_DOWNLOAD_FILE, SUBSYS_OBC, DATA_ID_SAT_LOG);
            }
            else if (str_ieq(command, "SATLOG"))
            {
                Send_Command_Ex(CMD_GET_SAT_LOG, SUBSYS_OBC, DATA_ID_SAT_LOG);
            }
            else if (str_ieq(command, "DIGIPEATER"))
            {
                Send_Command_Ex(CMD_DIGIPEATER, SUBSYS_COM, DATA_ID_DIGIPEATER);
            }
            else if (str_ieq(command, "RETRANSMIT"))
            {
                Send_Command(CMD_REQUEST_RETRANSMIT);
            }
            else if (str_ieq(command, "ABORT"))
            {
                Send_Command(CMD_ABORT_MISSION);
            }
            else if (str_starts_ieq(command, "TX "))
            {
                uint8_t payload[107]; /* Protocol_CreatePacket's real per-frame payload cap */
                int n = hex_decode(command + 3, payload, sizeof(payload));
                if (n < 0)
                {
                    uart2_puts("ERR bad hex (need an even number of 0-9/a-f characters, "
                               "e.g. TX 48656C6C6F)\r\n");
                }
                else
                {
                    uint8_t frame[AX25_MAX_FRAME_SIZE];
                    uint16_t frame_len = Protocol_CreatePacket(frame, payload, (uint16_t)n, &GroundStationProfile);
                    if (frame_len == 0)
                    {
                        uart2_puts("ERR payload too large for one frame\r\n");
                    }
                    else
                    {
                        tx_busy = 1;
                        RadioApp_Send(frame, frame_len);
                        uint32_t timeout_cnt = TX_TIMEOUT_MS;
                        while (tx_busy != 0 && timeout_cnt > 0) { CPU2_Delay_Ms(1); timeout_cnt--; }
                        if (tx_busy != 0) { Radio.Standby(); tx_busy = 0; }
                        uart2_printf("OK sent (%d byte%s)\r\n", n, n == 1 ? "" : "s");
                    }
                }
            }
            else if (str_starts_ieq(command, "POWER "))
            {
                int dbm = atoi(command + 6);
                RadioApp_SetTxPower((int8_t)dbm);
            }
            else
            {
                uart2_printf("UNKNOWN COMMAND : %s\r\n", command);
                uart2_puts("Available: COMMAND HEARTBEAT HEALTH HOUSEKEEPING BEACON1 BEACON2 "
                           "ADCS CAMERA DOWNLOAD SATLOG DIGIPEATER RETRANSMIT ABORT "
                           "TX <hex> POWER <dbm>\r\n");
            }

            uart2_puts("\r\n> ");
        }
    }

    return 0;
}
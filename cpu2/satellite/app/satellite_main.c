#include "obc_shared.h"
#include "s2s2_config.h"
#include "obc_led.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>

#include "stm32wlxx_hal.h"
#include "radio.h"
#include "subghz.h"
#include "radio_board_if.h"
#include "uart_debug.h"

#include "config.h"
#include "radio_app.h"
#include "radio_driver.h"
#include "protocol.h"
#include "seal_wrap.h"
#include "ax25.h"
#include "g3ruh.h"

#include "satellite_app.h"
#include "Sring_buffer.h"

#define ABS(x) ((x) < 0 ? -(x) : (x))

/* Compatibility alias for uart2 functions */
#ifndef uart2_puts
#define uart2_puts   uart1_puts
#endif
#ifndef uart2_printf
#define uart2_printf uart1_printf
#endif

const RadioConfig_t SatelliteProfile = {
    .txFrequency = DEFAULT_TX_FREQUENCY_HZ,
    .rxFrequency = DEFAULT_RX_FREQUENCY_HZ,
    .sourceCallsign = S2S2_SAT_CALLSIGN,   /* identity and radio parameters come from generated/s2s2_config.h */
    .sourceSSID = S2S2_SAT_SSID,
    .destCallsign = S2S2_GS_CALLSIGN,
    .destSSID = S2S2_GS_SSID,
    .isSatelliteMode = true
};

#define BURST_PACKET_COUNT      50
#define TX_TIMEOUT_MS           1000
/* 50-second command listen window between CW1 and CW2 as requested. */
#define SAT_LISTEN_DURATION_MS  (50UL * 1000UL)  /* nominal listen time (log text only) */
/* CHANGED: the CW beacon starts every SAT_BEACON_PERIOD_MS, measured from the START of the previous beacon
 * (was: 50 s of listening after each beacon ENDED, so the period drifted 79-86 s, and an OBC image transfer
 * held the beacon off for as long as it ran). Now the beacon is NOT blocked by an image transfer: when it is
 * due, the chunk in flight finishes, the beacon is sent, and the transfer resumes (CPU1 waits for tx_done_seq
 * with a 90 s timeout; the beacon is <= 36 s). Only a GS command session (hold, below) still defers it. */
#define SAT_BEACON_PERIOD_MS    S2S2_BEACON_PERIOD_MS
#define SAT_CMD_HOLD_TIME_MS    (60UL * 1000UL)  /* 60 seconds minimum hold after command */
/* Timeout for M4 to send first flash/camera chunk via ring buffer */
#define SAT_M4_RESPONSE_TIMEOUT_MS  (5000UL)     /* 5 seconds max wait for M4 ack */

/* Strict command validation:
 *   0 = accept ONLY a standard AX.25 FCS (CRC-16/X.25, residue 0xF0B8)
 *   1 = accept BOTH standard AX.25 X.25 and CCITT-FALSE CRC            */
#define SAT_ALLOW_CRC_METHOD2   1

/* Longest ASCII command accepted as a valid payload */
#define SAT_MAX_ASCII_CMD_LEN   32

/*
 * ============================================================================
 * SATELLITE THREE-STATE MISSION ARCHITECTURE
 * Sequence: CW1 -> 90s Command Listen -> CW2 -> 90s Command Listen (repeating)
 *
 * 1. STATE_CW            : Transmits Morse CW Telemetry (alternating CW1 and CW2)
 * 2. STATE_COMMAND_LISTEN: 90-Second Active RX Listening for Telecommands
 * 3. STATE_DOWNLINK      : Sends ACKs, downlinks requested Flash / HK / Cam / Burst
 * ============================================================================
 */
typedef enum {
    STATE_CW = 0,
    STATE_COMMAND_LISTEN,
    STATE_DOWNLINK,
} AppState_t;

volatile AppState_t app_state = STATE_CW;

static volatile bool s_cmd_service_active = false;
static volatile uint32_t s_last_cmd_time_ms = 0;
static uint32_t listen_start_tick = 0;
static uint32_t s_beacon_start_tick = 0;   /* HAL tick at which the last beacon started (period reference) */
static uint32_t s_cw_test_seen = 0;        /* last CWTEST request already handled (initialised in main) */
static uint8_t s_cw_step = 0; /* 0: CW1 next, 1: CW2 next */

static uint8_t s_sat_prev_rx[AX25_MAX_FRAME_SIZE];
static uint16_t s_sat_prev_len = 0;

/* RX working buffers - static to keep them off the small CPU2 stack */
static uint8_t s_rx_raw[AX25_MAX_FRAME_SIZE];
static uint8_t s_rx_stream[AX25_MAX_FRAME_SIZE * 2];
static uint8_t s_rx_decoded[AX25_MAX_FRAME_SIZE];

/* Pending command buffer passed from STATE_COMMAND_LISTEN to STATE_DOWNLINK */
static uint8_t  s_pending_cmd[AX25_MAX_FRAME_SIZE];
static uint16_t s_pending_cmd_len = 0;
static uint8_t  s_pending_err = 0;   /* ADDED: NACK error code for a frame that was received but cannot be executed (0 = none) */
static char     s_pending_src_cs[7] = S2S2_GS_CALLSIGN;

/* Telemetry data buffers populated via M4 IPC */
static int16_t b1_data[5] = { 400, 400, 250, 250, 250 }; /* 4.00V, 4.00V, 25.0C, 25.0C, 25.0C */
static int16_t b2_data[5] = { 100, 100, 100, 1, 2 };     /* 1.00A, 1.00A, 1.00A, Flag1=1, Flag2=2 */

static char cwBeaconlive_msg1[64];
static char cwBeaconlive_msg2[64];

/*
 * ADDED: CW beacon bytes per the "CW BEACON 1 / 2" sheet, sent as hex digits in four groups of four ("XXXX XXXX XXXX XXXX"),
 * after the project name and call sign ("S2S2 9N2SJ", see SAT_CFG_DEFAULT_CW_CALLSIGN). The sheet defines the bit fields but
 * not how they become Morse characters; hex digits were chosen. Values come from the M4 telemetry (b1_data / b2_data); fields
 * for which the M4 delivers nothing (gyro, magnetometer, solar-cell sunshine bits) are sent as 0.
 * Beacon 1: byte 1 = system status (2 bit) | mission status (2) | time after reset in hours, saturating at 15 (4);
 *           byte 2 = SOL_TOTAL_I; bytes 3-4 = SOL_TOTAL_V; 5-6 = BAT_V; 7-8 = BAT_I (16-bit big endian, 0.01 V / 0.01 A).
 * Beacon 2: bytes 1-3 gyro x,y,z; 4-6 magnetometer x,y,z; 7 = solar cell bits (5); 8 = BAT_TEMP (whole degrees C).
 * The sheet has no beacon-number field, so the two beacons are told apart only by their alternation (1, 2, 1, 2 ...).
 */
#define CW_SYS_NORMAL 3U        /* "NORMAL MODE 1 1" of the sheet's SYSTEM STATUS table */
#define CW_MISSION_RESERVED 0U

static void Cw_Hex8(char *out, size_t n, const uint8_t b[8])
{
    snprintf(out, n, "%02X%02X %02X%02X %02X%02X %02X%02X", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
}

static uint8_t Cw_Clamp8(int32_t v)
{
    return (uint8_t)((v < 0) ? 0 : (v > 255) ? 255 : v);
}

static void Cw_Beacon1_Hex(char *out, size_t n)
{
    uint8_t b[8];
    uint32_t hours = HAL_GetTick() / 3600000UL;
    b[0] = (uint8_t)((CW_SYS_NORMAL << 6) | (CW_MISSION_RESERVED << 4) | ((hours > 15U) ? 15U : hours));
    b[1] = Cw_Clamp8(b2_data[1]);                          /* SOL_TOTAL_I */
    b[2] = (uint8_t)((uint16_t)b1_data[1] >> 8);           /* SOL_TOTAL_V */
    b[3] = (uint8_t)((uint16_t)b1_data[1] & 0xFFU);
    b[4] = (uint8_t)((uint16_t)b1_data[0] >> 8);           /* BAT_V */
    b[5] = (uint8_t)((uint16_t)b1_data[0] & 0xFFU);
    b[6] = (uint8_t)((uint16_t)b2_data[2] >> 8);           /* BAT_I */
    b[7] = (uint8_t)((uint16_t)b2_data[2] & 0xFFU);
    Cw_Hex8(out, n, b);
}

static void Cw_Beacon2_Hex(char *out, size_t n)
{
    uint8_t b[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };             /* gyro x,y,z, magnetometer x,y,z, solar cells: no data from the M4 */
    b[7] = (uint8_t)(int8_t)(b1_data[3] / 10);             /* BAT_TEMP, whole degrees C */
    Cw_Hex8(out, n, b);
}

/* true once at least one real B1/B2 packet has arrived from the M4 */
static bool s_live_telem_seen = false;

/* Max time to wait at boot for the first live ADC/IMU sample before CW1 */
#define SAT_FIRST_TELEM_WAIT_MS (20UL * 1000UL)

/*
 * ============================================================================
 * STRICT G3RUH / AX.25 RX VALIDATION
 *
 * Pipeline: on-air bytes -> G3RUH descramble -> NRZI -> HDLC flags/destuff
 *           (all inside Protocol_ExtractFrame) -> structural AX.25 checks
 *           -> FCS check.  Only a frame that passes EVERY step is a command.
 *           Raw on-air bytes are never interpreted as a command.
 *
 * Decoded frame layout (as returned by Protocol_ExtractFrame):
 *   [0]      0x7E start flag
 *   [1..7]   destination (6 chars << 1, SSID byte)
 *   [8..14]  source      (6 chars << 1, SSID byte, bit0 = 1 = last address)
 *   [15]     control  = 0x03 (UI)
 *   [16]     PID      = 0xF0
 *   [17..]   payload
 *   [len-3..len-2] FCS
 *   [len-1]  0x7E end flag
 * ============================================================================
 */
static inline __attribute__((unused)) bool Sat_Callsign_Char_OK(uint8_t shifted)
{
    char c = (char)(shifted >> 1);
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c == ' ');
}
/*
 * Telecommand classification (13-byte and 8-byte 0x53 frames). Used BOTH when the command
 * is first validated and in STATE_DOWNLINK.
 */
/* CHANGED: strict opcode table of the "Packet Format" command sheet (13 bytes: SAT-ID 53, MCU ID, opcode B1..B3, DATA_ID,
 * ADDRESS, NO OF PACKETS). Everything else (the old loose data-id / mcu-id matches, ping, dummy camera, ADCS, EPDM) is an
 * invalid command and is answered with NACK 0xEE 0x09.
 *   53 04 CC 5E BD  run camera mission      53 01 1D D2 F5  download image (CAM DOWNLOAD)
 *   53 01 1D D1 F2  HEALTH SAT (sat_health) 53 01 1D D1 F8  housekeeping download
 *   53 01 DE EF AA  antenna deployment */
typedef struct {
    bool flash, cam_on, cam_download, hk, antenna;
} SatTcClass_t;

static void Sat_Classify_Telecommand(const uint8_t *p, uint16_t plen, SatTcClass_t *c)
{
    memset(c, 0, sizeof(*c));
    if (p == NULL || plen != CMD_PAYLOAD_LEN || p[0] != 0x53) return;

    const bool obc = (p[1] == 0x01);
    const bool cam = (p[1] == CMD_TYPE_CAMERA);
    c->cam_on       = cam && p[2] == 0xCC && p[3] == 0x5E && p[4] == 0xBD;
    c->cam_download = obc && p[2] == 0x1D && p[3] == 0xD2 && p[4] == 0xF5;
    c->hk           = obc && p[2] == 0x1D && p[3] == 0xD1 && p[4] == 0xF2;
    c->flash        = obc && p[2] == 0x1D && p[3] == 0xD1 && p[4] == 0xF8;
    c->antenna      = obc && p[2] == 0xDE && p[3] == 0xEF && p[4] == 0xAA;
}

static bool Sat_Tc_Is_Known(const SatTcClass_t *c)
{
    return c->flash || c->cam_on || c->cam_download || c->hk || c->antenna;
}

static bool Sat_Tc_Is_Valid(const SatTcClass_t *c, const uint8_t *p, uint16_t plen)
{
    /* any 0x53 frame is processed (a wrong length gets NACK "incomplete packet", an unknown opcode NACK "invalid command") */
    if (p != NULL && p[0] == 0x53) return true;
    return Sat_Tc_Is_Known(c);
}

/* HDLC destuff can leave 1 extra byte after a 13 B command. Hunt 0x53 and trim. */
static bool Sat_Extract_Telecommand(const uint8_t *pl, uint16_t plen, uint8_t *out, uint16_t *out_len)
{
    if (!pl || !out || !out_len || plen == 0) return false;

    for (uint16_t i = 0; i < plen; i++) {
        if (pl[i] != 0x53) continue;
        uint16_t remain = (uint16_t)(plen - i);
        if (remain >= CMD_PAYLOAD_LEN) {
            memcpy(out, &pl[i], CMD_PAYLOAD_LEN);
            *out_len = CMD_PAYLOAD_LEN;
            return true;
        }
    }
    return false;
}

/*
 * Try to parse an ASCII-HEX telecommand string from incoming payload,
 * e.g. "HHEX 53 04 CC 5E BE 00 D0 00 00 00 00 00 45" or "53 04 CC 5E BE..."
 * Returns true if valid 13-byte or 8-byte 0x53 telecommand extracted.
 */
static bool Sat_Try_Parse_Ascii_Hex_Command(const uint8_t *pl, uint16_t plen, uint8_t *out_bin, uint16_t *out_len)
{
    if (!pl || plen < 2 || !out_bin || !out_len) return false;

    const char *p = (const char *)pl;
    const char *end = p + plen;

    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '\"' || *p == '\'')) p++;

    /* Skip optional prefix: one or more 'H'/'h' followed by "EX", e.g. "HEX", "HHEX", "HHHEX" */
    while (p < end && (*p == 'H' || *p == 'h')) {
        if ((end - p) >= 4 &&
            (p[1] == 'E' || p[1] == 'e') &&
            (p[2] == 'X' || p[2] == 'x') &&
            (p[3] == ' ' || p[3] == ':' || p[3] == '_')) {
            p += 4;
            break;
        } else if ((end - p) >= 3 &&
                   (p[1] == 'E' || p[1] == 'e') &&
                   (p[2] == 'X' || p[2] == 'x')) {
            p += 3;
            break;
        }
        p++;
    }

    while (p < end && (*p == ' ' || *p == '\t' || *p == ':' || *p == '=')) p++;

    uint16_t count = 0;
    while (p < end && count < CMD_PAYLOAD_LEN) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '\"' || *p == '\'')) p++;
        if (p >= end) break;
        if (p[0] == '0' && (p + 1 < end) && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
        }
        if (p >= end) break;

        char c1 = *p++;
        int val1 = -1;
        if (c1 >= '0' && c1 <= '9') val1 = c1 - '0';
        else if (c1 >= 'a' && c1 <= 'f') val1 = c1 - 'a' + 10;
        else if (c1 >= 'A' && c1 <= 'F') val1 = c1 - 'A' + 10;
        else break;

        if (p < end && (*p != ' ' && *p != ',' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '\"' && *p != '\'')) {
            char c2 = *p++;
            int val2 = -1;
            if (c2 >= '0' && c2 <= '9') val2 = c2 - '0';
            else if (c2 >= 'a' && c2 <= 'f') val2 = c2 - 'a' + 10;
            else if (c2 >= 'A' && c2 <= 'F') val2 = c2 - 'A' + 10;
            else break;
            out_bin[count++] = (uint8_t)((val1 << 4) | val2);
        } else {
            out_bin[count++] = (uint8_t)val1;
        }
    }

    if (count == CMD_PAYLOAD_LEN && out_bin[0] == 0x53) {
        *out_len = count;
        return true;
    }
    return false;
}

static bool Sat_Validate_Frame(uint8_t *f, uint16_t len, const char **why)
{
    if (len < 19) { *why = "too short for AX.25 frame (<19B)"; return false; }

    if (f[0] != AX25_FLAG || f[len - 1] != AX25_FLAG) {
        *why = "missing 0x7E start/end flag";
        return false;
    }

    /* 1. Command-Centric Validation (ANY Radio / ANY GS):
     * If the frame contains a valid 0x53 CubeSat Telecommand (13-byte or 8-byte),
     * accept it immediately from ANY transmitter, regardless of GS callsign.
     */
    for (uint16_t off = 14; off <= 18 && (off + 8) <= len; off++) {
        if (f[off] == 0x53) {
            uint8_t *pl = &f[off];
            uint16_t plen = (len > (off + 3)) ? (len - 3 - off) : 0;
            SatTcClass_t tcc;
            Sat_Classify_Telecommand(pl, plen, &tcc);
            if (Sat_Tc_Is_Valid(&tcc, pl, plen)) {
                uart1_puts(" [VALIDATE] Verified 0x53 CubeSat Telecommand detected in frame!\r\n");
                if (off != 17 && (17 + plen) <= len) {
                    memmove(&f[17], &f[off], plen);
                }
                f[15] = 0x03;
                f[16] = 0xF0;
                return true;
            }
        }
    }

    /* Check if frame payload contains an ASCII-HEX telecommand */
    for (uint16_t off = 14; off <= 18 && (off + 8) <= len; off++) {
        uint8_t *pl = &f[off];
        uint16_t plen = (len > (off + 3)) ? (len - 3 - off) : 0;
        uint8_t bin_cmd[CMD_PAYLOAD_LEN];
        uint16_t bin_len = 0;
        if (Sat_Try_Parse_Ascii_Hex_Command(pl, plen, bin_cmd, &bin_len)) {
            if (bin_len == CMD_PAYLOAD_LEN && bin_cmd[0] == 0x53) {
                uart1_puts(" [VALIDATE] Verified ASCII-HEX 0x53 Telecommand detected in frame!\r\n");
                f[15] = 0x03;
                f[16] = 0xF0;
                return true;
            }
        }
    }

    /* 2. Standard clean AX.25 UI frame with valid CRC (Method 1 residue or Method 2 computed) */
    if (f[15] == 0x03 && f[16] == 0xF0 &&
        (AX25_VerifyCRC_Method1(f, len) || AX25_VerifyCRC_Method2(f, len, NULL, NULL, NULL))) {
        return true;
    }

    /* 3. Resilient repair path for weak RF signals */
    uint8_t test_f[AX25_MAX_FRAME_SIZE];
    if (len <= sizeof(test_f)) {
        memcpy(test_f, f, len);
        test_f[8]  = 'G' << 1;
        test_f[9]  = 'R' << 1;
        test_f[10] = 'O' << 1;
        test_f[11] = 'U' << 1;
        test_f[12] = 'N' << 1;
        test_f[13] = 'D' << 1;
        test_f[14] = 0x61;
        test_f[15] = 0x03;
        test_f[16] = 0xF0;

        if (AX25_VerifyCRC_Method1(test_f, len) || AX25_VerifyCRC_Method2(test_f, len, NULL, NULL, NULL)) {
            memcpy(f, test_f, len);
            uart1_puts(" [VALIDATE-REPAIR] Recovered frame with clean FCS!\r\n");
            return true;
        }
    }

    *why = "CRC/FCS error or invalid control/PID (not AX.25 UI)";
    return false;
}

/*
 * Protocol extraction wrapper: attempts extraction on single packet, then joined buffer if needed.
 */
static bool Sat_Extract_G3RUH_Frame(const uint8_t *raw, uint16_t raw_len,
                                    uint8_t *out, uint16_t out_cap, uint16_t *out_len,
                                    bool *was_joined)
{
    uint16_t n = 0;
    *was_joined = false;

    if (Protocol_ExtractFrame(raw, raw_len, out, out_cap, &n)) {
        *out_len = n;
        return true;
    }

    if (s_sat_prev_len > 0) {
        memcpy(&s_rx_stream[0], s_sat_prev_rx, s_sat_prev_len);
        memcpy(&s_rx_stream[s_sat_prev_len], raw, raw_len);
        n = 0;
        if (Protocol_ExtractFrame(s_rx_stream, (uint16_t)(s_sat_prev_len + raw_len), out, out_cap, &n)) {
            *out_len = n;
            *was_joined = true;
            return true;
        }
    }

    return false;
}

static inline __attribute__((unused)) bool Sat_Dest_Is_Ours(const char *dest_cs)
{
    return strcmp(dest_cs, SatelliteProfile.sourceCallsign) == 0 ||
           strcmp(dest_cs, S2S2_SAT_CALLSIGN) == 0 ||
           strcmp(dest_cs, S2S2_GS_CALLSIGN) == 0 ||
           strcmp(dest_cs, "CQ") == 0 ||
           strcmp(dest_cs, "BEACON") == 0;
}

static bool Sat_Payload_Is_Printable(const uint8_t *p, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        if (p[i] < 0x20 || p[i] >= 0x7F) return false;
    }
    return true;
}




/*
 * Forward a validated telecommand to the M4:
 *   1) write it into ring buffer 2  (data must be there BEFORE the M4 wakes)
 *   2) IPCC notify on IPCC_CH_COMMAND
 * Done immediately on a valid command, before any ACK is transmitted.
 */
static void Sat_Forward_Command_To_M4(const uint8_t *cmd, uint16_t len)
{
    struct rx_command_s rx_cmd;
    memset(&rx_cmd, 0, sizeof(rx_cmd));
    rx_cmd.len = CMD_PAYLOAD_LEN;
    uint16_t copy_len = (len < CMD_PAYLOAD_LEN) ? len : CMD_PAYLOAD_LEN;
    memcpy(rx_cmd.cmd, cmd, copy_len);
    rx_cmd.crc16 = 0;

    rb_rx_write(&rx_cmd);              /* 1. ring buffer */
    ipcc_m0_send(IPCC_CH_COMMAND);     /* 2. IPCC notify M4 */
    uart1_puts("[M0+ -> M4] Command written to RX ring buffer, IPCC notify sent\r\n");
}

static void M0_Display_CW_Signal(const char *beacon_name, const char *callsign, const char *msg, uint32_t cycle)
{
    uart1_puts("\r\n========================================================================\r\n");
    uart1_printf(">>> [STATE_CW | CYCLE #%lu] TRANSMITTING MORSE CW SIGNAL: %s <<<\r\n", (unsigned long)cycle, beacon_name);
    uart1_puts("========================================================================\r\n");
    uart1_puts("  Downlink RF Carrier : 435.000 MHz (CW OOK Morse Keying)\r\n");
    uart1_puts("  Morse Speed         : 15 WPM (Dit = 80ms, Dah = 240ms)\r\n");
    uart1_puts("  RF Front-End State  : 3.3V External Power Amplifier Active (PC2=1)\r\n");
    uart1_printf("  Transmitted Callsign: \"%s\"\r\n", callsign);
    uart1_printf("  Live Telemetry Text : \"%s\"\r\n", msg);
    uart1_puts("========================================================================\r\n");
}

/* All TX goes through RadioApp_Send() - same shared arm sequence as
   RX. No direct Radio.* calls anywhere in this file. */
static bool Radio_Send_And_Wait(uint8_t *buffer, uint16_t size, uint32_t timeout_ms) {
    const bool pa5v_on =
        (HAL_GPIO_ReadPin(AMP_5V_EN_PORT, AMP_5V_EN_PIN) == GPIO_PIN_SET) &&
        (HAL_GPIO_ReadPin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN) == GPIO_PIN_SET);
    if (!pa5v_on) {
        Satellite_GMSK_Prepare();
    }
    tx_busy = 1;
    RadioApp_Send(buffer, size);

    uint32_t start_ms = HAL_GetTick();
    while (tx_busy != 0) {
        if ((HAL_GetTick() - start_ms) > timeout_ms) {
            uart1_puts("ERROR: TX timeout\r\n");
            tx_busy = 0;
            if (!Satellite_GetDCDCHold()) {
                HAL_GPIO_WritePin(AMP_5V_EN_PORT, AMP_5V_EN_PIN, GPIO_PIN_RESET);
                HAL_GPIO_WritePin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN, GPIO_PIN_RESET);
                HAL_GPIO_WritePin(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN, GPIO_PIN_SET);
                Satellite_SetRFSwitch(RBI_SWITCH_RX);
            }
            return false;
        }
    }
    CPU2_Delay_Ms(2);
    if (Satellite_GetDCDCHold()) {
        return true;
    }
    /* Deassert 5V PA (PC3) and 5V DC/DC (PA0); 3.3V PA (PC2) back ON; return RF switch to RX */
    HAL_GPIO_WritePin(AMP_5V_EN_PORT, AMP_5V_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN, GPIO_PIN_SET);
    Satellite_SetRFSwitch(RBI_SWITCH_RX);
    return true;
}

/* 50x Telemetry Burst Stream Routine */
static void Transmit_Burst_50(void) {
    uint8_t tx_data[90] = "Namaste Everyone! From Antarikchya Nepal. S2S-2 CubeSat Beacon Communication Test";

    uint8_t frame_buffer[AX25_MAX_FRAME_SIZE];
    uint16_t frame_size = Protocol_CreatePacket(frame_buffer, tx_data, (uint16_t)strlen((const char *)tx_data), &SatelliteProfile);
    if (frame_size == 0) return;

    memcpy(last_tx_scrambled, frame_buffer, frame_size);
    last_tx_len = frame_size;

    char size_msg[80];
    snprintf(size_msg, sizeof(size_msg), "BURST START: %d packets, %d bytes each, scrambled\r\n", BURST_PACKET_COUNT, frame_size);
    uart1_puts(size_msg);

    uart1_puts(" TX plaintext payload : \"");
    uart1_puts((const char *)tx_data);
    uart1_puts("\"\r\n");

    /* Print unscrambled AX.25 frame starting with 0x7E */
    uint8_t ax25_clean[AX25_MAX_FRAME_SIZE];
    uint16_t ax25_len = Protocol_GetLastTxAx25Frame(ax25_clean, sizeof(ax25_clean));
    if (ax25_len > 0) {
        uart1_puts("\r\n========================================================================\r\n");
        uart1_printf(">>> [AX.25 TX FRAME (Starts with 0x7E, %u Bytes, Unscrambled)] <<<\r\n", ax25_len);
        uart1_puts("========================================================================\r\n");
        Print_Hex_Bytes(ax25_clean, ax25_len);
        Print_AX25_Fields("TX AX.25 FRAME", ax25_clean, ax25_len);
        uart1_puts("========================================================================\r\n");
    }

    uart1_printf(">>> [TX ON-AIR G3RUH SCRAMBLED BYTES (%u Bytes)] <<<\r\n", frame_size);
    Print_Hex_Bytes(frame_buffer, frame_size);

    uint32_t sent_ok = 0, sent_timeout = 0;
    for (int i = 0; i < BURST_PACKET_COUNT; i++) {
        bool ok = Radio_Send_And_Wait(frame_buffer, frame_size, TX_TIMEOUT_MS);
        if (ok) sent_ok++; else sent_timeout++;
        char pkt_msg[48];
        snprintf(pkt_msg, sizeof(pkt_msg), " TX packet %2d/%d : %s\r\n", i + 1, BURST_PACKET_COUNT, ok ? "sent OK" : "TIMEOUT");
        uart1_puts(pkt_msg);
        CPU2_Delay_Ms(100);
    }

    char done_msg[96];
    snprintf(done_msg, sizeof(done_msg), "BURST COMPLETE (%lu/%d sent cleanly, %lu timed out)\r\n", (unsigned long)sent_ok, BURST_PACKET_COUNT, (unsigned long)sent_timeout);
    uart1_puts(done_msg);
}

/*
 * ============================================================================
 * PERIODIC BEACON POLICY:
 * - NO AUTOMATIC GMSK BEACONS ARE TRANSMITTED.
 * - Periodic beacons are STRICTLY Morse CW1 and CW2 (OOK @ 15 WPM, 3.3V PA).
 * - GMSK (4800 bps, 5V PA, 435.000 MHz) is reserved SOLELY for:
 *     1. Command Acknowledgment (ACK Accepted [0xAC, 0x00, mission_id])
 *     2. Command Completion (ACK Completed [0xAC, 0x01, mission_id])
 *     3. Error Notification (NACK [0xEE, err_code, mission_id])
 *     4. Mission Data Downlink (Camera photos, Flash dumps, Requested HK)
 * ============================================================================
 */

/*
 * CPU1's OBC app (nuttx apps/examples/s2s2obc) keeps the image in its internal flash and, on request
 * from the PC, reads one chunk out of it into the shared flash ring (ipc/Sring_buffer.h). This takes
 * the chunk from the ring, transmits it over RF as a 0xCAFE camera packet and reports "done" back in
 * the shared OBC block (ipc/obc_shared.h). It only runs while CPU1 has a transfer session open, so
 * the GS-commanded CAM download path is untouched.
 */
/* keep_going() for Satellite_Gmsk_Hello_Stream: the CWTEST request is still the current one, not stopped, not timed out */
static uint32_t s_cw_test_t0, s_cw_test_secs;
static bool cw_test_continue(void)
{
    struct obc_shared_s *o = OBC_SHARED;
    if (o->cw_test_req != s_cw_test_seen || o->cw_test_s == 0U) return false;
    if ((HAL_GetTick() - s_cw_test_t0) >= s_cw_test_secs * 1000UL) return false;
    o->cpu2_beat++;                                  /* keep the heartbeat alive while streaming */
    return true;
}

static void Obc_Ring_Service(void)
{
    struct obc_shared_s *o = OBC_SHARED;

    o->cpu2_beat++;
    Obc_Led_Tick();

    if (o->reset_req != o->reset_ack) {              /* the GS counter restarted: forget the replay floor */
        Seal_ResetRxFloor();
        s_cmd_service_active = false;
        s_pending_cmd_len = 0;
        o->reset_ack = o->reset_req;
    }
    if (o->cw_test_req != s_cw_test_seen && !s_cmd_service_active) {
        /* ADDED: continuous CW "HELLO" for a spectrum analyzer (VCP command CWTEST, seconds = o->cw_test_s, 0 = stop).
         * Repeats [1 s carrier, HELLO at 80 ms/unit (15 WPM), 1.5 s quiet] until CPU1 clears cw_test_s, a new command
         * arrives, or the time is up. 80 ms keeps the dahs (240 ms) below the ground station's tuning-carrier
         * threshold (300 ms), so the GS decodes "HELLO" too. The heartbeat is kept running. */
        s_cw_test_seen = o->cw_test_req;
        uint32_t secs = (o->cw_test_s > 43200U) ? 43200U : o->cw_test_s;   /* 12 h maximum */
        if (secs != 0U) {
            uint32_t t0 = HAL_GetTick();
            o->cw_test_words = 0;
            o->cw_test_run = 1;
            Obc_Led(OBC_LED_YELLOW);
            if (o->cw_test_mode == 1U) {
                /* "HELLO" in standard GMSK AX.25 packets, back to back (spectrum analyzer view of the real signal) */
                s_cw_test_t0 = t0;
                s_cw_test_secs = secs;
                o->cw_test_words = Satellite_Gmsk_Hello_Stream(cw_test_continue);
            } else {
                Satellite_CW_Test_Begin();
                while (o->cw_test_req == s_cw_test_seen && o->cw_test_s != 0U && (HAL_GetTick() - t0) < secs * 1000UL) {
                    Satellite_CW_Test_Cycle(80U, 1000U);
                    o->cw_test_words++;
                    o->cpu2_beat++;
                    Obc_Led(OBC_LED_YELLOW);
                }
                Satellite_CW_Test_End();
            }
            RadioApp_StartRx();
            o->cw_test_run = 0;
        }
        return;
    }
    if (o->rate_seq != o->rate_done_seq && !s_cmd_service_active) {
        /* ADDED: raw air-rate test requested by CPU1 (VCP command RATE). Blocks for count * (0.37 s + gap), so the 90 s
         * beacon is late by at most that long; it is a lab test, keep count * 0.4 s well below 90 s. */
        uint32_t n = (o->rate_n > 1000U) ? 1000U : o->rate_n;
        uint32_t elapsed = 0, sent = 0;
        Obc_Led(OBC_LED_YELLOW);
        (void)Satellite_Rate_Test((uint16_t)n, o->rate_gap_ms, &elapsed, &sent);
        Obc_Led(OBC_LED_YELLOW);
        RadioApp_StartRx();
        o->rate_sent = sent;
        o->rate_elapsed_ms = elapsed;
        o->rate_done_seq = o->rate_seq;                  /* written last: CPU1 waits for it */
        return;
    }
    if (o->session == 0U || s_cmd_service_active) {
        return;
    }
    /* CHANGED: streaming chunks no longer refreshes s_last_cmd_time_ms. That stamp is the "a GS command was just
     * served, keep the CW beacon off" hold; using it here suppressed the mandatory beacon for the whole transfer
     * (and, written as HAL_GetTick() | 1U, it also underflowed on even ticks and let the beacon in at random). */

    struct flash_chunk_s c;
    if (flash_data_read(&c)) {
        uint8_t len = (c.data_len > CAM_DATA_LEN) ? (uint8_t)CAM_DATA_LEN : (uint8_t)c.data_len;
        Obc_Led(OBC_LED_YELLOW);                      /* transmitting */
        bool ok = Satellite_Send_Camera_Chunk(c.pkt_idx, c.total_pkts, c.data, len);
        Obc_Led(OBC_LED_YELLOW);
        RadioApp_StartRx();                           /* back to listening for GS commands */
        o->tx_status = ok ? OBC_TXST_OK : OBC_TXST_TXFAIL;
        o->tx_done_seq = (uint32_t)c.pkt_idx + 1U;    /* written last: CPU1 waits for this */
    }
}

int main(void) {
    /* 1. Low-level CPU2 hardware, clock, and RF switch init */
    Satellite_Hardware_Init();
    Satellite_Init(NULL);
    IPCC_Handshake();

    const SatelliteConfig_t *cfg = Satellite_GetConfig();

    uart1_puts("\r\n======================================================================\r\n");
    uart1_puts("     STM32WL55 AX.25 + G3RUH 3-STATE MISSION EXECUTIVE READY          \r\n");
    uart1_puts("     FW-ID: SAT-G3RUH-UPLINK7                                         \r\n");
    uart1_puts("     RX: 437.375 MHz | raw then decode then validate | 13B 0x53 trim  \r\n");
    uart1_puts("     CYCLE: CW1 -> LISTEN -> CW2 -> LISTEN                            \r\n");
    uart1_puts("======================================================================\r\n");

    RadioEvents_t events = {
        .TxDone = OnTxDone,
        .TxTimeout = OnTxTimeout,
        .RxDone = OnRxDone,
        .RxTimeout = OnRxTimeout,
        .RxError = OnRxError
    };

    RadioApp_Init(&SatelliteProfile, &events);
    RadioApp_SetPaSelect(SAT_PA_SEL);
    RadioApp_StartRx();

    /* Wait for the first LIVE ADC1/ADC2/IMU telemetry from the M4 (obc_main)
       so CW1 does not key the hard-coded defaults. */
    {
        uart1_printf("[M0+] Waiting up to %lu s for first live ADC1/ADC2/IMU telemetry from M4...\r\n",
                     (unsigned long)(SAT_FIRST_TELEM_WAIT_MS / 1000UL));
        uint32_t wait_start = HAL_GetTick();
        while ((HAL_GetTick() - wait_start) < SAT_FIRST_TELEM_WAIT_MS) {
            if (Satellite_Drain_Telemetry(b1_data, b2_data)) {
                s_live_telem_seen = true;
                CPU2_Delay_Ms(200);                        /* let B2 follow B1 */
                Satellite_Drain_Telemetry(b1_data, b2_data);
                break;
            }
            CPU2_Delay_Ms(50);
        }
        uart1_printf("[M0+] First telemetry: %s\r\n",
                     s_live_telem_seen ? "LIVE data received from M4"
                                       : "TIMEOUT - using defaults (is obc_main running on M4?)");
    }

    uint32_t cw_cycle = 0;
    s_cw_step = 0; /* Start with CW1 */
    app_state = STATE_CW;

    /* ADDED: initialise the CPU1<->CPU2 handshake block (0x20008F00). The magic is written LAST so CPU1
     * only trusts the block once every field is valid; reset_ack = reset_req so a stale reset request
     * left in SRAM2 by the previous run is not replayed. cpu2_beat then counts up in Obc_Ring_Service()
     * and is what CPU1's STATUS command uses to tell that CPU2 is really running. */
    OBC_SHARED->cpu2_beat = 0;
    OBC_SHARED->tx_done_seq = 0;
    OBC_SHARED->tx_status = OBC_TXST_OK;
    OBC_SHARED->reset_ack = OBC_SHARED->reset_req;
    s_cw_test_seen = OBC_SHARED->cw_test_req;             /* ADDED: a CWTEST request left over from before a reboot is not run */
    OBC_SHARED->cw_test_run = 0;
    OBC_SHARED->rate_done_seq = OBC_SHARED->rate_seq;     /* ADDED: a rate request left over from before a reboot is not run */
    OBC_SHARED->magic = OBC_SHARED_MAGIC;

    while (1) {
        Obc_Ring_Service();   /* ADDED: CPU1 ring -> RF; cheap no-op unless CPU1 has a session open */

        /* Update telemetry from M4 shared ring buffer every iteration */
        if (Satellite_Drain_Telemetry(b1_data, b2_data)) {
            s_live_telem_seen = true;
        }

        switch (app_state) {
            /*
             * ================================================================
             * STATE 1: STATE_CW (Morse CW Telemetry Beacon Transmission)
             * ================================================================
             */
            case STATE_CW: {
                uint32_t now = HAL_GetTick();

                /* If a command was serviced recently, hold/stuck CW beacons so Ground Station has full uplink access */
                if (s_cmd_service_active || (s_last_cmd_time_ms != 0 && (now - s_last_cmd_time_ms < SAT_CMD_HOLD_TIME_MS))) {
                    uint32_t rem_hold = (SAT_CMD_HOLD_TIME_MS > (now - s_last_cmd_time_ms))
                                        ? (SAT_CMD_HOLD_TIME_MS - (now - s_last_cmd_time_ms)) : 0;
                    uart1_printf(">>> [M0+] CW PAUSED (Command Session Active | %lu s remaining in hold). Listening on 437.375 MHz... <<<\r\n",
                                 (unsigned long)(rem_hold / 1000UL));
                    listen_start_tick = HAL_GetTick();
                    RadioApp_StartRx();
                    app_state = STATE_COMMAND_LISTEN;
                    break;
                }

                cw_cycle++;
                s_beacon_start_tick = HAL_GetTick();   /* ADDED: period reference for the next beacon (SAT_BEACON_PERIOD_MS) */

                /* Pull the newest M4 sample right before keying */
                if (Satellite_Drain_Telemetry(b1_data, b2_data)) {
                    s_live_telem_seen = true;
                }
                uart1_printf("[M0+] CW telemetry source: %s\r\n",
                             s_live_telem_seen ? "LIVE (M4 ADC1/ADC2/IMU)" : "DEFAULT (no M4 data yet)");

                if (s_cw_step == 0) {
                    /* Format and transmit CW1: the 8 bytes of "CW BEACON 1" of the Packet Format sheet, as hex digits */
                    Cw_Beacon1_Hex(cwBeaconlive_msg1, sizeof(cwBeaconlive_msg1));

                    M0_Display_CW_Signal("CW1 (Voltages & Temperatures)", cfg->cwBeaconCallsign, cwBeaconlive_msg1, cw_cycle);
                    Satellite_Send_CW_Beacon(cw_cycle,
                                             "CW1",
                                             cfg->cwBeaconCallsign,
                                             cwBeaconlive_msg1,
                                             cfg->morseUnitMs,
                                             cfg->cwTuningCarrierDurationMs,
                                             cfg->cwTuningPostDelayMs,
                                             cfg->cwInterStringDelayMs);
                    s_cw_step = 1; /* Next CW will be CW2 */
                    uart1_printf("\r\n>>> [M0+] CW1 COMPLETE. ENTERING %lu S RX LISTEN <<<\r\n",
                                 (unsigned long)(SAT_LISTEN_DURATION_MS / 1000UL));
                } else {
                    /* Format and transmit CW2: the 8 bytes of "CW BEACON 2" of the Packet Format sheet, as hex digits */
                    Cw_Beacon2_Hex(cwBeaconlive_msg2, sizeof(cwBeaconlive_msg2));

                    M0_Display_CW_Signal("CW2 (Currents & Status Flags)", cfg->cwBeaconCallsign, cwBeaconlive_msg2, cw_cycle);
                    Satellite_Send_CW_Beacon(cw_cycle,
                                             "CW2",
                                             cfg->cwBeaconCallsign,
                                             cwBeaconlive_msg2,
                                             cfg->morseUnitMs,
                                             cfg->cwTuningCarrierDurationMs,
                                             cfg->cwTuningPostDelayMs,
                                             cfg->cwInterStringDelayMs);
                    s_cw_step = 0; /* Next CW will be CW1 */
                    uart1_printf("\r\n>>> [M0+] CW2 COMPLETE. ENTERING %lu S RX LISTEN <<<\r\n",
                                 (unsigned long)(SAT_LISTEN_DURATION_MS / 1000UL));
                }

                /* After CW, arm RX and enter 30s command listen window */
                listen_start_tick = HAL_GetTick();
                RadioApp_StartRx();
                uart1_printf(">>> [M0+] RX Listening armed on 437.375 MHz (%lu s window) <<<\r\n\r\n",
                             (unsigned long)(SAT_LISTEN_DURATION_MS / 1000UL));
                app_state = STATE_COMMAND_LISTEN;
                break;
            }

            /*
             * ================================================================
             * STATE 2: STATE_COMMAND_LISTEN (90s Active RX Telecommand Listening)
             *
             * A command is accepted ONLY after:
             *   G3RUH descramble -> NRZI -> HDLC destuff -> AX.25 UI structure
             *   -> FCS (CRC-16/X.25) valid -> addressed to us -> valid payload.
             * ================================================================
             */
            case STATE_COMMAND_LISTEN: {
                uint32_t now = HAL_GetTick();

                /* Check for pending IPCC Handshake from M4 */
                if (ipcc_m0_received(IPCC_CH_HANDSHAKE)) {
                    ipcc_m0_clear(IPCC_CH_HANDSHAKE);
                    ipcc_m0_send(IPCC_CH_HANDSHAKE);
                    uart1_puts("[M0+ IPC] Handshake acknowledged to M4!\r\n");
                }

                /* 1. Periodic Heartbeat Countdown Log so operator clearly sees the 90s listen countdown */
                static uint32_t s_last_listen_hb = 0;
                if (now - s_last_listen_hb >= 10000) {
                    s_last_listen_hb = now;
                    uint32_t elapsed = now - s_beacon_start_tick;   /* CHANGED: time since the beacon STARTED (90 s period) */
                    if (s_cmd_service_active || (s_last_cmd_time_ms != 0 && (now - s_last_cmd_time_ms < SAT_CMD_HOLD_TIME_MS))) {
                        uint32_t rem_hold = (SAT_CMD_HOLD_TIME_MS > (now - s_last_cmd_time_ms))
                                            ? (SAT_CMD_HOLD_TIME_MS - (now - s_last_cmd_time_ms)) : 0;
                        uart1_printf(">>> [M0+ RX LISTEN] Active on 437.375 MHz (CW PAUSED | %lu s remaining in hold) <<<\r\n",
                                     (unsigned long)(rem_hold / 1000UL));
                    } else if (elapsed < SAT_BEACON_PERIOD_MS) {
                        uint32_t rem = SAT_BEACON_PERIOD_MS - elapsed;
                        uart1_printf(">>> [M0+ RX] FW-ID SAT-G3RUH-UPLINK7 | raw then decode then validate | %lu s before %s <<<\r\n",
                                     (unsigned long)((rem + 999) / 1000UL),
                                     (s_cw_step == 1) ? "CW2" : "CW1");
                    }
                }

                /* 2. Handle radio timeout or error */
                if (rx_timeout_flag || rx_error_flag) {
                    rx_timeout_flag = 0;
                    rx_error_flag = 0;
                    if (!s_cmd_service_active) {
                        RadioApp_StartRx();
                    }
                }

                /* 3. Process incoming packet when received:
                 * STEP 1: READ RAW
                 * STEP 2: DECODE AX.25 G3RUH
                 * STEP 3: VALIDATE
                 * STEP 4: DETECT & DISPATCH COMMAND
                 */
                if (rx_done_flag) {
                    rx_done_flag = 0;

                    if (s_cmd_service_active) {
                        /* Active command in progress: do not receive any new command */
                        break;
                    }

                    /* -------------------------------------------------------------
                     * STEP 1: READ RAW (RF physical layer)
                     * ------------------------------------------------------------- */
                    uint16_t raw_len = rx_frame_size;
                    if (raw_len == 0 || raw_len > sizeof(s_rx_raw)) {
                        RadioApp_StartRx();
                        break;
                    }

                    memcpy(s_rx_raw, (const void *)rx_frame_buffer, raw_len);

                    /* Always print the raw capture first, then decode, then validate. */
                    uart1_printf("\r\n============================================================\r\n");
                    uart1_printf(">>> [STEP 1: READ RAW] RSSI=%d dBm, SNR=%d dB, %u Bytes <<<\r\n",
                                 (int)last_rx_rssi, (int)last_rx_snr, raw_len);
                    Print_Hex_Bytes(s_rx_raw, raw_len);

                    /* -------------------------------------------------------------
                     * STEP 2: DECODE AX.25 G3RUH (Descramble + NRZI + HDLC Unstuff)
                     * ------------------------------------------------------------- */
                    uart1_puts(">>> [STEP 2: DECODE AX.25 G3RUH] Descramble + NRZI + HDLC <<<\r\n");
                    uint16_t decoded_len = 0;
                    bool was_joined = false;
                    bool decode_ok = Sat_Extract_G3RUH_Frame(s_rx_raw, raw_len,
                                                             s_rx_decoded, sizeof(s_rx_decoded),
                                                             &decoded_len, &was_joined);

                    if (!decode_ok) {
                        memcpy(s_sat_prev_rx, s_rx_raw, raw_len);
                        s_sat_prev_len = raw_len;
                        uart1_puts(">>> [DECODE FAILED] no valid G3RUH/AX.25 frame (stay in continuous RX) <<<\r\n");
                        /* Do not RadioApp_StartRx() here — re-arming drops the next GS packet. */
                        break;
                    }

                    s_sat_prev_len = 0;
                    if (was_joined) {
                        uart1_puts(" [DECODE] AX.25 frame recovered by joining previous and current packets!\r\n");
                    }
                    uart1_printf(">>> [DECODE SUCCESS] Extracted AX.25 Frame (%u Bytes, Starts 0x7E, Ends 0x7E) <<<\r\n", decoded_len);
                    Print_Hex_Bytes(s_rx_decoded, decoded_len);
                    Print_AX25_Fields("G3RUH DECODED FRAME", s_rx_decoded, decoded_len);

                    /* -------------------------------------------------------------
                     * STEP 3: VALIDATE AX.25 FRAME (FCS, UI headers, Callsigns)
                     * ------------------------------------------------------------- */
                    uart1_puts(">>> [STEP 3: VALIDATE AX.25] Check Headers, Callsigns & FCS/CRC <<<\r\n");
                    const char *why_invalid = "unknown";
                    if (!Sat_Validate_Frame(s_rx_decoded, decoded_len, &why_invalid)) {
                        uart1_printf(" [VALIDATE FAILED] Frame rejected: %s\r\n", why_invalid);
                        RadioApp_StartRx();
                        break;
                    }

                    uart1_puts(" [VALIDATE SUCCESS] AX.25 UI structure OK, FCS/CRC OK\r\n");

                    char src_cs[7];
                    char dest_cs[7];
                    AX25_DecodeAddress(&s_rx_decoded[1], dest_cs);
                    AX25_DecodeAddress(&s_rx_decoded[8], src_cs);
                    uart1_printf("FRAME: DEST=%s SRC=%s\r\n", dest_cs, src_cs);

                    if (strcmp(src_cs, SatelliteProfile.sourceCallsign) == 0) {
                        uart1_puts(" *** COMMAND RECEIVED: NO (source is our own callsign - ignoring echo) ***\r\n");
                        RadioApp_StartRx();
                        break;
                    }

                    /* -------------------------------------------------------------
                     * STEP 4: DETECT & DISPATCH TELECOMMAND
                     * ------------------------------------------------------------- */
                    /* Payload sits between the PID byte and the 2 FCS bytes + end flag */
                    const uint16_t payload_start = 17;
                    const uint16_t payload_end   = (uint16_t)(decoded_len - 3);
                    uint16_t plen = (payload_end > payload_start) ? (uint16_t)(payload_end - payload_start) : 0;
                    const uint8_t *pl = &s_rx_decoded[payload_start];

                    if (plen == 0) {
                        uart1_puts(" *** COMMAND RECEIVED: NO (empty payload, no opcode) ***\r\n");
                        RadioApp_StartRx();
                        break;
                    }

                    /* Integrity check: a command arrives with a 12-byte trailer (counter + tag). Verify it and continue
                       with the original payload. With AUTH on, anything without a valid trailer is dropped. */
                    uint8_t seal_cmd[SEAL_MAX_PAYLOAD];
                    uint16_t seal_cmd_len = 0;
                    if (SEAL_LEN_MAY_BE_WRAPPED(plen) &&
                        Protocol_DecodePayload(pl, plen, seal_cmd, sizeof(seal_cmd), &seal_cmd_len)) {
                        pl = seal_cmd;
                        plen = seal_cmd_len;
                    }
#if AUTH_ENABLE
                    else {
                        uart1_puts(" *** COMMAND RECEIVED: NO (no valid integrity trailer) ***\r\n");
                        RadioApp_StartRx();
                        break;
                    }
#endif

                    /* Check if payload is an ASCII HEX string (e.g. "HHEX 53 04 CC 5E BE 00 D0 00 00 00 00 00 45") */
                    uint8_t ascii_hex_bin[CMD_PAYLOAD_LEN];
                    uint16_t ascii_hex_len = 0;
                    if (Sat_Try_Parse_Ascii_Hex_Command(pl, plen, ascii_hex_bin, &ascii_hex_len)) {
                        uart1_printf("[M0+] Converted ASCII HEX payload (%u chars) -> Valid %uB Telecommand (Opcode 0x%02X)\r\n",
                                     plen, ascii_hex_len, ascii_hex_bin[1]);
                        pl = ascii_hex_bin;
                        plen = ascii_hex_len;
                    }

                    uint8_t cmd_norm[CMD_PAYLOAD_LEN];
                    uint16_t cmd_norm_len = 0;
                    bool is_telecommand = Sat_Extract_Telecommand(pl, plen, cmd_norm, &cmd_norm_len) &&
                                          cmd_norm[0] == 0x53;

                    if (!is_telecommand && pl[0] == 0x53) {
                        /* ADDED: a command that starts with the SAT-ID but is not 13 bytes long -> NACK 0xEE 0x04 (incomplete packet) */
                        uart1_printf(" *** COMMAND RECEIVED: INCOMPLETE (%u B, 13 B needed) -> NACK ***\r\n", plen);
                        s_pending_cmd_len = 0;
                        s_pending_err = SAT_ERR_INCOMPLETE;
                        strncpy(s_pending_src_cs, src_cs, sizeof(s_pending_src_cs) - 1);
                        s_pending_src_cs[sizeof(s_pending_src_cs) - 1] = '\0';
                        s_cmd_service_active = true;
                        s_last_cmd_time_ms = HAL_GetTick();
                        SUBGRF_SetStandby(STDBY_XOSC);
                        app_state = STATE_DOWNLINK;
                        break;
                    }
                    if (!is_telecommand) {
                        uart1_printf(" *** COMMAND RECEIVED: NO (payload of %u B is not a valid telecommand format; only 0x53 commands allowed) ***\r\n", plen);
                        SUBGRF_SetStandby(STDBY_XOSC);
                        RadioApp_StartRx();
                        break;
                    }

                    pl = cmd_norm;
                    plen = cmd_norm_len;

                    uart1_printf(" *** COMMAND RECEIVED: YES (from %s, opcode 0x%02X, %u bytes, %s) -> TRANSITIONING TO STATE_DOWNLINK ***\r\n",
                                 src_cs, pl[0], plen,
                                 "13B telecommand");

                    /* Store pending command and transition to STATE_DOWNLINK */
                    memcpy(s_pending_cmd, pl, plen);
                    s_pending_cmd_len = plen;
                    strncpy(s_pending_src_cs, src_cs, sizeof(s_pending_src_cs) - 1);
                    s_pending_src_cs[sizeof(s_pending_src_cs) - 1] = '\0';

                    /* ---- VALID COMMAND: act in this exact order ---- */

                    /* 1. Stop the CW beacon and disable RX: hold CW off and do not receive any new command */
                    s_cmd_service_active = true;
                    s_last_cmd_time_ms = HAL_GetTick();
                    /* STDBY_XOSC keeps the 32 MHz TCXO up. Radio.Standby() is STDBY_RC and brown-outs 5V PA TX. */
                    SUBGRF_SetStandby(STDBY_XOSC);
                    uart1_puts(">>> [M0+] VALID COMMAND ACCEPTED -> RX DISABLED & CW STOPPED UNTIL ALL PACKETS FINISHED <<<\r\n");

                    /* 2. Transition to STATE_DOWNLINK to send ACK first, then signal M4 */
                    app_state = STATE_DOWNLINK;
                    break;
                }

                /* 4. Check for UART command injection (from GUI / serial console /dev/ttyUSB0) */
                static char s_uart_cmd_line[128];
                static uint8_t s_uart_cmd_idx = 0;
                uint8_t u_c = 0;
                while (uart1_try_getc(&u_c)) {
                    if (u_c == '\r' || u_c == '\n') {
                        if (s_uart_cmd_idx > 0) {
                            s_uart_cmd_line[s_uart_cmd_idx] = '\0';
                            char *str = s_uart_cmd_line;
                            while (*str == ' ' || *str == '\t') str++;
                            if (strncmp(str, "HEX ", 4) == 0 || strncmp(str, "hex ", 4) == 0) {
                                str += 4;
                            }
                            uint8_t parsed_cmd[64];
                            uint16_t parsed_len = 0;
                            char hex_pair[3] = {0};
                            while (*str && parsed_len < sizeof(parsed_cmd)) {
                                while (*str == ' ' || *str == ',' || *str == '\t') str++;
                                if (!*str) break;
                                if (isxdigit((int)str[0])) {
                                    hex_pair[0] = *str++;
                                    hex_pair[1] = isxdigit((int)str[0]) ? *str++ : '\0';
                                    parsed_cmd[parsed_len++] = (uint8_t)strtoul(hex_pair, NULL, 16);
                                } else {
                                    break;
                                }
                            }

                            if (parsed_len == CMD_PAYLOAD_LEN && parsed_cmd[0] == 0x53) {
                                uart1_printf(">>> [UART-IN] Received Telecommand from serial console (%u bytes)\r\n", parsed_len);
                                memcpy(s_pending_cmd, parsed_cmd, parsed_len);
                                s_pending_cmd_len = parsed_len;
                                strncpy(s_pending_src_cs, S2S2_GS_CALLSIGN, sizeof(s_pending_src_cs) - 1);
                                s_pending_src_cs[sizeof(s_pending_src_cs) - 1] = '\0';
                                s_cmd_service_active = true;
                                s_last_cmd_time_ms = HAL_GetTick();
                                SUBGRF_SetStandby(STDBY_XOSC);
                                uart1_puts(">>> [M0+] VALID COMMAND ACCEPTED VIA UART -> RX DISABLED & CW STOPPED UNTIL ALL PACKETS FINISHED <<<\r\n");
                                SatTcClass_t tcc;
                                Sat_Classify_Telecommand(parsed_cmd, parsed_len, &tcc);
                                if (Sat_Tc_Is_Known(&tcc)) {
                                    Sat_Forward_Command_To_M4(parsed_cmd, parsed_len);
                                }
                                s_uart_cmd_idx = 0;
                                app_state = STATE_DOWNLINK;
                                break;
                            } else if (s_uart_cmd_idx > 0 && Sat_Payload_Is_Printable((const uint8_t *)s_uart_cmd_line, s_uart_cmd_idx)) {
                                if (strcasecmp(s_uart_cmd_line, "SNAP") == 0) {
                                    static const uint8_t snap_cmd[13] = {0x53, 0x04, 0xCC, 0x5E, 0xBD, 0,0,0,0,0,0,0,0};
                                    memcpy(s_pending_cmd, snap_cmd, 13);
                                    s_pending_cmd_len = 13;
                                    strncpy(s_pending_src_cs, S2S2_GS_CALLSIGN, sizeof(s_pending_src_cs) - 1);
                                    s_pending_src_cs[sizeof(s_pending_src_cs) - 1] = '\0';
                                    s_cmd_service_active = true;
                                    s_last_cmd_time_ms = HAL_GetTick();
                                    SUBGRF_SetStandby(STDBY_XOSC);
                                    Sat_Forward_Command_To_M4(snap_cmd, 13);
                                    s_uart_cmd_idx = 0;
                                    app_state = STATE_DOWNLINK;
                                    break;
                                } else if (strcasecmp(s_uart_cmd_line, "CAM") == 0 || strcasecmp(s_uart_cmd_line, "DUMMY") == 0) {
                                    static const uint8_t dummy_cmd[13] = {0x53, 0x04, 0xCC, 0x5E, 0xBD, 0,0,0,0,0,0,0,0};   /* CHANGED: sheet opcode (was ...BE dummy camera) */
                                    memcpy(s_pending_cmd, dummy_cmd, 13);
                                    s_pending_cmd_len = 13;
                                    strncpy(s_pending_src_cs, S2S2_GS_CALLSIGN, sizeof(s_pending_src_cs) - 1);
                                    s_pending_src_cs[sizeof(s_pending_src_cs) - 1] = '\0';
                                    s_cmd_service_active = true;
                                    s_last_cmd_time_ms = HAL_GetTick();
                                    SUBGRF_SetStandby(STDBY_XOSC);
                                    Sat_Forward_Command_To_M4(dummy_cmd, 13);
                                    s_uart_cmd_idx = 0;
                                    app_state = STATE_DOWNLINK;
                                    break;
                                }
                            }
                            s_uart_cmd_idx = 0;
                        }
                    } else if (s_uart_cmd_idx < (sizeof(s_uart_cmd_line) - 1)) {
                        if (u_c >= 32 && u_c <= 126) {
                            s_uart_cmd_line[s_uart_cmd_idx++] = (char)u_c;
                        }
                    }
                }

                if (app_state == STATE_DOWNLINK) {
                    break;
                }

                /* 5. Check 90-second listen window expiration */
                bool hold_active = (s_cmd_service_active || (s_last_cmd_time_ms != 0 && (now - s_last_cmd_time_ms < SAT_CMD_HOLD_TIME_MS)));

                if (!hold_active && (now - s_beacon_start_tick >= SAT_BEACON_PERIOD_MS)) {   /* CHANGED: fixed 90 s period, was 50 s after the beacon ended */
                    uart1_printf("\r\n>>> [M0+] %lus RX Listen Window Completed. Transitioning to %s beacon transmission. <<<\r\n\r\n",
                                 (unsigned long)(SAT_LISTEN_DURATION_MS / 1000UL),
                                 (s_cw_step == 1) ? "CW2" : "CW1");
                    app_state = STATE_CW;
                    break;
                }

                CPU2_Delay_Ms(10);
                break;
            }

            /*
             * ================================================================
             * STATE 3: STATE_DOWNLINK (GMSK AX.25 G3RUH Data & ACK Downlink)
             *
             * Protocol (per requirement):
             *   1. Parse command type
             *   2. If valid 13B telecommand already forwarded to M4:
             *        a. STOP CW (already done via s_cmd_service_active=true)
             *        b. Send GMSK ACK (0xAC) to GS immediately
             *        c. For commands needing M4 data (flash/camera):
             *             - Wait up to SAT_M4_RESPONSE_TIMEOUT_MS for M4 to respond
             *             - Stream data from M4 ring buffer
             *        d. For HK: drain ring buffer and transmit 128B HK packet
             *        e. Send GMSK ACK Completed (0xAC 0x01 mission)
             *   3. If unknown/invalid opcode: send GMSK NACK (0xEE 0x09 0xFF); wrong length: 0xEE 0x04 0xFF
             *   4. After full downlink: clear s_cmd_service_active, return to LISTEN
             * ================================================================
             */
            case STATE_DOWNLINK: {
                uart1_puts("\r\n>>> [M0+] ENTERING STATE: DOWNLINK <<<\r\n");
                /* ADDED: 700 ms before the first reply. The GS is still printing the report for the command it
                 * just sent and has not re-armed its receiver; an immediate ACK was lost (config.h). */
                CPU2_Delay_Ms(SAT_ACK_GUARD_MS);
                uint8_t *payload_ptr = s_pending_cmd;
                uint16_t payload_len = s_pending_cmd_len;

                /* Case 1: Structured Telecommand (0x53 prefix, 13B or 8B) */
                if (payload_len >= CMD_PAYLOAD_LEN && payload_ptr[0] == 0x53) {
                    uint8_t  mcu_id  = payload_ptr[1];
                    uint8_t  op0     = payload_ptr[2];
                    uint8_t  op1     = payload_ptr[3];
                    uint8_t  op2     = (payload_len >= 5) ? payload_ptr[4] : 0;
                    uint16_t data_id = (payload_len >= 7) ? (((uint16_t)payload_ptr[5] << 8) | (uint16_t)payload_ptr[6]) : 0;
                    uint32_t addr    = (payload_len >= 11) ? (((uint32_t)payload_ptr[7]  << 24) |
                                                              ((uint32_t)payload_ptr[8]  << 16) |
                                                              ((uint32_t)payload_ptr[9]  <<  8) |
                                                               (uint32_t)payload_ptr[10]) : 0;
                    uint16_t count   = (payload_len >= 13) ? (((uint16_t)payload_ptr[11] << 8) | (uint16_t)payload_ptr[12]) :
                                       (payload_len >= 8)  ? (((uint16_t)payload_ptr[6]  << 8) | (uint16_t)payload_ptr[7]) : 0;
                    uint16_t num_pkts = count ? count : 1;
                    (void)data_id;

                    uart1_printf("[M0+ RX CMD] hex(%u):", payload_len);
                    for (uint8_t i = 0; i < payload_len; i++)
                        uart1_printf("%02X", payload_ptr[i]);
                    uart1_puts("\r\n");
                    if (payload_len >= 13) {
                        uart1_printf("    SAT=%02X MCU=%02X OP=%02X%02X%02X DID=%02X%02X ADDR=%02X%02X%02X%02X CNT=%02X%02X\r\n",
                                     payload_ptr[0], mcu_id, op0, op1, op2,
                                     payload_ptr[5], payload_ptr[6],
                                     payload_ptr[7], payload_ptr[8],
                                     payload_ptr[9], payload_ptr[10],
                                     payload_ptr[11], payload_ptr[12]);
                    } else {
                        uart1_printf("    SAT=%02X MCU=%02X OP=%02X%02X PARAM=%02X%02X%02X%02X\r\n",
                                     payload_ptr[0], mcu_id, op0, op1,
                                     payload_ptr[4], payload_ptr[5], payload_ptr[6], payload_ptr[7]);
                    }

                    SatTcClass_t tcc;
                    Sat_Classify_Telecommand(payload_ptr, payload_len, &tcc);

                    /* CHANGED: only the opcodes of the Packet Format command sheet are served (see Sat_Classify_Telecommand);
                     * everything else gets NACK 0xEE 0x09 (invalid command), mission 0xFF (general). */
                    if (!Sat_Tc_Is_Known(&tcc)) {
                        uart1_printf("[M0+] COMMAND CHECK: UNRECOGNIZED / INVALID OPCODE (0x%02X 0x%02X 0x%02X) -> SENDING NACK\r\n",
                                     op0, op1, op2);
                        /* CW stays paused during NACK transmission */
                        Satellite_Send_Nack(SAT_ERR_INVALID_CMD, SAT_MISSION_UNKNOWN);
                    } else {
                        uart1_puts("[M0+] COMMAND CHECK: PASSED (13 B telecommand of the command sheet)\r\n");

                        /* ---- ACK Accepted (AC 00 mission) is sent FIRST, ACK Completed (AC 01 mission) LAST;
                         * an error ends the command with NACK (EE err mission) instead of Completed. ---- */

                        if (tcc.flash) {                         /* 53 01 1D D1 F8: housekeeping download */
                            uart1_printf("\r\n>>> [M0+ DOWNLINK] HOUSEKEEPING DOWNLOAD: addr=0x%08lX pkts=%u <<<\r\n",
                                         (unsigned long)addr, num_pkts);
                            Satellite_Send_Ack_Accepted(SAT_MISSION_HK);
                            Sat_Forward_Command_To_M4(payload_ptr, payload_len);
                            {
                                uint32_t wait_start = HAL_GetTick();
                                while (flash_data_empty() &&
                                       (HAL_GetTick() - wait_start) < SAT_M4_RESPONSE_TIMEOUT_MS) {
                                    CPU2_Delay_Ms(10);
                                }
                            }
                            if (flash_data_empty()) {
                                uart1_puts("[M0+] M4 delivered no data -> NACK (requested data missing)\r\n");
                                Satellite_Send_Nack(SAT_ERR_DATA_MISSING, SAT_MISSION_HK);
                            } else {
                                Satellite_Handle_Flash_Read(addr, num_pkts);
                                Satellite_Send_Ack_Completed(SAT_MISSION_HK);
                            }
                        } else if (tcc.cam_download) {           /* 53 01 1D D2 F5: download the stored image */
                            uint16_t cam_pkts = count ? count : 69;
                            uart1_printf("\r\n>>> [M0+ DOWNLINK] CAM DOWNLOAD: addr=0x%08lX pkts=%u <<<\r\n",
                                         (unsigned long)addr, cam_pkts);
                            Satellite_Send_Ack_Accepted(SAT_MISSION_CAM_RGB);
                            Sat_Forward_Command_To_M4(payload_ptr, payload_len);
                            {
                                uint32_t wait_start = HAL_GetTick();
                                while (flash_data_empty() &&
                                       (HAL_GetTick() - wait_start) < SAT_M4_RESPONSE_TIMEOUT_MS) {
                                    CPU2_Delay_Ms(10);
                                }
                            }
                            if (flash_data_empty()) {
                                uart1_puts("[M0+] no image in the ring -> NACK (requested data missing)\r\n");
                                Satellite_Send_Nack(SAT_ERR_DATA_MISSING, SAT_MISSION_CAM_RGB);
                            } else {
                                Satellite_Send_Camera_Image(cam_pkts, false);
                                Satellite_Send_Ack_Completed(SAT_MISSION_CAM_RGB);
                            }
                        } else if (tcc.cam_on) {                 /* 53 04 CC 5E BD: run the camera mission */
                            uart1_puts("\r\n>>> [M0+ DOWNLINK] RUN CAMERA MISSION <<<\r\n");
                            Satellite_Send_Ack_Accepted(SAT_MISSION_CAM_RGB);
                            Sat_Forward_Command_To_M4(payload_ptr, payload_len);
                            {
                                uint32_t wait_start = HAL_GetTick();
                                bool m4_captured = false;
                                while ((HAL_GetTick() - wait_start) < 12000UL) {
                                    if (ipcc_m0_received(IPCC_CH_COMMAND)) {
                                        ipcc_m0_clear(IPCC_CH_COMMAND);
                                        m4_captured = true;
                                        uart1_puts("[M0+] M4 IPCC ACK: camera mission done\r\n");
                                        break;
                                    }
                                    CPU2_Delay_Ms(20);
                                }
                                if (!m4_captured) {
                                    uart1_puts("[M0+] M4 camera capture completed (timeout reached).\r\n");
                                }
                            }
                            Satellite_Send_Ack_Completed(SAT_MISSION_CAM_RGB);
                        } else if (tcc.hk) {                     /* 53 01 1D D1 F2: HEALTH SAT (sat_health.txt) */
                            /* count 0/1 = live 128 B HK packet; count N>1 = N stored records from the M4 */
                            if (num_pkts <= 1U) {
                                uart1_puts("\r\n>>> [M0+ DOWNLINK] LIVE HK TELEMETRY (128 B combined ADC1/ADC2/IMU) <<<\r\n");
                                Satellite_Send_Ack_Accepted(SAT_MISSION_HK);
                                Sat_Forward_Command_To_M4(payload_ptr, payload_len);
                                {
                                    uint32_t wait_start = HAL_GetTick();
                                    while ((HAL_GetTick() - wait_start) < 200UL) {
                                        if (Satellite_Drain_Telemetry(b1_data, b2_data))
                                            break;
                                        CPU2_Delay_Ms(10);
                                    }
                                }
                                Satellite_Drain_Telemetry(b1_data, b2_data);
                                Satellite_Send_HK_Combined_Packet(b1_data, b2_data);
                                Satellite_Send_Ack_Completed(SAT_MISSION_HK);
                            } else {
                                uart1_printf("\r\n>>> [M0+ DOWNLINK] HK DOWNLOAD: %u x 128 B from M4 <<<\r\n", num_pkts);
                                Sat_Forward_Command_To_M4(payload_ptr, payload_len);
                                Satellite_Send_Ack_Accepted(SAT_MISSION_HK);
                                {
                                    uint32_t wait_start = HAL_GetTick();
                                    while (flash_data_empty() &&
                                           (HAL_GetTick() - wait_start) < SAT_M4_RESPONSE_TIMEOUT_MS) {
                                        CPU2_Delay_Ms(10);
                                    }
                                }
                                if (flash_data_empty()) {
                                    Satellite_Send_Nack(SAT_ERR_DATA_MISSING, SAT_MISSION_HK);
                                } else {
                                    Satellite_Handle_Flash_Read(addr, num_pkts);
                                    Satellite_Send_Ack_Completed(SAT_MISSION_HK);
                                }
                            }
                        } else if (tcc.antenna) {                /* 53 01 DE EF AA: antenna deployment (done by the M4) */
                            uart1_puts("\r\n>>> [M0+ DOWNLINK] ANTENNA DEPLOYMENT <<<\r\n");
                            Satellite_Send_Ack_Accepted(SAT_MISSION_UNKNOWN);
                            Sat_Forward_Command_To_M4(payload_ptr, payload_len);
                            Satellite_Send_Ack_Completed(SAT_MISSION_UNKNOWN);
                        }
                    }
                } else {
                    /* Nothing valid pending: a "0x53" command of the wrong length (incomplete packet) or no command at all */
                    uart1_printf("[M0+] COMMAND CHECK: %s -> SENDING NACK\r\n",
                                 (s_pending_err == SAT_ERR_INCOMPLETE) ? "INCOMPLETE COMMAND" : "INVALID COMMAND");
                    Satellite_Send_Nack(s_pending_err ? s_pending_err : SAT_ERR_INVALID_CMD, SAT_MISSION_UNKNOWN);
                    s_pending_err = 0;
                }

                /* Wait for M4 to confirm all streaming/flash tasks are finished via IPCC */
                if (payload_len >= CMD_PAYLOAD_LEN && payload_ptr[0] == 0x53) {
                    uart1_puts("\r\n>>> [M0+] ALL PACKETS SENT. WAITING FOR M4 IPCC ACK TO COMPLETE MISSION... <<<\r\n");
                    uint32_t ack_wait_start = HAL_GetTick();
                    while (!ipcc_m0_received(IPCC_CH_COMMAND) && (HAL_GetTick() - ack_wait_start) < 6000UL) {
                        CPU2_Delay_Ms(10);
                    }
                    if (ipcc_m0_received(IPCC_CH_COMMAND)) {
                        ipcc_m0_clear(IPCC_CH_COMMAND);
                        uart1_puts(">>> [M0+] M4 IPCC ACK CONFIRMED: M4 completed all operations! <<<\r\n");
                    } else {
                        uart1_puts(">>> [M0+] M4 IPCC ACK wait finished (timeout reached). <<<\r\n");
                    }
                }

                s_cmd_service_active = false;
                s_last_cmd_time_ms = 0; /* Clear hold delay so CW beacon resumes immediately */

                uart1_puts(">>> [M0+] ALL FINISHED. RESUMING CONTINUOUS CYCLE (CW -> 30s LISTEN) <<<\r\n\r\n");
                app_state = STATE_CW;
                break;
            }
        }
    }

    return 0;
}
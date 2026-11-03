/*
 * gs_app.c  –  STM32WL55JC2 Ground Station Application
 *
 * Hardware notes
 * ──────────────
 *   STM32WL55JC2 (Ground Station) : RFO_HP path (+22 dBm), used for UPLINK TX.
 *   STM32WL55JC1 (Satellite)      : RFO_LP path (+14 dBm), 5 V ext-PA for GMSK DL.
 *
 * Frequencies
 * ───────────
 *   GS TX  (uplink)   : 437.375 MHz   → satellite RX
 *   GS RX  (downlink) : 435.000 MHz   ← satellite TX (CW beacon + GMSK data)
 *
 * Receive pipeline (downlink)
 * ───────────────────────────
 *  1. RSSI polling every 5 ms → CwRx_Feed() → live CW Morse decode (callsign + telemetry)
 *  2. OnRxDone ISR latches raw G3RUH bytes + size flag
 *  3. GS_Process_Received_Frame() runs in the main loop:
 *       a. G3RUH descramble + NRZI + HDLC destuff (Protocol_ExtractFrame)
 *       b. AX.25 structural check + FCS (CRC-16/X.25)
 *       c. Payload type dispatch:
 *            • ACK  byte 0xAC (00 accepted, 01 completed)
 *            • NACK byte 0xEE (error code, mission id)
 *            • HK   128-byte telemetry packet (magic 0xAA55 / 0xBB66 + 0xAACC)
 *            • CAM  128-byte camera chunk (magic 0xCAFE or FLASH 0xFDF0)
 *            • Flash chunk (magic 0xFDF0 + header)
 *            • ASCII string (ping response, generic ACK text)
 *
 * Camera reassembly
 * ─────────────────
 *   The satellite downlinks camera data as sequential 128-byte AX.25 payloads.
 *   Each payload has a 14-byte header:
 *       [0..1]  magic (0xFE 0xCA for cam, 0xF0 0xFD for flash)
 *       [2..3]  pkt_idx  (uint16 LE)
 *       [4..5]  total_pkts (uint16 LE)
 *       [6..9]  offset (uint32 LE)
 *       [10..11] data_len (uint16 LE, = 128 normally)
 *       [12]    status
 *       [13]    reserved
 *       [14..141] 128 bytes of data
 *       [142..143] CRC-16 LE
 *   GS reassembles into gs_cam_buf[], tracks JPEG SOI/EOI, reports progress.
 */

#include "gs_app.h"
#include "digipeater.h"
#include "s2s2_config.h"
#include "ll_port.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "stm32wlxx_hal.h"
#include "radio.h"
#include "subghz.h"
#include "radio_driver.h"
#include "radio_board_if.h"
#include "uart/uart_debug.h"
#include "radio_app.h"
#include "protocol.h"
#include "seal_wrap.h"
#if AUTH_ENABLE
#include "secrets.h"    /* S2S2_AUTH_KEY_FPR for the boot message */
#endif
#include "protocol/ax25/ax25.h"
#include "protocol/ax25/crc16.h"
#include "protocol/g3ruh/g3ruh.h"
#include "cw_rx.h"

/* ============================================================================
 * FREQUENCIES
 * ========================================================================== */
#define GS_UPLINK_FREQ_HZ    S2S2_UPLINK_HZ     /* GS TX → satellite RX (generated/s2s2_config.h) */
#define GS_DOWNLINK_FREQ_HZ  S2S2_DOWNLINK_HZ   /* satellite TX → GS RX (CW + GMSK) */

/* Send 2 packets per command so the sat G3RUH descrambler can lock */
#define GS_SEND_COUNT         2
#define GS_BURST_INTERVAL_MS  120UL
#define GS_TX_WAIT_MS         3000UL

/* ============================================================================
 * DOWNLINK FRAME CONSTANTS
 * ========================================================================== */
/* HK packet (128 bytes): magic footers */
#define GS_HK_PKT_LEN          128U
#define GS_HK_FOOTER1_LO       0x55U   /* bytes 32..33 = 0xAA55 */
#define GS_HK_FOOTER1_HI       0xAAU
#define GS_HK_FOOTER2_LO       0x66U   /* bytes 70..71 = 0xBB66 */
#define GS_HK_FOOTER2_HI       0xBBU
#define GS_HK_END_LO            0xAAU  /* bytes 126..127 = 0xAACC */
#define GS_HK_END_HI            0xCCU

/* Camera fragment: CHANGED to the "Camera Fragment" sheet of the Packet Format (was the 0xCAFE packet of the reference
 * firmware). Information field = type 0x08 (1), image ID (1), fragment index (2 LE), total fragments (2 LE), image type (1),
 * AI score (1), 128 image bytes, CRC-16/X.25 (2, low byte first, over the preceding 136 bytes). It must match
 * CAM_PKT_* in satellite_app.h. Flash dump chunks keep the reference 14-byte header with magic 0xDA 0xDB. */
#define GS_CAM_PKT_TYPE         0x08U
#define GS_FLASH_PKT_MAGIC_LO  0xDAU   /* matches FLASH_PKT_MAGIC_LO in satellite_app.h */
#define GS_FLASH_PKT_MAGIC_HI  0xDBU   /* matches FLASH_PKT_MAGIC_HI in satellite_app.h */
#define GS_CAM_HDR_LEN           8U
#define GS_FLASH_HDR_LEN        14U
#define GS_CAM_DATA_LEN        128U
#define GS_CAM_PKT_TOTAL_LEN   (GS_CAM_HDR_LEN + GS_CAM_DATA_LEN + 2U)

/* A camera fragment: right type byte, exact length, valid CRC-16/X.25, index below the total. */
static bool GS_Is_Cam_Fragment(const uint8_t *p, uint16_t len)
{
    if (len < GS_CAM_PKT_TOTAL_LEN || p[0] != GS_CAM_PKT_TYPE) return false;
    if (!CRC16_X25_Verify(p, GS_CAM_PKT_TOTAL_LEN)) return false;
    uint16_t idx   = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
    uint16_t total = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
    return total != 0 && idx < total;
}

/* Flight-spec ACK/NACK bytes from satellite */
#define SAT_ACK_ACCEPTED_BYTE  0xACU
#define SAT_ACK_COMPLETED_BYTE 0xACU   /* CHANGED: completed = 0xAC 0x01; 0xEE is only NACK/error (Packet Format sheet 2) */
#define SAT_NACK_BYTE          0xEEU   /* CHANGED: was 0xFF */
#define SAT_LEGACY_ACK_BYTE    0xAAU
#define SAT_LEGACY_NACK_BYTE   0x55U

/* CW RSSI polling */
#define GS_CW_SAMPLE_MS         5U      /* poll RSSI every 5 ms */
#define GS_CW_MUTE_AFTER_TX_MS  1500U   /* mute CW decoder 1.5 s after our own TX */

/* Maximum camera buffer (≈9 KB covers a 320×240 JPEG comfortably) */
#define GS_CAM_BUF_MAX         (10U * 1024U)   /* 10 KB */

/* ============================================================================
 * CONFIGURATION
 * ========================================================================== */
const GroundStationConfig_t GroundStationDefaultConfig = GROUND_STATION_CONFIG_DEFAULT;
static GroundStationConfig_t s_config = GROUND_STATION_CONFIG_DEFAULT;

/* ============================================================================
 * RX STATE  (shared between ISR and main loop via volatile)
 * ========================================================================== */
volatile uint8_t  rx_frame_buffer[AX25_MAX_FRAME_SIZE];
volatile uint16_t rx_frame_size   = 0;
volatile uint8_t  rx_done_flag    = 0;
volatile uint8_t  rx_timeout_flag = 0;
volatile uint8_t  rx_error_flag   = 0;
volatile uint8_t  tx_busy         = 0;

#define GS_RX_SLOTS 2
static volatile uint8_t  s_rx_slot[GS_RX_SLOTS][AX25_MAX_FRAME_SIZE];
static volatile uint16_t s_rx_slot_len[GS_RX_SLOTS];
static volatile int16_t  s_rx_slot_rssi[GS_RX_SLOTS];
static volatile int8_t   s_rx_slot_snr[GS_RX_SLOTS];
static volatile uint8_t  s_rx_w = 0;
static volatile uint8_t  s_rx_n = 0;

static int16_t  s_last_rssi = -130;
static int8_t   s_last_snr  = 0;

/* ============================================================================
 * CAMERA REASSEMBLY STATE
 * ========================================================================== */
static uint8_t  gs_cam_buf[GS_CAM_BUF_MAX];
static uint32_t gs_cam_bytes     = 0;
static uint16_t gs_cam_pkt_recv  = 0;
static uint16_t gs_cam_total     = 0;
static bool     gs_cam_active    = false;
static bool     gs_cam_jpeg_soi  = false;
static bool     gs_cam_jpeg_eoi  = false;

/* ============================================================================
 * TX STATS
 * ========================================================================== */
static uint32_t stat_cmd_transmitted = 0;
static uint32_t stat_tx_timeouts     = 0;
static uint32_t stat_pkts_received   = 0;
static uint32_t stat_pkts_decoded    = 0;
static uint32_t stat_ack_received    = 0;
static uint32_t stat_nack_received   = 0;

static bool     s_auto_cmd_enabled     = false;
static uint32_t s_last_auto_cmd_time_ms = 0;

/* ============================================================================
 * COMMAND BYTES
 * ========================================================================== */
#define MCU_ID_OBC  0x01U
#define MCU_ID_CAM  0x04U

static const uint8_t OPCODE_FLASH[3] = { 0x1D, 0xD1, 0xF8 };   /* CHANGED: housekeeping download (was F2 = HEALTH SAT) */

static const uint8_t CMD_HK1[13]  = { 0x53, 0x01, 0x1D, 0xD1, 0xF2, 0x00, 0xF2,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t CMD_HK2[13]  = { 0x53, 0x01, 0x1D, 0xD1, 0xF8, 0x00, 0xF8,   /* CHANGED: housekeeping download (D2 F2 is not in the sheet) */
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t CMD_PING[13] = { 0x53, MCU_ID_OBC, 0x1D, 0xD1, 0xF5, 0x00, 0x00,   /* ping, as the reference firmware: ACK, "ACK: PING OK (OBC_ONLINE)", ACK */
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
/* ADDED: antenna deployment, 53 01 DE EF AA + 8 x 00 (Sheet 3) */
static const uint8_t CMD_ANTENNA[13] = { 0x53, 0x01, 0xDE, 0xEF, 0xAA, 0, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t CMD_CAMON[13]   = CMD_CAM_ON_COMMAND;
static const uint8_t CMD_CAMDOWN[13] = CMD_CAM_DOWNLOAD_COMMAND;
/* ADCS / EPDM mission run commands of the reference firmware (github.com/PrmBRana/dummy-camera-live Ground_Station/Mission/config.h):
 * MCU 03 = ADCS, MCU 05 = EPDM (earthquake precursor detection mission). The satellite answers ACK accepted + ACK completed (no data packet). */
static const uint8_t CMD_ADCS[13] = { 0x53, 0x03, 0xA0, 0x53, 0xCF, 0, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t CMD_EPDM[13] = { 0x53, 0x05, 0xEC, 0xCF, 0xCF, 0, 0, 0, 0, 0, 0, 0, 0 };
/* Files of the Packet Format file table (download = 53 01 1D <directory> <file no> <data id 00 Fn> <address> <packets>).
 * Optional argument: number of 128-byte chunks (default 5). ADCSFILE / EPDMFILE read adcs.txt / epdm.txt, the results of the missions. */
static const struct { const char *cmd, *file; uint8_t dir, no; } GS_FILES[] = {
    { "FLAGS", "flags.txt", 0xD1, 0xF1 }, { "SATLOG", "sat_log.txt", 0xD1, 0xF3 }, { "RSVTABLE", "rsv_table.txt", 0xD1, 0xF4 },
    { "EPDMFILE", "epdm.txt", 0xD2, 0xF6 }, { "ADCSFILE", "adcs.txt", 0xD2, 0xF7 },
};

static uint8_t s_tx_frame[AX25_MAX_FRAME_SIZE];
static uint8_t s_hex_buf[AX25_MAX_FRAME_SIZE];

/* Decode scratch buffers (static: keep off the tiny CPU2 stack) */
static uint8_t  s_rx_raw[AX25_MAX_FRAME_SIZE];
static uint8_t  s_rx_stream[AX25_MAX_FRAME_SIZE * 2];
static uint8_t  s_rx_decoded[AX25_MAX_FRAME_SIZE];
static uint8_t  s_prev_rx[AX25_MAX_FRAME_SIZE];
static uint16_t s_prev_rx_len = 0;

/* ============================================================================
 * RADIO IRQ LOCK
 * ========================================================================== */
static inline void GS_Radio_Lock(void)
{
    NVIC_DisableIRQ(SUBGHZ_Radio_IRQn);
    __DSB(); __ISB();
}

static inline void GS_Radio_Unlock(void)
{
    __DSB(); __ISB();
    NVIC_EnableIRQ(SUBGHZ_Radio_IRQn);
}

/* ============================================================================
 * RADIO CALLBACKS (ISR — latch flags ONLY)
 * ========================================================================== */
static void OnTxDone(void) { tx_busy = 0; }
static void OnTxTimeout(void) { tx_busy = 0; }

static void OnRxDone(uint8_t *p, uint16_t size, int16_t rssi, int8_t snr)
{
    if (size > 0 && size <= AX25_MAX_FRAME_SIZE && s_rx_n < GS_RX_SLOTS) {
        uint8_t i = s_rx_w;
        memcpy((void *)s_rx_slot[i], p, size);
        s_rx_slot_len[i]  = size;
        s_rx_slot_rssi[i] = rssi;
        s_rx_slot_snr[i]  = snr;
        s_rx_w = (uint8_t)((i + 1U) % GS_RX_SLOTS);
        s_rx_n++;
        memcpy((void *)rx_frame_buffer, p, size);
        rx_frame_size = size;
        s_last_rssi   = rssi;
        s_last_snr    = snr;
        rx_done_flag  = 1;
    }
    RadioApp_RearmRxFast();
}

static void OnRxTimeout(void) { rx_timeout_flag = 1; }
static void OnRxError(void)   { rx_error_flag   = 1; }

static RadioEvents_t s_RadioEvents = {
    .TxDone    = OnTxDone,
    .RxDone    = OnRxDone,
    .TxTimeout = OnTxTimeout,
    .RxTimeout = OnRxTimeout,
    .RxError   = OnRxError,
};

static void GS_Radio_Idle(void)
{
    GS_Radio_Lock();
    Radio.Standby();
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);
    GS_Radio_Unlock();
}

/* ============================================================================
 * RF SWITCH — GS uses RFO_HP for TX, normal RX for receive
 * ========================================================================== */
static void GS_Print_RF_Pins(void)
{
    uart2_printf("  [RF-PINS] PA8=%d PC3=%d PC4=%d PC5=%d  (HP TX=1,1,0,1)\r\n",
                 (int)HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8),
                 (int)HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_3),
                 (int)HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_4),
                 (int)HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_5));
}

/* ============================================================================
 * START RX  (arms receiver on downlink frequency 435.000 MHz)
 * ========================================================================== */
void GS_StartRx(void)
{
    GS_Radio_Idle();
    RadioApp_StartRx();   /* Radio stays in continuous RX */
}

/* After RxDone the radio is still in continuous RX and may already be
 * receiving the next downlink packet; re-arming would abort it. */
static void GS_ResumeRx(void)
{
    if (SUBGRF_GetOperatingMode() != MODE_RX)
        GS_StartRx();
}

/* ============================================================================
 * CW DECODER CALLBACK
 * ========================================================================== */
static void GS_CW_Event(CwEvt_t evt, char ch, const char *text,
                         uint32_t value, int16_t aux)
{
    switch (evt) {
    case CW_EVT_SIGNAL:
        uart2_printf("{\"type\":\"CWSIG\",\"on\":%u,\"rssi\":%d}\r\n",
                     (unsigned)value, (int)aux);
        if (value)
            uart2_printf("[CW] Carrier detected (RSSI %d dBm)\r\n", (int)aux);
        break;

    case CW_EVT_CARRIER:
        uart2_printf("{\"type\":\"CWCAR\",\"ms\":%lu,\"rssi\":%d}\r\n",
                     (unsigned long)value, (int)aux);
        uart2_printf("[CW] Tuning carrier: %lu ms (RSSI %d dBm)\r\n",
                     (unsigned long)value, (int)aux);
        break;

    case CW_EVT_CHAR:
        uart2_printf("{\"type\":\"CWCHAR\",\"ch\":\"%c\"}\r\n", ch);
        uart2_putc(ch);
        break;

    case CW_EVT_LINE:
        /* full decoded line — parse telemetry if callsign present */
        uart2_puts("\r\n");
        uart2_printf("{\"type\":\"CWRX\",\"text\":\"%s\",\"callsign\":%u}\r\n",
                     text ? text : "", (unsigned)value);
        uart2_printf("[CW DECODED] %s\r\n", text ? text : "");

        if (value && text) {
            /* CHANGED: the CW beacon now carries the 8 bytes of the CW BEACON 1 / 2 sheet as hex digits after
             * "S2S2 9N2SJ". The sheet has no beacon-number field, so both readings are printed. */
            const char *h = strstr(text, CW_CALLSIGN);
            uint8_t b[8];
            uint8_t nib = 0;
            if (h) {
                h += strlen(CW_CALLSIGN);
                for (; *h && nib < 16; h++) {
                    char c = *h;
                    uint8_t v;
                    if (c >= '0' && c <= '9') v = (uint8_t)(c - '0');
                    else if (c >= 'A' && c <= 'F') v = (uint8_t)(c - 'A' + 10);
                    else continue;                              /* skip the spaces between the groups */
                    if (nib & 1U) b[nib >> 1] |= v; else b[nib >> 1] = (uint8_t)(v << 4);
                    nib++;
                }
            }
            if (nib == 16) {
                uart2_printf("{\"type\":\"CWHK\",\"hex\":\"%02X%02X%02X%02X%02X%02X%02X%02X\"}\r\n",
                             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
                uart2_printf("  [CW BEACON 1?] sys=%u mission=%u t_reset=%uh SolI=%u SolV=%u.%02uV BatV=%u.%02uV BatI=%u.%02uA\r\n",
                             b[0] >> 6, (b[0] >> 4) & 3U, b[0] & 15U, b[1],
                             ((b[2] << 8) | b[3]) / 100U, ((b[2] << 8) | b[3]) % 100U,
                             ((b[4] << 8) | b[5]) / 100U, ((b[4] << 8) | b[5]) % 100U,
                             ((b[6] << 8) | b[7]) / 100U, ((b[6] << 8) | b[7]) % 100U);
                uart2_printf("  [CW BEACON 2?] gyro=%u,%u,%u mag=%u,%u,%u cells=0x%02X BatT=%dC\r\n",
                             b[0], b[1], b[2], b[3], b[4], b[5], b[6], (int)(int8_t)b[7]);
            }
        }
        break;
    }
    (void)ch;
}

/* ============================================================================
 * GS_CW_Poll – call every GS_CW_SAMPLE_MS while in RX
 * ========================================================================== */
extern volatile uint8_t g_radio_pkt_inflight;

void GS_CW_Poll(void)
{
    static uint32_t s_last_cw_sample_ms = 0;
    static uint32_t s_inflight_since_ms = 0;
    uint32_t now = HAL_GetTick();
    if ((now - s_last_cw_sample_ms) < GS_CW_SAMPLE_MS) return;
    s_last_cw_sample_ms = now;

    /* Do not SPI-poll RSSI while a GMSK payload is in the FIFO.
     * GetRssiInst during those ~333 ms (200 bytes @ 4800 bps) is why
     * ACK #1 decoded and every later packet failed FCS. */
    if (g_radio_pkt_inflight) {
        if (s_inflight_since_ms == 0)
            s_inflight_since_ms = now;
        if ((now - s_inflight_since_ms) < 600U)
            return;
        g_radio_pkt_inflight = 0; /* watchdog: RxDone never arrived */
    }
    s_inflight_since_ms = 0;

    int16_t rssi_dbm = Radio.Rssi(MODEM_FSK);
    CwRx_Feed(rssi_dbm, now);
}

/* ============================================================================
 * GS_RxDiag_Poll – report downlink receiver progress once per second when it
 * changes: preamble seen -> sync word matched -> 200-byte packet delivered.
 * ========================================================================== */
extern volatile uint32_t g_radio_preamble_cnt;
extern volatile uint32_t g_radio_sync_cnt;

static void GS_RxDiag_Poll(void)
{
    static uint32_t s_last_ms = 0, s_pre = 0, s_sync = 0, s_pkts = 0;
    uint32_t now = HAL_GetTick();
    if ((now - s_last_ms) < 1000U) return;
    s_last_ms = now;

    uint32_t pre = g_radio_preamble_cnt, sync = g_radio_sync_cnt;
    if (pre == s_pre && sync == s_sync && stat_pkts_received == s_pkts) return;

    int rssi_dbg = g_radio_pkt_inflight ? (int)s_last_rssi : (int)Radio.Rssi(MODEM_FSK);
    uart2_printf("[GS RX-DIAG] preamble=%lu (+%lu) sync=%lu (+%lu) packets=%lu | mode=%u RSSI=%d dBm\r\n",
                 (unsigned long)pre, (unsigned long)(pre - s_pre),
                 (unsigned long)sync, (unsigned long)(sync - s_sync),
                 (unsigned long)stat_pkts_received,
                 (unsigned)SUBGRF_GetOperatingMode(), rssi_dbg);
    if (pre != s_pre && sync == s_sync)
        uart2_puts("[GS RX-DIAG] Preamble without sync word 93 0B 51 DE "
                   "(normal during CW keying; during GMSK = framing mismatch or weak signal)\r\n");
    s_pre = pre; s_sync = sync; s_pkts = stat_pkts_received;
}

/* ============================================================================
 * DEBUG PRINT HELPERS
 * ========================================================================== */
static void Print_Hex_Bytes(const uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        if ((i % 16) == 0) uart2_puts("[RAW] ");
        uart2_printf("%02X ", buf[i]);
        if ((i + 1) % 16 == 0) uart2_puts("\r\n");
    }
    if (len % 16 != 0) uart2_puts("\r\n");
}

static void Print_Payload_ASCII(const uint8_t *buf, uint16_t start, uint16_t end)
{
    if (end <= start) { uart2_puts("Payload(text): (none)\r\n"); return; }
    char line[120];
    uint16_t n   = (uint16_t)(end - start);
    uint16_t pos = 0;
    pos += (uint16_t)snprintf(&line[pos], sizeof(line) - pos, "Payload(text): \"");
    for (uint16_t i = 0; i < n && pos < (uint16_t)(sizeof(line) - 4); i++) {
        uint8_t c = buf[start + i];
        line[pos++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    pos += (uint16_t)snprintf(&line[pos], sizeof(line) - pos, "\"\r\n");
    uart2_puts(line);
}

static void Print_AX25_Fields(const char *label, const uint8_t *buf, uint16_t len)
{
    uart2_printf("\r\n--- %s (%u bytes) ---\r\n", label, len);
    if (len < 20) { uart2_puts("(too short)\r\n"); return; }
    uart2_printf("  StartFlag : %02X %s\r\n", buf[0],
                 (buf[0] == AX25_FLAG) ? "(OK 0x7E)" : "(MISMATCH)");
    char dest_cs[7], src_cs[7];
    AX25_DecodeAddress(&buf[1], dest_cs);
    AX25_DecodeAddress(&buf[8], src_cs);
    uart2_printf("  Dest      : \"%s\" SSID %d\r\n", dest_cs, AX25_DecodeSSID(&buf[1]));
    uart2_printf("  Src       : \"%s\" SSID %d\r\n", src_cs,  AX25_DecodeSSID(&buf[8]));
    uart2_printf("  Control   : 0x%02X\r\n", buf[15]);
    uart2_printf("  PID       : 0x%02X\r\n", buf[16]);
    uint16_t ps = 17, pe = (uint16_t)(len - 3);
    if (pe > ps) {
        uart2_printf("  Payload   : %u bytes\r\n", (unsigned)(pe - ps));
        Print_Hex_Bytes(&buf[ps], (uint16_t)(pe - ps));
        Print_Payload_ASCII(buf, ps, pe);
    }
    uart2_printf("  FCS       : %02X %02X\r\n", buf[len - 3], buf[len - 2]);
    uart2_printf("  EndFlag   : %02X %s\r\n", buf[len - 1],
                 (buf[len - 1] == AX25_FLAG) ? "(OK 0x7E)" : "(MISMATCH)");
}

/* ============================================================================
 * HK PACKET DISPLAY  (128-byte combined packet)
 * Bytes  0..31  : ADC1 – 16× int16 (voltages + temps), x100 or x10
 * Bytes 32..33  : footer 0x55 0xAA
 * Bytes 34..57  : ADC2 – 12× int16 (currents + flags)
 * Bytes 58..69  : IMU  –  6× int16 (gyro + mag)
 * Bytes 70..71  : footer 0x66 0xBB
 * Bytes 72..83  : metadata (seq, timestamp_ms, total_sent)
 * Bytes 84..123 : padding/reserved
 * Bytes 124..125: CRC-16
 * Bytes 126..127: end footer 0xAA 0xCC
 * ========================================================================== */
static void GS_Display_HK_Packet(const uint8_t *pkt_in, uint16_t plen)
{
    if (plen < GS_HK_PKT_LEN) {
        uart2_printf("[GS RX HK] ERROR: packet too short (%u bytes, need %u)\r\n",
                     plen, GS_HK_PKT_LEN);
        return;
    }

    /* The payload starts at an odd offset of the frame buffer (after the 17-byte AX.25 header); the int16 views below
     * are halfword loads, which HardFault on the Cortex-M0+ when unaligned (seen on hardware 2026-11-03: the GS froze on
     * the first HK packet). Work on a word-aligned copy. */
    static uint32_t s_hk_words[GS_HK_PKT_LEN / 4U];
    memcpy(s_hk_words, pkt_in, GS_HK_PKT_LEN);
    const uint8_t *pkt = (const uint8_t *)s_hk_words;

    /* Verify structural footers */
    bool f1ok = (pkt[32] == GS_HK_FOOTER1_LO && pkt[33] == GS_HK_FOOTER1_HI);
    bool f2ok = (pkt[70] == GS_HK_FOOTER2_LO && pkt[71] == GS_HK_FOOTER2_HI);
    bool enok = (pkt[126] == GS_HK_END_LO    && pkt[127] == GS_HK_END_HI);

    uart2_puts("\r\n=============================================================\r\n");
    uart2_puts(">>> [GS RX] HOUSEKEEPING TELEMETRY (128 BYTES) <<<\r\n");
    uart2_printf("  Footer1(AA55): %s | Footer2(BB66): %s | End(AACC): %s\r\n",
                 f1ok ? "OK" : "BAD", f2ok ? "OK" : "BAD", enok ? "OK" : "BAD");
    uart2_puts("=============================================================\r\n");

    /* ADC1 – Voltages & Temperatures (16 channels × int16) */
    const int16_t *adc1 = (const int16_t *)&pkt[0];
    uart2_puts("  [ADC1 Voltages & Temps]\r\n");
    uart2_printf("    BatV  = %d.%02d V  |  SolV  = %d.%02d V  |  BusV  = %d.%02d V\r\n",
                 (int)(adc1[0] / 100), (int)(adc1[0] < 0 ? -adc1[0] % 100 : adc1[0] % 100),
                 (int)(adc1[1] / 100), (int)(adc1[1] < 0 ? -adc1[1] % 100 : adc1[1] % 100),
                 (int)(adc1[2] / 100), (int)(adc1[2] < 0 ? -adc1[2] % 100 : adc1[2] % 100));
    uart2_printf("    SP5V  = %d.%02d V  |  SP4V  = %d.%02d V  |  SP3V  = %d.%02d V\r\n",
                 (int)(adc1[3] / 100), (int)(adc1[3] < 0 ? -adc1[3] % 100 : adc1[3] % 100),
                 (int)(adc1[4] / 100), (int)(adc1[4] < 0 ? -adc1[4] % 100 : adc1[4] % 100),
                 (int)(adc1[5] / 100), (int)(adc1[5] < 0 ? -adc1[5] % 100 : adc1[5] % 100));
    uart2_printf("    SP1V  = %d.%02d V  |  SP2V  = %d.%02d V\r\n",
                 (int)(adc1[6] / 100), (int)(adc1[6] < 0 ? -adc1[6] % 100 : adc1[6] % 100),
                 (int)(adc1[7] / 100), (int)(adc1[7] < 0 ? -adc1[7] % 100 : adc1[7] % 100));
    uart2_printf("    AntT  = %d.%d C  |  BatT  = %d.%d C  |  BPBT  = %d.%d C\r\n",
                 (int)(adc1[8]  / 10), (int)(adc1[8]  < 0 ? -adc1[8]  % 10 : adc1[8]  % 10),
                 (int)(adc1[9]  / 10), (int)(adc1[9]  < 0 ? -adc1[9]  % 10 : adc1[9]  % 10),
                 (int)(adc1[10] / 10), (int)(adc1[10] < 0 ? -adc1[10] % 10 : adc1[10] % 10));

    /* ADC2 – Currents & Status */
    const int16_t *adc2 = (const int16_t *)&pkt[34];
    uart2_puts("  [ADC2 Currents & Flags]\r\n");
    uart2_printf("    Unreg = %d.%02d A  |  3V3I  = %d.%02d A  |  5VI   = %d.%02d A\r\n",
                 (int)(adc2[0] / 100), (int)(adc2[0] < 0 ? -adc2[0] % 100 : adc2[0] % 100),
                 (int)(adc2[1] / 100), (int)(adc2[1] < 0 ? -adc2[1] % 100 : adc2[1] % 100),
                 (int)(adc2[2] / 100), (int)(adc2[2] < 0 ? -adc2[2] % 100 : adc2[2] % 100));
    uart2_printf("    BatI  = %d.%02d A  |  SP1I  = %d.%02d A  |  BusI  = %d.%02d A\r\n",
                 (int)(adc2[3] / 100), (int)(adc2[3] < 0 ? -adc2[3] % 100 : adc2[3] % 100),
                 (int)(adc2[4] / 100), (int)(adc2[4] < 0 ? -adc2[4] % 100 : adc2[4] % 100),
                 (int)(adc2[9] / 100), (int)(adc2[9] < 0 ? -adc2[9] % 100 : adc2[9] % 100));
    uart2_printf("    Flag1 = %d  |  Flag2 = %d\r\n", (int)adc2[10], (int)adc2[11]);

    /* IMU */
    const int16_t *imu = (const int16_t *)&pkt[58];
    uart2_puts("  [IMU Gyro & Magnetometer]\r\n");
    uart2_printf("    GyroX = %d c-dps  GyroY = %d c-dps  GyroZ = %d c-dps\r\n",
                 (int)imu[0], (int)imu[1], (int)imu[2]);
    uart2_printf("    MagX  = %d uT     MagY  = %d uT     MagZ  = %d uT\r\n",
                 (int)imu[3], (int)imu[4], (int)imu[5]);

    /* Metadata */
    uint32_t seq      = (uint32_t)pkt[72] | ((uint32_t)pkt[73] << 8) |
                        ((uint32_t)pkt[74] << 16) | ((uint32_t)pkt[75] << 24);
    uint32_t ts_ms    = (uint32_t)pkt[76] | ((uint32_t)pkt[77] << 8) |
                        ((uint32_t)pkt[78] << 16) | ((uint32_t)pkt[79] << 24);
    uint32_t tot_sent = (uint32_t)pkt[80] | ((uint32_t)pkt[81] << 8) |
                        ((uint32_t)pkt[82] << 16) | ((uint32_t)pkt[83] << 24);
    uart2_printf("  [Meta] seq=%lu  timestamp=%lu ms  total_pkt_sent=%lu\r\n",
                 (unsigned long)seq, (unsigned long)ts_ms, (unsigned long)tot_sent);

    uint16_t crc_recv = (uint16_t)pkt[124] | ((uint16_t)pkt[125] << 8);
    uart2_printf("  [CRC-16] 0x%04X\r\n", (unsigned)crc_recv);
    uart2_puts("=============================================================\r\n");

    static char hk_hex[GS_HK_PKT_LEN * 2 + 1];
    static const char hexn[] = "0123456789ABCDEF";
    uint16_t hn = 0;
    for (uint16_t i = 0; i < GS_HK_PKT_LEN && hn + 2 < sizeof(hk_hex); i++) {
        hk_hex[hn++] = hexn[pkt[i] >> 4];
        hk_hex[hn++] = hexn[pkt[i] & 0x0F];
    }
    hk_hex[hn] = '\0';

    /* Machine-readable JSON for live GUI decode (all ADC channels + raw hex) */
    uart2_printf(
        "{\"type\":\"HK\",\"vbat\":%d,\"vsol\":%d,\"vbus\":%d,"
        "\"sp5\":%d,\"sp4\":%d,\"sp3\":%d,\"sp1\":%d,\"sp2\":%d,"
        "\"t_ant\":%d,\"t_bat\":%d,\"t_bpb\":%d,"
        "\"i_unreg\":%d,\"i_3v3\":%d,\"i_5v\":%d,\"i_bat\":%d,\"i_bus\":%d,"
        "\"f1\":%d,\"f2\":%d,"
        "\"gx\":%d,\"gy\":%d,\"gz\":%d,\"mx\":%d,\"my\":%d,\"mz\":%d,"
        "\"seq\":%lu,\"crc\":%d,\"rssi\":%d,\"hex\":\"",
        (int)adc1[0], (int)adc1[1], (int)adc1[2],
        (int)adc1[3], (int)adc1[4], (int)adc1[5], (int)adc1[6], (int)adc1[7],
        (int)adc1[8], (int)adc1[9], (int)adc1[10],
        (int)adc2[0], (int)adc2[1], (int)adc2[2], (int)adc2[3], (int)adc2[9],
        (int)adc2[10], (int)adc2[11],
        (int)imu[0], (int)imu[1], (int)imu[2],
        (int)imu[3], (int)imu[4], (int)imu[5],
        (unsigned long)seq, f1ok && f2ok && enok ? 1 : 0, (int)s_last_rssi);
    /* The 256-character hex string goes out separately: uart2_printf has a 360-byte buffer and the whole line (about 560
     * characters) was cut off in the middle of the hex, so Mission Control could not parse a single HK packet (seen on
     * hardware 2026-11-03). */
    uart2_puts(hk_hex);
    uart2_puts("\"}\r\n");
}

/* ============================================================================
 * CAMERA CHUNK HANDLER
 * Reassembles 128-byte payloads into a JPEG image buffer.
 * ========================================================================== */
static void GS_Handle_Camera_Chunk(const uint8_t *pkt, uint16_t plen)
{
    bool is_cam   = GS_Is_Cam_Fragment(pkt, plen);
    bool is_flash = (plen >= GS_FLASH_HDR_LEN + 1) &&
                    ((pkt[0] == GS_FLASH_PKT_MAGIC_LO && pkt[1] == GS_FLASH_PKT_MAGIC_HI) ||
                     (pkt[0] == GS_FLASH_PKT_MAGIC_HI && pkt[1] == GS_FLASH_PKT_MAGIC_LO));
    if (!is_cam && !is_flash) return;  /* not a chunk frame (or a camera fragment with a bad CRC) */

    uint16_t pkt_idx = (uint16_t)pkt[2] | ((uint16_t)pkt[3] << 8);
    uint16_t total_p = (uint16_t)pkt[4] | ((uint16_t)pkt[5] << 8);
    uint32_t offset;
    uint16_t data_len;
    uint8_t  status;
    const uint8_t *data;
    if (is_cam) {
        offset   = (uint32_t)pkt_idx * GS_CAM_DATA_LEN;     /* no offset/length fields in the sheet */
        data_len = GS_CAM_DATA_LEN;                         /* last fragment is zero padded */
        status   = 0;
        data     = &pkt[GS_CAM_HDR_LEN];
    } else {
        offset   = (uint32_t)pkt[6]  | ((uint32_t)pkt[7]  << 8) |
                   ((uint32_t)pkt[8]  << 16) | ((uint32_t)pkt[9]  << 24);
        data_len = (uint16_t)pkt[10] | ((uint16_t)pkt[11] << 8);
        status   = pkt[12];
        if (data_len == 0 || data_len > GS_CAM_DATA_LEN) data_len = GS_CAM_DATA_LEN;
        data     = &pkt[GS_FLASH_HDR_LEN];
    }

    /* Start of new transfer */
    if (pkt_idx == 0 || !gs_cam_active) {
        gs_cam_active   = true;
        gs_cam_bytes    = 0;
        gs_cam_pkt_recv = 0;
        gs_cam_total    = total_p;
        gs_cam_jpeg_soi = false;
        gs_cam_jpeg_eoi = false;
        uart2_printf("\r\n>>> [GS CAM] Starting camera download: %u chunks (%u bytes) <<<\r\n",
                     total_p, (unsigned)(total_p * GS_CAM_DATA_LEN));
    }

    gs_cam_total = total_p;

    /* Write data into reassembly buffer */
    if (offset + data_len <= GS_CAM_BUF_MAX) {
        memcpy(&gs_cam_buf[offset], data, data_len);
        if (offset + data_len > gs_cam_bytes)
            gs_cam_bytes = offset + data_len;
    } else {
        uart2_printf("[GS CAM] WARNING: chunk offset 0x%04lX out of buffer range\r\n",
                     (unsigned long)offset);
    }

    /* Check JPEG SOI / EOI in this chunk */
    for (uint16_t i = 0; i < data_len - 1; i++) {
        if (data[i] == 0xFF && data[i + 1] == 0xD8) { gs_cam_jpeg_soi = true; }
        if (data[i] == 0xFF && data[i + 1] == 0xD9) { gs_cam_jpeg_eoi = true; }
    }

    gs_cam_pkt_recv++;

    uart2_printf("[GS CAM] Pkt %u/%u  off=0x%04lX  len=%u  status=%u  %s%s\r\n",
                 pkt_idx + 1, total_p,
                 (unsigned long)offset, data_len, status,
                 gs_cam_jpeg_soi ? "[SOI] " : "",
                 gs_cam_jpeg_eoi ? "[EOI] " : "");

    /* Output machine-readable tags for Python gs_communicator.py / Mission Control (both camera and file chunks carry their bytes as hex) */
    static char cam_hex[GS_CAM_DATA_LEN * 2 + 1];
    {
        static const char hexn[] = "0123456789ABCDEF";
        uint16_t n = 0;
        for (uint16_t i = 0; i < data_len && n + 2 < sizeof(cam_hex); i++) {
            cam_hex[n++] = hexn[data[i] >> 4];
            cam_hex[n++] = hexn[data[i] & 0x0F];
        }
        cam_hex[n] = '\0';
    }
    if (is_cam) {
        uart2_printf("{\"type\":\"CAMERA\",\"pkt\":%u,\"total\":%u,\"offset\":%lu,\"len\":%u,\"hex\":\"%s\"}\r\n",
                     pkt_idx + 1, total_p, (unsigned long)offset, data_len, cam_hex);
    } else {
        uart2_printf("{\"type\":\"FLASH\",\"pkt\":%u,\"total\":%u,\"addr\":%lu,\"len\":%u,\"status\":%u,\"hex\":\"%s\"}\r\n",
                     pkt_idx + 1, total_p, (unsigned long)offset, data_len, status, cam_hex);
        uart2_puts("[FLASH-HEX] ");
        for (uint16_t i = 0; i < data_len; i++) {
            uart2_printf("%02X ", data[i]);
        }
        uart2_puts("\r\n");
    }

    /* Transfer complete */
    if (pkt_idx + 1 >= total_p || gs_cam_jpeg_eoi) {
        uart2_puts("\r\n=======================================================\r\n");
        uart2_printf(">>> [GS CAM] DOWNLOAD COMPLETE: %u/%u pkts | %lu bytes <<<\r\n",
                     gs_cam_pkt_recv, gs_cam_total, (unsigned long)gs_cam_bytes);
        uart2_printf("    JPEG SOI: %s | JPEG EOI: %s\r\n",
                     gs_cam_jpeg_soi ? "FOUND (0xFFD8)" : "NOT FOUND",
                     gs_cam_jpeg_eoi ? "FOUND (0xFFD9)" : "NOT FOUND");

        /* Print first 32 bytes of image for verification */
        if (gs_cam_bytes >= 2) {
            uart2_puts("    First 32 bytes: ");
            uint16_t print_n = (uint16_t)(gs_cam_bytes < 32 ? gs_cam_bytes : 32);
            for (uint16_t i = 0; i < print_n; i++)
                uart2_printf("%02X ", gs_cam_buf[i]);
            uart2_puts("\r\n");
        }
        uart2_puts("=======================================================\r\n");
        gs_cam_active = false;
    }
    (void)is_flash;
}

/* ============================================================================
 * AX.25 FRAME VALIDATION
 * ========================================================================== */
static bool GS_Validate_AX25(const uint8_t *f, uint16_t len)
{
    if (len < 20) return false;
    if (f[0] != AX25_FLAG || f[len - 1] != AX25_FLAG) return false;
    /* Address extension bit */
    for (uint8_t i = 1; i <= 13; i++)
        if (f[i] & 0x01) return false;
    if ((f[14] & 0x01) == 0) return false;
    if (f[15] != 0x03) return false;
    if (f[16] != 0xF0) return false;
    return AX25_VerifyCRC_Method1(f, len) || AX25_VerifyCRC_Method2(f, len, NULL, NULL, NULL);
}

/* Try to decode one received raw buffer (with overlap retry from previous) */
static bool GS_Decode_Frame(const uint8_t *raw, uint16_t raw_len,
                              uint8_t *out, uint16_t cap, uint16_t *out_len)
{
    uint16_t n = 0;
    if (Protocol_ExtractFrame(raw, raw_len, out, cap, &n) && GS_Validate_AX25(out, n)) {
        s_prev_rx_len = 0;
        *out_len = n;
        return true;
    }
    /* Overlap with previous packet */
    if (s_prev_rx_len > 0) {
        memcpy(&s_rx_stream[0], s_prev_rx, s_prev_rx_len);
        memcpy(&s_rx_stream[s_prev_rx_len], raw, raw_len);
        n = 0;
        if (Protocol_ExtractFrame(s_rx_stream, (uint16_t)(s_prev_rx_len + raw_len),
                                  out, cap, &n) && GS_Validate_AX25(out, n)) {
            s_prev_rx_len = 0;
            *out_len = n;
            uart2_puts("[GS RX] Frame recovered from previous+current packet\r\n");
            return true;
        }
    }
    memcpy(s_prev_rx, raw, raw_len);
    s_prev_rx_len = raw_len;
    return false;
}

/* ============================================================================
 * GS_Process_Received_Frame
 * Called from main loop whenever rx_done_flag is set.
 * Pipeline: raw G3RUH bytes → Protocol_ExtractFrame (descramble+HDLC)
 *           → AX.25 validate → payload dispatch
 * ========================================================================== */
/* ---- ADDED: raw air-rate test counters (commands RATE ON / RATE? / RATE OFF) ----------------------------------------
 * With RATE ON the console is silent (g_uart_quiet) and every decoded camera-format packet is only counted, so the GS can
 * keep up with a satellite that transmits back to back. RATE? prints one JSON line with the totals. */
static volatile uint8_t s_rate_on;
static uint32_t s_rate_rx, s_rate_bad, s_rate_uniq, s_rate_first, s_rate_last;
static int32_t  s_rate_rssi_sum;
static uint8_t  s_rate_seen[128];                 /* bitmap of packet indexes seen (up to 1024) */

static void GS_Rate_Reset(void)
{
    s_rate_rx = s_rate_bad = s_rate_uniq = s_rate_first = s_rate_last = 0;
    s_rate_rssi_sum = 0;
    memset(s_rate_seen, 0, sizeof(s_rate_seen));
}

static void GS_Rate_Report(void)
{
    uint8_t q = g_uart_quiet;
    g_uart_quiet = 0;
    uart2_printf("{\"type\":\"RATE\",\"on\":%u,\"rx\":%lu,\"bad\":%lu,\"uniq\":%lu,\"first\":%lu,\"last\":%lu,\"rssi\":%ld}\r\n",
                 (unsigned)s_rate_on, (unsigned long)s_rate_rx, (unsigned long)s_rate_bad, (unsigned long)s_rate_uniq,
                 (unsigned long)s_rate_first, (unsigned long)s_rate_last,
                 (long)(s_rate_rx ? s_rate_rssi_sum / (int32_t)s_rate_rx : 0));
    g_uart_quiet = q;
}

void GS_Process_Received_Frame(void)
{
    if (!rx_done_flag && s_rx_n == 0) return;

    GS_Radio_Lock();
    if (s_rx_n == 0) {
        rx_done_flag = 0;
        GS_Radio_Unlock();
        return;
    }
    uint8_t i = (uint8_t)((s_rx_w + GS_RX_SLOTS - s_rx_n) % GS_RX_SLOTS);
    uint16_t raw_len = s_rx_slot_len[i];
    if (raw_len > AX25_MAX_FRAME_SIZE) raw_len = AX25_MAX_FRAME_SIZE;
    memcpy(s_rx_raw, (const void *)s_rx_slot[i], raw_len);
    s_last_rssi = s_rx_slot_rssi[i];
    s_last_snr  = s_rx_slot_snr[i];
    s_rx_n--;
    rx_done_flag = (s_rx_n > 0) ? 1 : 0;
    GS_Radio_Unlock();

    stat_pkts_received++;
    if (s_rate_on) { s_rate_rx++; s_rate_rssi_sum += s_last_rssi; }

    uart2_printf("\r\n[GS RX] Packet #%lu | RSSI=%d dBm | SNR=%d dB | %u bytes\r\n",
                 (unsigned long)stat_pkts_received,
                 (int)s_last_rssi, (int)s_last_snr, raw_len);

    uint16_t decoded_len = 0;
    if (!GS_Decode_Frame(s_rx_raw, raw_len,
                         s_rx_decoded, sizeof(s_rx_decoded), &decoded_len)) {
        if (s_rate_on) s_rate_bad++;
        uart2_puts("[GS RX] *** DECODE FAILED: no valid G3RUH/AX.25 frame with good FCS ***\r\n");
        uart2_puts("[GS RX] Raw G3RUH on-air bytes:\r\n");
        Print_Hex_Bytes(s_rx_raw, raw_len);
        GS_ResumeRx();
        return;
    }

    stat_pkts_decoded++;
    uart2_printf("[GS RX] *** AX.25 DECODE OK (%u bytes, FCS verified) ***\r\n", decoded_len);

    /* ---- Step 3: Extract source callsign ---- */
    char src_cs[7], dest_cs[7];
    AX25_DecodeAddress(&s_rx_decoded[1], dest_cs);
    AX25_DecodeAddress(&s_rx_decoded[8], src_cs);
    uart2_printf("[GS RX] FROM: %s  TO: %s\r\n", src_cs, dest_cs);

    /* ---- Step 4: Payload extraction ---- */
    const uint16_t ps   = 17;                           /* payload start */
    const uint16_t pe   = (uint16_t)(decoded_len - 3);  /* payload end (before FCS+flag) */
    uint16_t plen = (pe > ps) ? (uint16_t)(pe - ps) : 0;
    const uint8_t *pl   = &s_rx_decoded[ps];

    /* ADDED: integrity check (AUTH=TRUE). A short payload carries a 12-byte trailer (counter + tag): verify it and continue
     * with the inner payload. Anything else (e.g. the 138-byte camera fragments, never wrapped) is used as-is.
     * plen/pl lost their const so the rest of this function sees the unwrapped data. */
    static uint8_t s_gs_unwrapped[SEAL_MAX_PAYLOAD];
    uint16_t unwrapped_len = 0;
    if (SEAL_LEN_MAY_BE_WRAPPED(plen) &&
        Protocol_DecodePayload(pl, plen, s_gs_unwrapped, sizeof(s_gs_unwrapped), &unwrapped_len)) {
        pl = s_gs_unwrapped;
        plen = unwrapped_len;
        uart2_printf("[GS RX] integrity check OK (%u byte payload)\r\n", plen);
    }

    if (s_rate_on && GS_Is_Cam_Fragment(pl, plen)) {
        /* rate test: count only (FCS was already verified by the decoder), do not assemble an image */
        uint16_t idx = (uint16_t)pl[2] | ((uint16_t)pl[3] << 8);
        uint32_t t = GS_GetTimeMs();
        if (idx < 1024U && !(s_rate_seen[idx >> 3] & (1U << (idx & 7U)))) {
            s_rate_seen[idx >> 3] |= (uint8_t)(1U << (idx & 7U));
            s_rate_uniq++;
        }
        if (s_rate_first == 0) s_rate_first = t;
        s_rate_last = t;
        GS_ResumeRx();
        return;
    }

    bool is_cam_early   = GS_Is_Cam_Fragment(pl, plen);
    bool is_flash_early = (plen >= 2) &&
        ((pl[0] == GS_FLASH_PKT_MAGIC_LO && pl[1] == GS_FLASH_PKT_MAGIC_HI) ||
         (pl[0] == GS_FLASH_PKT_MAGIC_HI && pl[1] == GS_FLASH_PKT_MAGIC_LO));
    if (!is_cam_early && !is_flash_early) {
        Print_AX25_Fields("DOWNLINK FRAME", s_rx_decoded, decoded_len);
    }

    if (plen == 0) {
        uart2_puts("[GS RX] Empty payload, ignoring\r\n");
        GS_ResumeRx();
        return;
    }

    uart2_printf("[GS RX] Payload: %u bytes | First byte: 0x%02X\r\n", plen, pl[0]);

    /* ---- Step 5: Dispatch by payload type ---- */

    /* 5a: ACK (0xAC): byte 1 = 0x00 command accepted, 0x01 command completed; byte 2 = mission id (Sheet 2) */
    if (pl[0] == SAT_ACK_ACCEPTED_BYTE && plen >= 3) {
        stat_ack_received++;
        if (pl[1] == 0x01) {
            uart2_printf("[SATELLITE-ACK] ACK COMPLETED (0xAC 0x01) from satellite. Mission=0x%02X\r\n", pl[2]);
            uart2_printf("{\"type\":\"ACK\",\"msg\":\"ACK Completed (0xAC) mission=0x%02X\"}\r\n", pl[2]);
        } else {
            uart2_printf("[SATELLITE-ACK] ACK ACCEPTED (0xAC 0x00) from satellite. Mission=0x%02X\r\n", pl[2]);
            uart2_printf("{\"type\":\"ACK\",\"msg\":\"ACK Accepted (0xAC) mission=0x%02X\"}\r\n", pl[2]);
        }
        GS_ResumeRx();
        return;
    }
    /* 5b: NACK / error (0xEE): byte 1 = error code 0x02..0x0B, byte 2 = mission id (Sheet 2). */
    if (pl[0] == SAT_NACK_BYTE && plen >= 3) {
        static const char *const err_names[] = {
            "command accepted", "command completed", "packet error", "CRC/checksum error", "incomplete packet",
            "requested data missing", "requested data unavailable", "satellite busy", "mission inactive",
            "invalid command", "memory/file error", "transmission failed" };
        stat_nack_received++;
        const char *en = (pl[1] < (sizeof(err_names) / sizeof(err_names[0]))) ? err_names[pl[1]] : "unknown error";
        uart2_printf("[SATELLITE-NACK] NACK (0xEE) from satellite! err=0x%02X (%s) mission=0x%02X\r\n", pl[1], en, pl[2]);
        uart2_printf("{\"type\":\"NACK\",\"msg\":\"NACK (0xEE) err=0x%02X %s mission=0x%02X\"}\r\n", pl[1], en, pl[2]);
        GS_ResumeRx();
        return;
    }
    /* Legacy 1-byte ACK */
    if (pl[0] == SAT_LEGACY_ACK_BYTE && plen == 1) {
        stat_ack_received++;
        uart2_puts("[SATELLITE-ACK] LEGACY ACK (0xAA) received\r\n");
        uart2_puts("{\"type\":\"ACK\",\"msg\":\"Legacy ACK (0xAA)\"}\r\n");
        GS_ResumeRx();
        return;
    }

    /* Legacy 1-byte NACK */
    if (pl[0] == SAT_LEGACY_NACK_BYTE && plen == 1) {
        stat_nack_received++;
        uart2_puts("[SATELLITE-NACK] LEGACY NACK (0x55) received\r\n");
        uart2_puts("{\"type\":\"NACK\",\"msg\":\"Legacy NACK (0x55)\"}\r\n");
        GS_ResumeRx();
        return;
    }

    /* 5c: HK 128-byte telemetry packet
     *     Identified by: plen == 128 AND footer bytes at [32..33] = 0x55 0xAA */
    if (plen == GS_HK_PKT_LEN &&
        pl[32] == GS_HK_FOOTER1_LO && pl[33] == GS_HK_FOOTER1_HI) {
        uart2_puts("[GS RX] *** HK TELEMETRY PACKET DETECTED (128 bytes) ***\r\n");
        GS_Display_HK_Packet(pl, plen);
        GS_ResumeRx();
        return;
    }

    /* 5d: Camera chunk / Flash chunk
     *     Identified by magic: 0xFE 0xCA (camera) or 0xF0 0xFD (flash) */
    bool is_cam_match   = GS_Is_Cam_Fragment(pl, plen);
    bool is_flash_match = (plen >= GS_FLASH_HDR_LEN + 1) &&
                          ((pl[0] == GS_FLASH_PKT_MAGIC_LO && pl[1] == GS_FLASH_PKT_MAGIC_HI) ||
                           (pl[0] == GS_FLASH_PKT_MAGIC_HI && pl[1] == GS_FLASH_PKT_MAGIC_LO));
    if (is_cam_match || is_flash_match) {
        uart2_printf("[GS RX] *** %s CHUNK (pkt_idx=%u total=%u) ***\r\n",
                     is_cam_match ? "CAMERA" : "FLASH",
                     (unsigned)((uint16_t)pl[2] | ((uint16_t)pl[3] << 8)),
                     (unsigned)((uint16_t)pl[4] | ((uint16_t)pl[5] << 8)));
        GS_Handle_Camera_Chunk(pl, plen);
        GS_ResumeRx();
        return;
    }

    /* 5e: Camera ACK string (ASCII "ACK: CAM CAPTURE ...") */
    bool is_ascii_printable = true;
    for (uint16_t i = 0; i < plen && i < 64; i++) {
        if (pl[i] < 0x20 || pl[i] >= 0x7F) { is_ascii_printable = false; break; }
    }
    if (is_ascii_printable) {
        char ascii_buf[68];
        uint16_t copy_n = (plen < 67) ? plen : 67;
        memcpy(ascii_buf, pl, copy_n);
        ascii_buf[copy_n] = '\0';
        uart2_printf("[GS RX] ASCII response: \"%s\"\r\n", ascii_buf);
        /* Check for ACK/NACK in ASCII text */
        if (strstr(ascii_buf, "ACK") || strstr(ascii_buf, "OK")) {
            stat_ack_received++;
            uart2_puts("[GS RX] *** SATELLITE ACK (ASCII) ***\r\n");
        } else if (strstr(ascii_buf, "NACK") || strstr(ascii_buf, "ERR")) {
            stat_nack_received++;
            uart2_puts("[GS RX] *** SATELLITE NACK (ASCII) ***\r\n");
        }
        GS_ResumeRx();
        return;
    }

    /* 5f: Unknown binary payload — just hex dump */
    uart2_puts("[GS RX] Unknown payload (binary):\r\n");
    Print_Hex_Bytes(pl, (plen < 64) ? plen : 64);

    GS_ResumeRx();
}

/* ============================================================================
 * GS_Listen_CW_And_Packets
 * Listen for up to max_duration_ms, polling CW + checking for GMSK packets.
 * ========================================================================== */
void GS_Listen_CW_And_Packets(uint32_t max_duration_ms, uint16_t expected_packets)
{
    uint32_t start = HAL_GetTick();
    uint16_t pkts  = 0;

    uart2_printf("[GS LISTEN] Waiting up to %lu ms for downlink "
                 "(expect %u packets)...\r\n",
                 (unsigned long)max_duration_ms, expected_packets);

    GS_StartRx();

    while ((HAL_GetTick() - start) < max_duration_ms) {
        GS_CW_Poll();
        GS_RxDiag_Poll();

        if (rx_timeout_flag || rx_error_flag) {
            rx_timeout_flag = 0;
            rx_error_flag   = 0;
            GS_StartRx();
        }

        if (rx_done_flag) {
            GS_Process_Received_Frame();
            pkts++;
            if (expected_packets > 0 && pkts >= expected_packets) break;
        }

        CPU2_Delay_Ms(1);
    }

    uart2_printf("[GS LISTEN] Done. Received %u frame(s).\r\n", pkts);
}

/* ============================================================================
 * HARDWARE
 * ========================================================================== */
extern void SysTick_Init_CPU2(uint32_t sys_freq_hz);
extern void MX_SUBGHZ_Init(void);
extern void CPU2_Delay_Ms(uint32_t ms);

uint32_t GS_GetTimeMs(void)  { return HAL_GetTick(); }
void     GS_DelayMs(uint32_t ms) { CPU2_Delay_Ms(ms); }

void GS_Init_VectorTable(void)
{
    SCB->VTOR = GS_CPU2_VECTOR_TABLE_ADDR;
    __DSB(); __ISB();
}

void GS_Init_PeripheralClocks(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_SUBGHZ_CLK_ENABLE();
    RCC->C2AHB2ENR |= RCC_C2AHB2ENR_GPIOAEN |
                      RCC_C2AHB2ENR_GPIOBEN |
                      RCC_C2AHB2ENR_GPIOCEN;
}

void GS_Init_IPCC_Isolation(void)
{
#ifdef IPCC_C2_RX_C2_TX_IRQn
    NVIC_DisableIRQ(IPCC_C2_RX_C2_TX_IRQn);   /* CHANGED: was HAL_NVIC_DisableIRQ */
#endif
}

void GS_Hardware_Init(void)
{
    SystemInit();
    GS_Init_VectorTable();
    LL_Init_CPU2();   /* CHANGED: was HAL_Init() (core/ll_port.c) */
    SysTick_Init_CPU2(HAL_RCC_GetHCLK2Freq());
    GS_Init_PeripheralClocks();
    GS_Init_IPCC_Isolation();
    uart2_init();
    RBI_Init();
    RBI_ConfigRFSwitch(GS_CFG_DEFAULT_RF_SWITCH);
    MX_SUBGHZ_Init();
    /* CHANGED: HAL_NVIC_* -> CMSIS NVIC_* (same registers, HAL cortex module no longer linked) */
    NVIC_SetPriority(SUBGHZ_Radio_IRQn, 0);
    NVIC_EnableIRQ(SUBGHZ_Radio_IRQn);
    __enable_irq();
    if (s_config.uartSettleDelayMs > 0)
        CPU2_Delay_Ms(s_config.uartSettleDelayMs);
}

/* ============================================================================
 * INIT
 * ========================================================================== */
void GS_Init(const GroundStationConfig_t *config)
{
    if (config != NULL) s_config = *config;
    else                s_config = GroundStationDefaultConfig;

    /* GS TX = 437.375 MHz (HP), GS RX = 435.000 MHz */
    s_config.radio.txFrequency = GS_UPLINK_FREQ_HZ;
    s_config.radio.rxFrequency = GS_DOWNLINK_FREQ_HZ;

    RadioApp_Init(&s_config.radio, &s_RadioEvents);

    /* GS is STM32WL55JC2 → RFO_HP (+22 dBm) for uplink TX */
    RadioApp_SetPaSelect(RFO_HP);
    RadioApp_SetTxPower(22);
    GS_SetRFSwitch(RBI_SWITCH_RFO_HP);

    s_last_auto_cmd_time_ms = GS_GetTimeMs();

    /* Initialise CW decoder (435.000 MHz RX path) */
    CwRx_Init(GS_CW_Event, GS_GetTimeMs());

    uart2_puts("\r\n========================================================================\r\n");
    uart2_puts("  ANTARIKCHYA PRATISTHAN NEPAL (APN)  —  S2S-2 GROUND STATION\r\n");
    uart2_puts("  FW-ID: GS-G3RUH-20261006 | RFO_HP +15 dBm | FULL DOWNLINK DECODE\r\n");
    uart2_puts("========================================================================\r\n");
    uart2_printf("  Uplink TX       : %lu Hz  (437.375 MHz, RFO_HP, +15 dBm)\r\n",
                 (unsigned long)s_config.radio.txFrequency);
    uart2_printf("  Downlink RX     : %lu Hz  (435.000 MHz, CW + GMSK 4800 bps)\r\n",
                 (unsigned long)s_config.radio.rxFrequency);
    uart2_puts(  "  Downlink framing: 128-bit preamble + sync word 93 0B 51 DE + 200 B\r\n");
    uart2_puts(  "  Decode pipeline : G3RUH descramble → AX.25 FCS → payload dispatch\r\n");
    uart2_puts(  "  Payloads        : ACK/NACK, HK 128B, Camera chunks (JPEG reasm.), Flash\r\n");
    uart2_printf("  Packets/command : %u  (single-shot uplink)\r\n", (unsigned)GS_SEND_COUNT);
    uart2_puts("========================================================================\r\n\r\n");

    /* Arm receiver on downlink frequency */
    GS_StartRx();
}

/* ============================================================================
 * CONFIG ACCESSORS
 * ========================================================================== */
void GS_SetConfig(const GroundStationConfig_t *config) { if (config) s_config = *config; }
const GroundStationConfig_t *GS_GetConfig(void)        { return &s_config; }

void GS_SetRFSwitch(RBI_Switch_TypeDef rf)
{
    s_config.rfSwitchConfig = rf;
    RBI_ConfigRFSwitch(rf);
}

RBI_Switch_TypeDef GS_GetRFSwitch(void) { return s_config.rfSwitchConfig; }

/* ============================================================================
 * UPLINK SEND  — single-shot, clamps burst_count to GS_SEND_COUNT = 1
 * ========================================================================== */
/* transmit the packet already built in s_tx_frame (burst_count times) and re-arm the receiver */
static bool GS_Transmit_Frame(uint16_t tx_frame_len, uint8_t burst_count)
{
    /* Show unscrambled AX.25 frame */
    uint8_t ax25_clean[AX25_MAX_FRAME_SIZE];
    uint16_t ax25_len = Protocol_GetLastTxAx25Frame(ax25_clean, sizeof(ax25_clean));
    if (ax25_len > 0) {
        uart2_printf("\r\n>>> [AX.25 TX FRAME (%u bytes, unscrambled)] <<<\r\n", ax25_len);
        Print_Hex_Bytes(ax25_clean, ax25_len);
        Print_AX25_Fields("TX AX.25", ax25_clean, ax25_len);
        uart2_puts(">>> [ENCODE] AX.25 -> HDLC stuff -> NRZI -> G3RUH scramble (200 B)\r\n");
        uart2_puts(">>> [RF] 128-bit preamble + sync 93 0B 51 DE + G3RUH payload @ 437.375 MHz\r\n");
    }

    uart2_printf(
        "\r\n=======================================================\r\n"
        ">>> [UPLINK-TX] %u PKTS @ %lu.%03lu MHz (%s %+d dBm) <<<\r\n"
        "=======================================================\r\n",
        (unsigned)burst_count,
        (unsigned long)(s_config.radio.txFrequency / 1000000UL),
        (unsigned long)((s_config.radio.txFrequency % 1000000UL) / 1000UL),
        (RadioApp_GetPaSelect() == RFO_HP) ? "RFO_HP" : "RFO_LP",
        (int)RadioApp_GetTxPower());

    /* Mute CW decoder during and after TX to avoid self-interference */
    CwRx_Mute(GS_GetTimeMs() + GS_TX_WAIT_MS + GS_CW_MUTE_AFTER_TX_MS);

    for (uint8_t b = 0; b < burst_count; b++) {
        GS_Radio_Idle();
        tx_busy = 1;
        stat_cmd_transmitted++;
        RadioApp_Send(s_tx_frame, tx_frame_len);
        GS_Print_RF_Pins();

        uint32_t cnt = GS_TX_WAIT_MS;
        while (tx_busy != 0 && cnt > 0) { CPU2_Delay_Ms(1); cnt--; }

        if (tx_busy != 0) {
            uart2_puts("TX TIMEOUT\r\n");
            stat_tx_timeouts++;
            GS_Radio_Idle();
            tx_busy = 0;
        }

        uart2_printf("  [%u/%u] Uplink packet sent OK\r\n", b + 1, burst_count);
        if (b + 1 < burst_count) CPU2_Delay_Ms(120);
    }

    uart2_puts(">>> UPLINK COMPLETE — arming downlink RX <<<\r\n");

    /* Re-arm receiver on downlink frequency immediately after TX */
    GS_StartRx();
    return true;
}


bool GS_Send_Burst_Packet(const uint8_t *payload, uint16_t len, uint8_t burst_count)
{
    if (!payload || len == 0) return false;
    burst_count = (burst_count > 0) ? burst_count : GS_SEND_COUNT;

    uint16_t tx_frame_len = Protocol_CreatePacket(s_tx_frame, payload, len, &s_config.radio);
    if (tx_frame_len == 0) {
        uart2_puts("!!! [TX ERROR] Protocol_CreatePacket failed\r\n");
        return false;
    }
    return GS_Transmit_Frame(tx_frame_len, burst_count);
}

/* ADDED: send a complete AX.25 frame (any source, destination and digipeater path) -- test traffic for the satellite digipeater.
 * body = destination, source, via path, control, PID, information (no flags, no FCS). No integrity trailer: it is not a command. */
bool GS_Send_Raw_Frame(const uint8_t *body, uint16_t len)
{
    uint16_t tx_frame_len = Protocol_CreateRawPacket(s_tx_frame, body, len);
    if (tx_frame_len == 0) {
        uart2_puts("!!! [TX ERROR] frame does not fit the radio packet\r\n");
        return false;
    }
    return GS_Transmit_Frame(tx_frame_len, 1);
}

bool GS_Send_Packet(const uint8_t *payload, uint16_t len)
{
    return GS_Send_Burst_Packet(payload, len, 1);
}

/* ============================================================================
 * TELECOMMAND BUILDERS
 * ========================================================================== */
bool GS_Send_Telecommand(uint8_t mcu_id, const uint8_t opcode[3],
                          uint16_t data_id, uint32_t addr, uint16_t count)
{
    uint8_t cmd[13];
    cmd[0]  = 0x53;
    cmd[1]  = mcu_id;
    cmd[2]  = opcode[0];
    cmd[3]  = opcode[1];
    cmd[4]  = opcode[2];
    cmd[5]  = (uint8_t)((data_id >> 8) & 0xFF);
    cmd[6]  = (uint8_t)(data_id & 0xFF);
    cmd[7]  = (uint8_t)((addr >> 24) & 0xFF);
    cmd[8]  = (uint8_t)((addr >> 16) & 0xFF);
    cmd[9]  = (uint8_t)((addr >> 8)  & 0xFF);
    cmd[10] = (uint8_t)(addr & 0xFF);
    cmd[11] = (uint8_t)((count >> 8) & 0xFF);
    cmd[12] = (uint8_t)(count & 0xFF);
    return GS_Send_Burst_Packet(cmd, sizeof(cmd), GS_SEND_COUNT);
}

/* FLAGS / SATLOG / RSVTABLE / EPDM / ADCS [chunks]: one file download of the file table. Returns true when the line was one of these. */
static bool GS_File_Command(const char *u)
{
    for (unsigned i = 0; i < sizeof(GS_FILES) / sizeof(GS_FILES[0]); i++) {
        size_t n = strlen(GS_FILES[i].cmd);
        if (strncmp(u, GS_FILES[i].cmd, n) != 0 || (u[n] != '\0' && u[n] != ' ')) continue;
        uint16_t cnt = (u[n] == ' ') ? (uint16_t)strtoul(u + n + 1, NULL, 10) : 5U;
        if (cnt == 0) cnt = 5;
        uart2_printf(">>> [UPLINK] %s download: %s (directory D%X, file F%X), %u chunks\r\n",
                     GS_FILES[i].cmd, GS_FILES[i].file, GS_FILES[i].dir & 0x0F, GS_FILES[i].no & 0x0F, cnt);
        const uint8_t opc[3] = { 0x1D, GS_FILES[i].dir, GS_FILES[i].no };
        GS_Send_Telecommand(MCU_ID_OBC, opc, GS_FILES[i].no, 0UL, cnt);
        return true;
    }
    return false;
}

bool GS_Send_Telecommand_ByName(const char *name, uint16_t count)
{
    if (strcmp(name, "HK1")  == 0) return GS_Send_Burst_Packet(CMD_HK1,    13, GS_SEND_COUNT);
    if (strcmp(name, "HK2")  == 0) return GS_Send_Burst_Packet(CMD_HK2,    13, GS_SEND_COUNT);
    if (strcmp(name, "CAM")  == 0) return GS_Send_Burst_Packet(CMD_CAMDOWN,13, GS_SEND_COUNT);
    if (strcmp(name, "SNAP") == 0) return GS_Send_Burst_Packet(CMD_CAMON,  13, GS_SEND_COUNT);
    if (strcmp(name, "DUMMY") == 0 || strcmp(name, "DUMMYCAM") == 0 ||
        strcmp(name, "DC") == 0 || strcmp(name, "TESTCAM") == 0) {
        static const uint8_t dummy_cmd[13] = { 0x53, 0x04, 0xCC, 0x5E, 0xBE,
                                               0x00, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x45 };
        return GS_Send_Burst_Packet(dummy_cmd, 13, GS_SEND_COUNT);
    }
    if (strcmp(name, "PING") == 0) return GS_Send_Burst_Packet(CMD_PING,   13, GS_SEND_COUNT);
    if (strcmp(name, "ANT") == 0 || strcmp(name, "ANTENNA") == 0) return GS_Send_Burst_Packet(CMD_ANTENNA, 13, GS_SEND_COUNT);
    if (strcmp(name, "FLASH") == 0 || strcmp(name, "D") == 0)
        return GS_Send_Telecommand(MCU_ID_OBC, OPCODE_FLASH, 0x00F8,
                                   0x00000000, count ? count : 5);
    return false;
}

/* ============================================================================
 * AUTOMATION
 * ========================================================================== */
void GS_Auto_Send_Command(void)
{
    if (!s_auto_cmd_enabled) return;
    uint32_t now = GS_GetTimeMs();
    if ((now - s_last_auto_cmd_time_ms) >= s_config.autoCmdIntervalMs) {
        s_last_auto_cmd_time_ms = now;
        uart2_puts(">>> [AUTO-CMD] Sending scheduled HK1 <<<\r\n");
        GS_Send_Burst_Packet(CMD_HK1, sizeof(CMD_HK1), GS_SEND_COUNT);
    }
}

/* ============================================================================
 * HELP / STATUS
 * ========================================================================== */
static void GS_Bw_Usage(void);

void GS_Print_Help(void)
{
    uart2_puts("\r\n=== GROUND STATION COMMANDS (1 packet per command) ===\r\n");
    uart2_puts("  1 / HK1         : Housekeeping telemetry request 1 (voltages+temps)\r\n");
    uart2_puts("  2 / HK2         : Housekeeping telemetry request 2 (currents+flags)\r\n");
    uart2_puts("  3 / SNAP/CAMON  : Camera ON — real photo via UART (0xCA), downlink\r\n");
    uart2_puts("  DUMMYCAM/DC     : Camera ON — dummy test JPEG downlink\r\n");
    uart2_puts("  5 / CD / CAM    : Camera download (downlink stored photo)\r\n");
    uart2_puts("  4 / D [cnt]     : Flash data request (cnt packets from address 0)\r\n");
    uart2_puts("  D ADDR CNT      : Flash data request (hex addr, decimal count)\r\n");
    uart2_puts("  P / PING        : Ping OBC\r\n");
    uart2_puts("  ADCS / EPDM     : run the ADCS / EPDM mission (ACK only)\r\n");
    uart2_puts("  ADCSFILE / EPDMFILE / FLAGS / SATLOG / RSVTABLE [n] : download adcs.txt / epdm.txt / flags.txt / sat_log.txt / rsv_table.txt (file table F7 F6 F1 F3 F4, n chunks of 128 bytes, default 5)\r\n");
    uart2_puts("  DIGI SRC>DEST,VIA:text : send one UI frame with that path (test the satellite digipeater)\r\n");
    uart2_puts("  BURST           : 50-packet GMSK burst test (0x01)\r\n");
    uart2_puts("  S               : Status\r\n");
    uart2_puts("  AUTO ON|OFF     : Periodic automatic HK1\r\n");
    uart2_puts("  HEX <bytes>     : Transmit raw hex bytes\r\n");
    uart2_puts("  PA LP|HP        : Select PA (LP=+14 dBm, HP=+22 dBm)\r\n");
    uart2_puts("  PWR <dbm>       : Set TX power\r\n");
    uart2_puts("  CARRIER <sec>   : Test CW carrier on uplink frequency\r\n");
    uart2_puts("  LISTEN [ms]     : Listen for downlink packets for <ms> (default 30000)\r\n");
    GS_Bw_Usage();
    uart2_puts("========================================================\r\n");
}

void GS_Print_Status(void)
{
    uart2_puts("\r\n=== GROUND STATION STATUS ===\r\n");
    uart2_printf("  TX Frequency  : %lu Hz (437.375 MHz uplink)\r\n",
                 (unsigned long)s_config.radio.txFrequency);
    uart2_printf("  RX Frequency  : %lu Hz (435.000 MHz downlink)\r\n",
                 (unsigned long)s_config.radio.rxFrequency);
    uart2_printf("  TX Power      : %+d dBm (%s)\r\n",
                 (int)RadioApp_GetTxPower(),
                 (RadioApp_GetPaSelect() == RFO_HP) ? "RFO_HP" : "RFO_LP");
    uart2_printf("  Pkts TX       : %lu | TX Timeouts: %lu\r\n",
                 (unsigned long)stat_cmd_transmitted, (unsigned long)stat_tx_timeouts);
    uart2_printf("  Pkts RX (raw) : %lu | Pkts Decoded: %lu\r\n",
                 (unsigned long)stat_pkts_received, (unsigned long)stat_pkts_decoded);
    uart2_printf("  ACK recv      : %lu | NACK recv: %lu\r\n",
                 (unsigned long)stat_ack_received, (unsigned long)stat_nack_received);
    uart2_printf("  Cam chunks    : %u/%u | Total bytes: %lu | JPEG: SOI=%s EOI=%s\r\n",
                 gs_cam_pkt_recv, gs_cam_total, (unsigned long)gs_cam_bytes,
                 gs_cam_jpeg_soi ? "YES" : "no",
                 gs_cam_jpeg_eoi ? "YES" : "no");
    uart2_printf("  CW noise floor: %d dBm | CW peak: %d dBm\r\n",
                 (int)CwRx_GetNoiseDbm(), (int)CwRx_GetPeakDbm());
    uart2_printf("  Auto-Command  : %s\r\n", s_auto_cmd_enabled ? "ON" : "OFF");
    uart2_puts("=============================\r\n");
}

/* ============================================================================
 * TX BANDWIDTH TEST (ADDED, for a spectrum analyser such as a tinySA)
 *
 *   BW CW  [s] [dBm]   unmodulated carrier: marks the centre frequency (and the analyser's own frequency error)
 *   BW DEV [s] [dBm]   packets of all-ones and all-zeros bits, alternating: two steady lines at fc +/- deviation.
 *                      With max-hold the line spacing = 2 x deviation (design value 2 x 1200 Hz = 2.4 kHz)
 *   BW CWB [s] [dBm]   Morse beacon "S2S2 9N2SJ" (500 ms tuning carrier, then the text at the satellite's keying speed), repeated
 *                      for [s] seconds: a keyed carrier that is easy to spot on a waterfall
 *   BW MOD [s] [dBm]   the real signal: framed pseudo-random packets (HDLC, NRZI, G3RUH scrambler, Gaussian BT 0.5, 4800 bit/s)
 *                      sent back to back. This is the one to read the occupied bandwidth from.
 *
 * Seconds default to 10 (max 120). The test runs on the normal GS output (RFO_HP, the proven path) at +10 dBm by default (-9 .. +10 dBm; check the analyser's
 * maximum input, use an attenuator if it is lower); the normal PA selection and power are restored afterwards. Nothing is received while it runs.
 * ========================================================================== */
#define GS_BW_DEFAULT_S     10UL
#define GS_BW_MAX_S         120UL
#define GS_BW_DEFAULT_DBM   10          /* requested for the tinySA bench; use an attenuator if the analyser is rated lower */
#define GS_BW_MIN_DBM       (-9)        /* SX126x high-power PA range -9 .. +22 dBm */
#define GS_BW_MAX_DBM       10          /* analyser inputs are rated about +6..+10 dBm */
#define GS_BW_PAYLOAD_LEN   120U

static void GS_Bw_Usage(void)
{
    uart2_puts("  BW CW|CWB|DEV|MOD [seconds] [dBm]  : TX spectrum test for a tinySA / SDR (RFO_HP, default +10 dBm, max +10)\r\n");
    uart2_puts("      CW = carrier, CWB = Morse beacon, DEV = lines at fc +/- deviation, MOD = real GMSK signal (read the bandwidth here)\r\n");
}

static void GS_Bw_Test(const char *args)
{
    while (*args == ' ') args++;
    const char *e = args;
    while (*e && *e != ' ') e++;
    size_t ml = (size_t)(e - args);
    int kind = 0;                                   /* 1 CW, 2 DEV, 3 MOD */
    if (ml == 2 && strncmp(args, "CW", 2) == 0)       kind = 1;
    else if (ml == 3 && strncmp(args, "CWB", 3) == 0) kind = 4;
    else if (ml == 3 && strncmp(args, "DEV", 3) == 0) kind = 2;
    else if (ml == 3 && strncmp(args, "MOD", 3) == 0) kind = 3;
    if (kind == 0) { GS_Bw_Usage(); return; }

    char *endp = (char *)e;
    unsigned long sec = strtoul(e, &endp, 10);
    while (*endp == ' ') endp++;
    long dbm = (*endp != '\0') ? strtol(endp, NULL, 10) : GS_BW_DEFAULT_DBM;
    if (sec == 0) sec = GS_BW_DEFAULT_S;
    if (sec > GS_BW_MAX_S) sec = GS_BW_MAX_S;
    if (dbm > GS_BW_MAX_DBM) { dbm = GS_BW_MAX_DBM; uart2_puts("  (power limited to +10 dBm for this test)\r\n"); }
    if (dbm < GS_BW_MIN_DBM) dbm = GS_BW_MIN_DBM;

    uint32_t fc = s_config.radio.txFrequency;
    uint8_t  saved_pa  = RadioApp_GetPaSelect();
    int8_t   saved_pwr = RadioApp_GetTxPower();

    /* the frame for MOD is built before the PA is touched: a failure leaves everything as it was */
    uint16_t flen = RADIO_FIXED_PACKET_LEN;
    if (kind == 3) {
        uint8_t pay[GS_BW_PAYLOAD_LEN];
        uint32_t x = 0x2545F491UL;                  /* xorshift32: any payload is whitened by the scrambler anyway */
        for (unsigned i = 0; i < sizeof(pay); i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; pay[i] = (uint8_t)x; }
        flen = Protocol_CreatePacket(s_tx_frame, pay, sizeof(pay), &s_config.radio);
        if (flen == 0) { uart2_puts("!!! [BW] could not build the test frame\r\n"); return; }
    }

    RadioApp_SetPaSelect(RFO_HP);
    RadioApp_SetTxPower((int8_t)dbm);
    GS_SetRFSwitch(RBI_SWITCH_RFO_HP);

    uart2_printf("\r\n>>> [BW] %s for %lu s @ %lu Hz, RFO_HP %+d dBm <<<\r\n",
                 kind == 1 ? "CW carrier" : kind == 4 ? "CW Morse beacon" : kind == 2 ? "DEV lines" : "MOD real GMSK signal",
                 sec, (unsigned long)fc, (int)dbm);
    uart2_puts("    tinySA: centre = TX frequency, span 30 kHz, RBW 1 kHz (300 Hz for detail), max hold.\r\n");
    if (kind == 2) uart2_puts("    Expect two lines 2 x deviation apart (design: 2.4 kHz, fc +/- 1.2 kHz).\r\n");
    if (kind == 3) uart2_puts("    Read the width at -3 / -20 / -40 dB below the peak with two markers (delta).\r\n");

    CwRx_Mute(GS_GetTimeMs() + sec * 1000UL + GS_CW_MUTE_AFTER_TX_MS);
    uint32_t sent = 0, timeouts = 0;
    if (kind == 1) {
        RadioApp_TransmitCarrier(fc, (uint32_t)sec);
    } else if (kind == 4) {
        const uint32_t unit = CW_UNIT_DEFAULT_MS;
        uint32_t t_end = GS_GetTimeMs() + sec * 1000UL;
        RadioApp_CwKeyBegin(fc);
        do {
            RadioApp_CwKey(true);  CPU2_Delay_Ms(500);              /* tuning carrier */
            RadioApp_CwKey(false); CPU2_Delay_Ms(1000);
            for (const char *t = S2S2_CW_CALLSIGN; *t; t++) {
                if (*t == ' ') { CPU2_Delay_Ms(4 * unit); continue; }   /* 3 (after the character) + 4 = 7 units word gap */
                const char *m = CwRx_MorseFor(*t);
                for (; m && *m; m++) {
                    RadioApp_CwKey(true);  CPU2_Delay_Ms(*m == '-' ? 3 * unit : unit);
                    RadioApp_CwKey(false); CPU2_Delay_Ms(unit);
                }
                CPU2_Delay_Ms(2 * unit);                            /* 1 + 2 = 3 units between characters */
            }
            sent++;
            CPU2_Delay_Ms(2000);
        } while ((int32_t)(t_end - GS_GetTimeMs()) > 0);
        RadioApp_CwKeyEnd();
    } else {
        uint32_t t_end = GS_GetTimeMs() + sec * 1000UL;
        g_uart_quiet = 1;                           /* RadioApp_Send prints two lines per packet: keep the gaps short */
        while ((int32_t)(t_end - GS_GetTimeMs()) > 0) {
            if (kind == 2) memset(s_tx_frame, (sent & 1U) ? 0x00 : 0xFF, flen);
            GS_Radio_Idle();
            tx_busy = 1;
            RadioApp_Send(s_tx_frame, flen);
            uint32_t cnt = GS_TX_WAIT_MS;
            while (tx_busy != 0 && cnt > 0) { CPU2_Delay_Ms(1); cnt--; }
            if (tx_busy != 0) { timeouts++; tx_busy = 0; }
            sent++;
        }
        g_uart_quiet = 0;
    }

    GS_Radio_Idle();
    RadioApp_SetPaSelect(saved_pa);
    RadioApp_SetTxPower(saved_pwr);
    GS_SetRFSwitch(saved_pa == RFO_HP ? RBI_SWITCH_RFO_HP : RBI_SWITCH_RFO_LP);
    GS_StartRx();
    uart2_printf(">>> [BW] done: %lu packet(s)/beacon(s), %lu timeout(s); PA restored to %s %+d dBm, receiver armed <<<\r\n",
                 (unsigned long)sent, (unsigned long)timeouts, saved_pa == RFO_HP ? "RFO_HP" : "RFO_LP", (int)saved_pwr);
}

/* ============================================================================
 * COMMAND HANDLER
 * ========================================================================== */
#define SEND13(arr)  GS_Send_Burst_Packet((arr), 13, GS_SEND_COUNT)

void GS_Handle_Command_Line(const char *p)
{
    char u[CMD_LINE_MAX];
    size_t n = strlen(p);
    if (n >= sizeof(u)) n = sizeof(u) - 1;
    for (size_t i = 0; i < n; i++) u[i] = (char)toupper((unsigned char)p[i]);
    u[n] = '\0';

    /* HK1 */
    if (strcmp(u, "1") == 0 || strcmp(u, "HK") == 0 || strcmp(u, "HK1") == 0) {
        uart2_puts(">>> [UPLINK] HK1 request\r\n");
        SEND13(CMD_HK1);
    }
    /* HK2 */
    else if (strcmp(u, "2") == 0 || strcmp(u, "HK2") == 0) {
        uart2_puts(">>> [UPLINK] HK2 request\r\n");
        SEND13(CMD_HK2);
    }
    /* Camera ON — real camera mission */
    else if (strcmp(u, "3") == 0 || strcmp(u, "CAM ON") == 0 ||
             strcmp(u, "CAMON") == 0 || strcmp(u, "SNAP") == 0) {
        uart2_puts(">>> [UPLINK] CAM ON (real camera via UART 0xCA, downlink JPEG)\r\n");
        SEND13(CMD_CAMON);
    }
    /* Dummy camera test */
    else if (strcmp(u, "DUMMYCAM") == 0 || strcmp(u, "DC") == 0 ||
             strcmp(u, "TESTCAM") == 0  || strcmp(u, "TC") == 0 ||
             strcmp(u, "DUMMY") == 0    || strcmp(u, "DUMMY CAM") == 0 ||
             strcmp(u, "DUMMY CAMERA") == 0 || strcmp(u, "DUMMY CAMERA DOWNLOAD") == 0 ||
             strcmp(u, "DUMMY CAM DOWNLOAD") == 0) {
        uart2_puts(">>> [UPLINK] DUMMY CAMERA TEST (stored test image downlink)\r\n");
        uint8_t dummy_cmd[13] = { 0x53, 0x04, 0xCC, 0x5E, 0xBE,
                                  0x00, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x45 };
        SEND13(dummy_cmd);
    }
    /* Camera download */
    else if (strcmp(u, "5") == 0 || strcmp(u, "CD") == 0 ||
             strcmp(u, "CAM") == 0 || strcmp(u, "CAMERA") == 0 ||
             strcmp(u, "CAMDOWN") == 0 || strcmp(u, "CAM DOWNLOAD") == 0) {
        uart2_puts(">>> [UPLINK] Camera download (downlink stored photo)\r\n");
        /* Reset camera reassembly state */
        gs_cam_active   = false;
        gs_cam_bytes    = 0;
        gs_cam_pkt_recv = 0;
        SEND13(CMD_CAMDOWN);
    }
    /* ADCS / EPDM: run the mission (reference firmware commands) */
    else if (strcmp(u, "ADCS") == 0) {
        uart2_puts(">>> [UPLINK] ADCS mission run (MCU 03)\r\n");
        SEND13(CMD_ADCS);
    }
    else if (strcmp(u, "EPDM") == 0) {
        uart2_puts(">>> [UPLINK] EPDM mission run (MCU 05)\r\n");
        SEND13(CMD_EPDM);
    }
    /* file downloads of the sheet's file table: FLAGS, SATLOG, RSVTABLE, EPDMFILE, ADCSFILE [chunks] */
    else if (GS_File_Command(u)) {
    }
    /* Flash / Data request: D [count] or D ADDR CNT */
    else if (strcmp(u, "4") == 0 || strncmp(u, "D ", 2) == 0 ||
             strcmp(u, "D") == 0  || strncmp(u, "DOWNLINK", 8) == 0 ||
             strncmp(u, "FLASH", 5) == 0)
    {
        const char *arg = u;
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;

        uint32_t addr = 0;
        uint16_t cnt  = 5;

        if (*arg != '\0') {
            /* Try parsing optional start address (hex) and count */
            char *end1;
            unsigned long av = strtoul(arg, &end1, 16);
            if (end1 != arg) {
                addr = (uint32_t)av;
                while (*end1 == ' ') end1++;
                if (*end1 != '\0') {
                    cnt = (uint16_t)strtoul(end1, NULL, 10);
                    if (cnt == 0) cnt = 1;
                }
            } else {
                cnt = (uint16_t)strtoul(arg, NULL, 0);
                if (cnt == 0) cnt = 5;
            }
        }

        uart2_printf(">>> [UPLINK] Flash request: addr=0x%08lX, %u chunks\r\n",
                     (unsigned long)addr, cnt);
        uint8_t flash_cmd[13] = {
            0x53, MCU_ID_OBC, 0x1D, 0xD1, 0xF8,
            0x00, 0xF8,
            (uint8_t)((addr >> 24) & 0xFF), (uint8_t)((addr >> 16) & 0xFF),
            (uint8_t)((addr >>  8) & 0xFF), (uint8_t)(addr & 0xFF),
            (uint8_t)((cnt  >>  8) & 0xFF), (uint8_t)(cnt  & 0xFF)
        };
        SEND13(flash_cmd);
    }
    /* DIGI SRC>DEST,VIA,VIA:text -- one UI frame with that path, for testing the satellite digipeater, e.g.
     *   DIGI K1ABC>APRS,APNDIG:hello      DIGI K1ABC>APRS,WIDE2-2:!4237.14N/07120.83W-test */
    else if (strncmp(u, "DIGI ", 5) == 0) {
        uint8_t body[DIGI_MAX_BODY];
        uint16_t bl = Digi_FromTnc2(p + 5, body, sizeof(body));
        if (bl == 0) {
            uart2_puts("usage: DIGI SRC>DEST,VIA1,VIA2:text   (call signs A-Z 0-9, SSID -0..-15, '*' marks the last used via)\r\n");
        } else {
            uart2_printf(">>> [UPLINK] DIGI test frame (%u bytes): %s\r\n", bl, p + 5);
            GS_Send_Raw_Frame(body, bl);
        }
    }
    /* Antenna deployment */
    else if (strcmp(u, "ANT") == 0 || strcmp(u, "ANTENNA") == 0) {
        uart2_puts(">>> [UPLINK] ANTENNA DEPLOYMENT\r\n");
        SEND13(CMD_ANTENNA);
    }
    /* Ping */
    else if (strcmp(u, "P") == 0 || strcmp(u, "PING") == 0) {
        uart2_puts(">>> [UPLINK] PING\r\n");
        SEND13(CMD_PING);
    }
    /* Burst test */
    else if (strcmp(u, "COMMAND") == 0 || strcmp(u, "BURST") == 0) {
        uint8_t b = 0x01;
        uart2_puts(">>> [UPLINK] Burst request (0x01)\r\n");
        GS_Send_Burst_Packet(&b, 1, GS_SEND_COUNT);
    }
    /* TX bandwidth test for a spectrum analyser */
    else if (strncmp(u, "BW", 2) == 0 && (u[2] == '\0' || u[2] == ' ')) {
        GS_Bw_Test(u + 2);
    }
    /* Listen for downlink */
    else if (strncmp(u, "LISTEN", 6) == 0) {
        const char *arg = u + 6;
        while (*arg == ' ') arg++;
        uint32_t ms = (*arg != '\0') ? strtoul(arg, NULL, 10) : 30000UL;
        GS_Listen_CW_And_Packets(ms, 0);
    }
    /* Status */
    /* Raw air-rate test: RATE ON (silent counting), RATE? (print totals), RATE OFF (normal console again) */
    else if (strncmp(u, "RATE", 4) == 0) {
        if (strstr(u, "ON") != NULL) {
            GS_Rate_Reset();
            s_rate_on = 1;
            g_uart_quiet = 1;
            g_uart_quiet = 0; GS_Rate_Report(); g_uart_quiet = 1;
        } else if (strstr(u, "OFF") != NULL) {
            s_rate_on = 0;
            g_uart_quiet = 0;
            GS_Rate_Report();
        } else {
            GS_Rate_Report();
        }
    }
    else if (strcmp(u, "S") == 0) { GS_Print_Status(); }
    else if (strcmp(u, "?") == 0 || strcmp(u, "HELP") == 0) { GS_Print_Help(); }
    /* Carrier / CW tone test */
    else if (strncmp(u, "CARRIER", 7) == 0 || strncmp(u, "CW ", 3) == 0 ||
             strncmp(u, "TONE ", 5) == 0)
    {
        const char *ns = u;
        while (*ns && *ns != ' ') ns++;
        while (*ns == ' ') ns++;
        uint32_t sec = strtoul(ns, NULL, 10);
        if (sec == 0) sec = 5;
        uart2_printf(">>> [CARRIER] %lu s @ %lu Hz\r\n",
                     (unsigned long)sec, (unsigned long)s_config.radio.txFrequency);
        CwRx_Mute(GS_GetTimeMs() + sec * 1000UL + GS_CW_MUTE_AFTER_TX_MS);
        RadioApp_TransmitCarrier(s_config.radio.txFrequency, sec);
        GS_Radio_Idle();
        GS_StartRx();
    }
    /* RF pins debug */
    else if (strcmp(u, "RF") == 0) { GS_Print_RF_Pins(); }
    /* PA selection */
    else if (strcmp(u, "PA LP") == 0) {
        RadioApp_SetPaSelect(RFO_LP);
        RadioApp_SetTxPower(14);
        GS_SetRFSwitch(RBI_SWITCH_RFO_LP);
        uart2_puts(">>> PA set to RFO_LP (+14 dBm) <<<\r\n");
    }
    else if (strcmp(u, "PA HP") == 0) {
        RadioApp_SetPaSelect(RFO_HP);
        RadioApp_SetTxPower(22);
        GS_SetRFSwitch(RBI_SWITCH_RFO_HP);
        uart2_puts(">>> PA set to RFO_HP (+22 dBm, JC2) <<<\r\n");
    }
    /* TX power */
    else if (strncmp(u, "PWR ", 4) == 0 || strncmp(u, "POWER ", 6) == 0) {
        const char *arg = u;
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;
        RadioApp_SetTxPower((int8_t)atoi(arg));
        uart2_printf(">>> TX Power: %+d dBm <<<\r\n", (int)RadioApp_GetTxPower());
    }
    /* Auto-command */
    else if (strncmp(u, "AUTO ", 5) == 0) {
        s_auto_cmd_enabled = (strcmp(u + 5, "ON") == 0);
        uart2_printf("Auto-Command %s\r\n", s_auto_cmd_enabled ? "ENABLED" : "DISABLED");
    }
    /* Raw HEX uplink */
    else if (strncmp(u, "HEX ", 4) == 0 || strncmp(u, "HHEX ", 5) == 0 || strncmp(u, "HHHEX ", 6) == 0) {
        const char *hs  = (strncmp(u, "HHHEX ", 6) == 0) ? (u + 6) :
                          (strncmp(u, "HHEX ", 5) == 0) ? (u + 5) : (u + 4);
        uint16_t    hlen = 0;
        char byte_str[3] = {0};
        while (*hs && hlen < sizeof(s_hex_buf)) {
            while (*hs == ' ') hs++;
            if (!*hs) break;
            byte_str[0] = *hs++;
            byte_str[1] = *hs ? *hs++ : '0';
            s_hex_buf[hlen++] = (uint8_t)strtoul(byte_str, NULL, 16);
        }
        if ((hlen == 13 || hlen == 8) && s_hex_buf[0] == 0x53) {
            uart2_printf(">>> [UPLINK] Transmitting %u-byte Telecommand (Opcode 0x%02X)\r\n", hlen, s_hex_buf[1]);
            GS_Send_Burst_Packet(s_hex_buf, hlen, GS_SEND_COUNT);
        } else if (hlen == 1 && (s_hex_buf[0] == 0x01 || s_hex_buf[0] == 0x53)) {
            uart2_printf(">>> [UPLINK] Transmitting Burst Request (0x%02X)\r\n", s_hex_buf[0]);
            GS_Send_Burst_Packet(s_hex_buf, hlen, GS_SEND_COUNT);
        } else {
            uart2_printf("!!! [BLOCKED] Payload of %u bytes is NOT a valid Telecommand. Communication is restricted to valid Telecommands (13B starting with 0x53) only!\r\n", hlen);
        }
    }
    /* Check for direct hex telecommand string without 'HEX ' prefix, e.g. "53 04 CC 5E BE..." */
    else {
        const char *hs = u;
        uint16_t hlen = 0;
        char byte_str[3] = {0};
        while (*hs && hlen < sizeof(s_hex_buf)) {
            while (*hs == ' ') hs++;
            if (!*hs) break;
            if (!isxdigit((int)*hs)) { hlen = 0; break; }
            byte_str[0] = *hs++;
            if (!isxdigit((int)*hs)) { hlen = 0; break; }
            byte_str[1] = *hs++;
            s_hex_buf[hlen++] = (uint8_t)strtoul(byte_str, NULL, 16);
        }
        if ((hlen == 13 || hlen == 8) && s_hex_buf[0] == 0x53) {
            uart2_printf(">>> [UPLINK] Transmitting %u-byte Telecommand (Opcode 0x%02X)\r\n", hlen, s_hex_buf[1]);
            GS_Send_Burst_Packet(s_hex_buf, hlen, GS_SEND_COUNT);
        } else {
            uart2_printf("!!! [BLOCKED] \"%s\" is not a valid Telecommand. AX.25 G3RUH communication strictly allows only valid Telecommands (13B starting with 0x53)!\r\n", p);
        }
    }
}

/* ============================================================================
 * CONSOLE
 * ========================================================================== */
bool GS_Console_TryReadLine(char *out, uint8_t max)
{
    static char   buffer[CMD_LINE_MAX];
    static uint8_t index = 0;
    uint8_t c;

    while (uart2_try_getc(&c)) {
        if (c == '\b' || c == 0x7F) {
            if (index > 0) { index--; }
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (index == 0) continue;
            uint8_t copy_n = (index < (max - 1)) ? index : (uint8_t)(max - 1);
            memcpy(out, buffer, copy_n);
            out[copy_n] = '\0';
            index = 0;
            return true;
        }
        if (c >= 32 && c <= 126) {
            if (index < sizeof(buffer) - 1) {
                buffer[index++] = (char)c;
            }
        }
    }
    return false;
}

/* ============================================================================
 * MAIN  — STM32WL55JC2 Ground Station (CPU2)
 * ========================================================================== */
int main(void)
{
    GS_Hardware_Init();
    GS_Init(NULL);      /* also calls GS_StartRx() */

    char line_buf[CMD_LINE_MAX];

    uart2_puts("Ground Station Ready. RX armed on 435.000 MHz.\r\n");
    /* ADDED: run the integrity self-test at start-up (result also in g_sealSelfTest, readable over SWD) */
#if AUTH_ENABLE
    uart2_printf("[SEAL] integrity self-test: %s, key %s\r\n", Seal_SelfTest() ? "PASS" : "FAIL", S2S2_AUTH_KEY_FPR);
#else
    uart2_puts("[SEAL] AUTH OFF: plain AX.25 packets in the Packet Format sheet layout\r\n");
#endif
    uart2_puts("Type '?' or 'HELP' for command list.\r\n> ");

    while (1)
    {
        /* ---- Uplink automation ---- */
        GS_Auto_Send_Command();

        /* ---- CW beacon decode (continuous RSSI polling) ---- */
        GS_CW_Poll();
        GS_RxDiag_Poll();

        /* ---- GMSK packet decode ---- */
        if (rx_timeout_flag || rx_error_flag) {
            rx_timeout_flag = 0;
            rx_error_flag   = 0;
            GS_StartRx();   /* re-arm on error */
        }
        if (rx_done_flag) {
            GS_Process_Received_Frame();  /* decode + display */
            /* GS_Process_Received_Frame() re-arms RX internally if needed */
        }

        /* ---- Console command line ---- */
        if (GS_Console_TryReadLine(line_buf, sizeof(line_buf))) {
            char *cmd = line_buf;
            while (*cmd == ' ' || *cmd == '\t') cmd++;
            if (*cmd != '\0')
                GS_Handle_Command_Line(cmd);
            uart2_puts("> ");
        }
    }
    return 0;
}
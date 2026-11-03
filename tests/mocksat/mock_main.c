/*
 * MOCK SATELLITE for a stock Nucleo-WL55JC (CPU2 image, flash 0x08032000). NOT the flight firmware.
 *
 * Purpose: the ground station and Mission Control can be tested in REAL mode (real UART, real radio) against something that answers like
 * the satellite, without CPU1, the OBC, sensors or a camera. Everything on the air is built by the same code as the real satellite
 * (cpu2/satellite/app/satellite_app.c, mission/protocol.c, the radio layer): ACK / NACK frames, the 128-byte HK packet, camera fragments,
 * file chunks, the CW beacon. Only the DATA is made up, and only this file decides what to answer.
 *
 * What it does (same timing as the real satellite: a CW beacon every SAT_BEACON_PERIOD_MS measured from the start of the previous one, then
 * a listen window; a command received in the window is served at once and followed by the next beacon):
 *   - CW beacon 1 / 2 alternating, "S2S2 9N2SJ" + 8 bytes as hex groups, values from a compressed 10-minute orbit (sunlit 62 %, eclipse 38 %)
 *   - 53 01 1D D1 F5            ping            ACK, "ACK: PING OK (OBC_ONLINE)", ACK
 *   - 53 01 1D D1 F2            HEALTH SAT      ACK, one HK packet, ACK        (count N > 1: N stored HK records)
 *   - 53 01 1D D1 F8            HK download     ACK, N HK packets, ACK
 *   - 53 01 1D D1/D3 F1 F3 F4, D2/D4 F6 F7      file download: ACK, 3 text chunks, ACK
 *   - 53 01 1D D2/D4 F5         camera download ACK, the 8-fragment test picture (952 byte JPEG), ACK         <- the "quick exam"
 *   - 53 04 CC 5E BE            dummy camera    same picture
 *   - 53 04 CC 5E BD            run camera      ACK, 2 s, ACK
 *   - 53 03 .. .. ..  / 53 05 ..               run ADCS / EPDM mission: ACK, ACK
 *   - 53 01 DE EF AA            antenna deploy  ACK, 3 s, ACK        (nothing moves: this is a mock)
 *   - anything else             NACK EE 09 FF ; a 0x53 frame that is not 13 bytes: NACK EE 04 FF
 * With AUTH on, a replay-counter rejection resets the receive floor once (a rebooted ground station restarts its counter at 1; the real
 * satellite needs the VCP RESET for that and this image has no VCP). The tag is still checked.
 *
 * Build / flash: tests/mocksat/Makefile. The image is always built for the Nucleo front end; the flash guard refuses a CUBUS board.
 */
#include "s2s2_config.h"
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
#include "radio_driver.h"
#include "protocol.h"
#include "seal_wrap.h"
#include "ax25.h"
#include "g3ruh.h"

#include "satellite_app.h"
#include "Sring_buffer.h"
#include "mock_image.h"

const RadioConfig_t SatelliteProfile = {
    .txFrequency = DEFAULT_TX_FREQUENCY_HZ,
    .rxFrequency = DEFAULT_RX_FREQUENCY_HZ,
    .sourceCallsign = S2S2_SAT_CALLSIGN,
    .sourceSSID = S2S2_SAT_SSID,
    .destCallsign = S2S2_GS_CALLSIGN,
    .destSSID = S2S2_GS_SSID,
    .isSatelliteMode = true
};

#define BEACON_PERIOD_MS   S2S2_BEACON_PERIOD_MS
#define MOCK_ORBIT_MS      600000UL     /* compressed orbit (the real one is about 5600 s) so charts move during a bench session */
#define MOCK_SUN_PM        620U         /* sunlit part of the orbit, per mille */
#define MOCK_MAX_HK        12U          /* most stored HK records one download returns */

/* ------------------------------------------------------------------ made-up telemetry ------------------------------------------------------------------ */
typedef struct {
    bool    sunlit;
    int16_t sol_v, sol_i, bat_v, bat_i, i33, i5;          /* x100 */
    int16_t ant_t, bat_t, bpb_t;                          /* x10 degC */
    int16_t gyro[3];                                      /* centi-deg/s */
    int8_t  mag[3];
} MockTelem_t;

static MockTelem_t m;
static int16_t b1_data[5], b2_data[5];

static int32_t Wave(uint32_t period_ms, int32_t amp, uint32_t shift_ms)   /* triangle wave, -amp .. +amp */
{
    uint32_t t = (HAL_GetTick() + shift_ms) % period_ms;
    int32_t h = (int32_t)(period_ms / 2U);
    int32_t x = (t < (uint32_t)h) ? (int32_t)t : (int32_t)period_ms - (int32_t)t;       /* 0 .. h .. 0 */
    return (x * 2 - h) * amp / h;
}

static void Mock_Update(void)
{
    uint32_t ph = (HAL_GetTick() % MOCK_ORBIT_MS) / (MOCK_ORBIT_MS / 1000UL);             /* 0 .. 999 */
    int32_t charge, s;                                                                    /* state of charge and sun angle shape, 0 .. 1000 */
    m.sunlit = ph < MOCK_SUN_PM;
    if (m.sunlit) {
        charge = 300 + (int32_t)(600U * ph / MOCK_SUN_PM);
        s = (ph < MOCK_SUN_PM / 2U) ? (int32_t)(ph * 1000U / (MOCK_SUN_PM / 2U)) : (int32_t)((MOCK_SUN_PM - ph) * 1000U / (MOCK_SUN_PM / 2U));
    } else {
        charge = 900 - (int32_t)(600U * (ph - MOCK_SUN_PM) / (1000U - MOCK_SUN_PM));
        s = 0;
    }
    m.sol_v = m.sunlit ? (int16_t)(420 + 80 * s / 1000) : 0;
    m.sol_i = m.sunlit ? (int16_t)(20 + 60 * s / 1000) : 0;
    m.bat_v = (int16_t)(360 + 45 * charge / 1000);
    m.bat_i = m.sunlit ? (int16_t)(m.sol_i - 38) : (int16_t)(-42 + Wave(7000, 3, 0));
    m.i33   = (int16_t)(118 + Wave(5300, 6, 900));
    m.i5    = (int16_t)(18 + Wave(3100, 2, 0));
    m.ant_t = (int16_t)(charge / 3 - 190);
    m.bat_t = (int16_t)(180 + charge / 10);
    m.bpb_t = (int16_t)(200 + charge / 12);
    m.gyro[0] = (int16_t)Wave(9000, 24, 0);
    m.gyro[1] = (int16_t)Wave(11000, 18, 3000);
    m.gyro[2] = (int16_t)Wave(13000, 12, 5000);
    m.mag[0] = (int8_t)(22 + Wave(40000, 20, 0));
    m.mag[1] = (int8_t)(-8 + Wave(52000, 14, 9000));
    m.mag[2] = (int8_t)(-41 + Wave(61000, 9, 20000));

    b1_data[0] = m.bat_v; b1_data[1] = m.sol_v; b1_data[2] = m.ant_t; b1_data[3] = m.bat_t; b1_data[4] = m.bpb_t;
    b2_data[0] = m.i33;   b2_data[1] = m.i5;    b2_data[2] = m.bat_i; b2_data[3] = m.gyro[0]; b2_data[4] = m.gyro[1];
}

/* CW BEACON 1 / 2 of the Packet Format sheet, as in cpu2/satellite/app/satellite_main.c, with the made-up values */
static void Cw_Hex8(char *out, size_t n, const uint8_t b[8])
{
    snprintf(out, n, "%02X%02X %02X%02X %02X%02X %02X%02X", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
}

static void Cw_Beacon1(char *out, size_t n)
{
    uint8_t b[8];
    uint32_t hours = HAL_GetTick() / 3600000UL;
    b[0] = (uint8_t)((3U << 6) | ((hours > 15U) ? 15U : hours));          /* NORMAL MODE, mission 0, hours since reset */
    b[1] = (uint8_t)((m.sol_i > 255) ? 255 : m.sol_i);                    /* SOL_TOTAL_I */
    b[2] = (uint8_t)((uint16_t)m.sol_v >> 8); b[3] = (uint8_t)((uint16_t)m.sol_v & 0xFFU);
    b[4] = (uint8_t)((uint16_t)m.bat_v >> 8); b[5] = (uint8_t)((uint16_t)m.bat_v & 0xFFU);
    b[6] = (uint8_t)((uint16_t)m.bat_i >> 8); b[7] = (uint8_t)((uint16_t)m.bat_i & 0xFFU);
    Cw_Hex8(out, n, b);
}

static void Cw_Beacon2(char *out, size_t n)
{
    uint8_t b[8];
    for (int i = 0; i < 3; i++) {
        int32_t g = m.gyro[i] / 4;                                        /* int8 */
        b[i] = (uint8_t)(int8_t)g;
        b[3 + i] = (uint8_t)m.mag[i];
    }
    b[6] = m.sunlit ? 0x1FU : 0x00U;                                      /* solar cells lit */
    b[7] = (uint8_t)(int8_t)(m.bat_t / 10);                               /* BAT_TEMP, whole degrees */
    Cw_Hex8(out, n, b);
}

/* ------------------------------------------------------------------ receive path (as the real satellite) ------------------------------------------------------------------ */
static uint8_t s_rx_raw[AX25_MAX_FRAME_SIZE];
static uint8_t s_rx_decoded[AX25_MAX_FRAME_SIZE];

typedef struct {
    bool flash, cam_on, cam_download, hk, antenna, adcs, epdm, ping, dummy_cam, file_dl;
    uint8_t file_mission;
} Tc_t;

static void Tc_Classify(const uint8_t *p, Tc_t *c)
{
    memset(c, 0, sizeof(*c));
    if (p[0] != 0x53) return;
    const bool obc = (p[1] == 0x01), cam = (p[1] == CMD_TYPE_CAMERA);
    c->cam_on    = cam && p[2] == 0xCC && p[3] == 0x5E && p[4] == 0xBD;
    c->dummy_cam = cam && p[2] == 0xCC && p[3] == 0x5E && p[4] == 0xBE;
    c->antenna   = obc && p[2] == 0xDE && p[3] == 0xEF && p[4] == 0xAA;
    c->adcs      = (p[1] == CMD_TYPE_ADCS);
    c->epdm      = (p[1] == CMD_TYPE_EPDM);
    if (!obc || p[2] != 0x1D) return;
    const uint8_t dir = p[3], file = p[4];
    const bool d13 = (dir == 0xD1 || dir == 0xD3), d24 = (dir == 0xD2 || dir == 0xD4);
    c->ping         = dir == 0xD1 && file == 0xF5;
    c->hk           = d13 && file == 0xF2;
    c->cam_download = d24 && file == 0xF5;
    c->flash        = dir == 0xD1 && file == 0xF8;
    if (d13 && file == 0xF1) { c->file_dl = true; c->file_mission = SAT_MISSION_UNKNOWN; }
    if (d13 && file == 0xF3) { c->file_dl = true; c->file_mission = SAT_MISSION_LOG; }
    if (d13 && file == 0xF4) { c->file_dl = true; c->file_mission = SAT_MISSION_UNKNOWN; }
    if (d24 && file == 0xF6) { c->file_dl = true; c->file_mission = SAT_MISSION_EPDM; }
    if (d24 && file == 0xF7) { c->file_dl = true; c->file_mission = SAT_MISSION_ADCS; }
}

static bool Tc_Known(const Tc_t *c)
{
    return c->flash || c->cam_on || c->cam_download || c->hk || c->antenna || c->file_dl || c->adcs || c->epdm || c->ping || c->dummy_cam;
}

/* frame check of the real satellite (Sat_Validate_Frame) without the ASCII-hex route and the repair path */
static bool Frame_Valid(uint8_t *f, uint16_t len)
{
    if (len < 19 || f[0] != AX25_FLAG || f[len - 1] != AX25_FLAG) return false;
    for (uint16_t off = 14; off <= 18 && (off + 8) <= len; off++) {
        if (f[off] == 0x53) {
            uint16_t plen = (len > (off + 3)) ? (uint16_t)(len - 3 - off) : 0;
            if (off != 17 && (17 + plen) <= len) memmove(&f[17], &f[off], plen);
            f[15] = 0x03; f[16] = 0xF0;
            return true;
        }
    }
    return f[15] == 0x03 && f[16] == 0xF0 && (AX25_VerifyCRC_Method1(f, len) || AX25_VerifyCRC_Method2(f, len, NULL, NULL, NULL));
}

typedef enum { RX_NOTHING = 0, RX_COMMAND, RX_NACK } RxResult_t;

/* one received radio packet -> a 13-byte command, or a NACK reason, or nothing */
static RxResult_t Rx_Process(uint8_t cmd[CMD_PAYLOAD_LEN], uint8_t *nack_err)
{
    uint16_t raw_len = rx_frame_size, n = 0;
    if (raw_len == 0 || raw_len > sizeof(s_rx_raw)) return RX_NOTHING;
    memcpy(s_rx_raw, (const void *)rx_frame_buffer, raw_len);
    if (!Protocol_ExtractFrame(s_rx_raw, raw_len, s_rx_decoded, sizeof(s_rx_decoded), &n)) return RX_NOTHING;
    if (!Frame_Valid(s_rx_decoded, n)) return RX_NOTHING;

    char src[7];
    AX25_DecodeAddress(&s_rx_decoded[8], src);
    if (strcmp(src, SatelliteProfile.sourceCallsign) == 0) return RX_NOTHING;       /* our own echo */

    const uint16_t start = 17, end = (uint16_t)(n - 3);
    uint16_t plen = (end > start) ? (uint16_t)(end - start) : 0;
    const uint8_t *pl = &s_rx_decoded[start];
    if (plen == 0) return RX_NOTHING;

    uint8_t plain[SEAL_MAX_PAYLOAD];
    uint16_t plain_len = 0;
    if (SEAL_LEN_MAY_BE_WRAPPED(plen)) {
        bool ok = Protocol_DecodePayload(pl, plen, plain, sizeof(plain), &plain_len);
#if AUTH_ENABLE
        if (!ok) {                                  /* a restarted ground station counts from 1 again: forget the floor once, tag still checked */
            Seal_ResetRxFloor();
            ok = Protocol_DecodePayload(pl, plen, plain, sizeof(plain), &plain_len);
        }
#endif
        if (ok) { pl = plain; plen = plain_len; }
    }
#if AUTH_ENABLE
    else { return RX_NOTHING; }
    if (pl != plain) return RX_NOTHING;             /* AUTH on: no valid trailer, no command */
#endif

    for (uint16_t i = 0; i < plen; i++) {
        if (pl[i] == 0x53 && (uint16_t)(plen - i) >= CMD_PAYLOAD_LEN) { memcpy(cmd, &pl[i], CMD_PAYLOAD_LEN); return RX_COMMAND; }
    }
    if (pl[0] == 0x53) { *nack_err = SAT_ERR_INCOMPLETE; return RX_NACK; }
    return RX_NOTHING;
}

/* ------------------------------------------------------------------ answers ------------------------------------------------------------------ */
static void Send_Hk(void)
{
    Mock_Update();
    Satellite_Send_HK_Combined_Packet(b1_data, b2_data);
}

/* text file as FLASH chunks of FLASH_DATA_LEN bytes (zero padded) */
static void Send_Text_File(uint32_t base, const char *text)
{
    uint8_t chunk[FLASH_DATA_LEN];
    uint16_t len = (uint16_t)strlen(text), total = (uint16_t)((len + FLASH_DATA_LEN - 1U) / FLASH_DATA_LEN);
    Satellite_SetDCDCHold(true);
    for (uint16_t i = 0; i < total; i++) {
        memset(chunk, 0, sizeof(chunk));
        uint16_t part = (uint16_t)((len - i * FLASH_DATA_LEN) > FLASH_DATA_LEN ? FLASH_DATA_LEN : (len - i * FLASH_DATA_LEN));
        memcpy(chunk, text + i * FLASH_DATA_LEN, part);
        Satellite_Send_Flash_Chunk(i, total, base + (uint32_t)i * FLASH_DATA_LEN, 0, chunk);
        CPU2_Delay_Ms(500);
    }
}

static const char *File_Text(const uint8_t *c, char *buf, size_t n)
{
    switch (c[4]) {
    case 0xF1: return "flags.txt MOCK\nANT_DEPLOYED=0\nBAT_OK=1\nMODE=NORMAL\nRESET_COUNT=0\nCAM_READY=1\nADCS_RUN=0\nEPDM_RUN=0\n";
    case 0xF3: return "sat_log.txt MOCK\n[00:00:02] boot ok\n[00:00:03] radio init ok 435.000 MHz\n[00:00:05] beacon 1 sent\n"
                      "[00:01:35] beacon 2 sent\n[00:03:00] command served\n";
    case 0xF4: return "rsv_table.txt MOCK\nslot0 free\nslot1 free\nslot2 reserved CAM\nslot3 reserved ADCS\n";
    case 0xF6: snprintf(buf, n, "epdm.txt MOCK\nsample,t_ms,field_nT,e_mV_m\n0,%lu,%d,%d\n1,%lu,%d,%d\n2,%lu,%d,%d\n3,%lu,%d,%d\n",
                        (unsigned long)HAL_GetTick(), 48120, 3, (unsigned long)HAL_GetTick() + 1000UL, 48122, 2,
                        (unsigned long)HAL_GetTick() + 2000UL, 48119, 4, (unsigned long)HAL_GetTick() + 3000UL, 48121, 3);
               return buf;
    default:   snprintf(buf, n, "adcs.txt MOCK\nt_ms,q0,q1,q2,q3,wx,wy,wz\n%lu,0.9990,0.0210,-0.0150,0.0330,%d,%d,%d\n"
                        "%lu,0.9989,0.0212,-0.0152,0.0331,%d,%d,%d\n",
                        (unsigned long)HAL_GetTick(), m.gyro[0], m.gyro[1], m.gyro[2],
                        (unsigned long)HAL_GetTick() + 1000UL, m.gyro[0] + 1, m.gyro[1] - 1, m.gyro[2]);
               return buf;
    }
}

static void Send_Picture(void)
{
    const uint16_t total = (uint16_t)((MOCK_IMAGE_SIZE + CAM_DATA_LEN - 1U) / CAM_DATA_LEN);
    Satellite_SetDCDCHold(true);
    for (uint16_t i = 0; i < total; i++) {
        uint32_t off = (uint32_t)i * CAM_DATA_LEN, left = MOCK_IMAGE_SIZE - off;
        Satellite_Send_Camera_Chunk(i, total, &g_mock_image[off], (uint8_t)(left > CAM_DATA_LEN ? CAM_DATA_LEN : left));
        CPU2_Delay_Ms(500);                                           /* as the real image downlink: pace the ground station's UART */
    }
}

static void Serve(const uint8_t *c)
{
    Tc_t t;
    Tc_Classify(c, &t);
    const uint16_t count = (uint16_t)(((uint16_t)c[11] << 8) | c[12]);
    char buf[200];

    CPU2_Delay_Ms(SAT_ACK_GUARD_MS);                                  /* the ground station is still printing its report */
    if (!Tc_Known(&t)) {
        Satellite_Send_Nack(SAT_ERR_INVALID_CMD, SAT_MISSION_UNKNOWN);
        return;
    }
    if (t.flash || (t.hk && count > 1U)) {                            /* stored housekeeping records */
        uint16_t n = (count == 0U) ? 1U : (count > MOCK_MAX_HK ? MOCK_MAX_HK : count);
        Satellite_Send_Ack_Accepted(SAT_MISSION_HK);
        Satellite_SetDCDCHold(true);
        for (uint16_t i = 0; i < n; i++) { Send_Hk(); CPU2_Delay_Ms(500); }
        Satellite_Send_Ack_Completed(SAT_MISSION_HK);
    } else if (t.hk) {
        Satellite_Send_Ack_Accepted(SAT_MISSION_HK);
        Send_Hk();
        Satellite_Send_Ack_Completed(SAT_MISSION_HK);
    } else if (t.ping) {
        Satellite_Send_Ack_Accepted(SAT_MISSION_UNKNOWN);
        Satellite_Send_Ping_Response();
        Satellite_Send_Ack_Completed(SAT_MISSION_UNKNOWN);
    } else if (t.file_dl) {
        Satellite_Send_Ack_Accepted(t.file_mission);
        Send_Text_File(0x08040000UL, File_Text(c, buf, sizeof(buf)));
        Satellite_Send_Ack_Completed(t.file_mission);
    } else if (t.cam_download || t.dummy_cam) {
        Satellite_Send_Ack_Accepted(SAT_MISSION_CAM_RGB);
        Send_Picture();
        Satellite_Send_Ack_Completed(SAT_MISSION_CAM_RGB);
    } else if (t.cam_on) {
        Satellite_Send_Ack_Accepted(SAT_MISSION_CAM_RGB);
        CPU2_Delay_Ms(2000);
        Satellite_Send_Ack_Completed(SAT_MISSION_CAM_RGB);
    } else if (t.adcs || t.epdm) {
        const uint8_t mid = t.adcs ? SAT_MISSION_ADCS : SAT_MISSION_EPDM;
        Satellite_Send_Ack_Accepted(mid);
        CPU2_Delay_Ms(1000);
        Satellite_Send_Ack_Completed(mid);
    } else if (t.antenna) {
        Satellite_Send_Ack_Accepted(SAT_MISSION_UNKNOWN);
        CPU2_Delay_Ms(3000);
        Satellite_Send_Ack_Completed(SAT_MISSION_UNKNOWN);
    }
}

/* ------------------------------------------------------------------ main loop ------------------------------------------------------------------ */
int main(void)
{
    Satellite_Hardware_Init();
    Satellite_Init(NULL);
    const SatelliteConfig_t *cfg = Satellite_GetConfig();

    uart1_puts("\r\n=== MOCK SATELLITE (tests/mocksat) : not flight firmware, made-up data ===\r\n");

    RadioEvents_t events = { .TxDone = OnTxDone, .TxTimeout = OnTxTimeout, .RxDone = OnRxDone, .RxTimeout = OnRxTimeout, .RxError = OnRxError };
    RadioApp_Init(&SatelliteProfile, &events);
    RadioApp_SetPaSelect(SAT_PA_SEL);
    RadioApp_StartRx();

    uint32_t cycle = 0;
    uint8_t step = 0;
    char msg[64];

    for (;;) {
        /* ---- CW beacon: CW1 / CW2 alternating ---- */
        const uint32_t beacon_start = HAL_GetTick();
        cycle++;
        Mock_Update();
        if (step == 0U) Cw_Beacon1(msg, sizeof(msg)); else Cw_Beacon2(msg, sizeof(msg));
        uart1_printf("[MOCK] CW%u #%lu: %s %s\r\n", step + 1U, (unsigned long)cycle, cfg->cwBeaconCallsign, msg);
        Satellite_Send_CW_Beacon(cycle, step == 0U ? "CW1" : "CW2", cfg->cwBeaconCallsign, msg, cfg->morseUnitMs,
                                 cfg->cwTuningCarrierDurationMs, cfg->cwTuningPostDelayMs, cfg->cwInterStringDelayMs);
        step ^= 1U;

        /* ---- listen window until the next beacon is due ---- */
        RadioApp_StartRx();
        while ((uint32_t)(HAL_GetTick() - beacon_start) < BEACON_PERIOD_MS) {
            if (rx_timeout_flag || rx_error_flag) {
                rx_timeout_flag = 0;
                rx_error_flag = 0;
                RadioApp_StartRx();
            }
            if (rx_done_flag) {
                rx_done_flag = 0;
                uint8_t cmd[CMD_PAYLOAD_LEN], err = 0;
                RxResult_t r = Rx_Process(cmd, &err);
                if (r == RX_COMMAND) {
                    uart1_printf("[MOCK] command %02X %02X %02X %02X %02X ...\r\n", cmd[0], cmd[1], cmd[2], cmd[3], cmd[4]);
                    SUBGRF_SetStandby(STDBY_XOSC);
                    Serve(cmd);
                    break;                                                    /* like the real satellite: the next beacon follows at once */
                } else if (r == RX_NACK) {
                    SUBGRF_SetStandby(STDBY_XOSC);
                    CPU2_Delay_Ms(SAT_ACK_GUARD_MS);
                    Satellite_Send_Nack(err, SAT_MISSION_UNKNOWN);
                    break;
                }
            }
            CPU2_Delay_Ms(10);
        }
    }
}

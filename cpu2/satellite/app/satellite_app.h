#ifndef SATELLITE_APP_H
#define SATELLITE_APP_H

#include <stdint.h>
#include "s2s2_config.h"
#include <stdbool.h>
#include "config.h"
#include "radio_board_if.h"
#include "ax25.h"
#include "Sring_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * SATELLITE DEFAULT CONFIGURATION PARAMETERS & TIMINGS
 * ============================================================================
 * Adjust default operational frequencies, RF power, Morse beacon parameters,
 * GMSK burst settings, and mission guard intervals here.
 * ============================================================================
 */

/* 1. Radio Frequency & Profile Defaults */
#define SAT_CFG_DEFAULT_TX_FREQ_HZ         DEFAULT_TX_FREQUENCY_HZ /* 868.000 MHz Downlink (JC1) */
#define SAT_CFG_DEFAULT_RX_FREQ_HZ         DEFAULT_RX_FREQUENCY_HZ /* 868.000 MHz Uplink (JC1) */
#define SAT_CFG_DEFAULT_SRC_CALLSIGN       S2S2_SAT_CALLSIGN
#define SAT_CFG_DEFAULT_SRC_SSID           S2S2_SAT_SSID
#define SAT_CFG_DEFAULT_DEST_CALLSIGN      S2S2_GS_CALLSIGN
#define SAT_CFG_DEFAULT_DEST_SSID          S2S2_GS_SSID
#define SAT_CFG_DEFAULT_TX_POWER_DBM       RADIO_TX_POWER_DBM      /* Transmit RF power in dBm (controlled by config.h) */
#define SAT_CFG_DEFAULT_BITRATE_BPS        4800                    /* GMSK Bitrate (4800 bps) */
#define SAT_CFG_DEFAULT_FDEV_HZ            1200                    /* GMSK Frequency Deviation (+/- 1.2 kHz) */
#define SAT_CFG_DEFAULT_TX_TIMEOUT_MS      3000                    /* Radio transmission timeout in ms */

/* RF Front-End Switch Selection:
 * - STM32WL55JC1 (Satellite) strictly runs in Low Power (RFO_LP / PB2)
 *   connected to the external PA front-end:
 */
#define SAT_CFG_DEFAULT_RF_SWITCH          RBI_SWITCH_RFO_LP   /* CW: RFO_LP (PB2) -> 3.3V PA (PC2) */
#define SAT_CFG_DEFAULT_RF_SWITCH_5V       RBI_SWITCH_RFO_LP5V /* GMSK: RFO_LP (PB2) -> 5V PA (PC3) + 5V DC/DC (PA0) */

/* CPU2 (Cortex-M0+) Vector Table Base Address in Flash */
#define SATELLITE_CPU2_VECTOR_TABLE_ADDR   0x08032000UL

/* 2. Startup / Console Settle Delay */
#define SAT_CFG_DEFAULT_UART_SETTLE_MS     50                      /* Settle delay in ms after UART init */

/* 3. CW / Morse Code Configuration & Delays */
/* CHANGED: CW beacon identification per the CW Beacon sheet: project name, then call sign, then the housekeeping bytes */
#define SAT_CFG_DEFAULT_CW_CALLSIGN        S2S2_CW_CALLSIGN
#define SAT_CFG_DEFAULT_CW_MESSAGE         "Namaste everyone"
/* CHANGED (CW beacon shortened, was 80 ms / 2000 / 1000 / 1000 = 15 WPM, ~41 s per beacon, now ~28 s):
 * the ground-station decoder (cw_rx.h CW_UNIT_DEFAULT_MS) must use the SAME unit. */
#define SAT_CFG_DEFAULT_CW_MORSE_UNIT_MS   S2S2_CW_UNIT_MS         /* dit length; dah = 3 units */
#define SAT_CFG_DEFAULT_CW_DURATION_MS     90000                   /* 90-Second continuous CW beacon session */
#define SAT_CFG_DEFAULT_CW_TUNING_MS       500                     /* Pre-beacon tuning carrier tone (was 2000; GS needs >= 5 units = 300 ms) */
#define SAT_CFG_DEFAULT_CW_POST_DELAY_MS   300                     /* Delay after tuning carrier tone before Morse (was 1000) */
#define SAT_CFG_DEFAULT_CW_INTER_STR_MS    700                     /* Gap between callsign and payload (was 1000; GS needs >= 10 units = 600 ms for a segment break) */
#define SAT_CFG_DEFAULT_CW_REPEAT_INT_MS   1500                    /* 1.5s delay gap between repeat beacon sequences */

/* 4. GMSK Burst Configuration & Delays */
#define SAT_CFG_DEFAULT_GMSK_PAYLOAD       "Namaste everyone, Testing GMSK signal "
#define SAT_CFG_DEFAULT_GMSK_DURATION_MS   60000                   /* 1-Minute continuous GMSK burst session */
#define SAT_CFG_DEFAULT_GMSK_INTERVAL_MS   250                     /* 250 ms: CubeSat optimal safe interval (~102 pkts/min, 57% duty cycle, perfect SDR decode) */
#define SAT_CFG_DEFAULT_GMSK_TUNING_MS     0                       /* 0 ms: Disabled unmodulated CW tone before GMSK burst */
#define SAT_CFG_DEFAULT_GMSK_POST_DELAY_MS 0                       /* Guard delay after tuning tone */

/* 5. Mission State & Guard Delays */
#define SAT_CFG_DEFAULT_CYCLE_GUARD_MS     90000                 /* Guard delay between CW and GMSK */
#define SAT_CFG_DEFAULT_TELEMETRY_INT_MS   1000                    /* Delay after telemetry transmission */

/*
 * ============================================================================
 * SATELLITE CONFIGURATION & TIMING STRUCTURE
 * ============================================================================
 */
typedef struct {
    /* 1. Radio Frequency & Profile Parameters */
    RadioConfig_t      radio;                     /* txFrequency, rxFrequency, callsigns, SSID, satellite flag */
    int8_t             txPowerDbm;                /* Transmit RF power in dBm (e.g. 14 or 15 dBm) */
    uint32_t           bitrateBps;                /* GMSK Bitrate in bps (e.g. 4800 or 9600) */
    uint32_t           fdevHz;                    /* Frequency deviation in Hz (e.g. 1200 or 4800) */
    uint32_t           txTimeoutMs;               /* Radio TX timeout in ms (e.g. 3000 ms) */
    RBI_Switch_TypeDef rfSwitchConfig;            /* RF Switch: RBI_SWITCH_RFO_LP or RBI_SWITCH_RFO_HP */
    RBI_Switch_TypeDef rfSwitchConfig5V;          /* RF Switch: RBI_SWITCH_RFO_LP5V */

    /* 2. System Startup Delays */
    uint32_t           uartSettleDelayMs;         /* Settle delay after UART init before logs (e.g. 50 ms) */

    /* 3. CW / Morse Code Configuration & Delays */
    const char        *cwBeaconCallsign;          /* CW callsign text (e.g. "9NS2S2NEPAL") */
    const char        *cwBeaconMessage;           /* CW message text (e.g. "Namaste everyone...") */
    uint32_t           morseUnitMs;               /* Morse dit unit in ms (e.g. 80 ms for 15 WPM) */
    uint32_t           cwDurationMs;              /* Total CW session duration in ms (e.g. 60000 ms) */
    uint32_t           cwTuningCarrierDurationMs; /* Duration of pre-beacon tuning carrier tone (e.g. 2000 ms) */
    uint32_t           cwTuningPostDelayMs;       /* Delay after tuning carrier tone before Morse (e.g. 1000 ms) */
    uint32_t           cwInterStringDelayMs;      /* Gap delay between callsign and payload message (e.g. 1000 ms) */
    uint32_t           cwRepeatIntervalMs;        /* Gap delay between repeat beacon sequences (e.g. 2000 ms) */

    /* 4. GMSK Burst Configuration & Delays */
    const char        *gmskPayloadText;           /* GMSK payload message string */
    uint32_t           gmskDurationMs;            /* Total GMSK burst session duration in ms (e.g. 60000 ms) */
    uint32_t           gmskPacketIntervalMs;      /* Delay between burst packets in ms (e.g. 300 ms) */
    uint32_t           gmskTuningCarrierDurationMs; /* Duration of pre-burst 5V tuning carrier tone (e.g. 2000 ms) */
    uint32_t           gmskTuningPostDelayMs;       /* Delay after 5V tuning carrier tone before burst (e.g. 500 ms) */

    /* 5. Mission State Guard Delays */
    uint32_t           cycleGuardDelayMs;         /* Guard interval delay between sessions in ms (e.g. 1000 ms) */
    uint32_t           telemetryIntervalMs;       /* Delay after telemetry transmission in ms (e.g. 1000 ms) */
} SatelliteConfig_t;

/* Default Configuration Initializer Macro */
#define SATELLITE_CONFIG_DEFAULT { \
    .radio = { \
        .txFrequency    = SAT_CFG_DEFAULT_TX_FREQ_HZ, \
        .rxFrequency    = SAT_CFG_DEFAULT_RX_FREQ_HZ, \
        .sourceCallsign = SAT_CFG_DEFAULT_SRC_CALLSIGN, \
        .sourceSSID     = SAT_CFG_DEFAULT_SRC_SSID, \
        .destCallsign   = SAT_CFG_DEFAULT_DEST_CALLSIGN, \
        .destSSID       = SAT_CFG_DEFAULT_DEST_SSID, \
        .isSatelliteMode = true \
    }, \
    .txPowerDbm                 = SAT_CFG_DEFAULT_TX_POWER_DBM, \
    .bitrateBps                 = SAT_CFG_DEFAULT_BITRATE_BPS, \
    .fdevHz                     = SAT_CFG_DEFAULT_FDEV_HZ, \
    .txTimeoutMs                = SAT_CFG_DEFAULT_TX_TIMEOUT_MS, \
    .rfSwitchConfig             = SAT_CFG_DEFAULT_RF_SWITCH, \
    .rfSwitchConfig5V           = SAT_CFG_DEFAULT_RF_SWITCH_5V, \
    .uartSettleDelayMs          = SAT_CFG_DEFAULT_UART_SETTLE_MS, \
    .cwBeaconCallsign           = SAT_CFG_DEFAULT_CW_CALLSIGN, \
    .cwBeaconMessage            = SAT_CFG_DEFAULT_CW_MESSAGE, \
    .morseUnitMs                = SAT_CFG_DEFAULT_CW_MORSE_UNIT_MS, \
    .cwDurationMs               = SAT_CFG_DEFAULT_CW_DURATION_MS, \
    .cwTuningCarrierDurationMs  = SAT_CFG_DEFAULT_CW_TUNING_MS, \
    .cwTuningPostDelayMs        = SAT_CFG_DEFAULT_CW_POST_DELAY_MS, \
    .cwInterStringDelayMs       = SAT_CFG_DEFAULT_CW_INTER_STR_MS, \
    .cwRepeatIntervalMs         = SAT_CFG_DEFAULT_CW_REPEAT_INT_MS, \
    .gmskPayloadText            = SAT_CFG_DEFAULT_GMSK_PAYLOAD, \
    .gmskDurationMs             = SAT_CFG_DEFAULT_GMSK_DURATION_MS, \
    .gmskPacketIntervalMs       = SAT_CFG_DEFAULT_GMSK_INTERVAL_MS, \
    .gmskTuningCarrierDurationMs = SAT_CFG_DEFAULT_GMSK_TUNING_MS, \
    .gmskTuningPostDelayMs       = SAT_CFG_DEFAULT_GMSK_POST_DELAY_MS, \
    .cycleGuardDelayMs          = SAT_CFG_DEFAULT_CYCLE_GUARD_MS, \
    .telemetryIntervalMs        = SAT_CFG_DEFAULT_TELEMETRY_INT_MS, \
}

/* Global Default Configuration Instance */
extern const SatelliteConfig_t SatelliteDefaultConfig;

/*
 * ============================================================================
 * SATELLITE API FUNCTIONS - ACCESSIBLE FROM MAIN
 * ============================================================================
 */

/* 0. Low-Level CPU2 Hardware & Peripheral Bus Initialization */
void                     Satellite_Hardware_Init(void);
void                     Satellite_Init_VectorTable(void);
void                     Satellite_Init_PeripheralClocks(void);
void                     Satellite_Init_IPCC_Isolation(void);

/* 1. Core Lifecycle & Configuration */
void                     Satellite_Init(const SatelliteConfig_t *config);
void                     Satellite_SetConfig(const SatelliteConfig_t *config);
const SatelliteConfig_t* Satellite_GetConfig(void);

/* 2. RF Switch Selection (Choose RBI_SWITCH_RFO_LP or RBI_SWITCH_RFO_HP from main) */
void               Satellite_SetRFSwitch(RBI_Switch_TypeDef rf_switch);
RBI_Switch_TypeDef Satellite_GetRFSwitch(void);

/* 3. Timing & Delay Controls (Send and control delays directly from main) */
void     Satellite_DelayMs(uint32_t ms);
void     CPU2_Delay_Ms(uint32_t ms);
uint32_t Satellite_GetTimeMs(void);

/* 4. High-Level Sessions (Driven by configuration passed from main) */
void Satellite_Hold_Continuous_Carrier(uint32_t seconds);
void Satellite_Hold_Continuous_Carrier_5V(uint32_t seconds);
bool Satellite_Run_CW_Session(uint32_t cycle);
void Satellite_Run_GMSK_Burst_Session(uint32_t cycle);
void Satellite_Run_Cycle(uint32_t cycle);
void Satellite_Run_Cycle_Ex(uint32_t cycle, uint32_t guard_delay_ms);
void Satellite_Run(void);

/* 5. Explicit Sessions (Delays & parameters sent directly as function arguments from main) */
bool Satellite_Send_CW_Beacon(uint32_t cycle,
                              const char *name,
                              const char *callsign,
                              const char *msg,
                              uint32_t unit_ms,
                              uint32_t carrier_tone_ms,
                              uint32_t carrier_post_delay_ms,
                              uint32_t inter_str_delay_ms);

bool Satellite_Run_CW_Session_Ex(uint32_t cycle,
                                 const char *callsign,
                                 const char *msg,
                                 uint32_t unit_ms,
                                 uint32_t duration_ms,
                                 uint32_t carrier_tone_ms,
                                 uint32_t carrier_post_delay_ms,
                                 uint32_t inter_str_delay_ms,
                                 uint32_t repeat_delay_ms);

void Satellite_Run_GMSK_Burst_Session_Ex(uint32_t cycle,
                                        const void *payload,
                                        uint32_t duration_ms,
                                        uint32_t interval_ms,
                                        uint32_t timeout_ms);

/* 6. Granular Packet Transmission (Delays & timeouts sent from main) */
bool Satellite_Send_Packet(const uint8_t *payload, uint16_t len);
bool Satellite_Send_Packet_Timeout(const uint8_t *payload, uint16_t len, uint32_t timeout_ms);
bool Satellite_Send_Raw_Frame(const uint8_t *body, uint16_t len);   /* complete AX.25 frame body, see satellite_app.c */
bool Satellite_Send_AX25_String(const char *text);
bool Satellite_Send_AX25_String_Timeout(const char *text, uint32_t timeout_ms);

/* 7. Granular CW Carrier & Morse Keying */
void Satellite_CW_Prepare(void);
void Satellite_CW_CarrierOn(void);
void Satellite_CW_CarrierOff(void);
void Satellite_CW_SendChar(char c, uint32_t unit_ms);
void Satellite_CW_SendString(const char *str, uint32_t unit_ms);
void Satellite_CW_Finish(void);
void Satellite_CW_Abort_To_Rx(void);

/* 8. Granular GMSK Radio Preparation & Packet Statistics */
void     Satellite_GMSK_Prepare(void);
uint32_t Satellite_GetTotalPacketsSent(void);
void     IPCC_Handshake(void);

/* 10. Mission HK Telemetry & Flash Download Functions (128-byte payload standard) */
#define HK_PKT_LEN           128U
#define FLASH_PKT_MAGIC_LO   0xDA
#define FLASH_PKT_MAGIC_HI   0xDB
#define FLASH_PKT_HDR_LEN    14U
#define FLASH_PKT_CRC_LEN    2U
#define FLASH_PKT_TOTAL_LEN  (FLASH_PKT_HDR_LEN + FLASH_DATA_LEN + FLASH_PKT_CRC_LEN)

/* CHANGED: camera fragment now follows the Packet Format sheet "Camera Fragment" (was the reference firmware's 0xCAFE
 * packet with a 14-byte header). Information field: type 0x08 (1), image ID (1), fragment index (2, little endian),
 * total fragments (2, little endian), image type (1: 0 RGB, 1 NIR, 2 NDWI), AI score (1), 128 image bytes, then
 * CRC-16/X.25 (2, low byte first) over everything before it. The sheet gives no length field: the last fragment is
 * zero padded and the receiver trims to the real image size (JPEG end marker FFD9, or the size known from the command). */
#define CAM_PKT_TYPE         0x08
#define CAM_IMG_TYPE_RGB     0U
#define CAM_IMG_TYPE_NIR     1U
#define CAM_IMG_TYPE_NDWI    2U
#define CAM_PKT_HDR_LEN      8U
#define CAM_DATA_LEN         128U
#define CAM_PKT_CRC_LEN      2U
#define CAM_PKT_TOTAL_LEN    (CAM_PKT_HDR_LEN + CAM_DATA_LEN + CAM_PKT_CRC_LEN)
#define CAM_IMAGE_TOTAL_SIZE 8832U
#define CAM_TOTAL_CHUNKS     69U

bool Satellite_Drain_Telemetry(int16_t b1[5], int16_t b2[5]); /* true = new live data from M4 */
void Satellite_Send_HK_Combined_Packet(const int16_t b1[5], const int16_t b2[5]);
void Satellite_Send_Flash_Chunk(uint16_t pkt_idx, uint16_t total_pkts, uint32_t flash_addr, uint8_t status, const uint8_t data[FLASH_DATA_LEN]);
void Satellite_Handle_Flash_Read(uint32_t start_addr, uint16_t num_pkts);
void Satellite_Send_Camera_Image(uint16_t expected_chunks, bool local_dummy);
bool Satellite_Send_Camera_Chunk(uint16_t idx, uint16_t total, const uint8_t *data, uint8_t len);
void Satellite_CW_Test_Begin(void);
void Satellite_CW_Test_Cycle(uint32_t unit_ms, uint32_t tone_ms);
void Satellite_CW_Test_End(void);
uint32_t Satellite_Gmsk_Hello_Stream(bool (*keep_going)(void));
bool Satellite_Rate_Test(uint16_t count, uint32_t gap_ms, uint32_t *elapsed_ms, uint32_t *sent_ok);
void Satellite_Send_Camera_Ack(uint16_t count);
void Satellite_Send_Ping_Response(void);
void Satellite_SetDCDCHold(bool hold);
bool Satellite_GetDCDCHold(void);

/* Binary ACK / NACK definitions according to flight protocol specification (Packet Format.xlsx) */
#define SAT_ACK_ACCEPTED_BYTE    0xACU  /* ACK Packet - Byte 0: 0xAC (ACK) */
#define SAT_ACK_COMPLETED_BYTE   0xACU  /* CHANGED: ACK completed = 0xAC 0x01 (0xEE is reserved for NACK/error, Sheet 2 legend) */
#define SAT_NACK_BYTE            0xEEU  /* NACK Packet - Byte 0: 0xEE (NACK/Error) */

/* Byte 1: Status / Error Codes from Packet Format.xlsx (Sheet 2) */
#define SAT_STATUS_ACCEPTED      0x00U  /* Command accepted */
#define SAT_STATUS_COMPLETED     0x01U  /* Command completed */
#define SAT_ERR_PACKET           0x02U  /* Packet error */
#define SAT_ERR_CRC              0x03U  /* CRC/checksum error */
#define SAT_ERR_INCOMPLETE       0x04U  /* Incomplete packet */
#define SAT_ERR_DATA_MISSING     0x05U  /* Requested data missing */
#define SAT_ERR_DATA_UNAVAIL     0x06U  /* Requested data unavailable */
#define SAT_ERR_SAT_BUSY         0x07U  /* Satellite busy */
#define SAT_ERR_MISSION_INACTIVE 0x08U  /* Mission inactive */
#define SAT_ERR_INVALID_CMD      0x09U  /* Invalid command */
#define SAT_ERR_MEM_FILE         0x0AU  /* Memory/file error */
#define SAT_ERR_TX_FAILED        0x0BU  /* Transmission failed */

/* Byte 2: Mission / Data Identifiers from Packet Format.xlsx (Sheet 2) */
#define SAT_MISSION_HK           0x01U  /* Housekeeping */
#define SAT_MISSION_BEACON1      0x02U  /* Beacon 1 */
#define SAT_MISSION_BEACON2      0x03U  /* Beacon 2 */
#define SAT_MISSION_ADCS         0x04U  /* ADCS */
#define SAT_MISSION_CAM_RGB      0x05U  /* Camera/RGB */
#define SAT_MISSION_NIR_CAM      0x06U  /* NIR camera */
#define SAT_MISSION_NDWI         0x07U  /* NDWI image */
#define SAT_MISSION_EPDM         0x08U  /* EPDM */
#define SAT_MISSION_DIGIPEATER   0x09U  /* Digipeater */
#define SAT_MISSION_LOG          0x0AU  /* Satellite log */
#define SAT_MISSION_UNKNOWN      0xFFU  /* General/unknown */

void Satellite_Send_Ack_Accepted(uint8_t mission_id);
void Satellite_Send_Ack_Completed(uint8_t mission_id);
void Satellite_Send_Nack(uint8_t err_code, uint8_t mission_id);

/* Diagnostic Display Functions (AX.25 field breakdown) */
void Print_Hex_Bytes(const uint8_t *buf, uint16_t len);
void Print_AX25_Fields(const char *label, const uint8_t *buf, uint16_t len);
bool Looks_Like_Stale_TX_Buffer(const uint8_t *buf, uint16_t len);

/* 11. Uplink Radio Receive Buffers and Flags */
extern volatile uint8_t tx_busy;
extern volatile uint8_t rx_frame_buffer[AX25_MAX_FRAME_SIZE];
extern volatile uint16_t rx_frame_size;
extern volatile uint8_t rx_done_flag;
extern volatile uint8_t rx_timeout_flag;
extern volatile uint8_t rx_error_flag;
extern volatile int16_t last_rx_rssi;
extern volatile int8_t  last_rx_snr;
extern uint8_t last_tx_scrambled[AX25_MAX_FRAME_SIZE];
extern uint16_t last_tx_len;

/* Radio interrupt callbacks */
void OnTxDone(void);
void OnTxTimeout(void);
void OnRxTimeout(void);
void OnRxError(void);
void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr);

#ifdef __cplusplus
}
#endif

#endif /* SATELLITE_APP_H */

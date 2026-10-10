#ifndef GS_APP_H
#define GS_APP_H

#include <stdint.h>
#include "s2s2_config.h"
#include <stdbool.h>
#include "config.h"
#include "radio_board_if.h"
#include "protocol/ax25/ax25.h"
#include "cw_rx.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * GROUND STATION CONFIGURATION PARAMETERS & TIMINGS
 * ============================================================================
 *   Hardware: STM32WL55JC2  (RFO_HP, +22 dBm uplink TX)
 *
 *   Uplink   TX: 437.375 MHz  →  satellite M0+ RX (RFO_LP + external PA)
 *   Downlink RX: 435.000 MHz  ←  satellite M0+ TX (CW beacon + GMSK 4800 bps)
 *
 *   Receive pipeline:
 *     1. CW RSSI polling (5 ms) → CwRx decoder → live Morse telemetry display
 *     2. OnRxDone ISR latches raw G3RUH bytes
 *     3. Protocol_ExtractFrame (G3RUH descramble + NRZI + HDLC destuff)
 *     4. AX.25 structural check + CRC-16/X.25 FCS
 *     5. Payload dispatch:
 *          ACK 0xAC / ACK 0xEE / NACK 0xFF
 *          HK 128-byte telemetry packet  (footers 0xAA55 / 0xBB66 / 0xAACC)
 *          Camera chunks (magic 0xCAFE)  → JPEG reassembly
 *          Flash chunks  (magic 0xFDF0)  → 128-byte data display
 *          ASCII strings (ping response etc.)
 * ============================================================================
 */

/* 1. Radio Frequency & Profile Defaults */
#define GS_CFG_DEFAULT_TX_FREQ_HZ         DEFAULT_TX_FREQUENCY_HZ /* 437.375 MHz Uplink (High) */
#define GS_CFG_DEFAULT_RX_FREQ_HZ         DEFAULT_RX_FREQUENCY_HZ /* 435.000 MHz Downlink (Low) */
#define GS_CFG_DEFAULT_SRC_CALLSIGN       S2S2_GS_CALLSIGN
#define GS_CFG_DEFAULT_SRC_SSID           S2S2_GS_SSID
#define GS_CFG_DEFAULT_DEST_CALLSIGN      S2S2_SAT_CALLSIGN
#define GS_CFG_DEFAULT_DEST_SSID          S2S2_SAT_SSID
#define GS_CFG_DEFAULT_TX_POWER_DBM       RADIO_TX_POWER_DBM      /* +22 dBm (High Power RFO_HP) */
#define GS_CFG_DEFAULT_BITRATE_BPS        RADIO_BIT_RATE_BPS      /* 4800 bps GMSK */
#define GS_CFG_DEFAULT_FDEV_HZ            RADIO_FDEV_HZ           /* +/- 1200 Hz */
#define GS_CFG_DEFAULT_TX_TIMEOUT_MS      RADIO_TX_TIMEOUT_MS     /* 3000 ms */

/* RF Switch: High Power RFO_HP for Ground Station STM32WL55 Nucleo board */
#define GS_CFG_DEFAULT_RF_SWITCH          RBI_SWITCH_RFO_HP

/* CPU2 (Cortex-M0+) Vector Table Base Address in Flash */
#define GS_CPU2_VECTOR_TABLE_ADDR         0x08032000UL

/* 2. Startup / Console Settle Delay */
#define GS_CFG_DEFAULT_UART_SETTLE_MS     50                      /* Settle delay in ms */

/* 3. CW / Morse Code Configuration & Delays */
#define GS_CFG_DEFAULT_CW_CALLSIGN        S2S2_CW_CALLSIGN
#define GS_CFG_DEFAULT_CW_MORSE_UNIT_MS   S2S2_CW_UNIT_MS
#define GS_CW_MUTE_AFTER_PACKET_MS        1500UL
#define GS_CW_MUTE_AFTER_TX_MS            300UL

/* 4. Automated Commands & Heartbeats */
#define GS_AUTO_CMD_EVERY_N_PACKETS       2U
#define GS_AUTO_CMD_INTERVAL_MS           (120UL * 1000UL)        /* 2 minutes */
#define CMD_LINE_MAX                      128

/*
 * Protocol_CreatePacket() always returns RADIO_FIXED_PACKET_LEN bytes, and the
 * TX/RX buffers in the ground station sources are sized AX25_MAX_FRAME_SIZE.
 */
_Static_assert(AX25_MAX_FRAME_SIZE >= RADIO_FIXED_PACKET_LEN,
               "AX25_MAX_FRAME_SIZE must be >= RADIO_FIXED_PACKET_LEN (200)");

/*
 * ============================================================================
 * GROUND STATION CONFIGURATION STRUCTURE
 * ============================================================================
 */
typedef struct {
    /* 1. Radio Frequency & Profile Parameters */
    RadioConfig_t      radio;
    int8_t             txPowerDbm;
    uint32_t           bitrateBps;
    uint32_t           fdevHz;
    uint32_t           txTimeoutMs;
    RBI_Switch_TypeDef rfSwitchConfig;

    /* 2. System Startup Delays */
    uint32_t           uartSettleDelayMs;

    /* 3. CW / Morse Beacon Parameters */
    const char        *cwBeaconCallsign;
    uint32_t           morseUnitMs;

    /* 4. Automation Parameters */
    bool               autoCmdEnabled;
    uint32_t           autoCmdIntervalMs;
} GroundStationConfig_t;

/* Default Configuration Initializer Macro */
#define GROUND_STATION_CONFIG_DEFAULT { \
    .radio = { \
        .txFrequency    = GS_CFG_DEFAULT_TX_FREQ_HZ, \
        .rxFrequency    = GS_CFG_DEFAULT_RX_FREQ_HZ, \
        .sourceCallsign = GS_CFG_DEFAULT_SRC_CALLSIGN, \
        .sourceSSID     = GS_CFG_DEFAULT_SRC_SSID, \
        .destCallsign   = GS_CFG_DEFAULT_DEST_CALLSIGN, \
        .destSSID       = GS_CFG_DEFAULT_DEST_SSID, \
        .isSatelliteMode = false \
    }, \
    .txPowerDbm         = GS_CFG_DEFAULT_TX_POWER_DBM, \
    .bitrateBps         = GS_CFG_DEFAULT_BITRATE_BPS, \
    .fdevHz             = GS_CFG_DEFAULT_FDEV_HZ, \
    .txTimeoutMs        = GS_CFG_DEFAULT_TX_TIMEOUT_MS, \
    .rfSwitchConfig     = GS_CFG_DEFAULT_RF_SWITCH, \
    .uartSettleDelayMs  = GS_CFG_DEFAULT_UART_SETTLE_MS, \
    .cwBeaconCallsign   = GS_CFG_DEFAULT_CW_CALLSIGN, \
    .morseUnitMs        = GS_CFG_DEFAULT_CW_MORSE_UNIT_MS, \
    .autoCmdEnabled     = false, \
    .autoCmdIntervalMs  = GS_AUTO_CMD_INTERVAL_MS, \
}

extern const GroundStationConfig_t GroundStationDefaultConfig;

/*
 * ============================================================================
 * GROUND STATION API FUNCTIONS - ACCESSIBLE FROM MAIN
 * ============================================================================
 */

/* 0. Low-Level CPU2 Hardware & Peripheral Bus Initialization */
void                         GS_Hardware_Init(void);
void                         GS_Init_VectorTable(void);
void                         GS_Init_PeripheralClocks(void);
void                         GS_Init_IPCC_Isolation(void);

/* 1. Core Lifecycle & Configuration */
void                         GS_Init(const GroundStationConfig_t *config);
void                         GS_SetConfig(const GroundStationConfig_t *config);
const GroundStationConfig_t* GS_GetConfig(void);

/* 2. RF Switch Selection */
void               GS_SetRFSwitch(RBI_Switch_TypeDef rf_switch);
RBI_Switch_TypeDef GS_GetRFSwitch(void);

/* 3. Timing & Delay Controls */
void     GS_DelayMs(uint32_t ms);
void     CPU2_Delay_Ms(uint32_t ms);
uint32_t GS_GetTimeMs(void);

/* 3b. IRQ-safe radio access (use instead of calling RadioApp_StartRx directly) */
void     GS_StartRx(void);

/* 4. Telecommand Transmission */
bool GS_Send_Raw_Frame(const uint8_t *body, uint16_t len);   /* complete AX.25 frame body, see gs_app.c */
bool GS_Send_Packet(const uint8_t *payload, uint16_t len);
bool GS_Send_Telecommand(uint8_t mcu_id, const uint8_t opcode[3],
                         uint16_t data_id, uint32_t addr, uint16_t count);
bool GS_Send_Telecommand_ByName(const char *name, uint16_t count);

/* 5. Continuous CW & GMSK Reception */
void GS_CW_Poll(void);
void GS_Process_Received_Frame(void);
void GS_Listen_CW_And_Packets(uint32_t max_duration_ms, uint16_t expected_packets);

/* 6. Automation & Command Handler */
void GS_Auto_Send_Command(void);
void GS_Handle_Command_Line(const char *cmd_line);
bool GS_Console_TryReadLine(char *out, uint8_t max);
void GS_Print_Help(void);
void GS_Print_Status(void);

/* 7. Radio Receive Buffers and Flags */
extern volatile uint8_t  rx_frame_buffer[AX25_MAX_FRAME_SIZE];
extern volatile uint16_t rx_frame_size;
extern volatile uint8_t  rx_done_flag;
extern volatile uint8_t  rx_timeout_flag;
extern volatile uint8_t  rx_error_flag;
extern volatile uint8_t  tx_busy;

#ifdef __cplusplus
}
#endif

#endif /* GS_APP_H */
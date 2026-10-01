#ifndef SATELLITE_APP_H
#define SATELLITE_APP_H

#include <stdint.h>
#include <stdbool.h>
#include "Mission/config.h"
#include "radio_board_if.h"
#include "ax25.h"

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
#define SAT_CFG_DEFAULT_SRC_CALLSIGN       "9NS2S2"
#define SAT_CFG_DEFAULT_SRC_SSID           1
#define SAT_CFG_DEFAULT_DEST_CALLSIGN      "GROUND"
#define SAT_CFG_DEFAULT_DEST_SSID          0
#define SAT_CFG_DEFAULT_TX_POWER_DBM       14                      /* Transmit RF power in dBm (14 dBm into External PA = full +24.5 dBm on 3.3V, +27.0 dBm on 5V) */
#define SAT_CFG_DEFAULT_BITRATE_BPS        4800                    /* GMSK Bitrate (4800 bps) */
#define SAT_CFG_DEFAULT_FDEV_HZ            1200                    /* GMSK Frequency Deviation (+/- 1.2 kHz) */
#define SAT_CFG_DEFAULT_TX_TIMEOUT_MS      3000                    /* Radio transmission timeout in ms */

/* RF Front-End Switch Selection:
 * - 3.3V External PA for CW (PC2/SO2, hardware-always-on 3.3V rail): RBI_SWITCH_RFO_LP
 * - GMSK Mode PA:
 *     If SAT_CFG_USE_5V_PA_FOR_GMSK==1: RBI_SWITCH_RFO_LP5V (5V External PA PC3 + 5V DC/DC PA0)
 *     If SAT_CFG_USE_5V_PA_FOR_GMSK==0: RBI_SWITCH_RFO_LP   (3.3V External PA PC2, Safe Bench Mode)
 */
#define SAT_CFG_DEFAULT_RF_SWITCH          RBI_SWITCH_RFO_LP   /* 3.3V PA for CW (PC2 / SO2) */
#if (SAT_CFG_USE_5V_PA_FOR_GMSK == 1)
#define SAT_CFG_DEFAULT_RF_SWITCH_5V       RBI_SWITCH_RFO_LP5V /* 5V PA for GMSK (PA0 + PC3 / SI2) */
#else
#define SAT_CFG_DEFAULT_RF_SWITCH_5V       RBI_SWITCH_RFO_LP   /* Safe Bench Mode: 3.3V PA for GMSK */
#endif

/* CPU2 (Cortex-M0+) Vector Table Base Address in Flash */
#define SATELLITE_CPU2_VECTOR_TABLE_ADDR   0x08032000UL

/* 2. Startup / Console Settle Delay */
#define SAT_CFG_DEFAULT_UART_SETTLE_MS     50                      /* Settle delay in ms after UART init */

/* 3. CW / Morse Code Configuration & Delays */
#define SAT_CFG_DEFAULT_CW_CALLSIGN        "9NS2S2NEPAL"
#define SAT_CFG_DEFAULT_CW_MESSAGE         "Namaste everyone"
#define SAT_CFG_DEFAULT_CW_MORSE_UNIT_MS   80                      /* 15 WPM (dit = 80ms, dah = 240ms) */
#define SAT_CFG_DEFAULT_CW_DURATION_MS     90000                   /* 90-Second continuous CW beacon session */
#define SAT_CFG_DEFAULT_CW_TUNING_MS       2000                    /* Pre-beacon 2-second tuning carrier tone */
#define SAT_CFG_DEFAULT_CW_POST_DELAY_MS   1000                    /* Delay after tuning carrier tone before Morse */
#define SAT_CFG_DEFAULT_CW_INTER_STR_MS    1000                    /* Delay gap between callsign and payload message */
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

/* 9. Uplink Radio Receive Buffers and Flags */
extern volatile uint8_t rx_frame_buffer[AX25_MAX_FRAME_SIZE];
extern volatile uint16_t rx_frame_size;
extern volatile uint8_t rx_done_flag;
extern volatile uint8_t rx_timeout_flag;
extern volatile uint8_t rx_error_flag;

#ifdef __cplusplus
}
#endif

#endif /* SATELLITE_APP_H */

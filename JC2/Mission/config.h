#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================
 * FREQUENCIES
 * ============================================================
 *
 * Satellite:
 *
 *     TX = 435.000 MHz
 *     RX = 437.375 MHz
 *
 * Ground station must use:
 *
 *     TX = 437.375 MHz
 *     RX = 435.000 MHz
 * ============================================================
 */

#define DEFAULT_TX_FREQUENCY_HZ        437375000UL
#define DEFAULT_RX_FREQUENCY_HZ        435000000UL

/*
 * ============================================================
 * DEFAULT AX.25 ADDRESS
 * ============================================================
 */

#define DEFAULT_SOURCE_CALLSIGN       "9NS2S2"
#define DEFAULT_SOURCE_SSID            1

#define DEFAULT_DEST_CALLSIGN         "GROUND"
#define DEFAULT_DEST_SSID              0

/*
 * ============================================================
 * GFSK / G3RUH
 * ============================================================
 */
// Full +14 dBm drive power: yields full +24.5 dBm on 3.3V PA and +27.0 dBm on 5V PA
#define RADIO_TX_POWER_DBM             14

/*
 * GMSK External Power Amplifier Selection:
 *   1: 5V External PA (PC3 / SI2) with 5V DC/DC Boost (PA0) [High Power Flight Mode: ~26.5 dBm]
 *   0: 3.3V External PA (PC2 / SO2) [Safe Bench / ST-Link USB Mode: ~24.5 dBm, <170 mA]
 */
#ifndef SAT_CFG_USE_5V_PA_FOR_GMSK
#define SAT_CFG_USE_5V_PA_FOR_GMSK     0
#endif

/*
 * GMSK / G3RUH data rate: 4800 bps (matched to Ground Station).
 */
#define RADIO_BIT_RATE_BPS             4800

/*
 * GMSK frequency deviation (+/- 1.2 kHz for 4800 bps, BT 0.5, h=0.5).
 */
#define RADIO_FDEV_HZ                  1200

/*
 * SX126x GFSK bandwidth.
 */
#define RADIO_RX_BANDWIDTH_HZ         23400

/*
 * 128 bits = 16 bytes.
 */
#define RADIO_PREAMBLE_LENGTH_BITS     128

#define RADIO_TX_TIMEOUT_MS            3000

/*
 * ============================================================
 * PHYSICAL PACKET LENGTH
 *
 * The SX126x packet is deliberately fixed length.
 *
 * This is NOT an AX.25 requirement.
 * It is our radio transport configuration.
 * ============================================================
 */

#define RADIO_FIXED_PACKET_LEN         200

/*
 * Number of HDLC flags before actual frame.
 * 24 flags = 192 bits = 40 ms audio lock time at 4800 baud (Direwolf optimal).
 */
#define PROTOCOL_LEADING_FLAG_COUNT    24

/*
 * ============================================================
 * RADIO CONFIG
 * ============================================================
 */

typedef struct
{
    uint32_t txFrequency;
    uint32_t rxFrequency;

    const char *sourceCallsign;
    uint8_t sourceSSID;

    const char *destCallsign;
    uint8_t destSSID;

    bool isSatelliteMode;

} RadioConfig_t;

/*
 * ============================================================
 * COMMANDS
 * ============================================================
 */

typedef enum
{
    CMD_NONE               = 0x00,
    CMD_REQUEST_BURST      = 0x01,
    CMD_ACK                = 0x02,
    CMD_NACK               = 0x03,
    CMD_HEARTBEAT          = 0x04,
    CMD_ABORT_MISSION      = 0x05,
    CMD_REQUEST_RETRANSMIT = 0x06,

    CMD_RUN_CAMERA         = 0x10,
    CMD_DOWNLOAD_FILE      = 0x11,
    CMD_GET_HEALTH         = 0x12,
    CMD_GET_HOUSEKEEPING   = 0x13,
    CMD_GET_BEACON1        = 0x14,
    CMD_GET_BEACON2        = 0x15,
    CMD_GET_ADCS           = 0x16,
    CMD_DIGIPEATER         = 0x17,
    CMD_GET_SAT_LOG        = 0x18

} CommandOpcode_t;

/* Subsystem / data identifiers for Send_Command_Ex's 3-byte payload
   (gs_main.c) -- ground-console-side only, since the satellite's own
   command dispatch (CPU1/NuttX) is outside this build. */
typedef enum
{
    SUBSYS_OBC  = 0x01,
    SUBSYS_COM  = 0x02,
    SUBSYS_ADCS = 0x03,
    SUBSYS_CAM  = 0x04,
    SUBSYS_EPDM = 0x05
} CommandSubsystem_t;

typedef enum
{
    DATA_ID_HOUSEKEEPING = 0x01,
    DATA_ID_BEACON1      = 0x02,
    DATA_ID_BEACON2      = 0x03,
    DATA_ID_ADCS         = 0x04,
    DATA_ID_CAMERA_RGB   = 0x05,
    DATA_ID_SAT_LOG      = 0x0A,
    DATA_ID_DIGIPEATER   = 0x09
} CommandDataId_t;

#ifdef __cplusplus
}
#endif

#endif
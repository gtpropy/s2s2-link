#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include "s2s2_config.h"
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

#define DEFAULT_TX_FREQUENCY_HZ        S2S2_UPLINK_HZ      /* generated/s2s2_config.h */
#define DEFAULT_RX_FREQUENCY_HZ        S2S2_DOWNLINK_HZ

/*
 * ============================================================
 * DEFAULT AX.25 ADDRESS
 * ============================================================
 */

#define DEFAULT_SOURCE_CALLSIGN       S2S2_SAT_CALLSIGN
#define DEFAULT_SOURCE_SSID            1

#define DEFAULT_DEST_CALLSIGN         S2S2_GS_CALLSIGN
#define DEFAULT_DEST_SSID              0

/*
 * ============================================================
 * GFSK / G3RUH
 * ============================================================
 * STM32WL55JC2 High-Power Ground Station Transceiver: up to +22 dBm on RFO_HP.
 * STM32WL55JC1 Low-Power Satellite Transceiver: up to +14 dBm on RFO_LP.
 */
#define RADIO_TX_POWER_DBM             15      /* bench setting (was 22, the RFO_HP maximum); change at run time with the PWR <dbm> console command */

/*
 * GMSK / G3RUH data rate: 4800 bps (matched to Satellite M0+).
 */
#define RADIO_BIT_RATE_BPS             4800

/*
 * GMSK frequency deviation (+/-1.2 kHz for 4800 bps, BT=0.5, modulation index h=0.5).
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

/*
 * Downlink (satellite TX -> GS RX) hardware sync word.
 * The satellite sends it between its 128-bit preamble and the G3RUH bitstream;
 * the GS RX waits for preamble + this word, so each 200-byte packet lands
 * bit-aligned in the FIFO instead of in a random noise window.
 * MUST be identical to Satellite_M0+/Com_sat/Mission/config.h.
 * Uplink (GS TX -> satellite RX) does not use it.
 */
#define RADIO_DOWNLINK_SYNC_WORD_BITS  32
#define RADIO_DOWNLINK_SYNC_WORD       { 0x93, 0x0B, 0x51, 0xDE, 0x00, 0x00, 0x00, 0x00 }

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
 * Number of preamble HDLC flags before opening frame flag.
 * 16 preamble flags (128 bits) gives PLL & G3RUH descrambler ample time to lock
 * while leaving sufficient bit budget for 144-byte payloads inside the fixed 200B buffer.
 */
#define PROTOCOL_LEADING_FLAG_COUNT    16

/*
 * Mission COMMAND CONFIGURATION 13 BYTES
 */
#define CMD_CAMERA_COMMAND_LEN 13

#define CMD_CAMERA_COMMAND       {0x53, 0x04, 0xCC, 0x5E, 0xBD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#define CMD_CAM_ON_COMMAND       {0x53, 0x04, 0xCC, 0x5E, 0xBD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#define CMD_CAM_DOWNLOAD_COMMAND {0x53, 0x01, 0x1D, 0xD2, 0xF5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A}

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
    CMD_NONE          = 0x00,

    CMD_REQUEST_BURST = 0x01,

    CMD_ACK           = 0x02,

    CMD_NACK          = 0x03,

    CMD_HEARTBEAT     = 0x04

} CommandOpcode_t;

#ifdef __cplusplus
}
#endif

#endif
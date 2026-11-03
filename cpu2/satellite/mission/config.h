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

#define DEFAULT_TX_FREQUENCY_HZ        S2S2_DOWNLINK_HZ    /* generated/s2s2_config.h */
#define DEFAULT_RX_FREQUENCY_HZ        S2S2_UPLINK_HZ

/*
 * ============================================================
 * DEFAULT AX.25 ADDRESS
 * ============================================================
 */

/* CHANGED: 9N2SJ (Packet Format sheets: CW sheet call sign 9N2SJ), was 9NS2S2 */
#define DEFAULT_SOURCE_CALLSIGN       S2S2_SAT_CALLSIGN
#define DEFAULT_SOURCE_SSID            1

#define DEFAULT_DEST_CALLSIGN         S2S2_GS_CALLSIGN
#define DEFAULT_DEST_SSID              0

/*
 * ============================================================
 * GFSK / G3RUH
 * ============================================================
 */
/*
 * RF Transmit Drive Power (dBm) on RFO_LP (PB2):
 *   - Set to 14 dBm for Full Flight / Power Amplifier Output:
 *       * 3.3V External PA (PC2): ~26 dBm output
 *       * 5V External PA (PC3 / PA0 Boost): ~27 to 28.5 dBm output
 *   - (Note: 9 dBm was previously used for current-limited ST-Link USB bench testing)
 */
#ifndef RADIO_TX_POWER_DBM              /* the mock satellite (tests/mocksat) sets its own */
#define RADIO_TX_POWER_DBM             14
#endif
/* Highest drive the satellite accepts, and the drive kept during a GMSK session (14 dBm brown-outs the 5 V PA of the flight board).
 * The mock satellite runs on a stock Nucleo (RFO_HP, no external PA), where RADIO_TX_POWER_DBM is used for both. */
#ifdef S2S2_MOCKSAT
#define SAT_TX_DBM_MAX                 22
#define SAT_GMSK_DBM                   RADIO_TX_POWER_DBM
#else
#define SAT_TX_DBM_MAX                 14
#define SAT_GMSK_DBM                   10
#endif

/*
 * SX126x RFO_LP paDutyCycle used when driving the 5V External PA (GMSK).
 * Must match the 3.3V PA and the 5V CW carrier test: 0x04 = full +14 dBm drive.
 * Previous 0x03 under-drove the 5V PA (~13 dBm), so GMSK looked weaker than CW.
 * Keep the 5V PA biased for the whole GMSK burst (no PC3 off / PC2 on between
 * packets) so 0x04 does not cause inrush brownouts.
 */
#define RADIO_GMSK_5V_PA_DUTY          0x04

/*
 * External Power Amplifier mapping (fixed by hardware, both fed from RFO_LP / PB2):
 *   CW Morse beacon      -> 3.3V External PA (PC2 / SO2), 5V DC/DC (PA0) OFF
 *   GMSK AX.25 + G3RUH   -> 5V External PA (PC3 / SI2) with 5V DC/DC Boost (PA0) ON
 * Only one PA is enabled at a time; both are OFF in RX.
 */

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

/*
 * Downlink (satellite TX -> GS RX) hardware sync word.
 * Sent between the SX126x preamble and the G3RUH bitstream so the GS radio
 * captures every 200-byte packet bit-aligned from its first byte.
 * SDR G3RUH decoders ignore it: the leading HDLC flags re-sync the descrambler.
 * MUST be identical to Ground_Station/Mission/config.h.
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
 * while leaving sufficient bit budget for 144-byte camera payloads inside the fixed 200B buffer.
 */
#define PROTOCOL_LEADING_FLAG_COUNT    16

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

/*
 * Front end of the board the satellite firmware runs on.
 *   FRONTEND=NUCLEO (make default): stock Nucleo-WL55JC wiring, same as the ground station
 *       (PC3 = switch enable, PC4/PC5 select RX / HP TX, RFO_HP PA). Use this for bench tests
 *       on Nucleo boards.
 *   FRONTEND=CUSTOM: reference satellite board (RFO_LP + external 3.3 V / 5 V PAs, PC3 = 5 V PA enable).
 */
#ifdef SAT_NUCLEO_FRONTEND
#define SAT_PA_SEL              RFO_HP
#define SAT_SET_PA_CONFIG()     SUBGRF_SetPaConfig(0x04, 0x07, 0x00, 0x01)
#define SAT_CW_OCP              0x38
#else
#define SAT_PA_SEL              RFO_LP
#define SAT_SET_PA_CONFIG()     SUBGRF_SetPaConfig(0x04, 0x00, 0x01, 0x01)
#define SAT_CW_OCP              0x18
#endif

/*
 * Turn-around guard: time the satellite waits after accepting a command before it starts the
 * downlink. The ground station sends each command twice (~0.9 s on air) and then has to switch
 * its radio from TX to RX; answering sooner means the first ACKs go out while it is still deaf.
 * (With the console log on, printing the received frame used to add this delay by accident.)
 */
#define SAT_ACK_GUARD_MS        700U

/*
 * Gap after every downlink packet. The ground station prints a full decode report per frame
 * (~0.25 s at 115200 baud) and its single RX buffer is re-armed afterwards, so back-to-back
 * packets are lost. Lower it only together with a quieter GS.
 */
#define SAT_TX_GAP_MS           300U

/* PA bring-up / tear-down waits around a GMSK packet. The custom board's 5 V boost converter and PA
 * need them; a Nucleo has neither, so only the RF switch settle time remains. */
#ifdef SAT_NUCLEO_FRONTEND
#define SAT_PA_PREP_MS          1U
#define SAT_DCDC_SETTLE_MS      0U
#define SAT_SWITCH_SETTLE_MS    5U
#define SAT_PA_OFF_MS           0U
#else
#define SAT_PA_PREP_MS          5U
#define SAT_DCDC_SETTLE_MS      100U
#define SAT_SWITCH_SETTLE_MS    40U
#define SAT_PA_OFF_MS           10U
#endif

/*
 * config.h
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-21
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#include "s2s2com.h"
#include "s2s2_commands.h"
#include "cubus_ax25.h"

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

#define DEFAULT_TX_FREQUENCY_HZ        435000000UL
#define DEFAULT_RX_FREQUENCY_HZ        437375000UL

/*
 * ============================================================
 * S2S2 PROTOCOL NODE IDS (cubus_proto src_id / dst_id)
 * ============================================================
 */

#define NODE_ID_GROUND                  0
#define NODE_ID_SATELLITE                1

#define DEFAULT_SESSION_ID             0x0001U

/*
 * ============================================================
 * GMSK RADIO PARAMETERS
 *
 * The SX126x has no dedicated "GMSK mode" - GMSK is realized on the
 * same FSK modem as a special case: modulation index h = 0.5 (Fdev =
 * bitrate/4) with a tighter Gaussian pulse-shaping filter (BT = 0.3)
 * than plain GFSK (which typically uses h = 1.0, BT = 0.5). See
 * s2s2com.h section 4 for the full radio configuration this mirrors.
 * ============================================================
 */

#define RADIO_TX_POWER_DBM             22

/*
 * DEBUG_BUILD-only bench-test TX power (dBm). At +22 dBm (max HP-PA
 * output) and under ~30cm separation, almost any receiver front end
 * saturates regardless of FEC strength -- this is an RF-level effect,
 * not a protocol bug. The DEBUG_BUILD "POWER <dbm>" UART command
 * (see ipcc_m0plus_main.c / gs_main.c) lets a bench session drop to
 * this level; production builds have no runtime way to change TX
 * power and always run at RADIO_TX_POWER_DBM.
 */
#ifdef DEBUG_BUILD
#define BENCH_TX_POWER_DBM             2
#endif

/*
 * Data rate. 4800 bps -- the S2S2_PHASE_MID design point in s2s2com.h's
 * per-elevation table, now the fixed operating rate (not just a
 * mid-pass fallback): better link margin than 9600 at only half the
 * throughput, without the 1200 bps HORIZON phase's convolutional-code
 * overhead.
 */
#define RADIO_BIT_RATE_BPS             4800

/*
 * GMSK frequency deviation: Fdev = bitrate/4 for h = 0.5.
 */
#define RADIO_FDEV_HZ                  1200

/*
 * SX126x RX bandwidth. Carson's rule: BW > bitrate + 2*Fdev + freq_error,
 * i.e. > 4800 + 2400 = 7200 Hz minimum; 9700 Hz is the nearest real
 * SX126x discrete bandwidth step (sx126x.c's GFSK bandwidth table) above
 * that minimum -- same margin ratio the previous 9600 bps/19500 Hz pair
 * used (its 19500 Hz was itself an exact table step, not a round number).
 */
#define RADIO_RX_BANDWIDTH_HZ          9700

/*
 * 128 bits = 16 bytes.
 */
#define RADIO_PREAMBLE_LENGTH_BITS     128

#define RADIO_TX_TIMEOUT_MS            3000

/*
 * ============================================================
 * AX.25/FX.25 LINK ADDRESSING
 *
 * Real AX.25 UI-frame addresses (callsign + SSID), used by
 * cubus_ax25_fragment()/cubus_ax25_accept_frame() (core/Inc/cubus_ax25.h)
 * so any AX.25/FX.25-aware ground station (SatNOGS, gr-satellites,
 * Direwolf) can receive S2S2 traffic without custom software.
 *
 * NEPSAT/GROUND are placeholder callsigns carried over from the original
 * AX.25 bench-test design doc (packet_format_AX.25.json) - swap in a
 * real assigned amateur-radio callsign before any actual RF transmit;
 * transmitting under an invalid callsign is a licensing violation, not
 * just a config nit.
 *
 * FX.25 correlation tag 0x06 (fx25.h) is a *shortened* RS(255,223): 128
 * real data bytes + 32 parity = 160-byte RS block + 8-byte tag = 168-byte
 * frame, same t=16 correction strength as the unshortened code, but small
 * enough to fit the SX126x's 255-byte native packet-engine buffer. Sent
 * with the packet engine in fixed-length/CRC-off/whitening-off mode
 * (radio_app.c) so the actual on-air bytes are real, standards-compliant
 * AX.25/FX.25 -- no continuous raw-bitstream driver needed.
 * ============================================================
 */

#define AX25_SATELLITE_CALLSIGN "NEPSAT"
#define AX25_SATELLITE_SSID     1
#define AX25_GROUND_CALLSIGN    "GROUND"
#define AX25_GROUND_SSID        0

/*
 * ============================================================
 * RADIO CONFIG
 * ============================================================
 */

typedef struct
{
    uint32_t txFrequency;
    uint32_t rxFrequency;

    uint8_t nodeId;      /* this node's own id: NODE_ID_SATELLITE or NODE_ID_GROUND */
    uint8_t peerId;      /* the node on the other end of the link */

} RadioConfig_t;

/*
 * ============================================================
 * COMMANDS
 *
 * The standard command set (opcodes, subsystem IDs, mission/data IDs,
 * ACK/NACK status codes) and the structured telemetry payload formats
 * (Beacon 1/2, camera fragment) live in s2s2_commands.h.
 * ============================================================
 */

#ifdef __cplusplus
}
#endif

#endif

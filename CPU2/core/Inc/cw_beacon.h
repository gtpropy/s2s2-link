/*
 * cw_beacon.h -- real Morse CW beacon, keyed via RadioApp_KeyCw's raw
 * carrier (not a digital simulation of CW): "DE <CALLSIGN> <5 digits>"
 * where the digits are single-digit (0-9) coarse telemetry codes for
 * V/Gx/Gy/Gz/T, matching real amateur-CubeSat CW beacon practice
 * (JY1SAT/AO-40 heritage). Any listener with a plain SSB/CW receiver can
 * copy this by ear -- no demodulator, no ground station software, same
 * interoperability goal as the AX.25/FX.25 path.
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-31
 */
#ifndef CW_BEACON_H
#define CW_BEACON_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PARIS-standard word length (50 dit-units), matching how WPM is
   conventionally measured. unit_ms = 1200 / wpm. */
#define CW_DEFAULT_WPM 15

/* =====================================================================
 * CW Beacon 1/2 -- real bit-packed housekeeping telemetry, per Packet
 * Format Sheet4 (referenced there against BIRDS4_CW_Format_Timing.pdf).
 * Distinct from the AX.25/FX.25 beacon1_t/beacon2_t in s2s2_commands.h --
 * this is a SEPARATE, much coarser telemetry set meant to be copied by
 * ear with no demodulator, following BIRDS4's own convention: pack the
 * fields into N raw bytes, hex-encode those bytes as uppercase ASCII
 * digit characters, then Morse-send "<project> <callsign> <hex...>" --
 * see cw_beacon_pack_hex() below and BIRDS4_CW_Format_Timing.pdf pages
 * 2-3 (project name + call sign + housekeeping-as-hex-digits framing).
 *
 * The sheet's own "SOLAR CELL" cell list (Sheet4 rows 23-27) repeats
 * "+Y" twice -- BIRDS4's own reference table (the cited PDF) lists +X/
 * -Y/-Z/+Y/+Z, so that's used here as the corrected 5-cell set.
 * ===================================================================== */

#define CW_BEACON_PACKED_LEN 8   /* both CW1 and CW2 pack to 8 raw bytes */
#define CW_BEACON_HEX_LEN    16  /* 8 bytes -> 16 hex digit characters */

typedef struct {
    uint8_t  system_stat;      /* 2-bit, low 2 bits used */
    uint8_t  mission_status;   /* 2-bit, low 2 bits used */
    uint8_t  time_after_reset; /* 4-bit, low 4 bits used (hours mod 16) */
    uint8_t  sol_total_i;
    uint16_t sol_total_v;
    uint16_t bat_v;
    uint16_t bat_i;
} cw_beacon1_t;

typedef struct {
    uint8_t gyro_x;
    uint8_t gyro_y;
    uint8_t gyro_z;
    uint8_t mag_x;
    uint8_t mag_y;
    uint8_t mag_z;
    uint8_t sol_plus_x  : 1;
    uint8_t sol_minus_y : 1;
    uint8_t sol_minus_z : 1;
    uint8_t sol_plus_y  : 1;
    uint8_t sol_plus_z  : 1;
    uint8_t bat_temp;
} cw_beacon2_t;

void cw_beacon1_pack(const cw_beacon1_t *b, uint8_t out[CW_BEACON_PACKED_LEN]);
void cw_beacon2_pack(const cw_beacon2_t *b, uint8_t out[CW_BEACON_PACKED_LEN]);

/* Renders `len` raw bytes as `2*len` uppercase hex digit characters
   (Morse has codes for 0-9 and A-F, same table as callsigns) into `out`,
   which must have room for 2*len+1 bytes (out is NUL-terminated). */
void cw_beacon_hex_encode(const uint8_t *bytes, int len, char *out);

/* Encodes and blocking-transmits `message` as International Morse code.
   Supports A-Z (case-insensitive), 0-9, and space (7-unit inter-word
   gap); any other character is silently skipped (no gap inserted for
   it). wpm<=0 uses CW_DEFAULT_WPM.
 *
 * Blocking: returns only once the whole message plus its trailing
 * inter-character gap has been sent -- there is no background/async
 * variant, matching how every other TX in this firmware (Send_Message,
 * Transmit_Burst) already blocks the single-threaded main loop.
 *
 * Radio is left in Standby afterward, NOT Rx -- caller must call
 * RadioApp_StartRx() to resume normal GMSK listening, same requirement
 * as RadioApp_Send. */
void cw_beacon_send(const char *message, int wpm);

#ifdef __cplusplus
}
#endif

#endif /* CW_BEACON_H */

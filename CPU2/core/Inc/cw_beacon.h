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

#ifdef __cplusplus
extern "C" {
#endif

/* PARIS-standard word length (50 dit-units), matching how WPM is
   conventionally measured. unit_ms = 1200 / wpm. */
#define CW_DEFAULT_WPM 15

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

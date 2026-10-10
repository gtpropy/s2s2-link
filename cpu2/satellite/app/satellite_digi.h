#ifndef SATELLITE_DIGI_H
#define SATELLITE_DIGI_H

/*
 * Satellite digipeater glue: connects the radio receive path to the digipeater engine (cpu2/common/digipeater), the
 * store-and-forward memory and the 4800 bit/s GMSK transmitter. Built only with DIGI=TRUE (make configure / TUI).
 *
 * Receive:  a decoded AX.25 frame (flag | body | FCS | flag) with at least one digipeater address and a good FCS is a
 *           digipeater candidate and is NOT treated as a command; frames without a path go on to the command logic.
 * Repeat:   a frame the engine accepts is sent again on the downlink frequency after a short guard time with the rewritten
 *           path and a fresh FCS (if S2S2_SAF_IMMEDIATE) and is KEPT in the store-and-forward memory.
 * Forward:  Sat_Digi_Service() sends stored messages again, one at a time, `S2S2_SAF_INTERVAL_MS` apart per message, at most
 *           `S2S2_SAF_MAX_FORWARDS` times, until they expire; messages that still have hop budget go first.
 * Persist:  the store is mirrored to a journal on CPU1's flash through the mailbox (ipc/saf_shared.h) and restored after a restart.
 * Half duplex: the satellite has one radio, so frames that arrive during the CW beacon (about a third of the 90 s cycle)
 *           are not heard. See docs/DIGIPEATER.md.
 */

#include <stdint.h>
#include <stdbool.h>
#include "digipeater.h"
#include "saf.h"

/* statistics and the boot self-test result live in RAM so they can be read over SWD */
extern volatile uint32_t g_digiSelfTest;     /* 0xA5A50002 = pass, 0xDEAD0002 = fail, 0 = not run */
extern volatile uint32_t g_digiSelfStep;     /* which check failed, 0 = none */
extern Digi_t g_digi;
extern Saf_t  g_saf;

/* configure the engine from generated/s2s2_config.h, run the self-test of the rules, open the mailbox and ask CPU1 for the
 * stored messages; returns the self-test result */
bool Sat_Digi_Init(void);

/* true when the frame had a digipeater path (handled here, repeated or not), false for everything else */
bool Sat_Digi_Handle(const uint8_t *frame, uint16_t len, uint32_t now_ms);

/* call from the listen loop: journal traffic with CPU1, injected test frames, and sending stored messages when allow_tx */
void Sat_Digi_Service(uint32_t now_ms, bool allow_tx);

#endif

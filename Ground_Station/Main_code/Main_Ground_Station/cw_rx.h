/*
 * cw_rx.h - CW (Morse / OOK) beacon decoder for the STM32WL55 Ground Station
 *
 * The SX126x has no OOK demodulator, so the decoder works on the
 * instantaneous RSSI (envelope) of the 435.000 MHz carrier:
 *   RSSI samples (every CW_SAMPLE_MS) -> adaptive threshold -> mark/space
 *   durations -> dit/dah -> characters -> lines.
 *
 * Satellite keyer timing (unit = 80 ms, 15 WPM):
 *   dit = 1 unit (80 ms), dah = 3 units (240 ms), intra-char gap = 1 unit,
 *   char gap = 3 units, word gap = 7 units,
 *   2 s tuning carrier at start, 1.0 s gap between callsign and message.
 *
 * Pure C, no HAL calls.
 */
#ifndef CW_RX_H
#define CW_RX_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Tunables ------------------------------------------------------------ */
#define CW_SAMPLE_MS           5U      /* RSSI sampling period                  */
/* CHANGED: was 80 ms (15 WPM). Must equal SAT_CFG_DEFAULT_CW_MORSE_UNIT_MS in the satellite; every threshold
 * below is a multiple of it. */
#define CW_UNIT_DEFAULT_MS     60U     /* 1 unit = dit = 60 ms (20 WPM)         */
#define CW_DIT_MS              (CW_UNIT_DEFAULT_MS)          /*  60 ms nominal */
#define CW_DAH_MS              (3U * CW_UNIT_DEFAULT_MS)     /* 180 ms nominal */

/* Classification thresholds (midpoints between nominal values) */
#define CW_DAH_MIN_MS          (2U * CW_UNIT_DEFAULT_MS)     /* mark >= 120 -> dah     */
#define CW_TUNE_MIN_MS         (5U * CW_UNIT_DEFAULT_MS)     /* mark >= 300 -> carrier */
#define CW_MARK_WATCHDOG_MS    (20U * CW_UNIT_DEFAULT_MS)    /* 1200 ms: force mark-end if stuck in mark */
#define CW_CHAR_GAP_MS         (2U * CW_UNIT_DEFAULT_MS)     /* space >= 120 -> end of char */
#define CW_WORD_GAP_MS         (5U * CW_UNIT_DEFAULT_MS)     /* space >= 300 -> word gap    */
#define CW_SEG_GAP_MS          (10U * CW_UNIT_DEFAULT_MS)    /* space >= 600 -> segment gap */

#define CW_MIN_SNR_DB          12      /* min (level - noise floor) for carrier */
#define CW_NOISE_INIT_DBM      (-125)  /* initial noise floor estimate          */
#define CW_LOST_HOLD_MS        500U    /* carrier "lost" after this much silence (was 3000, kept short to prevent stuck-mark) */
#define CW_DEBOUNCE_SAMPLES    3U      /* edge must persist 3 samples (15 ms)   */
#define CW_GLITCH_MS           40U     /* pulses < 40 ms ignored as noise spikes */
#define CW_LINE_IDLE_MS        2500U   /* flush line after this much silence    */
#define CW_LINE_MAX            96U
#define CW_SYM_MAX             8U
#define CW_CALLSIGN            "9N2SJ"     /* CHANGED: CW beacon = "S2S2 9N2SJ" + 8 housekeeping bytes as hex digits */

typedef enum
{
    CW_EVT_SIGNAL,    /* value = 1 carrier present / 0 lost, rssi in aux       */
    CW_EVT_CARRIER,   /* long tuning carrier, value = duration ms              */
    CW_EVT_CHAR,      /* ch = decoded char, text = morse pattern               */
    CW_EVT_LINE       /* text = complete line, value = 1 if callsign matched   */
} CwEvt_t;

typedef void (*CwRx_EventCb)(CwEvt_t evt, char ch, const char *text,
                             uint32_t value, int16_t aux);

void CwRx_Init(CwRx_EventCb cb, uint32_t now_ms);
void CwRx_Reset(uint32_t now_ms);                 /* drop state (after TX etc.) */
void CwRx_Mute(uint32_t until_ms);                /* ignore samples until time  */
void CwRx_Feed(int16_t rssi_dbm, uint32_t now_ms);/* call every CW_SAMPLE_MS    */
uint32_t CwRx_GetUnitMs(void);
int16_t  CwRx_GetNoiseDbm(void);
int16_t  CwRx_GetPeakDbm(void);

#ifdef __cplusplus
}
#endif
#endif /* CW_RX_H */
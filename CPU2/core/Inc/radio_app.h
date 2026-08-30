#ifndef RADIO_APP_H
#define RADIO_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#include "config.h"
#include "radio.h"
#include "radio_driver.h"

/*==========================================================
    API

    NOTE: context is kept internally (static) rather than
    passed by the caller - this matches how main.c actually
    calls RadioApp_Init(&profile, &events), with only two
    arguments.

    TX and RX now share one identical arming sequence
    (RadioApp_ForceRadioParams + buffer base + channel) -
    see radio_app.c. The ONLY difference between the transmit
    path and the receive path anywhere in this file is the
    single final driver call: Radio.Send(...) vs Radio.Rx(0).
    Nothing else can silently diverge between the two sides.
==========================================================*/

void RadioApp_Init(
    const RadioConfig_t *config,
    RadioEvents_t *events);

/* Arms RX: identical shared setup, then Radio.Rx(0). */
void RadioApp_StartRx(void);

/* Sends one native radio packet (<=255B):
   identical shared setup, then Radio.Send(). */
void RadioApp_Send(
    uint8_t *buffer,
    uint16_t length);

void RadioApp_Sleep(void);

/* Forces variable-length GMSK PacketParams directly on the SX126x
   (hardware sync word, hardware CRC16-CCITT, whitening on).
   MUST produce the identical chip state on satellite and ground station.
   Called internally by RadioApp_Send()/StartRx()/ResumeRx() as the
   last SPI write before Send/Rx - you normally do not call this
   yourself. */
void RadioApp_ForceRadioParams(void);

/* Cheap re-arm for repeated listen-window loops - goes through the
   same shared setup as RadioApp_StartRx(). */
void RadioApp_ResumeRx(void);

/* Overrides the TX power (dBm) used by RadioApp_Send()/ForceRadioParams()
   from here on, clamped to the SX126x HP-PA range [-9, 22]. Defaults to
   RADIO_TX_POWER_DBM (config.h) at boot. Intended for DEBUG_BUILD bench
   testing at short range, where +22 dBm saturates most receiver front
   ends regardless of FEC strength -- NOT a substitute for a real link
   budget. Production builds do not expose a way to call this at runtime. */
void RadioApp_SetTxPower(int8_t dbm);

/* Returns the TX power currently in effect (see RadioApp_SetTxPower). */
int8_t RadioApp_GetTxPower(void);

#ifdef __cplusplus
}
#endif

#endif

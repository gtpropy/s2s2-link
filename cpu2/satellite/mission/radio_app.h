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
    (RadioApp_ForceOpenGfskParams + buffer base + channel) -
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

/* Sends one frame: identical shared setup, then Radio.Send(). */
void RadioApp_Send(
    uint8_t *buffer,
    uint16_t length);

void RadioApp_Sleep(void);

/* Dynamic TX Power & PA Path Control */
void RadioApp_SetTxPower(int8_t power);
int8_t RadioApp_GetTxPower(void);
void RadioApp_SetPaSelect(uint8_t paSelect);
uint8_t RadioApp_GetPaSelect(void);

/* Forces sync word + fixed-length GFSK PacketParams directly on the
   SX126x (CRC off, whitening off, symmetric preamble/sync). MUST
   produce the identical chip state on satellite and ground station.
   Called internally by RadioApp_Send()/StartRx()/ResumeRx() as the
   last SPI write before Send/Rx - you normally do not call this
   yourself. Always configures fixed-length mode; there is no
   variable-length mode in this protocol anymore. */
void RadioApp_ForceOpenGfskParams(void);

/* Cheap re-arm for repeated listen-window loops - goes through the
   same shared setup as RadioApp_StartRx(). */
void RadioApp_ResumeRx(void);

#ifdef __cplusplus
}
#endif

#endif
/*
 * radio_app.c
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-21
 */

#include <string.h>

#include "radio_app.h"
#include "radio_driver.h"
#include "s2s2com.h"
#include "fx25.h"
#include "uart_debug.h"

extern void SUBGRF_SetBufferBaseAddress(
    uint8_t txBaseAddress,
    uint8_t rxBaseAddress);

#define SUBGHZ_TX_BUFFER_BASE  0x00U
#define SUBGHZ_RX_BUFFER_BASE  0x80U

/* This is the NATIVE GMSK packet-engine path (hardware sync-word
   detection, hardware CRC16, hardware whitening) -- a separate, still-
   available transport from the AX.25/FX.25 path in cubus_ax25.h, which
   does its own HDLC framing and FX.25 RS FEC and needs the radio driven
   as a continuous raw bitstream instead (see config.h's AX.25/FX.25
   LINK ADDRESSING section for that gap). Register write is always 8
   bytes; only the low RADIO_SYNCWORD_LEN_BITS bits (this driver's
   PacketParams.SyncWordLength field, taken as raw bits - see
   radio_driver.c's SetPacketParams) are actually correlated against on
   air. */
#define RADIO_SYNCWORD_LEN_BYTES 4U
#define RADIO_SYNCWORD_LEN_BITS  (RADIO_SYNCWORD_LEN_BYTES * 8U)
static const uint8_t s_syncWordReg[8] = { 0x1A, 0xCF, 0xFC, 0x1D, 0x00, 0x00, 0x00, 0x00 };

static RadioConfig_t s_config;
static bool s_initialized = false;
static int8_t s_tx_power_dbm = RADIO_TX_POWER_DBM;

#define RADIO_TX_POWER_MIN_DBM (-9)
#define RADIO_TX_POWER_MAX_DBM 22

void RadioApp_SetTxPower(int8_t dbm)
{
    if (dbm < RADIO_TX_POWER_MIN_DBM) dbm = RADIO_TX_POWER_MIN_DBM;
    if (dbm > RADIO_TX_POWER_MAX_DBM) dbm = RADIO_TX_POWER_MAX_DBM;
    s_tx_power_dbm = dbm;
    uart2_printf("RadioApp_SetTxPower: %d dBm\r\n", (int)s_tx_power_dbm);
}

int8_t RadioApp_GetTxPower(void)
{
    return s_tx_power_dbm;
}

static void RadioApp_ArmCommon(
    uint32_t channelHz);

/*
 * ============================================================
 * INITIALIZE
 * ============================================================
 */

void RadioApp_Init(
    const RadioConfig_t *config,
    RadioEvents_t *events)
{
    if (config == NULL ||
        events == NULL)
    {
        return;
    }

    s_config = *config;

    s_initialized = true;

    uart2_printf(
        "RadioApp_Init: ENTER\r\n");

    Radio.Init(events);

    uart2_printf(
        "RadioApp_Init: Radio.Init done\r\n");

    /*
     * TX/RX FIFO regions.
     */
    SUBGRF_SetBufferBaseAddress(
        SUBGHZ_TX_BUFFER_BASE,
        SUBGHZ_RX_BUFFER_BASE);

    /*
     * Initial channel.
     */
    Radio.SetChannel(
        config->rxFrequency);

    uart2_printf(
        "Radio RX = %lu Hz\r\n",
        (unsigned long)
        config->rxFrequency);

    uart2_printf(
        "Radio TX = %lu Hz\r\n",
        (unsigned long)
        config->txFrequency);

    uart2_printf(
        "Bitrate = %d\r\n",
        RADIO_BIT_RATE_BPS);

    uart2_printf(
        "Deviation = %d Hz (GMSK, h=0.5)\r\n",
        RADIO_FDEV_HZ);

    /*
     * ========================================================
     * TX CONFIG
     * ========================================================
     */

    Radio.SetTxConfig(
        MODEM_FSK,
        RADIO_TX_POWER_DBM,
        RADIO_FDEV_HZ,
        0,
        RADIO_BIT_RATE_BPS,
        0,
        RADIO_PREAMBLE_LENGTH_BITS / 8U,
        false,
        true,
        false,
        0,
        false,
        RADIO_TX_TIMEOUT_MS);

    /*
     * ========================================================
     * RX CONFIG
     * ========================================================
     */

    Radio.SetRxConfig(
        MODEM_FSK,
        RADIO_RX_BANDWIDTH_HZ,
        RADIO_BIT_RATE_BPS,
        0,
        RADIO_RX_BANDWIDTH_HZ,
        RADIO_PREAMBLE_LENGTH_BITS / 8U,
        0,
        false,
        0,
        true,
        false,
        0,
        false,
        true);

    /*
     * Force exact SX126x GMSK packet parameters.
     */
    RadioApp_ForceRadioParams();

    uart2_printf(
        "RadioApp_Init: EXIT\r\n");
}

/*
 * ============================================================
 * COMMON RADIO ARM
 * ============================================================
 */

static void RadioApp_ArmCommon(
    uint32_t channelHz)
{
    /*
     * Always restore FIFO addresses.
     */
    SUBGRF_SetBufferBaseAddress(
        SUBGHZ_TX_BUFFER_BASE,
        SUBGHZ_RX_BUFFER_BASE);

    /*
     * Restore GMSK packet parameters.
     */
    RadioApp_ForceRadioParams();

    /*
     * Select frequency.
     */
    Radio.SetChannel(channelHz);
}

/*
 * ============================================================
 * START RX
 * ============================================================
 */

void RadioApp_StartRx(void)
{
    if (!s_initialized)
        return;

    RadioApp_ArmCommon(
        s_config.rxFrequency);

    Radio.Rx(0);
}

/*
 * ============================================================
 * SEND
 * ============================================================
 */

void RadioApp_Send(
    uint8_t *buffer,
    uint16_t length)
{
    if (!s_initialized ||
        buffer == NULL ||
        length == 0)
    {
        return;
    }

    uart2_printf(
        "RadioApp_Send: len=%u\r\n",
        length);

    /* 1. Put radio in Standby */
    Radio.Standby();

    /* 2. Configure RF frequency */
    SUBGRF_SetRfFrequency(s_config.txFrequency);

    /* 3. Configure packet parameters: FIXED-length GMSK, hardware CRC and
       whitening OFF -- this is what makes the transmitted bytes a real,
       standards-compliant AX.25/FX.25 frame (fx25.h) instead of an
       SX126x-proprietary one. A stock AX.25/FX.25 ground-station decoder
       has no way to strip a chip-inserted length byte, an extra hardware
       CRC, or Semtech's whitening scramble -- so all three must be off;
       HDLC bit-stuffing (already applied by fx25_pack_data_field) is
       what provides transition density instead of whitening. */
    PacketParams_t packetParams;
    memset(&packetParams, 0, sizeof(packetParams));
    packetParams.PacketType = PACKET_TYPE_GMSK;
    packetParams.Params.Gfsk.PreambleLength = RADIO_PREAMBLE_LENGTH_BITS;
    packetParams.Params.Gfsk.PreambleMinDetect = RADIO_PREAMBLE_DETECTOR_16_BITS;
    packetParams.Params.Gfsk.SyncWordLength = RADIO_SYNCWORD_LEN_BITS;
    packetParams.Params.Gfsk.AddrComp = RADIO_ADDRESSCOMP_FILT_OFF;
    packetParams.Params.Gfsk.HeaderType = RADIO_PACKET_FIXED_LENGTH;
    packetParams.Params.Gfsk.PayloadLength = (uint8_t)length;
    packetParams.Params.Gfsk.CrcLength = RADIO_CRC_OFF;
    packetParams.Params.Gfsk.DcFree = RADIO_DC_FREE_OFF;
    SUBGRF_SetPacketParams(&packetParams);

    /* 4. Configure modulation parameters (9600 baud, 2400 Hz Fdev, GMSK BT 0.3) */
    ModulationParams_t modParams;
    memset(&modParams, 0, sizeof(modParams));
    modParams.PacketType = PACKET_TYPE_GMSK;
    modParams.Params.Gfsk.BitRate = RADIO_BIT_RATE_BPS;
    modParams.Params.Gfsk.Fdev = RADIO_FDEV_HZ;
    modParams.Params.Gfsk.ModulationShaping = MOD_SHAPING_G_BT_03;
    modParams.Params.Gfsk.Bandwidth = SUBGRF_GetFskBandwidthRegValue(RADIO_RX_BANDWIDTH_HZ);
    SUBGRF_SetModulationParams(&modParams);

    /* 5. Hardware sync word (matches s2s2com.h WL55_SYNCWORD_BYTES) */
    SUBGRF_SetSyncWord((uint8_t *)s_syncWordReg);

    /* 6. Set High Power PA and 200us ramp time (power: see RadioApp_SetTxPower) */
    SUBGRF_SetPaConfig(0x04, 0x07, 0x00, 0x01);
    SUBGRF_SetTxParams(RFO_HP, s_tx_power_dbm, RADIO_RAMP_200_US);

    /* 7. Set RF switch to TX HP */
    SUBGRF_SetSwitch(RFO_HP, RFSWITCH_TX);

    /* 8. Set buffer base and write payload */
    SUBGRF_SetBufferBaseAddress(SUBGHZ_TX_BUFFER_BASE, SUBGHZ_RX_BUFFER_BASE);
    SUBGRF_SetPayload(buffer, (uint8_t)length);

    /* 9. Enable TX IRQs */
    SUBGRF_SetDioIrqParams(
        IRQ_TX_DONE | IRQ_RX_TX_TIMEOUT,
        IRQ_TX_DONE | IRQ_RX_TX_TIMEOUT,
        IRQ_RADIO_NONE,
        IRQ_RADIO_NONE);

    /* 10. Clear IRQs */
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);

    /* 11. Start transmission */
    SUBGRF_SetTx(0);
}

/*
 * ============================================================
 * RAW CARRIER (CW) -- for cw_beacon.h, not the GMSK packet path
 * ============================================================
 */

void RadioApp_KeyCw(bool on)
{
    if (!s_initialized) return;

    if (on) {
        Radio.Standby();
        SUBGRF_SetRfFrequency(s_config.txFrequency);
        /* Radio.TxCw (radio.c's RadioTxCw) handles PA select/config and
           the RF switch internally via SUBGRF_SetRfTxPower -- unlike the
           GMSK path above, no separate SetPaConfig/SetTxParams call is
           needed here. */
        Radio.TxCw(s_tx_power_dbm);
    } else {
        Radio.Standby();
    }
}

/*
 * ============================================================
 * GMSK PACKET PARAMETERS
 * ============================================================
 */

void RadioApp_ForceRadioParams(void)
{
    Radio.Standby();

    PacketParams_t packetParams;

    memset(
        &packetParams,
        0,
        sizeof(packetParams));

    packetParams.PacketType =
        PACKET_TYPE_GMSK;

    packetParams.Params.Gfsk.PreambleLength =
        RADIO_PREAMBLE_LENGTH_BITS;

    packetParams.Params.Gfsk.PreambleMinDetect =
        RADIO_PREAMBLE_DETECTOR_16_BITS;

    packetParams.Params.Gfsk.SyncWordLength =
        RADIO_SYNCWORD_LEN_BITS;

    packetParams.Params.Gfsk.AddrComp =
        RADIO_ADDRESSCOMP_FILT_OFF;

    /*
     * FIXED LENGTH:
     *
     * Every frame this firmware sends or expects to receive is exactly
     * FX25_FRAME_LEN (168) bytes -- the whole point of AX.25/FX.25
     * compatibility is that frame boundaries come from HDLC flags inside
     * the payload, not a chip-inserted length byte a stock ground
     * station wouldn't know to strip. Both TX and RX must agree on this
     * same fixed length in advance, which they do (it's a compile-time
     * constant from fx25.h).
     */
    packetParams.Params.Gfsk.HeaderType =
        RADIO_PACKET_FIXED_LENGTH;

    packetParams.Params.Gfsk.PayloadLength = FX25_FRAME_LEN;

    /*
     * Hardware CRC OFF.
     *
     * AX.25's own FCS (ax25.c) plus FX.25's RS(255,223)-shortened FEC
     * (fx25.c) already cover integrity/correction; a stock AX.25/FX.25
     * decoder has no idea an extra hardware CRC16 would be appended and
     * doesn't need one.
     */
    packetParams.Params.Gfsk.CrcLength =
        RADIO_CRC_OFF;

    /*
     * Hardware whitening OFF.
     *
     * A stock ground station has no way to de-whiten Semtech's
     * proprietary whitening sequence. HDLC bit-stuffing
     * (fx25_pack_data_field) provides the bit-transition density for
     * GMSK clock/data recovery instead -- the same role G3RUH scrambling
     * plays in classic AX.25 hardware.
     */
    packetParams.Params.Gfsk.DcFree =
        RADIO_DC_FREE_OFF;

    SUBGRF_SetPacketParams(
        &packetParams);

    /*
     * Modulation parameters (9600 baud, GMSK h=0.5 BT=0.3).
     */
    ModulationParams_t modParams;

    memset(
        &modParams,
        0,
        sizeof(modParams));

    modParams.PacketType =
        PACKET_TYPE_GMSK;

    modParams.Params.Gfsk.BitRate =
        RADIO_BIT_RATE_BPS;

    modParams.Params.Gfsk.Fdev =
        RADIO_FDEV_HZ;

    modParams.Params.Gfsk.ModulationShaping =
        MOD_SHAPING_G_BT_03;

    modParams.Params.Gfsk.Bandwidth =
        SUBGRF_GetFskBandwidthRegValue(
            RADIO_RX_BANDWIDTH_HZ);

    SUBGRF_SetModulationParams(
        &modParams);

    /*
     * Hardware sync word - lets the radio's own
     * sync detector find frame boundaries in hardware.
     */
    SUBGRF_SetSyncWord((uint8_t *)s_syncWordReg);

    /*
     * Configure High Power PA and PA Ramp Time (200 us).
     * Power: see RadioApp_SetTxPower / s_tx_power_dbm.
     */
    SUBGRF_SetPaConfig(0x04, 0x07, 0x00, 0x01);
    SUBGRF_SetTxParams(RFO_HP, s_tx_power_dbm, RADIO_RAMP_200_US);
}

/*
 * ============================================================
 * RESUME RX
 * ============================================================
 */

void RadioApp_ResumeRx(void)
{
    if (!s_initialized)
        return;

    RadioApp_ArmCommon(
        s_config.rxFrequency);

    Radio.Rx(0);
}

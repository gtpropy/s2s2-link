#include <string.h>

#include "radio_app.h"
#include "radio_driver.h"
#include "ax25.h"
#include "uart_debug.h"

extern void SUBGRF_SetBufferBaseAddress(
    uint8_t txBaseAddress,
    uint8_t rxBaseAddress);

#define SUBGHZ_TX_BUFFER_BASE  0x00U
#define SUBGHZ_RX_BUFFER_BASE  0x80U

static RadioConfig_t s_config;
static bool s_initialized = false;

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
        "Deviation = %d Hz\r\n",
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
        true,
        false,
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
        true,
        0,
        false,
        false,
        0,
        false,
        true);

    /*
     * Force exact SX126x GFSK packet parameters.
     */
    RadioApp_ForceOpenGfskParams();

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
     * Restore GFSK packet parameters.
     */
    RadioApp_ForceOpenGfskParams();

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

    /* 3. Configure packet parameters: pure G3RUH bitstream (no HW syncword or preamble) */
    PacketParams_t packetParams;
    memset(&packetParams, 0, sizeof(packetParams));
    packetParams.PacketType = PACKET_TYPE_GFSK;
    packetParams.Params.Gfsk.PreambleLength = 0;
    packetParams.Params.Gfsk.PreambleMinDetect = RADIO_PREAMBLE_DETECTOR_OFF;
    packetParams.Params.Gfsk.SyncWordLength = 0;
    packetParams.Params.Gfsk.AddrComp = RADIO_ADDRESSCOMP_FILT_OFF;
    packetParams.Params.Gfsk.HeaderType = RADIO_PACKET_FIXED_LENGTH;
    packetParams.Params.Gfsk.PayloadLength = length;
    packetParams.Params.Gfsk.CrcLength = RADIO_CRC_OFF;
    packetParams.Params.Gfsk.DcFree = RADIO_DC_FREE_OFF;
    SUBGRF_SetPacketParams(&packetParams);

    /* 4. Configure modulation parameters (9600 baud, 2400 Hz Fdev, BT 0.5) */
    ModulationParams_t modParams;
    memset(&modParams, 0, sizeof(modParams));
    modParams.PacketType = PACKET_TYPE_GFSK;
    modParams.Params.Gfsk.BitRate = RADIO_BIT_RATE_BPS;
    modParams.Params.Gfsk.Fdev = RADIO_FDEV_HZ;
    modParams.Params.Gfsk.ModulationShaping = MOD_SHAPING_G_BT_05;
    modParams.Params.Gfsk.Bandwidth = SUBGRF_GetFskBandwidthRegValue(RADIO_RX_BANDWIDTH_HZ);
    SUBGRF_SetModulationParams(&modParams);

    /* 5. Set High Power PA (+22 dBm) and 200us ramp time */
    SUBGRF_SetPaConfig(0x04, 0x07, 0x00, 0x01);
    SUBGRF_SetTxParams(RFO_HP, RADIO_TX_POWER_DBM, RADIO_RAMP_200_US);

    /* 6. Set RF switch to TX HP */
    SUBGRF_SetSwitch(RFO_HP, RFSWITCH_TX);

    /* 7. Set buffer base and write payload */
    SUBGRF_SetBufferBaseAddress(SUBGHZ_TX_BUFFER_BASE, SUBGHZ_RX_BUFFER_BASE);
    SUBGRF_SetPayload(buffer, (uint8_t)length);

    /* 8. Enable TX IRQs */
    SUBGRF_SetDioIrqParams(
        IRQ_TX_DONE | IRQ_RX_TX_TIMEOUT,
        IRQ_TX_DONE | IRQ_RX_TX_TIMEOUT,
        IRQ_RADIO_NONE,
        IRQ_RADIO_NONE);

    /* 9. Clear IRQs */
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);

    /* 10. Start transmission */
    SUBGRF_SetTx(0);
}

/*
 * ============================================================
 * GFSK PACKET PARAMETERS
 * ============================================================
 */

void RadioApp_ForceOpenGfskParams(void)
{
    Radio.Standby();

    PacketParams_t packetParams;

    memset(
        &packetParams,
        0,
        sizeof(packetParams));

    packetParams.PacketType =
        PACKET_TYPE_GFSK;

    /*
     * V1.4 Reference: Pure G3RUH bitstream mode.
     * No hardware syncword or preamble to interfere with audio G3RUH framing.
     */
    packetParams.Params.Gfsk.PreambleLength =
        0;

    packetParams.Params.Gfsk.PreambleMinDetect =
        RADIO_PREAMBLE_DETECTOR_OFF;

    packetParams.Params.Gfsk.SyncWordLength =
        0;

    packetParams.Params.Gfsk.AddrComp =
        RADIO_ADDRESSCOMP_FILT_OFF;

    /*
     * FIXED LENGTH:
     *
     * Every SX126x payload is exactly RADIO_FIXED_PACKET_LEN
     * bytes.
     */
    packetParams.Params.Gfsk.HeaderType =
        RADIO_PACKET_FIXED_LENGTH;

    packetParams.Params.Gfsk.PayloadLength =
        RADIO_FIXED_PACKET_LEN;

    /*
     * Hardware CRC OFF.
     *
     * AX.25 supplies the FCS.
     */
    packetParams.Params.Gfsk.CrcLength =
        RADIO_CRC_OFF;

    /*
     * Hardware whitening OFF.
     *
     * G3RUH is performed in software.
     */
    packetParams.Params.Gfsk.DcFree =
        RADIO_DC_FREE_OFF;

    SUBGRF_SetPacketParams(
        &packetParams);

    /*
     * Modulation parameters (9600 baud standard).
     */
    ModulationParams_t modParams;

    memset(
        &modParams,
        0,
        sizeof(modParams));

    modParams.PacketType =
        PACKET_TYPE_GFSK;

    modParams.Params.Gfsk.BitRate =
        RADIO_BIT_RATE_BPS;

    modParams.Params.Gfsk.Fdev =
        RADIO_FDEV_HZ;

    modParams.Params.Gfsk.ModulationShaping =
        MOD_SHAPING_G_BT_05;

    modParams.Params.Gfsk.Bandwidth =
        SUBGRF_GetFskBandwidthRegValue(
            RADIO_RX_BANDWIDTH_HZ);

    SUBGRF_SetModulationParams(
        &modParams);

    /*
     * Configure High Power PA (+22 dBm) and PA Ramp Time (200 us).
     */
    SUBGRF_SetPaConfig(0x04, 0x07, 0x00, 0x01);
    SUBGRF_SetTxParams(RFO_HP, RADIO_TX_POWER_DBM, RADIO_RAMP_200_US);
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
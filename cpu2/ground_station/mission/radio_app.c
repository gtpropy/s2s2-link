#include <string.h>

#include "radio_app.h"
#include "radio_driver.h"
#include "radio_board_if.h"
#include "ax25.h"
#include "uart_debug.h"

extern void SUBGRF_SetBufferBaseAddress(
    uint8_t txBaseAddress,
    uint8_t rxBaseAddress);

#define SUBGHZ_TX_BUFFER_BASE  0x00U
#define SUBGHZ_RX_BUFFER_BASE  0x80U

static RadioConfig_t s_config;
static bool s_initialized = false;
static int8_t s_radio_tx_power_dbm = 22;
static uint8_t s_radio_pa_select = RFO_HP; /* default RFO_HP (+22 dBm) safe on both STM32WL55JC1 and JC2 */

void RadioApp_SetTxPower(int8_t power)
{
    if (power > 22) power = 22;
    if (power < -17) power = -17;
    s_radio_tx_power_dbm = power;
}

int8_t RadioApp_GetTxPower(void)
{
    return s_radio_tx_power_dbm;
}

void RadioApp_SetPaSelect(uint8_t paSelect)
{
    s_radio_pa_select = paSelect;
}

uint8_t RadioApp_GetPaSelect(void)
{
    return s_radio_pa_select;
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
        0,
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
        0,
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

    /* 1. Put radio in Standby with XOSC: keep 32 MHz TCXO active */
    extern void CPU2_Delay_Ms(uint32_t ms);
    SUBGRF_SetStandby(STDBY_XOSC);
    CPU2_Delay_Ms(5);
    SUBGRF_SetPacketType(PACKET_TYPE_GFSK);

    /* 2. Configure RF frequency */
    SUBGRF_SetRfFrequency(s_config.txFrequency);

    /* 3. AX.25/G3RUH payload, preceded by HW preamble + sync so the satellite
     *    RX FIFO starts at byte 0 of the scrambled frame (not a random slice). */
    static uint8_t s_uplink_sync[8] = RADIO_DOWNLINK_SYNC_WORD;
    SUBGRF_SetSyncWord(s_uplink_sync);

    PacketParams_t packetParams;
    memset(&packetParams, 0, sizeof(packetParams));
    packetParams.PacketType = PACKET_TYPE_GFSK;
    packetParams.Params.Gfsk.PreambleLength = RADIO_PREAMBLE_LENGTH_BITS;
    packetParams.Params.Gfsk.PreambleMinDetect = RADIO_PREAMBLE_DETECTOR_OFF;
    packetParams.Params.Gfsk.SyncWordLength = RADIO_DOWNLINK_SYNC_WORD_BITS;
    packetParams.Params.Gfsk.AddrComp = RADIO_ADDRESSCOMP_FILT_OFF;
    packetParams.Params.Gfsk.HeaderType = RADIO_PACKET_FIXED_LENGTH;
    packetParams.Params.Gfsk.PayloadLength = length;
    packetParams.Params.Gfsk.CrcLength = RADIO_CRC_OFF;
    packetParams.Params.Gfsk.DcFree = RADIO_DC_FREE_OFF;
    SUBGRF_SetPacketParams(&packetParams);
    SUBGRF_SetSyncWord(s_uplink_sync);

    /* 4. Configure modulation parameters */
    ModulationParams_t modParams;
    memset(&modParams, 0, sizeof(modParams));
    modParams.PacketType = PACKET_TYPE_GFSK;
    modParams.Params.Gfsk.BitRate = RADIO_BIT_RATE_BPS;
    modParams.Params.Gfsk.Fdev = RADIO_FDEV_HZ;
    modParams.Params.Gfsk.ModulationShaping = MOD_SHAPING_G_BT_05;
    modParams.Params.Gfsk.Bandwidth = SUBGRF_GetFskBandwidthRegValue(RADIO_RX_BANDWIDTH_HZ);
    SUBGRF_SetModulationParams(&modParams);

    /* 5. Set PA configuration and output power */
    if (s_radio_pa_select == RFO_HP) {
        SUBGRF_SetPaConfig(0x04, 0x07, 0x00, 0x01);
        SUBGRF_SetTxParams(RFO_HP, s_radio_tx_power_dbm, RADIO_RAMP_200_US);
    } else {
        SUBGRF_SetPaConfig(0x04, 0x00, 0x01, 0x01);
        SUBGRF_SetTxParams(RFO_LP, s_radio_tx_power_dbm, RADIO_RAMP_200_US);
    }
    SUBGRF_WriteRegister(REG_DRV_CTRL, 0x7 << 1);

    /* 6. Set RF switch to TX */
    SUBGRF_SetSwitch(s_radio_pa_select, RFSWITCH_TX);

    /* 7. Set buffer base and write payload to TX FIFO */
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

    RadioPhyStatus_t stat = SUBGRF_GetStatus();
    uart2_printf(
        "RadioApp_Send: started (Mode=0x%02X [%s], CmdStat=0x%02X)\r\n",
        stat.Fields.ChipMode,
        (stat.Fields.ChipMode == 6) ? "TX ACTIVE" : (stat.Fields.ChipMode == 3) ? "STDBY_XOSC" : "OTHER",
        stat.Fields.CmdStatus);
}

/*
 * ============================================================
 * GFSK PACKET PARAMETERS
 * ============================================================
 */

void RadioApp_ForceOpenGfskParams(void)
{
    SUBGRF_SetStandby(STDBY_XOSC);
    SUBGRF_SetPacketType(PACKET_TYPE_GFSK);

    PacketParams_t packetParams;

    memset(
        &packetParams,
        0,
        sizeof(packetParams));

    packetParams.PacketType =
        PACKET_TYPE_GFSK;

    /*
     * Downlink RX: wait for the satellite preamble + sync word, then capture
     * RADIO_FIXED_PACKET_LEN bytes bit-aligned with the satellite's G3RUH buffer.
     * Without this the radio free-runs over noise and a frame only decodes when
     * it happens to fall entirely inside one 200-byte window.
     */
    static uint8_t s_downlink_sync[8] = RADIO_DOWNLINK_SYNC_WORD;
    SUBGRF_SetSyncWord(s_downlink_sync);

    packetParams.Params.Gfsk.PreambleLength =
        RADIO_PREAMBLE_LENGTH_BITS;

    packetParams.Params.Gfsk.PreambleMinDetect =
        RADIO_PREAMBLE_DETECTOR_16_BITS;

    packetParams.Params.Gfsk.SyncWordLength =
        RADIO_DOWNLINK_SYNC_WORD_BITS;

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
     * Modulation parameters.
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
     * Configure PA power and PA Ramp Time.
     */
    if (s_radio_pa_select == RFO_HP) {
        SUBGRF_SetPaConfig(0x04, 0x07, 0x00, 0x01);
        SUBGRF_SetTxParams(RFO_HP, s_radio_tx_power_dbm, RADIO_RAMP_200_US);
    } else {
        SUBGRF_SetPaConfig(0x07, 0x00, 0x01, 0x01);
        SUBGRF_SetTxParams(RFO_LP, s_radio_tx_power_dbm, RADIO_RAMP_200_US);
    }
    SUBGRF_WriteRegister(REG_DRV_CTRL, 0x7 << 1);
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

/*
 * After the first packet in RX-continuous GFSK mode the SX126x keeps
 * detecting preambles but no longer matches the sync word, so only the
 * first downlink packet was ever received. Restarting RX after every
 * packet re-initialises the packet engine.
 */
void RadioApp_RearmRxFast(void)
{
    if (!s_initialized)
        return;

    SUBGRF_SetStandby(STDBY_XOSC);
    SUBGRF_SetBufferBaseAddress(SUBGHZ_TX_BUFFER_BASE, SUBGHZ_RX_BUFFER_BASE);
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);
    SUBGRF_SetRx(0xFFFFFF);
}

/*
 * ============================================================
 * CONTINUOUS TEST CARRIER (FOR SDR ALIGNMENT / SPECTRUM TEST)
 * ============================================================
 */

extern void CPU2_Delay_Ms(uint32_t ms);

void RadioApp_TransmitCarrier(
    uint32_t frequencyHz,
    uint32_t durationSeconds)
{
    if (!s_initialized || durationSeconds == 0)
        return;

    uart2_printf(
        "\r\n>>> [RF-TEST] Starting Continuous Carrier @ %lu Hz (%s %+d dBm) for %lu s <<<\r\n",
        (unsigned long)frequencyHz,
        (s_radio_pa_select == RFO_HP) ? "RFO_HP" : "RFO_LP",
        (int)s_radio_tx_power_dbm,
        (unsigned long)durationSeconds);
    uart2_puts("    Check your SDR display centered at 437.375 MHz!\r\n");

    /* 1. Put radio in Standby with XOSC so TCXO runs */
    Radio.Standby();
    SUBGRF_SetStandby(STDBY_XOSC);
    CPU2_Delay_Ms(10); /* TCXO stabilize delay */
    SUBGRF_SetPacketType(PACKET_TYPE_GFSK);

    /* 2. Configure RF frequency */
    SUBGRF_SetRfFrequency(frequencyHz);

    /* 3. Configure PA configuration and output power */
    if (s_radio_pa_select == RFO_HP) {
        SUBGRF_SetPaConfig(0x04, 0x07, 0x00, 0x01);
        SUBGRF_SetTxParams(RFO_HP, s_radio_tx_power_dbm, RADIO_RAMP_200_US);
    } else {
        SUBGRF_SetPaConfig(0x04, 0x00, 0x01, 0x01);
        SUBGRF_SetTxParams(RFO_LP, s_radio_tx_power_dbm, RADIO_RAMP_200_US);
    }
    SUBGRF_WriteRegister(REG_DRV_CTRL, 0x7 << 1);

    /* 4. Set RF switch to TX */
    SUBGRF_SetSwitch(s_radio_pa_select, RFSWITCH_TX);

    /* 5. Transmit pure unmodulated carrier */
    SUBGRF_SetTxContinuousWave();

    RadioPhyStatus_t stat = SUBGRF_GetStatus();
    uart2_printf(
        "    Carrier Active: Mode=0x%02X (%s), CmdStat=0x%02X\r\n",
        stat.Fields.ChipMode,
        (stat.Fields.ChipMode == 6) ? "TX OK" : "OTHER",
        stat.Fields.CmdStatus);

    /* 6. Hold for requested duration */
    for (uint32_t s = 0; s < durationSeconds; s++)
    {
        uart2_printf(
            " [CARRIER ACTIVE @ %lu Hz] %lu s / %lu s\r\n",
            (unsigned long)frequencyHz,
            (unsigned long)(s + 1),
            (unsigned long)durationSeconds);
        CPU2_Delay_Ms(1000);
    }

    /* 7. Stop carrier, return to Standby and arm RX */
    SUBGRF_SetStandby(STDBY_XOSC);
    Radio.Standby();

    uart2_puts(
        ">>> [RF-TEST] Carrier Complete. Returning to 435 MHz RX <<<\r\n");

    RadioApp_StartRx();
}
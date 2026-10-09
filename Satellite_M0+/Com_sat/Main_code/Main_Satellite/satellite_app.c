#include "satellite_app.h"
#include "ll_port.h"
#include "seal_wrap.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "stm32wlxx_hal.h"
#include "radio.h"
#include "subghz.h"
#include "radio_board_if.h"
#include "uart/uart_debug.h"

#include "Mission/config.h"
#include "Mission/radio_app.h"
#include "Mission/protocol.h"
#include "protocol/ax25/ax25.h"
#include "protocol/ax25/crc16.h"
#include "protocol/g3ruh/g3ruh.h"

#include "Sring_buffer.h"

/*
 * ============================================================================
 * MODULE STATE & BUFFERS
 * ============================================================================
 */

const SatelliteConfig_t SatelliteDefaultConfig = SATELLITE_CONFIG_DEFAULT;
static SatelliteConfig_t s_config = SATELLITE_CONFIG_DEFAULT;

volatile uint8_t tx_busy = 0;
volatile uint8_t rx_frame_buffer[AX25_MAX_FRAME_SIZE];
volatile uint16_t rx_frame_size = 0;
volatile uint8_t rx_done_flag = 0;
volatile uint8_t rx_timeout_flag = 0;
volatile uint8_t rx_error_flag = 0;
volatile int16_t last_rx_rssi = -120;
volatile int8_t  last_rx_snr  = 0;

uint8_t last_tx_scrambled[AX25_MAX_FRAME_SIZE];
uint16_t last_tx_len = 0;

bool Looks_Like_Stale_TX_Buffer(const uint8_t *buf, uint16_t len) {
    if (last_tx_len == 0) return false;
    uint16_t compare_len = (len < last_tx_len) ? len : last_tx_len;
    if (compare_len < 8) return false;
    return (memcmp(buf, last_tx_scrambled, compare_len) == 0);
}

/* Static packet buffers to guarantee zero stack overflow on Cortex-M0+ */
static uint8_t s_raw_frame[AX25_MAX_FRAME_SIZE + 64];
static uint8_t s_burst_scrambled[RADIO_FIXED_PACKET_LEN + 64];
static uint32_t s_total_packets_sent = 0;
static bool s_dcdc_hold = false;

void Satellite_SetDCDCHold(bool hold) {
    s_dcdc_hold = hold;
}

bool Satellite_GetDCDCHold(void) {
    return s_dcdc_hold;
}

extern void CPU2_Delay_Ms(uint32_t ms);

static void Satellite_GMSK_Stop(void)
{
    s_dcdc_hold = false;
    SUBGRF_SetStandby(STDBY_XOSC);
    HAL_GPIO_WritePin(AMP_5V_EN_PORT, AMP_5V_EN_PIN, GPIO_PIN_RESET);
    /* CHANGED: the 10 ms / 20 ms power-down waits are now SAT_PA_OFF_MS (x2) from Mission/config.h:
     * unchanged on FRONTEND=CUSTOM, 0 on a Nucleo (no 5 V boost rail to discharge). */
    CPU2_Delay_Ms(SAT_PA_OFF_MS);
    HAL_GPIO_WritePin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN, GPIO_PIN_RESET);
    CPU2_Delay_Ms(SAT_PA_OFF_MS * 2U);
    HAL_GPIO_WritePin(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN, GPIO_PIN_SET);
    Satellite_SetRFSwitch(RBI_SWITCH_RX);
}

/* After a GMSK packet: keep 5V PA biased during a burst; restore 3.3V only
 * when the session is over. */
static void Satellite_GMSK_AfterTx(void)
{
    CPU2_Delay_Ms(2);
    if (s_dcdc_hold) {
        return;
    }
    Satellite_GMSK_Stop();
}

uint32_t Satellite_GetTotalPacketsSent(void) {
    return s_total_packets_sent;
}

/* Externs from system / IT files */
extern SUBGHZ_HandleTypeDef hsubghz;
extern void SysTick_Init_CPU2(uint32_t sys_freq_hz);
extern void CPU2_Delay_Ms(uint32_t ms);
extern volatile uint32_t g_system_tick_ms;

/*
 * ============================================================================
 * INTERRUPT CALLBACKS & HARDFAULT HANDLER
 * ============================================================================
 */

void OnTxDone(void) {
    tx_busy = 0;
}

void OnTxTimeout(void) {
    uart1_puts("TX TIMEOUT ERROR\r\n");
    tx_busy = 0;
    Satellite_SetRFSwitch(RBI_SWITCH_OFF);
}

void OnRxTimeout(void) {
    uart1_puts("*** PACKET RECEIVED: NO (RX timeout, no signal) ***\r\n");
    rx_timeout_flag = 1;
}

void OnRxError(void) {
    uart1_puts("RX ERROR (CRC/sync fail at radio level)\r\n");
    uart1_puts("*** PACKET RECEIVED: NO (radio-level RX error) ***\r\n");
    rx_error_flag = 1;
}

void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr) {
    uint16_t copy_len = (size < AX25_MAX_FRAME_SIZE) ? size : AX25_MAX_FRAME_SIZE;
    memcpy((void *)rx_frame_buffer, payload, copy_len);
    rx_frame_size = copy_len;
    last_rx_rssi = rssi;
    last_rx_snr = snr;
    rx_done_flag = 1;
}

/*
 * ============================================================================
 * UTILITY & TELEMETRY FORMATTERS
 * ============================================================================
 */

static uint32_t Get_Time_Ms(void) {
    return HAL_GetTick();
}

void Print_Hex_Bytes(const uint8_t *buf, uint16_t len) {
    char line[56];
    uint16_t pos = 0;
    uint16_t on_this_line = 0;
    for (uint16_t i = 0; i < len; i++) {
        pos += snprintf(&line[pos], sizeof(line) - pos, "%02X ", buf[i]);
        on_this_line++;
        if (on_this_line == 16 || i == (uint16_t)(len - 1)) {
            uart1_puts(line);
            uart1_puts("\r\n");
            pos = 0;
            on_this_line = 0;
        }
    }
}

static void Print_Payload_ASCII(const uint8_t *buf, uint16_t start, uint16_t end) {
    if (end <= start) {
        uart1_puts(" Payload (text) : (none)\r\n");
        return;
    }
    char line[96];
    uint16_t n = (uint16_t)(end - start);
    uint16_t max_chars = (uint16_t)(sizeof(line) - 24);
    if (n > max_chars) n = max_chars;
    uint16_t pos = 0;
    pos += (uint16_t)snprintf(&line[pos], sizeof(line) - pos, " Payload (text): \"");
    for (uint16_t i = 0; i < n; i++) {
        uint8_t c = buf[start + i];
        line[pos++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    pos += (uint16_t)snprintf(&line[pos], sizeof(line) - pos, "\"\r\n");
    uart1_puts(line);
}

void Print_AX25_Fields(const char *label, const uint8_t *buf, uint16_t len) {
    char line[112];
    snprintf(line, sizeof(line), " --- %s breakdown (%u bytes) ---\r\n", label, len);
    uart1_puts(line);
    if (len < 20) {
        uart1_puts(" (too short for a full AX.25 header+FCS - skipping field breakdown)\r\n");
        return;
    }
    snprintf(line, sizeof(line), "  Start Flag : 0x%02X %s\r\n", buf[0], (buf[0] == AX25_FLAG) ? "(OK)" : "(MISMATCH)");
    uart1_puts(line);
    char dest_cs[7], src_cs[7];
    AX25_DecodeAddress(&buf[1], dest_cs);
    snprintf(line, sizeof(line), "  Dest       : %s (SSID %d)\r\n", dest_cs, AX25_DecodeSSID(&buf[1]));
    uart1_puts(line);
    AX25_DecodeAddress(&buf[8], src_cs);
    snprintf(line, sizeof(line), "  Src        : %s (SSID %d)\r\n", src_cs, AX25_DecodeSSID(&buf[8]));
    uart1_puts(line);
    snprintf(line, sizeof(line), "  Control    : 0x%02X (UI-Frame)\r\n", buf[15]); uart1_puts(line);
    snprintf(line, sizeof(line), "  PID        : 0x%02X (No layer 3)\r\n", buf[16]); uart1_puts(line);

    uint16_t payload_start = 17;
    uint16_t payload_end = (uint16_t)(len - 3);
    if (payload_end > payload_start) {
        uint16_t plen = (uint16_t)(payload_end - payload_start);
        snprintf(line, sizeof(line), "  Payload    : %u bytes\r\n", plen);
        uart1_puts(line);
        Print_Hex_Bytes(&buf[payload_start], plen);
        Print_Payload_ASCII(buf, payload_start, payload_end);
    } else {
        uart1_puts("  Payload    : (none)\r\n");
    }
    if (len >= 3) {
        snprintf(line, sizeof(line), "  FCS / CRC  : 0x%02X 0x%02X\r\n", buf[len - 3], buf[len - 2]);
        uart1_puts(line);
    }
    snprintf(line, sizeof(line), "  End Flag   : 0x%02X %s\r\n", buf[len - 1], (buf[len - 1] == AX25_FLAG) ? "(OK)" : "(MISMATCH)");
    uart1_puts(line);
}

static bool Radio_Send_And_Wait(uint8_t *buffer, uint16_t size, uint32_t timeout_ms) {
    tx_busy = 1;
    RadioApp_Send(buffer, size);

    uint32_t start_ms = Get_Time_Ms();
    uint32_t max_loop = timeout_ms;
    while (tx_busy != 0) {
        if ((Get_Time_Ms() - start_ms) > timeout_ms || max_loop == 0) {
            uart1_puts("ERROR: TX timeout\r\n");
            Radio.Standby();
            tx_busy = 0;
            return false;
        }
        max_loop--;
        CPU2_Delay_Ms(1);
    }
    return true;
}

/*
 * ============================================================================
 * RF SWITCH SELECTION API
 * ============================================================================
 */

void Satellite_SetRFSwitch(RBI_Switch_TypeDef rf_switch) {
    RBI_ConfigRFSwitch(rf_switch);
}

RBI_Switch_TypeDef Satellite_GetRFSwitch(void) {
    return s_config.rfSwitchConfig;
}

/*
 * ============================================================================
 * CW / MORSE CODE BEACON ENGINE
 * ============================================================================
 */

static const char* Get_Morse_Code(char c) {
    switch (c) {
        case 'A': case 'a': return ".-";
        case 'B': case 'b': return "-...";
        case 'C': case 'c': return "-.-.";
        case 'D': case 'd': return "-..";
        case 'E': case 'e': return ".";
        case 'F': case 'f': return "..-.";
        case 'G': case 'g': return "--.";
        case 'H': case 'h': return "....";
        case 'I': case 'i': return "..";
        case 'J': case 'j': return ".---";
        case 'K': case 'k': return "-.-";
        case 'L': case 'l': return ".-..";
        case 'M': case 'm': return "--";
        case 'N': case 'n': return "-.";
        case 'O': case 'o': return "---";
        case 'P': case 'p': return ".--.";
        case 'Q': case 'q': return "--.-";
        case 'R': case 'r': return ".-.";
        case 'S': case 's': return "...";
        case 'T': case 't': return "-";
        case 'U': case 'u': return "..-";
        case 'V': case 'v': return "...-";
        case 'W': case 'w': return ".--";
        case 'X': case 'x': return "-..-";
        case 'Y': case 'y': return "-.--";
        case 'Z': case 'z': return "--..";
        case '0': return "-----";
        case '1': return ".----";
        case '2': return "..---";
        case '3': return "...--";
        case '4': return "....-";
        case '5': return ".....";
        case '6': return "-....";
        case '7': return "--...";
        case '8': return "---..";
        case '9': return "----.";
        case '.': return ".-.-.-";
        case ',': return "--..--";
        case '?': return "..--..";
        case '/': return "-..-.";
        case '-': return "-....-";
        default:  return NULL;
    }
}

/*
 * CW keying state. CHANGED: the reference re-sent the whole PA set-up (PA config, TX params, OCP, drive,
 * RF switch = 6 SPI transactions) for EVERY dit/dah. The set-up does not change between elements, so
 * Satellite_CW_Prepare() does it once and CarrierOn() only issues SetTxContinuousWave (1 transaction);
 * CarrierOff() is SetStandby(XOSC), which keeps the PA settings. s_cw_cfg_valid falls back to the full
 * set-up if CarrierOn() is ever called without Prepare(), and is cleared whenever the PA path is torn down.
 */
static bool s_cw_cfg_valid = false;

void Satellite_CW_Prepare(void) {
    int8_t power = (s_config.txPowerDbm != 0) ? s_config.txPowerDbm : RADIO_TX_POWER_DBM;
    if (power > 14) power = 14;
    /* CHANGED: was hard-coded RFO_LP. SAT_PA_SEL = RFO_LP on FRONTEND=CUSTOM, RFO_HP on the stock
     * Nucleo (its RF switch only routes the HP PA to the antenna), see Mission/config.h. */
    uint8_t pa_sel = SAT_PA_SEL;

    /* CW always uses the 3.3V PA (PC2); 5V DC/DC (PA0) and 5V PA (PC3) stay OFF */
    uart1_puts(">>> CW PREPARE: 3.3V PA (PC2=1, 5V PA OFF PC3=0, 5V Boost OFF PA0=0)\r\n");
    RBI_Enable5VDCDC(0);
    HAL_GPIO_WritePin(AMP_5V_EN_PORT, AMP_5V_EN_PIN, GPIO_PIN_RESET); /* PC3 = 0 (5V PA OFF) */
    HAL_GPIO_WritePin(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN, GPIO_PIN_SET);   /* PC2 = 1 (3.3V PA ON for CW) */

    /* Configure RF Switch for CW mode (RFO_LP path to 3.3V PA: PC4=0, PC5=1) */
    RBI_SetTxSwitchConfig(RBI_SWITCH_RFO_LP);
    Satellite_SetRFSwitch(RBI_SWITCH_RFO_LP);
    CPU2_Delay_Ms(5); /* Settle delay for 3.3V PA */

    /* Put radio in STDBY_XOSC: 32 MHz TCXO runs continuously without shutting down */
    SUBGRF_SetStandby(STDBY_XOSC);
    CPU2_Delay_Ms(10); /* Ensure TCXO is 100% stable */
    SUBGRF_SetDioIrqParams(IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE);
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);
    SUBGRF_SetPacketType(PACKET_TYPE_GFSK);
    SUBGRF_SetRfFrequency(s_config.radio.txFrequency);
    SAT_SET_PA_CONFIG(); /* was SUBGRF_SetPaConfig(0x04,0x00,0x01,0x01) = RFO_LP; the macro picks LP or HP per FRONTEND */
    SUBGRF_SetTxParams(pa_sel, power, RADIO_RAMP_200_US); /* soft ramp: smaller supply current step */
    SUBGRF_WriteRegister(REG_OCP, SAT_CW_OCP); /* was 0x18; value per FRONTEND in config.h. 60 mA current clamp: prevents 3.3V rail collapse & MCU brownout */
    SUBGRF_WriteRegister(REG_DRV_CTRL, 0x7 << 1);
    SUBGRF_SetSwitch(pa_sel, RFSWITCH_TX);
    s_cw_cfg_valid = true;
}

void Satellite_CW_CarrierOn(void) {
    if (s_cw_cfg_valid) {
        SUBGRF_SetTxContinuousWave();   /* PA already configured by Satellite_CW_Prepare(): key the carrier only */
        return;
    }
    int8_t power = (s_config.txPowerDbm != 0) ? s_config.txPowerDbm : RADIO_TX_POWER_DBM;
    if (power > 14) power = 14;
    SAT_SET_PA_CONFIG(); /* CHANGED: was RFO_LP-only; PA and switch path now follow FRONTEND (Mission/config.h) */
    SUBGRF_SetTxParams(SAT_PA_SEL, power, RADIO_RAMP_200_US); /* soft ramp: smaller supply current step */
    SUBGRF_WriteRegister(REG_OCP, SAT_CW_OCP);
    SUBGRF_WriteRegister(REG_DRV_CTRL, 0x7 << 1);
    SUBGRF_SetSwitch(SAT_PA_SEL, RFSWITCH_TX);
    SUBGRF_SetTxContinuousWave();
    s_cw_cfg_valid = true;
}

void Satellite_CW_CarrierOff(void) {
    /* Return to STDBY_XOSC: cuts RF carrier immediately while keeping 32MHz TCXO running */
    SUBGRF_SetStandby(STDBY_XOSC);
}

void Satellite_CW_Finish(void) {
    s_cw_cfg_valid = false;   /* PA path is being torn down: next CarrierOn() must set it up again */
    SUBGRF_SetStandby(STDBY_RC);
    Radio.Standby();
    /* Deassert 5V DC/DC; 5V PA OFF (PC3=0); 3.3V PA OFF in RX (PC2=0); switch RF to RX */
    HAL_GPIO_WritePin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(AMP_5V_EN_PORT, AMP_5V_EN_PIN, GPIO_PIN_RESET);   /* PC3 = 0 (5V PA OFF) */
    HAL_GPIO_WritePin(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN, GPIO_PIN_SET);   /* PC2 = 1 (3.3V PA stays ON) */
    Satellite_SetRFSwitch(RBI_SWITCH_RX);
    SUBGRF_SetDioIrqParams(IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE);
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);
    tx_busy = 0;
}

/*
 * ADDED: continuous CW "HELLO" test for a spectrum analyzer (TinySA). Begin() sets the PA path up once, Cycle() sends one
 * round -- a steady carrier of tone_ms (easy to find on the screen), a short pause, then HELLO in Morse -- and End() powers
 * the PA down again. The caller (satellite_main.c, driven by the VCP command CWTEST) repeats Cycle() until told to stop.
 */
void Satellite_CW_Test_Begin(void) {
    Satellite_CW_Prepare();
}

void Satellite_CW_Test_Cycle(uint32_t unit_ms, uint32_t tone_ms) {
    Satellite_CW_CarrierOn();
    CPU2_Delay_Ms(tone_ms);
    Satellite_CW_CarrierOff();
    CPU2_Delay_Ms(400);
    Satellite_CW_SendString("HELLO", unit_ms);
    CPU2_Delay_Ms(1500);                          /* quiet gap before the next round */
}

void Satellite_CW_Test_End(void) {
    Satellite_CW_Finish();
}

/*
 * ADDED: "HELLO" in our standard packets, sent back to back for a spectrum analyzer. Every packet is a normal downlink
 * packet: AX.25 UI frame (callsigns from the config) with the 5-byte payload "HELLO", light FEC wrap, G3RUH scrambled,
 * 4800 bit/s GMSK, 200 bytes. The PA is set up once; the next packet is built while the current one is on the air, so
 * the signal is continuous apart from a few ms between packets. keep_going() is polled before every packet.
 * Returns the number of packets sent.
 */
uint32_t Satellite_Gmsk_Hello_Stream(bool (*keep_going)(void))
{
    static uint8_t hbuf[2][RADIO_FIXED_PACKET_LEN + 64];
    static const uint8_t hello[5] = { 'H', 'E', 'L', 'L', 'O' };
    const uint16_t hello_len = (uint16_t)sizeof(hello);
    uint32_t sent = 0;
    int cur = 0;

    Satellite_SetDCDCHold(true);
    Satellite_GMSK_Prepare();
    if (Protocol_CreatePacket(hbuf[cur], hello, hello_len, &s_config.radio) != 0) {
        while (keep_going()) {
            tx_busy = 1;
            RadioApp_Send(hbuf[cur], RADIO_FIXED_PACKET_LEN);                    /* TX starts now ... */
            if (Protocol_CreatePacket(hbuf[cur ^ 1], hello, hello_len, &s_config.radio) == 0) {
                break;                                                           /* ... next packet built meanwhile */
            }
            uint32_t w0 = Get_Time_Ms();
            while (tx_busy != 0 && (Get_Time_Ms() - w0) < 3000U) { /* TX-done IRQ clears tx_busy */ }
            if (tx_busy != 0) { tx_busy = 0; Radio.Standby(); break; }
            sent++;
            cur ^= 1;
        }
    }
    Satellite_SetDCDCHold(false);
    Satellite_GMSK_AfterTx();
    return sent;
}

void Satellite_CW_Abort_To_Rx(void) {
    s_cw_cfg_valid = false;
    Satellite_CW_CarrierOff();
    HAL_GPIO_WritePin(AMP_5V_EN_PORT, AMP_5V_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN, GPIO_PIN_SET);   /* 3.3V PA back ON */
    Satellite_SetRFSwitch(RBI_SWITCH_RX);
    /* Immediately keep radio armed in RX mode with IRQs enabled to capture full burst */
    RadioApp_StartRx();
    tx_busy = 0;
}

void Satellite_Hold_Continuous_Carrier(uint32_t seconds) {
    uart1_puts("\r\n============================================================\r\n");
    uart1_printf(">>> STARTING CONTINUOUS CARRIER HOLD (%lu SECONDS) <<<\r\n", (unsigned long)seconds);
    uart1_printf(" Frequency : %lu.%03lu MHz\r\n",
                 (unsigned long)(s_config.radio.txFrequency / 1000000UL),
                 (unsigned long)((s_config.radio.txFrequency % 1000000UL) / 1000UL));
    uart1_printf(" Power     : +%d dBm (RFO_%s)\r\n",
                 (s_config.txPowerDbm != 0) ? s_config.txPowerDbm : RADIO_TX_POWER_DBM,
                 (s_config.rfSwitchConfig == RBI_SWITCH_RFO_HP) ? "HP" : "LP");
    uart1_puts(" Ideal for spectrum analyzer / oscilloscope RF power probing\r\n");
    uart1_puts("============================================================\r\n");

    Satellite_CW_Prepare();
    Satellite_CW_CarrierOn();

    uint32_t start_ms = Get_Time_Ms();
    uint32_t duration_ms = seconds * 1000;
    while ((Get_Time_Ms() - start_ms) < duration_ms) {
        uint32_t elapsed_s = (Get_Time_Ms() - start_ms) / 1000;
        uart1_printf(" [CARRIER ACTIVE] Elapsed: %lu s / %lu s\r\n",
                     (unsigned long)elapsed_s, (unsigned long)seconds);
        CPU2_Delay_Ms(1000);
    }

    Satellite_CW_CarrierOff();
    Satellite_CW_Finish();
    uart1_puts(">>> CONTINUOUS CARRIER HOLD COMPLETE <<<\r\n\r\n");
}

void Satellite_Hold_Continuous_Carrier_5V(uint32_t seconds) {
    uart1_puts("\r\n============================================================\r\n");
    uart1_printf(">>> STARTING 5V PA CONTINUOUS CARRIER HOLD (%lu SECONDS) <<<\r\n", (unsigned long)seconds);
    uart1_printf(" Frequency : %lu.%03lu MHz\r\n",
                 (unsigned long)(s_config.radio.txFrequency / 1000000UL),
                 (unsigned long)((s_config.radio.txFrequency % 1000000UL) / 1000UL));
    uart1_printf(" Power     : +%d dBm (5V External PA PC3 / SI2, Boost PA0 ON)\r\n",
                 (s_config.txPowerDbm != 0) ? s_config.txPowerDbm : RADIO_TX_POWER_DBM);
    uart1_puts(" Pure unmodulated CW carrier for Spectrum Analyzer / Power Meter\r\n");
    uart1_puts(" Eliminates GMSK modulation spreading to verify true 27 dBm saturation\r\n");
    uart1_puts("============================================================\r\n");

    int8_t power = (s_config.txPowerDbm != 0) ? s_config.txPowerDbm : RADIO_TX_POWER_DBM;
    if (power > 14) power = 14;
    uint8_t pa_sel = SAT_PA_SEL;
    Radio.Standby();

    /* 1. Enable 5V DC/DC Converter (PA0 = 1); PC2 is stopped after PC3 in step 2 */
    RBI_Enable5VDCDC(1);
    CPU2_Delay_Ms(10); /* 10 ms soft-start settle delay for 5V boost capacitor charging */

    /* 3. Configure RF switch and assert 5V External PA (PC3 = 1) */
    RBI_SetTxSwitchConfig(RBI_SWITCH_RFO_LP5V);
    Satellite_SetRFSwitch(RBI_SWITCH_RFO_LP5V);
    CPU2_Delay_Ms(2);  /* 2 ms PA bias settle delay */

    SUBGRF_SetStandby(STDBY_RC);
    SUBGRF_SetDioIrqParams(IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE);
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);
    SUBGRF_SetPacketType(PACKET_TYPE_GFSK);
    SUBGRF_SetRfFrequency(s_config.radio.txFrequency);
    SAT_SET_PA_CONFIG(); /* CHANGED: was RFO_LP-only, now per FRONTEND (Mission/config.h) */
    SUBGRF_SetTxParams(pa_sel, power, RADIO_RAMP_40_US);
    SUBGRF_WriteRegister(REG_OCP, 0x38);
    SUBGRF_WriteRegister(REG_DRV_CTRL, 0x7 << 1);
    SUBGRF_SetSwitch(pa_sel, RFSWITCH_TX);

    /* Turn on pure continuous carrier */
    SUBGRF_SetTxContinuousWave();

    uint32_t start_ms = Get_Time_Ms();
    uint32_t duration_ms = seconds * 1000;
    while ((Get_Time_Ms() - start_ms) < duration_ms) {
        uint32_t elapsed_s = (Get_Time_Ms() - start_ms) / 1000;
        uart1_printf(" [5V CARRIER ACTIVE] Elapsed: %lu s / %lu s\r\n",
                     (unsigned long)elapsed_s, (unsigned long)seconds);
        CPU2_Delay_Ms(1000);
    }

    /* Cut carrier and put radio in standby */
    SUBGRF_SetStandby(STDBY_RC);
    Radio.Standby();

    /* Power down 5V PA PC3, 5V DC/DC PA0, restore 3.3V PA (always enabled) and RF switch to RX */
    HAL_GPIO_WritePin(AMP_5V_EN_PORT, AMP_5V_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN, GPIO_PIN_SET);   /* 3.3V PA back ON */
    Satellite_SetRFSwitch(RBI_SWITCH_RX);
    SUBGRF_SetDioIrqParams(IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE);
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);

    uart1_puts(">>> 5V CONTINUOUS CARRIER HOLD COMPLETE <<<\r\n\r\n");
}

void Satellite_CW_SendChar(char c, uint32_t unit_ms) {
    if (c == ' ') {
        CPU2_Delay_Ms(unit_ms * 4);
        return;
    }

    const char *morse = Get_Morse_Code(c);
    if (!morse) return;

    for (int i = 0; morse[i] != '\0'; i++) {
        Satellite_CW_CarrierOn();
        if (morse[i] == '.') {
            CPU2_Delay_Ms(unit_ms);
        } else if (morse[i] == '-') {
            CPU2_Delay_Ms(unit_ms * 3);
        }
        Satellite_CW_CarrierOff();
        CPU2_Delay_Ms(unit_ms); /* Intra-character element gap */
    }

    CPU2_Delay_Ms(unit_ms * 2); /* Inter-character gap */
}

void Satellite_CW_SendString(const char *str, uint32_t unit_ms) {
    if (!str) return;
    while (*str) {
        Satellite_CW_SendChar(*str, unit_ms);
        str++;
    }
}

/*
 * ============================================================================
 * PUBLIC TIMING & CONFIGURATION APIS
 * ============================================================================
 */

void Satellite_DelayMs(uint32_t ms) {
    CPU2_Delay_Ms(ms);
}

uint32_t Satellite_GetTimeMs(void) {
    return Get_Time_Ms();
}

void Satellite_SetConfig(const SatelliteConfig_t *config) {
    if (config != NULL) {
        memcpy(&s_config, config, sizeof(SatelliteConfig_t));
    } else {
        memcpy(&s_config, &SatelliteDefaultConfig, sizeof(SatelliteConfig_t));
    }
    int8_t power = (s_config.txPowerDbm != 0) ? s_config.txPowerDbm : RADIO_TX_POWER_DBM;
    if (power > 14) power = 14;
    uint8_t pa_sel = SAT_PA_SEL;
    RadioApp_SetTxPower(power);
    RadioApp_SetPaSelect(pa_sel);
}

const SatelliteConfig_t* Satellite_GetConfig(void) {
    return &s_config;
}

/*
 * ============================================================================
 * LOW-LEVEL HARDWARE INITIALIZATION (CPU2 / CORTEX-M0+ DOMAIN)
 * ============================================================================
 */

/**
 * @brief  Point Cortex-M0+ Vector Table Offset Register (VTOR) to CPU2 Flash base.
 * @note   CPU2 firmware binary is located at 0x08032000 in Dual-Core Flash layout.
 */
void Satellite_Init_VectorTable(void) {
    SCB->VTOR = SATELLITE_CPU2_VECTOR_TABLE_ADDR;
    __DSB();
    __ISB();
}

/**
 * @brief  Enable all required peripheral bus clocks in the CPU2 (Cortex-M0+) domain.
 *         Uses standard CMSIS RCC register definitions:
 *         - RCC->C2AHB2ENR: GPIO Ports A, B, C, and H
 *         - RCC->C2AHB3ENR: IPCC, Flash interface, HSEM
 *         - RCC->C2APB2ENR: USART1 debug console
 *         - RCC->C2APB3ENR: SUBGHZSPI radio interface
 */
void Satellite_Init_PeripheralClocks(void) {
    /* 1. Enable GPIOA, GPIOB, GPIOC, GPIOH clocks in CPU2 domain (AHB2: 0x5800014C |= 0x87) */
    RCC->C2AHB2ENR |= (RCC_C2AHB2ENR_GPIOAEN |
                       RCC_C2AHB2ENR_GPIOBEN |
                       RCC_C2AHB2ENR_GPIOCEN |
                       RCC_C2AHB2ENR_GPIOHEN);

    /* 2. Enable IPCC, Flash interface, and HSEM clocks in CPU2 domain (AHB3: 0x58000150 |= (1<<0) | (1<<25)) */
    (*(volatile uint32_t *)0x58000150UL) |= (1UL << 0) | (1UL << 25);
    RCC->C2AHB3ENR |= (RCC_C2AHB3ENR_IPCCEN  |
                       RCC_C2AHB3ENR_FLASHEN |
                       RCC_C2AHB3ENR_HSEMEN);

    /* 3. Enable USART1 bus clock in CPU2 domain (APB2: 0x58000160 |= (1<<14)) */
    RCC->C2APB2ENR |= RCC_C2APB2ENR_USART1EN;

    /* 4. Enable SUBGHZSPI radio SPI interface bus clock in CPU2 domain (APB3) */
    RCC->C2APB3ENR |= RCC_C2APB3ENR_SUBGHZSPIEN;
}

/**
 * @brief  Disable and mask IPCC (Inter-Processor Communication Controller) interrupts
 *         in CPU2 domain to prevent unhandled mailbox IRQs from trapping the CPU2 core.
 */
void Satellite_Init_IPCC_Isolation(void) {
    /* Clear CPU2 control register: disable RX occupied and TX free interrupt generation (0x58000C10) */
    IPCC->C2CR = 0x00000000;

    /* Mask all 6 bidirectional mailbox channels for CPU2 (0x58000C14) */
    IPCC->C2MR = 0xFFFFFFFF;

    /* Disable IRQ1 (vector position 1 in startup_cm0p.s: IPCC_C2_RX_C2_TX) and CMSIS IRQ 18 */
    NVIC_DisableIRQ((IRQn_Type)1);
    NVIC_DisableIRQ(IPCC_C2_RX_C2_TX_IRQn);
}

/**
 * @brief  Complete low-level CPU2 hardware initialization sequence:
 *         1. Relocates vector table to CPU2 flash base
 *         2. Enables peripheral bus clocks in CPU2 domain
 *         3. Isolates IPCC inter-core interrupts
 */
void Satellite_Hardware_Init(void) {
    Satellite_Init_VectorTable();
    Satellite_Init_PeripheralClocks();
    Satellite_Init_IPCC_Isolation();
}

void Satellite_Init(const SatelliteConfig_t *config) {
    Satellite_SetConfig(config);

    if (s_config.rfSwitchConfig == 0) {
        s_config.rfSwitchConfig = SAT_CFG_DEFAULT_RF_SWITCH;
    }

    uint32_t uart_delay = (s_config.uartSettleDelayMs > 0) ? s_config.uartSettleDelayMs : 50;
    int8_t   tx_power   = (s_config.txPowerDbm != 0) ? s_config.txPowerDbm : RADIO_TX_POWER_DBM;
    uint32_t bitrate    = (s_config.bitrateBps > 0) ? s_config.bitrateBps : RADIO_BIT_RATE_BPS;
    uint32_t fdev       = (s_config.fdevHz > 0) ? s_config.fdevHz : RADIO_FDEV_HZ;

    /* 1. Ensure low-level hardware, vector table, and peripheral clocks are initialized */
    Satellite_Hardware_Init();

    /* 2. Initialize USART1 immediately so console is alive */
    uart1_init();
    uart1_puts("\r\n[CPU2] Cortex-M0+ Active at 0x08032000\r\n");

    /* 3. Core architecture & peripheral clock init */
    SystemInit();
    LL_Init_CPU2();   /* CHANGED: was HAL_Init(); LL version only sets up flash/prefetch + the tick (core/ll_port.c) */
    SysTick_Init_CPU2(HAL_RCC_GetHCLK2Freq());

    CPU2_Delay_Ms(uart_delay); /* Allow UART line voltage to settle */

    /* 7. Structured M0+ Initialization Logs */
    uart1_puts("\r\n\r\n");
    uart1_puts("======================================================================\r\n");
    uart1_puts("     STM32WL55 CORTEX-M0+ SATELLITE RADIO SYSTEM INITIALIZING         \r\n");
    uart1_puts("======================================================================\r\n");
    uart1_puts("[INIT] 1. SystemCoreClock & HAL Base Initialized .......... [OK]\r\n");

    /* Read and display hardware reset reason from RCC->CSR */
    uint32_t csr = RCC->CSR;
    uart1_printf("       - Hardware Reset Cause (RCC->CSR=0x%08lX): ", (unsigned long)csr);
    if (csr & RCC_CSR_LPWRRSTF) uart1_puts("LowPower ");
    if (csr & RCC_CSR_WWDGRSTF) uart1_puts("WWDG ");
    if (csr & RCC_CSR_IWDGRSTF) uart1_puts("IWDG ");
    if (csr & RCC_CSR_SFTRSTF)  uart1_puts("Software ");
    if (csr & RCC_CSR_BORRSTF)  uart1_puts("BOR(BrownOut) ");
    if (csr & RCC_CSR_PINRSTF)  uart1_puts("PIN(NRST/STLink) ");
    if (csr & RCC_CSR_OBLRSTF)  uart1_puts("OBL ");
    uart1_puts("\r\n");
    RCC->CSR |= RCC_CSR_RMVF; /* Clear reset flags for clean subsequent boot diagnostic */

    uart1_printf("[INIT] 2. CPU2 SysTick Timer Started (%lu MHz) .............. [OK]\r\n",
                 (unsigned long)(HAL_RCC_GetHCLK2Freq() / 1000000UL));
    uart1_puts("[INIT] 3. USART1 Console Initialized (PA9 TX / PA10 RX @ 115200) .. [OK]\r\n");

    RBI_Init();
    Satellite_SetRFSwitch(RBI_SWITCH_OFF);
    /* External PAs initially OFF to minimize idle current */
    HAL_GPIO_WritePin(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(AMP_5V_EN_PORT, AMP_5V_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN, GPIO_PIN_RESET);
    uart1_puts("[INIT] 4. RF Dual-PA & 5V DC/DC Configured (JC1 Low Power Mode) ... [OK]\r\n");
    uart1_puts("       - CW Mode   : 3.3V External PA (PC2/SO2)  | 5V DC/DC (PA0): OFF\r\n");
    uart1_puts("       - GMSK Mode : 5V External PA (PC3/SI2)    | 5V DC/DC (PA0): ENABLED FOR TX\r\n");
    uart1_puts("       - GMSK TX   : 128-bit preamble + sync word 93 0B 51 DE + 200 B (RFO_LP, 5V PA duty 0x03)\r\n");

    MX_SUBGHZ_Init();
    /* CHANGED: HAL_NVIC_* -> CMSIS NVIC_* (same registers, no HAL cortex module linked) */
    NVIC_SetPriority(SUBGHZ_Radio_IRQn, 0);
    NVIC_EnableIRQ(SUBGHZ_Radio_IRQn);
    __enable_irq();
    uart1_puts("[INIT] 5. Sub-GHz Radio (SX1262) & NVIC IRQs Enabled ...... [OK]\r\n");

    static RadioEvents_t s_sat_radio_events = {
        .TxDone = OnTxDone,
        .TxTimeout = OnTxTimeout,
        .RxDone = OnRxDone,
        .RxTimeout = OnRxTimeout,
        .RxError = OnRxError
    };

    RadioApp_Init(&s_config.radio, &s_sat_radio_events);
    RadioApp_SetTxPower(tx_power);
    RadioApp_SetPaSelect(SAT_PA_SEL); /* LP on the custom board, HP on a Nucleo (was a fixed RFO_LP) */
    uart1_puts("[INIT] 6. Radio Application Layer Configured:              [OK]\r\n");
    uart1_printf("       - Downlink Frequency : %lu.%03lu MHz\r\n",
                 (unsigned long)(s_config.radio.txFrequency / 1000000UL),
                 (unsigned long)((s_config.radio.txFrequency % 1000000UL) / 1000UL));
    uart1_printf("       - Transmit RF Power  : +%d dBm (RFO_LP)\r\n", tx_power);
    uart1_puts("       - RF Front-End Switch: RBI_SWITCH_RFO_LP (Low Power STM32WL55JC1)\r\n");
    uart1_printf("       - GMSK Bitrate       : %lu bps (Fdev: %lu Hz, BT: 0.5)\r\n",
                 (unsigned long)bitrate, (unsigned long)fdev);
    uart1_printf("       - Callsign Profile   : %s-%d -> %s-%d\r\n",
                 s_config.radio.sourceCallsign, s_config.radio.sourceSSID,
                 s_config.radio.destCallsign, s_config.radio.destSSID);
    uart1_puts("       - Supported Modes    : 1) CW Morse Beacon (Keying)\r\n");
    uart1_puts("                              2) GMSK AX.25 UI Frame + G3RUH\r\n");
    uart1_puts("======================================================================\r\n");
    uart1_puts("[INIT COMPLETE] Satellite Application Ready.\r\n");
    /* Run the self-test OUTSIDE the print call: with LOG=0 uart1_printf() is a macro that expands to
     * ((void)0), so its arguments are never evaluated. Calling Seal_SelfTest() inside the argument
     * list meant the test (and the g_sealSelfTest result word) silently vanished from LOG=0 builds. */
    bool seal_ok = Seal_SelfTest();
    uart1_printf("[SEAL] FEC core self-test: %s (AUTH=%d)\r\n", seal_ok ? "PASS" : "FAIL", (int)AUTH_ENABLE);
    (void)seal_ok;
    uart1_puts("======================================================================\r\n\r\n");
}

/*
 * ============================================================================
 * CW BEACON EXECUTION
 * ============================================================================
 */

/*
 * ============================================================================
 * SINGLE COMPLETE CW BEACON EXECUTION
 * Transmits complete CW beacon from start to finish without interruption.
 * ============================================================================
 */

bool Satellite_Send_CW_Beacon(uint32_t cycle,
                              const char *name,
                              const char *callsign,
                              const char *msg,
                              uint32_t unit_ms,
                              uint32_t carrier_tone_ms,
                              uint32_t carrier_post_delay_ms,
                              uint32_t inter_str_delay_ms) {
    if (!callsign) callsign = "S2S2 9N2SJ";
    if (!msg) msg = "";
    if (unit_ms == 0) unit_ms = 80;

    int8_t power = (s_config.txPowerDbm != 0) ? s_config.txPowerDbm : RADIO_TX_POWER_DBM;

    uart1_puts("\r\n============================================================\r\n");
    uart1_printf(">>> [CYCLE #%lu: TRANSMITTING COMPLETE %s BEACON] <<<\r\n",
                 (unsigned long)cycle, name ? name : "CW");
    uart1_printf(" Downlink Frequency : %lu.%03lu MHz | Power: +%d dBm\r\n",
                 (unsigned long)(s_config.radio.txFrequency / 1000000UL),
                 (unsigned long)((s_config.radio.txFrequency % 1000000UL) / 1000UL),
                 power);
    uart1_printf(" RF Switch          : %s (3.3V External PA PC2 / SO2)\r\n",
                 (s_config.rfSwitchConfig == RBI_SWITCH_RFO_HP) ? "RBI_SWITCH_RFO_HP" : "RBI_SWITCH_RFO_LP");
    uart1_puts(" 5V DC/DC (PA0)     : OFF\r\n");
    uart1_puts(" Modulation         : Continuous Wave (CW / Carrier Keying)\r\n");
    uart1_printf(" Morse Unit         : %lu ms\r\n", (unsigned long)unit_ms);
    uart1_printf(" Callsign           : \"%s\"\r\n", callsign);
    uart1_printf(" Payload Message    : \"%s\"\r\n", msg);
    uart1_puts("============================================================\r\n");

    Satellite_CW_Prepare();

    /* Optional pre-beacon tuning carrier tone */
    if (carrier_tone_ms > 0) {
        uart1_printf("[CW] Emitting %lu ms continuous carrier tone...\r\n", (unsigned long)carrier_tone_ms);
        Satellite_CW_CarrierOn();
        CPU2_Delay_Ms(carrier_tone_ms);
        Satellite_CW_CarrierOff();
        if (carrier_post_delay_ms > 0) {
            CPU2_Delay_Ms(carrier_post_delay_ms);
        }
    }

    /* 1. Transmit Callsign completely */
    uart1_printf("[CW] Keying Callsign \"%s\"...\r\n", callsign);
    Satellite_CW_SendString(callsign, unit_ms);

    /* 2. Gap between Callsign and Payload Message */
    if (inter_str_delay_ms > 0) {
        CPU2_Delay_Ms(inter_str_delay_ms);
    }

    /* 3. Transmit Payload Message completely */
    uart1_printf("[CW] Keying Payload \"%s\"...\r\n", msg);
    Satellite_CW_SendString(msg, unit_ms);

    /* 4. Complete CW transmission and power down PA */
    Satellite_CW_Finish();

    uart1_printf(">>> [CW %s TRANSMISSION COMPLETE] <<<\r\n", name ? name : "");
    uart1_puts("------------------------------------------------------------\r\n\r\n");
    return true;
}

bool Satellite_Run_CW_Session_Ex(uint32_t cycle,
                                 const char *callsign,
                                 const char *msg,
                                 uint32_t unit_ms,
                                 uint32_t duration_ms,
                                 uint32_t carrier_tone_ms,
                                 uint32_t carrier_post_delay_ms,
                                 uint32_t inter_str_delay_ms,
                                 uint32_t repeat_delay_ms) {
    if (duration_ms == 0) {
        return Satellite_Send_CW_Beacon(cycle, "CW", callsign, msg, unit_ms,
                                        carrier_tone_ms, carrier_post_delay_ms, inter_str_delay_ms);
    }

    if (!callsign) callsign = "S2S2 9N2SJ";
    if (unit_ms == 0) unit_ms = 80;

    int8_t power = (s_config.txPowerDbm != 0) ? s_config.txPowerDbm : RADIO_TX_POWER_DBM;

    uart1_puts("\r\n============================================================\r\n");
    uart1_printf(">>> [CYCLE #%lu: STARTING CW MORSE SESSION] <<<\r\n", (unsigned long)cycle);
    uart1_printf(" Downlink Frequency : %lu.%03lu MHz | Power: +%d dBm\r\n",
                 (unsigned long)(s_config.radio.txFrequency / 1000000UL),
                 (unsigned long)((s_config.radio.txFrequency % 1000000UL) / 1000UL),
                 power);
    uart1_printf(" RF Switch          : %s (3.3V External PA PC2 / SO2)\r\n",
                 (s_config.rfSwitchConfig == RBI_SWITCH_RFO_HP) ? "RBI_SWITCH_RFO_HP" : "RBI_SWITCH_RFO_LP");
    uart1_puts(" 5V DC/DC (PA0)     : OFF\r\n");
    uart1_puts(" Modulation         : Continuous Wave (CW / Carrier Keying)\r\n");
    uart1_printf(" Morse Unit         : %lu ms\r\n", (unsigned long)unit_ms);
    uart1_printf(" Session Duration   : %lu Seconds\r\n", (unsigned long)(duration_ms / 1000));
    uart1_puts("============================================================\r\n");

    Satellite_CW_Prepare();
    uint32_t cw_start_time = Get_Time_Ms();

    /* Optional pre-beacon tuning carrier tone */
    if (carrier_tone_ms > 0) {
        uart1_printf("[CW] Emitting %lu ms continuous carrier tone...\r\n", (unsigned long)carrier_tone_ms);
        Satellite_CW_CarrierOn();
        CPU2_Delay_Ms(carrier_tone_ms);
        Satellite_CW_CarrierOff();
        if (carrier_post_delay_ms > 0) {
            CPU2_Delay_Ms(carrier_post_delay_ms);
        }
    }

    uint32_t morse_iter = 0;
    while ((Get_Time_Ms() - cw_start_time) < duration_ms) {
        morse_iter++;
        uint32_t elapsed_s = (Get_Time_Ms() - cw_start_time) / 1000;
        uart1_printf("[CW #%lu] Keying \"%s\" (Elapsed: %lu s / %lu s)\r\n",
                     (unsigned long)morse_iter, callsign, (unsigned long)elapsed_s, (unsigned long)(duration_ms / 1000));

        Satellite_CW_SendString(callsign, unit_ms);

        if (inter_str_delay_ms > 0) {
            CPU2_Delay_Ms(inter_str_delay_ms);
        }

        if ((Get_Time_Ms() - cw_start_time) >= duration_ms) break;

        uart1_printf("[CW] Keying \"%s\"\r\n", msg);
        Satellite_CW_SendString(msg, unit_ms);

        if (repeat_delay_ms > 0) {
            CPU2_Delay_Ms(repeat_delay_ms);
        }
    }

    Satellite_CW_Finish();

    uart1_printf(">>> CW TRANSMISSION COMPLETE (Cycle #%lu, %lu beacon iterations) <<<\r\n",
                 (unsigned long)cycle, (unsigned long)morse_iter);
    uart1_puts("------------------------------------------------------------\r\n\r\n");
    return true;
}

/*
 * ============================================================================
 * GMSK PACKET & BURST TRANSMISSION
 * ============================================================================
 */

void Satellite_GMSK_Prepare(void) {
    int8_t power = (s_config.txPowerDbm != 0) ? s_config.txPowerDbm : RADIO_TX_POWER_DBM;
    if (power > 14) power = 14;
    uint8_t pa_sel = SAT_PA_SEL;

    const bool pa5v_already_on =
        (HAL_GPIO_ReadPin(AMP_5V_EN_PORT, AMP_5V_EN_PIN) == GPIO_PIN_SET) &&
        (HAL_GPIO_ReadPin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN) == GPIO_PIN_SET);

    /* Already biased for this downlink session: do not re-toggle switch/PA. */
    if (pa5v_already_on) {
        RBI_SetTxSwitchConfig(RBI_SWITCH_RFO_LP5V);
        RadioApp_SetPaSelect(pa_sel);
        return;
    }

    /* Keep TCXO in STDBY_XOSC. Radio.Standby() drops to STDBY_RC and brown-outs 5V PA TX. */
    SUBGRF_SetStandby(STDBY_XOSC);

    HAL_GPIO_WritePin(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN, GPIO_PIN_RESET);
    CPU2_Delay_Ms(SAT_PA_PREP_MS);   /* CHANGED: was 5 ms; macro is 0 on a Nucleo (no external PAs to sequence) */

    RBI_Enable5VDCDC(1);
    CPU2_Delay_Ms(SAT_DCDC_SETTLE_MS);   /* CHANGED: was 100 ms boost-converter settle; 0 on a Nucleo (RBI_Enable5VDCDC is a no-op there) */

    RBI_SetTxSwitchConfig(RBI_SWITCH_RFO_LP5V);
    Satellite_SetRFSwitch(RBI_SWITCH_RFO_LP5V);
    CPU2_Delay_Ms(SAT_SWITCH_SETTLE_MS);   /* CHANGED: was 40 ms; see Mission/config.h */

    SUBGRF_SetPacketType(PACKET_TYPE_GFSK);
    RadioApp_SetTxPower(10); /* keep 10 dBm for whole 5V PA session (14 dBm brown-outs) */
    RadioApp_SetPaSelect(pa_sel);
}

bool Satellite_Send_Packet_Timeout(const uint8_t *payload, uint16_t len, uint32_t timeout_ms) {
    if (!payload || len == 0) return false;
    if (timeout_ms == 0) timeout_ms = (s_config.txTimeoutMs > 0) ? s_config.txTimeoutMs : 3000;

    /* First enable 5V DC/DC (PA0) and 5V PA (PC3/SI2) before GMSK transmission */
    Satellite_GMSK_Prepare();

    uint16_t frame_size = Protocol_CreatePacket(s_burst_scrambled, payload, len, &s_config.radio);
    if (frame_size == 0) {
        uart1_puts("ERROR: Failed to assemble packet!\r\n");
        Satellite_GMSK_AfterTx();
        return false;
    }

    memcpy(last_tx_scrambled, s_burst_scrambled, frame_size);
    last_tx_len = frame_size;

    bool res = Radio_Send_And_Wait(s_burst_scrambled, frame_size, timeout_ms);
    Satellite_GMSK_AfterTx();
    /* ADDED: pause after every packet. The GS prints a full decode report per frame (UART at 115200)
     * and drops a packet that arrives while it is still printing; 300 ms made 183-chunk runs lossless. */
    CPU2_Delay_Ms(SAT_TX_GAP_MS);
    return res;
}

bool Satellite_Send_Packet(const uint8_t *payload, uint16_t len) {
    return Satellite_Send_Packet_Timeout(payload, len, s_config.txTimeoutMs);
}

bool Satellite_Send_AX25_String_Timeout(const char *text, uint32_t timeout_ms) {
    if (!text) return false;
    return Satellite_Send_Packet_Timeout((const uint8_t *)text, (uint16_t)strlen(text), timeout_ms);
}

bool Satellite_Send_AX25_String(const char *text) {
    return Satellite_Send_AX25_String_Timeout(text, s_config.txTimeoutMs);
}

void Satellite_Run_GMSK_Burst_Session_Ex(uint32_t cycle,
                                        const void *payload,
                                        uint32_t duration_ms,
                                        uint32_t interval_ms,
                                        uint32_t timeout_ms) {
    const uint8_t *raw_bytes = (const uint8_t *)payload;
    const uint8_t *beacon_payload = NULL;
    uint16_t beacon_len = 0;
    bool is_beacon = false;
    const char *beacon_name = "Beacon";
    char safe_base_text[64];

    if (raw_bytes != NULL) {
        /* Check for B1 footer 0xAA55 at offset 32/33 */
        uint16_t f1 = (uint16_t)(raw_bytes[32] | (raw_bytes[33] << 8));
        uint16_t f1_be = (uint16_t)((raw_bytes[32] << 8) | raw_bytes[33]);
        /* Check for B2 footer 0xBB66 at offset 36/37 */
        uint16_t f2 = (uint16_t)(raw_bytes[36] | (raw_bytes[37] << 8));
        uint16_t f2_be = (uint16_t)((raw_bytes[36] << 8) | raw_bytes[37]);

        if (f1 == 0xAA55 || f1_be == 0xAA55) {
            is_beacon = true;
            beacon_payload = raw_bytes;
            beacon_len = 34;
            beacon_name = "Beacon 1 (HK1: 34B)";
        } else if (f2 == 0xBB66 || f2_be == 0xBB66) {
            is_beacon = true;
            beacon_payload = raw_bytes;
            beacon_len = 38;
            beacon_name = "Beacon 2 (HK2: 38B)";
        }
    }

    if (!is_beacon) {
        const char *payload_text = (const char *)payload;
        if (payload_text != NULL && payload_text[0] >= 0x20 && (uint8_t)payload_text[0] < 0x7F) {
            strncpy(safe_base_text, payload_text, sizeof(safe_base_text) - 1);
            safe_base_text[sizeof(safe_base_text) - 1] = '\0';
        } else {
            strncpy(safe_base_text, SAT_CFG_DEFAULT_GMSK_PAYLOAD, sizeof(safe_base_text) - 1);
            safe_base_text[sizeof(safe_base_text) - 1] = '\0';
        }
    }

    if (duration_ms == 0) duration_ms = 60000;
    if (interval_ms == 0) interval_ms = 300;
    if (timeout_ms == 0) timeout_ms = (s_config.txTimeoutMs > 0) ? s_config.txTimeoutMs : 3000;

    int8_t power = (s_config.txPowerDbm != 0) ? s_config.txPowerDbm : RADIO_TX_POWER_DBM;
    uint32_t bitrate = (s_config.bitrateBps > 0) ? s_config.bitrateBps : RADIO_BIT_RATE_BPS;

    uart1_puts("============================================================\r\n");
    uart1_printf(">>> [CYCLE #%lu: STARTING GMSK BURST SESSION] <<<\r\n", (unsigned long)cycle);
    uart1_printf(" Downlink Frequency : %lu.%03lu MHz | Power: +%d dBm\r\n",
                 (unsigned long)(s_config.radio.txFrequency / 1000000UL),
                 (unsigned long)((s_config.radio.txFrequency % 1000000UL) / 1000UL),
                 power);
    if (s_config.rfSwitchConfig5V == RBI_SWITCH_RFO_LP5V) {
        uart1_printf(" RF Switch          : %s (5V External PA PC3 / SI2)\r\n", "RBI_SWITCH_RFO_LP5V");
        uart1_puts(" 5V DC/DC (PA0)     : ENABLED (Powering 5V External PA)\r\n");
    } else {
        uart1_printf(" RF Switch          : %s (3.3V External PA PC2 / SO2)\r\n", "RBI_SWITCH_RFO_LP");
        uart1_puts(" 5V DC/DC (PA0)     : OFF (Safe Bench Mode)\r\n");
    }
    uart1_puts(" Protocol           : AX.25 UI Frame + G3RUH Scrambler\r\n");
    uart1_printf(" Bitrate            : %lu bps GMSK\r\n", (unsigned long)bitrate);
    uart1_printf(" Session Duration   : %lu Seconds continuous burst\r\n", (unsigned long)(duration_ms / 1000));
    uart1_printf(" Inter-Packet Delay : %lu ms | TX Timeout: %lu ms\r\n", (unsigned long)interval_ms, (unsigned long)timeout_ms);
    uart1_printf(" Destination        : %s-%d | Source: %s-%d\r\n",
                 s_config.radio.destCallsign, s_config.radio.destSSID,
                 s_config.radio.sourceCallsign, s_config.radio.sourceSSID);
    if (is_beacon) {
        uart1_printf(" Payload Type       : Live %s (%u Bytes)\r\n", beacon_name, beacon_len);
    }
    uart1_puts("============================================================\r\n");

    /* First enable 5V DC/DC (PA0) and 5V PA (PC3) before GMSK burst session starts */
    Satellite_SetDCDCHold(true);
    Satellite_GMSK_Prepare();

    uint16_t raw_len = 0;

    if (is_beacon) {
        AX25_BuildFrame(s_raw_frame, sizeof(s_raw_frame), &raw_len,
                        s_config.radio.destCallsign, s_config.radio.destSSID,
                        s_config.radio.sourceCallsign, s_config.radio.sourceSSID,
                        beacon_payload, beacon_len);
        if (raw_len > 0) {
            Print_AX25_Fields("GMSK LIVE BEACON AX.25 TEMPLATE", s_raw_frame, raw_len);
        }
    } else {
        char template_payload[96];
        snprintf(template_payload, sizeof(template_payload), "%s #%lu", safe_base_text, (unsigned long)(s_total_packets_sent + 1));
        AX25_BuildFrame(s_raw_frame, sizeof(s_raw_frame), &raw_len,
                        s_config.radio.destCallsign, s_config.radio.destSSID,
                        s_config.radio.sourceCallsign, s_config.radio.sourceSSID,
                        (const uint8_t *)template_payload, (uint16_t)strlen(template_payload));
        if (raw_len > 0) {
            Print_AX25_Fields("GMSK BURST AX.25 PACKET TEMPLATE", s_raw_frame, raw_len);
        }
    }

    /* Pre-burst unmodulated 5V tuning carrier tone for spectrum analyzer power calibration */
    if (s_config.gmskTuningCarrierDurationMs > 0) {
        uart1_printf("[GMSK] Emitting %lu ms 5V tuning carrier tone for power calibration...\r\n",
                     (unsigned long)s_config.gmskTuningCarrierDurationMs);
        SUBGRF_SetTxContinuousWave();
        CPU2_Delay_Ms(s_config.gmskTuningCarrierDurationMs);
        SUBGRF_SetStandby(STDBY_XOSC);
        if (s_config.gmskTuningPostDelayMs > 0) {
            CPU2_Delay_Ms(s_config.gmskTuningPostDelayMs);
        }
    }

    uart1_printf("[GMSK] Starting burst session (%lu s, interval: %lu ms)...\r\n",
                 (unsigned long)(duration_ms / 1000), (unsigned long)interval_ms);

    uint32_t burst_start_time = Get_Time_Ms();
    uint32_t sent_ok = 0;
    uint32_t sent_timeout = 0;
    uint32_t pkt_num = 0;

    while (1) {
        uint32_t cur_time = Get_Time_Ms();
        uint32_t elapsed_ms = (cur_time >= burst_start_time) ? (cur_time - burst_start_time) : 0;
        if (elapsed_ms >= duration_ms) {
            break;
        }

        pkt_num++;
        s_total_packets_sent++;

        uint16_t frame_size = 0;
        char dynamic_payload[96];

        if (is_beacon) {
            frame_size = Protocol_CreatePacket(s_burst_scrambled,
                                                beacon_payload,
                                                beacon_len,
                                                &s_config.radio);
        } else {
            /* Dynamically format payload with increasing packet counter */
            snprintf(dynamic_payload, sizeof(dynamic_payload), "%s #%lu",
                     safe_base_text, (unsigned long)s_total_packets_sent);
            frame_size = Protocol_CreatePacket(s_burst_scrambled,
                                                (const uint8_t *)dynamic_payload,
                                                (uint16_t)strlen(dynamic_payload),
                                                &s_config.radio);
        }

        if (frame_size == 0) {
            uart1_puts("ERROR: Failed to assemble AX.25 frame!\r\n");
            continue;
        }

        if (pkt_num == 1) {
            uint8_t ax25_unscrambled[AX25_MAX_FRAME_SIZE];
            uint16_t ax25_len = Protocol_GetLastTxAx25Frame(ax25_unscrambled, sizeof(ax25_unscrambled));
            if (ax25_len > 0) {
                uart1_printf("\r\n>>> [AX.25 TX FRAME (Starts with 0x7E, %u Bytes, Unscrambled)] <<<\r\n", ax25_len);
                Print_Hex_Bytes(ax25_unscrambled, ax25_len);
                Print_AX25_Fields("TX AX.25 FRAME", ax25_unscrambled, ax25_len);
            }
        }

        memcpy(last_tx_scrambled, s_burst_scrambled, frame_size);
        last_tx_len = frame_size;

        bool ok = Radio_Send_And_Wait(s_burst_scrambled, frame_size, timeout_ms);
        if (ok) sent_ok++; else sent_timeout++;

        cur_time = Get_Time_Ms();
        elapsed_ms = (cur_time >= burst_start_time) ? (cur_time - burst_start_time) : 0;
        uint32_t elapsed_s = elapsed_ms / 1000;
        if (is_beacon) {
            uart1_printf(" [GMSK TX #%lu (Total: %lu)] %s | %s (Elapsed: %lu s / %lu s)\r\n",
                         (unsigned long)pkt_num, (unsigned long)s_total_packets_sent,
                         ok ? "sent OK" : "TIMEOUT", beacon_name,
                         (unsigned long)elapsed_s, (unsigned long)(duration_ms / 1000));
        } else {
            uart1_printf(" [GMSK TX #%lu (Total: %lu)] %s | \"%s\" (Elapsed: %lu s / %lu s)\r\n",
                         (unsigned long)pkt_num, (unsigned long)s_total_packets_sent,
                         ok ? "sent OK" : "TIMEOUT", dynamic_payload,
                         (unsigned long)elapsed_s, (unsigned long)(duration_ms / 1000));
        }

        CPU2_Delay_Ms(interval_ms);
    }

    /* Guard delay: PA ramp-down, then 5V PA off and 3.3V PA back ON */
    CPU2_Delay_Ms(2);
    SUBGRF_SetStandby(STDBY_XOSC);
    Satellite_GMSK_Stop();
    SUBGRF_SetDioIrqParams(IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE);
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);
    tx_busy = 0;

    uart1_printf(">>> GMSK BURST COMPLETE: %lu sent OK, %lu timeouts (Total Packets: %lu) <<<\r\n",
                 (unsigned long)sent_ok, (unsigned long)sent_timeout, (unsigned long)s_total_packets_sent);
    uart1_puts("============================================================\r\n\r\n");
}

bool Satellite_Run_CW_Session(uint32_t cycle) {
    return Satellite_Run_CW_Session_Ex(cycle,
                                       s_config.cwBeaconCallsign,
                                       s_config.cwBeaconMessage,
                                       s_config.morseUnitMs,
                                       s_config.cwDurationMs,
                                       s_config.cwTuningCarrierDurationMs,
                                       s_config.cwTuningPostDelayMs,
                                       s_config.cwInterStringDelayMs,
                                       s_config.cwRepeatIntervalMs);
}

void Satellite_Run_GMSK_Burst_Session(uint32_t cycle) {
    Satellite_Run_GMSK_Burst_Session_Ex(cycle,
                                       s_config.gmskPayloadText,
                                       s_config.gmskDurationMs,
                                       s_config.gmskPacketIntervalMs,
                                       s_config.txTimeoutMs);
}

void Satellite_Run_Cycle_Ex(uint32_t cycle, uint32_t guard_delay_ms) {
    if (guard_delay_ms == 0) {
        guard_delay_ms = (s_config.cycleGuardDelayMs > 0) ? s_config.cycleGuardDelayMs : 1000;
    }

    /* 1. Continuous CW Morse Beacon Session */
    Satellite_Run_CW_Session(cycle);

    /* Guard interval between CW and Burst sent from parameter */
    CPU2_Delay_Ms(guard_delay_ms);

    /* 2. Continuous GMSK Burst Data Stream */
    Satellite_Run_GMSK_Burst_Session(cycle);

    /* Guard interval between Burst and next CW sent from parameter */
    CPU2_Delay_Ms(guard_delay_ms);
}

void Satellite_Run_Cycle(uint32_t cycle) {
    Satellite_Run_Cycle_Ex(cycle, s_config.cycleGuardDelayMs);
}

void Satellite_Run(void) {
    uint32_t cycle = 0;
    while (1) {
        cycle++;
        Satellite_Run_Cycle(cycle);
    }
}

void IPCC_Handshake(void)
{
  uart1_printf("[M0+] Initializing IPCC Channel 1 Handshake with Cortex-M4...\r\n");
  radio_log_write(RADIO_EVT_INFO,
                   "[M0+] Initializing IPCC Channel 1 Handshake with Cortex-M4...");

  /* Signal M4 that M0+ is active and ready */
  ipcc_m0_send(IPCC_CH_HANDSHAKE);

  uint32_t start_time = Get_Time_Ms();

  while ((Get_Time_Ms() - start_time) < 2000)
    {
      if (ipcc_m0_received(IPCC_CH_HANDSHAKE))
        {
          ipcc_m0_clear(IPCC_CH_HANDSHAKE);
          ipcc_m0_send(IPCC_CH_HANDSHAKE);   /* Acknowledge back to M4 */

          uart1_printf("[M0+ IPC] Handshake Synchronized with M4! Radio Link Active.\r\n");
          radio_log_write(RADIO_EVT_INFO,
                           "[M0+ RF] Handshake Synchronized with M4! Radio Link Active.");
          return;
        }

      CPU2_Delay_Ms(10);
    }

  uart1_printf("[M0+ IPC] Handshake check finished. Continuing to CW beacon cycle.\r\n");
}

/* ============================================================================
 * MISSION HK TELEMETRY & FLASH DOWNLOAD FUNCTIONS
 * ============================================================================ */

#define ABS_VAL(x) ((x) < 0 ? -(x) : (x))

static uint16_t s_hk_seq = 0;

static uint16_t hk_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFFU;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= ((uint16_t)buf[i] << 8);
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x8000U) ? ((crc << 1) ^ 0x1021U) : (crc << 1);
    }
    return crc;
}

bool Satellite_Drain_Telemetry(int16_t b1[5], int16_t b2[5])
{
    struct tx_packet_s pkt;
    bool got_new = false;

    if (ipcc_m0_received(IPCC_CH_HANDSHAKE))
    {
        ipcc_m0_clear(IPCC_CH_HANDSHAKE);
        ipcc_m0_send(IPCC_CH_HANDSHAKE);
        uart1_puts("[M0+ IPC] Handshake acknowledged to M4!\r\n");
    }

    if (ipcc_m0_received(IPCC_CH_BEACON))
    {
        ipcc_m0_clear(IPCC_CH_BEACON);
    }

    while (!rb_tx_empty())
    {
        if (rb_tx_read(&pkt))
        {
            if (pkt.id == TELEM_ID_B1)
            {
                got_new = true;
                b1[0] = pkt.data[0];
                b1[1] = pkt.data[1];
                b1[2] = pkt.data[2];
                b1[3] = pkt.data[3];
                b1[4] = pkt.data[4];
                uart1_printf("[M0+ IPC] Received B1: %d.%02dV %d.%02dV %d.%dC %d.%dC %d.%dC\r\n",
                             b1[0] / 100, (int)ABS_VAL(b1[0] % 100),
                             b1[1] / 100, (int)ABS_VAL(b1[1] % 100),
                             b1[2] / 10,  (int)ABS_VAL(b1[2] % 10),
                             b1[3] / 10,  (int)ABS_VAL(b1[3] % 10),
                             b1[4] / 10,  (int)ABS_VAL(b1[4] % 10));
            }
            else if (pkt.id == TELEM_ID_B2)
            {
                got_new = true;
                b2[0] = pkt.data[0];
                b2[1] = pkt.data[1];
                b2[2] = pkt.data[2];
                b2[3] = pkt.data[3];
                b2[4] = pkt.data[4];
                uart1_printf("[M0+ IPC] Received B2: %d.%02dA %d.%02dA %d.%02dA %d %d\r\n",
                             b2[0] / 100, (int)ABS_VAL(b2[0] % 100),
                             b2[1] / 100, (int)ABS_VAL(b2[1] % 100),
                             b2[2] / 100, (int)ABS_VAL(b2[2] % 100),
                             (int)b2[3],
                             (int)b2[4]);
            }
        }
    }
    return got_new;
}

void Satellite_Send_HK_Combined_Packet(const int16_t b1[5], const int16_t b2[5])
{
    uint8_t  pkt[HK_PKT_LEN]; /* HK_PKT_LEN = 128U */
    memset(pkt, 0, sizeof(pkt));

    /* --- ADC1: 16 Channels (Voltages & Signed Temperatures, Bytes 0..31) --- */
    int16_t *adc1 = (int16_t *)&pkt[0];
    adc1[0]  = (b1[0] != 0) ? b1[0] : (int16_t)385;  /* Bat Voltage  3.85V x100 */
    adc1[1]  = (b1[1] != 0) ? b1[1] : (int16_t)492;  /* Solar Voltage 4.92V x100 */
    adc1[2]  = (int16_t)330;                         /* Bus 3.3V x100 */
    adc1[3]  = (int16_t)490;                         /* SP5 V 4.90V */
    adc1[4]  = (int16_t)488;                         /* SP4 V 4.88V */
    adc1[5]  = (int16_t)491;                         /* SP3 V 4.91V */
    adc1[6]  = (int16_t)493;                         /* SP1 V 4.93V */
    adc1[7]  = (int16_t)490;                         /* SP2 V 4.90V */
    adc1[8]  = b1 ? b1[2] : (int16_t)0;              /* Ant Temp: live reading (0.1 deg C, e.g. 0 = 0.0 C) */
    adc1[9]  = (b1[3] != 0) ? b1[3] : (int16_t)215;  /* Bat Temp +21.5 C (signed) */
    adc1[10] = (b1[4] != 0) ? b1[4] : (int16_t)220;  /* BPB Temp +22.0 C (signed) */
    adc1[11] = (int16_t)250;                         /* Temp 1 */
    adc1[12] = (int16_t)250;                         /* Temp 5 */
    adc1[13] = (int16_t)250;                         /* Temp 4 */
    adc1[14] = (int16_t)250;                         /* Temp 3 */
    adc1[15] = (int16_t)250;                         /* Temp 2 */

    /* Footer 1: 0x55, 0xAA (0xAA55 little-endian) at Bytes 32..33 */
    pkt[32] = 0x55;
    pkt[33] = 0xAA;

    /* --- ADC2: 12 Channels (Currents & Status, Bytes 34..57) --- */
    int16_t *adc2 = (int16_t *)&pkt[34];
    adc2[0]  = (int16_t)45;                          /* Unreg Current 0.45A */
    adc2[1]  = (b2[0] != 0) ? b2[0] : (int16_t)120;  /* 3.3V Rail Current 1.20A */
    adc2[2]  = (b2[1] != 0) ? b2[1] : (int16_t)18;   /* 5V Rail Current 0.18A */
    adc2[3]  = (b2[2] != 0) ? b2[2] : (int16_t)-35;  /* Battery Current -0.35A (signed) */
    adc2[4]  = (int16_t)15;                          /* SP1 Current */
    adc2[5]  = (int16_t)15;                          /* SP2 Current */
    adc2[6]  = (int16_t)15;                          /* SP3 Current */
    adc2[7]  = (int16_t)15;                          /* SP4 Current */
    adc2[8]  = (int16_t)15;                          /* SP5 Current */
    adc2[9]  = (int16_t)85;                          /* Raw Bus Current */
    adc2[10] = (int16_t)1;                           /* Status Flag 1 */
    adc2[11] = (int16_t)0;                           /* Status Flag 2 */

    /* --- IMU: 6 Channels (Gyro & Mag, Bytes 58..69) --- */
    int16_t *imu = (int16_t *)&pkt[58];
    imu[0]  = (b2[3] != 0) ? b2[3] : (int16_t)-12;   /* Gyro X: -12 c-dps (signed) */
    imu[1]  = (b2[4] != 0) ? b2[4] : (int16_t)24;    /* Gyro Y: +24 c-dps (signed) */
    imu[2]  = (int16_t)8;                            /* Gyro Z: +8 c-dps (signed) */
    imu[3]  = (int16_t)320;                          /* Mag X: +320 uT (signed) */
    imu[4]  = (int16_t)110;                          /* Mag Y: +110 uT (signed) */
    imu[5]  = (int16_t)-450;                         /* Mag Z: -450 uT (signed) */

    /* Footer 2: 0x66, 0xBB (0xBB66 little-endian) at Bytes 70..71 */
    pkt[70] = 0x66;
    pkt[71] = 0xBB;

    /* --- Metadata & Extension (Bytes 72..123) --- */
    s_hk_seq++;
    uint32_t now_ms = HAL_GetTick();
    pkt[72] = (uint8_t)(s_hk_seq & 0xFF);
    pkt[73] = (uint8_t)((s_hk_seq >> 8) & 0xFF);
    pkt[74] = (uint8_t)((s_hk_seq >> 16) & 0xFF);
    pkt[75] = (uint8_t)((s_hk_seq >> 24) & 0xFF);

    pkt[76] = (uint8_t)(now_ms & 0xFF);
    pkt[77] = (uint8_t)((now_ms >> 8) & 0xFF);
    pkt[78] = (uint8_t)((now_ms >> 16) & 0xFF);
    pkt[79] = (uint8_t)((now_ms >> 24) & 0xFF);

    uint32_t total_sent = Satellite_GetTotalPacketsSent();
    pkt[80] = (uint8_t)(total_sent & 0xFF);
    pkt[81] = (uint8_t)((total_sent >> 8) & 0xFF);
    pkt[82] = (uint8_t)((total_sent >> 16) & 0xFF);
    pkt[83] = (uint8_t)((total_sent >> 24) & 0xFF);

    /* CRC-16 over bytes 0..123 */
    uint16_t crc = hk_crc16(pkt, 124);
    pkt[124] = (uint8_t)(crc & 0xFF);
    pkt[125] = (uint8_t)(crc >> 8);

    /* End Footer: 0xAA, 0xCC (0xAACC little-endian) at Bytes 126..127 */
    pkt[126] = 0xAA;
    pkt[127] = 0xCC;

    const char *pa_mode_str = (s_config.rfSwitchConfig5V == RBI_SWITCH_RFO_LP5V) ?
        "5V PA (SX1262 10 dBm RFO_LP / PA0 boost)" : "3.3V PA (Safe Mode)";
    uart1_printf("[M0+ TX HK] Sending 128-Byte HK Telemetry Packet (seq=%u, CRC=0x%04X) via %s...\r\n",
                 s_hk_seq, crc, pa_mode_str);

    Satellite_Send_Packet(pkt, HK_PKT_LEN);
    /* Keep 5V PA biased for ACK Completed. Switching to RX here brown-outs. */
}

void Satellite_Send_Flash_Chunk(
    uint16_t pkt_idx,
    uint16_t total_pkts,
    uint32_t flash_addr,
    uint8_t  status,
    const uint8_t data[FLASH_DATA_LEN])
{
    uint8_t frame[FLASH_PKT_TOTAL_LEN];

    frame[0]  = FLASH_PKT_MAGIC_LO;
    frame[1]  = FLASH_PKT_MAGIC_HI;
    frame[2]  = (uint8_t)(pkt_idx & 0xFF);
    frame[3]  = (uint8_t)(pkt_idx >> 8);
    frame[4]  = (uint8_t)(total_pkts & 0xFF);
    frame[5]  = (uint8_t)(total_pkts >> 8);
    frame[6]  = (uint8_t)(flash_addr & 0xFF);
    frame[7]  = (uint8_t)((flash_addr >> 8) & 0xFF);
    frame[8]  = (uint8_t)((flash_addr >> 16) & 0xFF);
    frame[9]  = (uint8_t)((flash_addr >> 24) & 0xFF);
    frame[10] = (uint8_t)(FLASH_DATA_LEN & 0xFF);
    frame[11] = (uint8_t)(FLASH_DATA_LEN >> 8);
    frame[12] = status;
    frame[13] = 0x00;  /* reserved */

    memcpy(&frame[FLASH_PKT_HDR_LEN], data, FLASH_DATA_LEN);

    uint16_t crc = hk_crc16(frame, FLASH_PKT_HDR_LEN + FLASH_DATA_LEN);
    frame[FLASH_PKT_HDR_LEN + FLASH_DATA_LEN]     = (uint8_t)(crc & 0xFF);
    frame[FLASH_PKT_HDR_LEN + FLASH_DATA_LEN + 1] = (uint8_t)(crc >> 8);

    uart1_printf("[M0+ FLASH TX] pkt=%u/%u addr=0x%08lX status=%u\r\n",
                 pkt_idx + 1, total_pkts, (unsigned long)flash_addr, status);

    Satellite_Send_Packet(frame, FLASH_PKT_TOTAL_LEN);

    CPU2_Delay_Ms(500); /* pacing so GS finishes CAMERA/FLASH JSON and re-arms RX */
}

void Satellite_Handle_Flash_Read(
    uint32_t start_addr,
    uint16_t num_pkts)
{
    if (num_pkts == 0) num_pkts = 1;

    uart1_printf("\r\n>>> [M0+ FLASH] READ addr=0x%08lX  npkts=%u (%u B) <<<\r\n",
                 (unsigned long)start_addr, num_pkts,
                 (unsigned)(num_pkts * FLASH_DATA_LEN));

    Satellite_SetDCDCHold(true);

    /* Only signal M4 via IPCC_CH_FLASH if the flash chunk ring buffer is empty.
     * If M4 already received the telecommand and started streaming chunks into the ring buffer,
     * do not redundantly re-trigger another stream. */
    if (flash_data_empty()) {
        flash_req_send(start_addr, num_pkts);
    }

    static const uint32_t CHUNK_TIMEOUT_MS = 3000U;
    bool ipc_active = (SHARED_FLASH_DATA->magic == FLASH_DATA_MAGIC);

    for (uint16_t p = 0; p < num_pkts; p++)
    {
        uint32_t this_addr = start_addr + (uint32_t)p * FLASH_DATA_LEN;
        uint8_t  stub[FLASH_DATA_LEN];

        if (ipc_active)
        {
            struct flash_chunk_s chunk;
            uint32_t waited_ms = 0;
            bool got_chunk = false;

            while (waited_ms < CHUNK_TIMEOUT_MS)
            {
                if (flash_data_read(&chunk))
                {
                    got_chunk = true;
                    if (ipcc_m0_received(IPCC_CH_FLASH))
                    {
                        ipcc_m0_clear(IPCC_CH_FLASH);
                    }
                    break;
                }
                CPU2_Delay_Ms(10);
                waited_ms += 10;
            }

            if (got_chunk)
            {
                Satellite_Send_Flash_Chunk(
                    chunk.pkt_idx, num_pkts,
                    chunk.flash_addr,
                    chunk.status,
                    chunk.data);
                continue;
            }
            uart1_puts("[M0+ FLASH] IPC timeout waiting for M4 chunk\r\n");
        }

        /* Pack structured HEX payload containing ADC1 + ADC2 with IMU between header and footer */
        memset(stub, 0, FLASH_DATA_LEN);
        int16_t *s1_16 = (int16_t *)&stub[0];
        /* ADC 1: 16 Channels (Voltages & Signed Temperatures) */
        s1_16[0]  = (int16_t)385;  /* Bat V  3.85V x100 */
        s1_16[1]  = (int16_t)492;  /* Solar V 4.92V x100 */
        s1_16[2]  = (int16_t)330;  /* Bus V 3.30V x100 */
        s1_16[3]  = (int16_t)490;  /* SP5 V 4.90V */
        s1_16[4]  = (int16_t)488;  /* SP4 V 4.88V */
        s1_16[5]  = (int16_t)491;  /* SP3 V 4.91V */
        s1_16[6]  = (int16_t)493;  /* SP1 V 4.93V */
        s1_16[7]  = (int16_t)490;  /* SP2 V 4.90V */
        s1_16[8]  = (int16_t)0;    /* Ant Temp 0.0 C (signed) */
        s1_16[9]  = (int16_t)215;  /* Bat Temp +21.5 C (signed) */
        s1_16[10] = (int16_t)220;  /* BPB Temp +22.0 C (signed) */
        s1_16[11] = (int16_t)250;  /* Temp 1 */
        s1_16[12] = (int16_t)250;  /* Temp 5 */
        s1_16[13] = (int16_t)250;  /* Temp 4 */
        s1_16[14] = (int16_t)250;  /* Temp 3 */
        s1_16[15] = (int16_t)250;  /* Temp 2 */
        /* Footer 1: 0xAA55 */
        stub[32] = 0x55;
        stub[33] = 0xAA;

        /* ADC 2: 12 Channels (Currents, signed/unsigned) */
        int16_t *s2_16 = (int16_t *)&stub[34];
        s2_16[0]  = (int16_t)45;   /* Unreg I 0.45A */
        s2_16[1]  = (int16_t)120;  /* 3V3 Main I 1.20A */
        s2_16[2]  = (int16_t)18;   /* 5V Rail I 0.18A */
        s2_16[3]  = (int16_t)-35;  /* Bat I -0.35A (signed discharge) */
        s2_16[4]  = (int16_t)15;   /* SP1 Current */
        s2_16[5]  = (int16_t)15;   /* SP2 Current */
        s2_16[6]  = (int16_t)15;   /* SP3 Current */
        s2_16[7]  = (int16_t)15;   /* SP4 Current */
        s2_16[8]  = (int16_t)15;   /* SP5 Current */
        s2_16[9]  = (int16_t)85;   /* Raw Bus Current */
        s2_16[10] = (int16_t)1;    /* Flag 1 */
        s2_16[11] = (int16_t)0;    /* Flag 2 */

        /* IMU: 6 Channels (3-Axis Gyro & 3-Axis Mag, signed!) */
        int16_t *imu16 = (int16_t *)&stub[58];
        imu16[0]  = (int16_t)-12;  /* Gyro X: -12 c-dps (signed) */
        imu16[1]  = (int16_t)24;   /* Gyro Y: +24 c-dps (signed) */
        imu16[2]  = (int16_t)8;    /* Gyro Z: +8 c-dps (signed) */
        imu16[3]  = (int16_t)320;  /* Mag X: +320 uT (signed) */
        imu16[4]  = (int16_t)110;  /* Mag Y: +110 uT (signed) */
        imu16[5]  = (int16_t)-450; /* Mag Z: -450 uT (signed) */

        /* Footer 2: 0xBB66 */
        stub[70] = 0x66;
        stub[71] = 0xBB;

        /* Packet Data Footer: 0xAA 0xCC at end of 128B */
        stub[126] = 0xAA;
        stub[127] = 0xCC;

        Satellite_Send_Flash_Chunk(
            p, num_pkts, this_addr,
            ipc_active ? 3U : 0U,
            stub);
    }

    Satellite_GMSK_Stop();

    uart1_printf("[M0+ FLASH] Download complete: %u packets sent\r\n", num_pkts);
}

#include "camera_image.h"

/* Camera fragment header / CRC per the Packet Format sheet (see CAM_PKT_TYPE in satellite_app.h). */
static void Cam_Fill_Header(uint8_t *frame, uint16_t idx, uint16_t total)
{
    frame[0] = CAM_PKT_TYPE;
    frame[1] = 0x00;                         /* image ID */
    frame[2] = (uint8_t)(idx & 0xFF);
    frame[3] = (uint8_t)(idx >> 8);
    frame[4] = (uint8_t)(total & 0xFF);
    frame[5] = (uint8_t)(total >> 8);
    frame[6] = (uint8_t)CAM_IMG_TYPE_RGB;    /* image type */
    frame[7] = 0x00;                         /* AI score: no on-board scoring yet */
}

static void Cam_Seal(uint8_t *frame)
{
    uint16_t crc = CRC16_X25(frame, CAM_PKT_HDR_LEN + CAM_DATA_LEN);
    frame[CAM_PKT_HDR_LEN + CAM_DATA_LEN]     = (uint8_t)(crc & 0xFF);
    frame[CAM_PKT_HDR_LEN + CAM_DATA_LEN + 1] = (uint8_t)(crc >> 8);
}

void Satellite_Send_Camera_Image(uint16_t expected_chunks, bool local_dummy)
{
    uint16_t total_chunks = (expected_chunks > 0) ? expected_chunks : CAM_TOTAL_CHUNKS;
    bool skip_ipc_wait = local_dummy;

    uart1_printf("\r\n============================================================\r\n");
    uart1_printf(">>> [M0+ CAMERA] DOWNLINKING CUBESAT PHOTO IMAGE (%u PACKETS%s) <<<\r\n",
                 (unsigned int)total_chunks, local_dummy ? ", dummy/local" : "");
    uart1_puts(" Frequency : 435.000 MHz (GMSK Downlink)\r\n");
    const char *cam_pa_str = (s_config.rfSwitchConfig5V == RBI_SWITCH_RFO_LP5V) ?
        "5V External Power Amplifier Active (PC3=1, PA0=1 5V Boost)" :
        "3.3V External Power Amplifier Active (PC2=1, Safe Mode)";
    uart1_printf(" Amplifier : %s\r\n", cam_pa_str);
    uart1_puts(" Format    : JPEG 320x240 CubeSat Photo (Starts 0xFFD8, Ends 0xFFD9)\r\n");
    uart1_puts(" Transport : 128-Byte Chunks via M4 Ring Buffer (Packet Format sheet: camera fragment, type 0x08)\r\n");
    uart1_puts("============================================================\r\n");

    Satellite_SetDCDCHold(true);

    uint8_t frame[CAM_PKT_TOTAL_LEN];

    for (uint16_t p = 0; p < total_chunks; p++)
    {
        uint32_t offset = (uint32_t)p * CAM_DATA_LEN;

        /* Attempt to read 128B chunk from M4 via Shared Ring Buffer */
        struct flash_chunk_s ipc_chunk;
        bool got_ipc = false;
        uint32_t wait_ms = 0;
        const uint32_t ipc_wait_limit = skip_ipc_wait ? 0U : 4000U;

        while (wait_ms <= ipc_wait_limit)
        {
            if (flash_data_read(&ipc_chunk))
            {
                got_ipc = true;
                if (ipcc_m0_received(IPCC_CH_FLASH))
                {
                    ipcc_m0_clear(IPCC_CH_FLASH);
                }
                /* If M4 signaled actual total_pkts, synchronize total_chunks */
                if (ipc_chunk.total_pkts > 0 && ipc_chunk.total_pkts != total_chunks)
                {
                    total_chunks = ipc_chunk.total_pkts;
                }
                break;
            }
            if (ipc_wait_limit == 0U)
            {
                break;
            }
            CPU2_Delay_Ms(5);
            wait_ms += 5;
        }
        if (!got_ipc && local_dummy)
        {
            skip_ipc_wait = true;
        }

        Cam_Fill_Header(frame, p, total_chunks);

        if (got_ipc)
        {
            uart1_printf("[M0+ CAMERA IPC RX] Read Chunk %u/%u from M4 Ring Buffer (Offset 0x%04lX)\r\n",
                         ipc_chunk.pkt_idx + 1, total_chunks, (unsigned long)ipc_chunk.flash_addr);
            memcpy(&frame[CAM_PKT_HDR_LEN], ipc_chunk.data, CAM_DATA_LEN);
        }
        else
        {
            uart1_printf("[M0+ CAMERA] WARNING: no M4 chunk for pkt %u/%u (ring empty)\r\n",
                         p + 1, total_chunks);
            memset(&frame[CAM_PKT_HDR_LEN], 0x00, CAM_DATA_LEN);
        }

        Cam_Seal(frame);

        const char *tx_pa_str = (s_config.rfSwitchConfig5V == RBI_SWITCH_RFO_LP5V) ? "5V External PA (PC3 / PA0 Boost)" : "3.3V External PA (PC2 Safe Mode)";
        uart1_printf("[M0+ CAMERA TX] Downlinking Packet %u/%u (Offset 0x%04lX, %u B) via %s...\r\n",
                     p + 1, total_chunks, (unsigned long)offset, CAM_DATA_LEN, tx_pa_str);

        Satellite_Send_Packet(frame, CAM_PKT_TOTAL_LEN);

        CPU2_Delay_Ms(500); /* keep 5V PA biased; pace GS UART JSON */
    }

    Satellite_GMSK_Stop();

    uart1_printf("\r\n>>> [M0+ CAMERA] CUBESAT PHOTO DOWNLINK COMPLETE (%u PACKETS TRANSMITTED) <<<\r\n\r\n",
                 (unsigned int)total_chunks);
}

/*
 * ADDED: transmit ONE camera/file chunk. Caller is Obc_Ring_Service() in satellite_main.c, which pulls
 * the chunk out of the shared flash ring that CPU1 (NuttX app s2s2obc) filled from its LittleFS file.
 * Same fragment as Satellite_Send_Camera_Image (sheet format, see CAM_PKT_TYPE).
 * idx/total let the GS rebuild the file in any order; the last fragment is zero padded (no length field in the sheet).
 */
/* Build a camera fragment (sheet format: 8-byte header, 128 data bytes zero padded, CRC-16/X.25) into frame[CAM_PKT_TOTAL_LEN]. */
static void Build_Camera_Packet(uint8_t *frame, uint16_t idx, uint16_t total, const uint8_t *data, uint8_t len)
{
    Cam_Fill_Header(frame, idx, total);
    memset(&frame[CAM_PKT_HDR_LEN], 0, CAM_DATA_LEN);
    if (len > CAM_DATA_LEN) len = CAM_DATA_LEN;
    memcpy(&frame[CAM_PKT_HDR_LEN], data, len);
    Cam_Seal(frame);
}

/*
 * ADDED: transmit ONE camera/file chunk. Caller is Obc_Ring_Service() in satellite_main.c, which pulls
 * the chunk out of the shared flash ring that CPU1 (NuttX app s2s2obc) filled from its LittleFS file.
 * Same fragment as Satellite_Send_Camera_Image (sheet format, see CAM_PKT_TYPE).
 * idx/total let the GS rebuild the file in any order; the last fragment is zero padded (no length field in the sheet).
 */
bool Satellite_Send_Camera_Chunk(uint16_t idx, uint16_t total, const uint8_t *data, uint8_t len)
{
    uint8_t frame[CAM_PKT_TOTAL_LEN];
    Build_Camera_Packet(frame, idx, total, data, len);
    return Satellite_Send_Packet(frame, CAM_PKT_TOTAL_LEN);
}

/*
 * ADDED: raw air-rate test. Sends `count` camera-format packets (pseudo-random 128-byte payloads, so the GS can check
 * them) with `gap_ms` between the end of one transmission and the start of the next -- 0 means back to back. The PA path
 * is set up ONCE, and the next packet is assembled while the previous one is still on the air, so with gap_ms = 0 the
 * only dead time is the SPI write of the 200-byte buffer. Everything the normal path adds (per-packet PA sequencing,
 * the 300 ms SAT_TX_GAP_MS, CPU1/ring hand-over, flash, beacons) is left out.
 * elapsed_ms = first TX start -> last TX done (1 ms tick); sent_ok = packets whose TX-done interrupt arrived.
 */
bool Satellite_Rate_Test(uint16_t count, uint32_t gap_ms, uint32_t *elapsed_ms, uint32_t *sent_ok)
{
    static uint8_t buf[2][RADIO_FIXED_PACKET_LEN + 64];
    uint8_t pkt[CAM_PKT_TOTAL_LEN];
    uint8_t data[CAM_DATA_LEN];
    uint32_t ok = 0;
    uint32_t t_start = 0, t_end = 0;

    *elapsed_ms = 0;
    *sent_ok = 0;
    if (count == 0) return false;

    Satellite_SetDCDCHold(true);                /* keep the PA biased between packets */
    Satellite_GMSK_Prepare();

    uint32_t rnd = 0x1234ABCDu;
    int cur = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (i == 0) {                           /* only the first packet is built before the clock starts */
            for (int k = 0; k < CAM_DATA_LEN; k++) { rnd = rnd * 1664525u + 1013904223u; data[k] = (uint8_t)(rnd >> 24); }
            Build_Camera_Packet(pkt, i, count, data, CAM_DATA_LEN);
            if (Protocol_CreatePacket(buf[cur], pkt, CAM_PKT_TOTAL_LEN, &s_config.radio) == 0) break;
            t_start = Get_Time_Ms();
        }
        tx_busy = 1;
        RadioApp_Send(buf[cur], RADIO_FIXED_PACKET_LEN);          /* TX starts now ... */
        if (i + 1 < count) {                                       /* ... and the next packet is built while it is on air */
            for (int k = 0; k < CAM_DATA_LEN; k++) { rnd = rnd * 1664525u + 1013904223u; data[k] = (uint8_t)(rnd >> 24); }
            Build_Camera_Packet(pkt, (uint16_t)(i + 1), count, data, CAM_DATA_LEN);
            if (Protocol_CreatePacket(buf[cur ^ 1], pkt, CAM_PKT_TOTAL_LEN, &s_config.radio) == 0) { count = (uint16_t)(i + 1); }
        }
        uint32_t w0 = Get_Time_Ms();
        while (tx_busy != 0 && (Get_Time_Ms() - w0) < 3000U) { /* TX-done IRQ clears tx_busy */ }
        if (tx_busy != 0) { tx_busy = 0; Radio.Standby(); break; }
        ok++;
        t_end = Get_Time_Ms();
        cur ^= 1;
        if (gap_ms > 0) CPU2_Delay_Ms(gap_ms);
    }

    Satellite_SetDCDCHold(false);
    Satellite_GMSK_AfterTx();
    *elapsed_ms = t_end - t_start;
    *sent_ok = ok;
    return ok == count;
}

void Satellite_Send_Camera_Ack(uint16_t count)
{
    char resp_buf[64];
    snprintf(resp_buf, sizeof(resp_buf), "ACK: CAM CAPTURE (pkt_count=%u)", count);
    uart1_printf("[M0+ TX] Sending Camera ACK via 5V External PA (PC3 / PA0 Boost): \"%s\"\r\n", resp_buf);
    Satellite_Send_AX25_String(resp_buf);
}

void Satellite_Send_Ping_Response(void)
{
    static const char *ping_ack = "ACK: PING OK (OBC_ONLINE)";
    uint16_t ack_len = (uint16_t)strlen(ping_ack);
    uart1_printf("[M0+ TX PING] hex(%u):", ack_len);
    for (uint16_t i = 0; i < ack_len; i++)
        uart1_printf("%02X", (uint8_t)ping_ack[i]);
    uart1_puts("\r\n");
    uart1_printf("[M0+ TX PING] ascii: \"%s\"\r\n", ping_ack);
    Satellite_Send_AX25_String(ping_ack);
}

/*
 * ============================================================================
 * FLIGHT SPECIFICATION BINARY ACK / NACK HANDLERS (Photo / Telemetry Protocol)
 * ============================================================================
 * Frame format:
 * - 20 Leading Flags (0x7E)
 * - AX.25 Header: Destination (GROUND / 7B), Source (9NS2S2 / 7B), Control (0x03), PID (0xF0)
 * - Information / Data Field (3 Bytes):
 *     ACK Accepted:  [0]=0xAC, [1]=0x00, [2]=0x05 (or packet count)
 *     ACK Completed: [0]=0xEE, [1]=0x00, [2]=0x05 (or packet count)
 *     NACK Error:    [0]=0xFF, [1]=err_code, [2]=count
 * - CRC-16 CCITT (2 Bytes)
 * - 5 Trailing Flags (0x7E)
 * Transmitted via GMSK 4800 bps AX.25 G3RUH scrambled on 435.000 MHz (5V PA).
 * ============================================================================
 */

void Satellite_Send_Ack_Accepted(uint8_t mission_id)
{
    /*
     * Packet Format.xlsx (Sheet 2):
     * - Byte 0: 0xAC (ACK)
     * - Byte 1: 0x00 (Command accepted)
     * - Byte 2: Mission / Data Identifier (0x01=HK, 0x04=ADCS, 0x05=Camera, 0x08=EPDM, etc.)
     * Transmitted in GMSK 4800 bps via 5V PA twice ("ack, ack") for flight link reliability.
     */
    uint8_t ack_payload[3];
    ack_payload[0] = SAT_ACK_ACCEPTED_BYTE; /* 0xAC */
    ack_payload[1] = SAT_STATUS_ACCEPTED;   /* 0x00 */
    ack_payload[2] = mission_id ? mission_id : SAT_MISSION_CAM_RGB;

    Satellite_SetDCDCHold(true);
    Satellite_GMSK_Prepare();
    CPU2_Delay_Ms(50);

    const char *ack_pa_str = (s_config.rfSwitchConfig5V == RBI_SWITCH_RFO_LP5V) ? "5V PA" : "3.3V PA (Safe Mode)";
    uart1_printf("\r\n>>> [M0+ TX ACK 1/2] Sending Flight Spec ACK Accepted (GMSK): 0x%02X 0x%02X 0x%02X (mission=0x%02X) via %s <<<\r\n",
                 ack_payload[0], ack_payload[1], ack_payload[2], ack_payload[2], ack_pa_str);
    uart1_printf("[TX-HEX] hex(3):%02X%02X%02X\r\n", ack_payload[0], ack_payload[1], ack_payload[2]);
    Satellite_Send_Packet(ack_payload, sizeof(ack_payload));
    CPU2_Delay_Ms(80);

    uart1_printf(">>> [M0+ TX ACK 2/2] Sending Flight Spec ACK Accepted (GMSK): 0x%02X 0x%02X 0x%02X (mission=0x%02X) via %s <<<\r\n",
                 ack_payload[0], ack_payload[1], ack_payload[2], ack_payload[2], ack_pa_str);
    uart1_printf("[TX-HEX] hex(3):%02X%02X%02X\r\n", ack_payload[0], ack_payload[1], ack_payload[2]);
    Satellite_Send_Packet(ack_payload, sizeof(ack_payload));
    /* Stay at 10 dBm for the following HK/data downlink. 14 dBm + 5V PA brown-outs. */
}

void Satellite_Send_Ack_Completed(uint8_t mission_id)
{
    /*
     * Packet Format.xlsx (Sheet 2):
     * - Byte 0: 0xAC / 0xEE
     * - Byte 1: 0x01 (Command completed)
     * - Byte 2: Mission / Data Identifier
     */
    uint8_t ack_payload[3];
    ack_payload[0] = SAT_ACK_COMPLETED_BYTE; /* 0xEE (or 0xAC) */
    ack_payload[1] = SAT_STATUS_COMPLETED;   /* 0x01 */
    ack_payload[2] = mission_id ? mission_id : SAT_MISSION_CAM_RGB;

    const char *ack_pa_str = (s_config.rfSwitchConfig5V == RBI_SWITCH_RFO_LP5V) ? "5V PA" : "3.3V PA (Safe Mode)";
    uart1_printf("\r\n>>> [M0+ TX ACK] Sending ACK Completed (GMSK): 0x%02X 0x%02X 0x%02X (mission=0x%02X) via %s <<<\r\n",
                 ack_payload[0], ack_payload[1], ack_payload[2], ack_payload[2], ack_pa_str);
    uart1_printf("[TX-HEX] hex(3):%02X%02X%02X\r\n", ack_payload[0], ack_payload[1], ack_payload[2]);

    Satellite_Send_Packet(ack_payload, sizeof(ack_payload));
    Satellite_GMSK_Stop();
}

void Satellite_Send_Nack(uint8_t err_code, uint8_t mission_id)
{
    /*
     * Packet Format.xlsx (Sheet 2):
     * - Byte 0: 0xEE (NACK / Error)
     * - Byte 1: Status / Error Code (0x02=Packet err, 0x03=CRC err, 0x09=Invalid cmd, etc.)
     * - Byte 2: Mission / Data Identifier
     */
    uint8_t nack_payload[3];
    nack_payload[0] = SAT_NACK_BYTE; /* 0xEE */
    nack_payload[1] = err_code;
    nack_payload[2] = mission_id ? mission_id : SAT_MISSION_UNKNOWN;

    const char *nack_pa_str = (s_config.rfSwitchConfig5V == RBI_SWITCH_RFO_LP5V) ? "5V PA" : "3.3V PA (Safe Mode)";
    uart1_printf("\r\n>>> [M0+ TX NACK] Sending Flight Spec NACK (GMSK): 0x%02X 0x%02X 0x%02X (err=0x%02X, mission=0x%02X) via %s <<<\r\n",
                 nack_payload[0], nack_payload[1], nack_payload[2], err_code, nack_payload[2], nack_pa_str);
    uart1_printf("[TX-HEX] hex(3):%02X%02X%02X\r\n", nack_payload[0], nack_payload[1], nack_payload[2]);

    Satellite_Send_Packet(nack_payload, sizeof(nack_payload));
    Satellite_GMSK_Stop();
}
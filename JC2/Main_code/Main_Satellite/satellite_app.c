#include "satellite_app.h"

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

/* Static packet buffers to guarantee zero stack overflow on Cortex-M0+ */
static uint8_t s_raw_frame[AX25_MAX_FRAME_SIZE + 64];
static uint8_t s_burst_scrambled[RADIO_FIXED_PACKET_LEN + 64];
static uint32_t s_total_packets_sent = 0;

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
    rx_timeout_flag = 1;
}

void OnRxError(void) {
    rx_error_flag = 1;
}

void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr) {
    (void)rssi;
    (void)snr;
    uint16_t copy_len = (size < AX25_MAX_FRAME_SIZE) ? size : AX25_MAX_FRAME_SIZE;
    memcpy((void *)rx_frame_buffer, payload, copy_len);
    rx_frame_size = copy_len;
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

static void Print_Hex_Bytes(const uint8_t *buf, uint16_t len) {
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

static void Print_AX25_Fields(const char *label, const uint8_t *buf, uint16_t len) {
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

void Satellite_CW_Prepare(void) {
    int8_t power = (s_config.txPowerDbm != 0) ? s_config.txPowerDbm : RADIO_TX_POWER_DBM;
    if (s_config.rfSwitchConfig != RBI_SWITCH_RFO_HP && power > 14) power = 14;
    if (power > 22) power = 22;
    uint8_t pa_sel = (s_config.rfSwitchConfig == RBI_SWITCH_RFO_HP) ? RFO_HP : RFO_LP;
    Radio.Standby();

    /* Ensure 5V DC/DC (PA0) and 5V PA (PC3) are completely OFF for CW mode */
    uart1_puts(">>> CW PREPARE: 5V DC/DC & 5V PA OFF, 3.3V PA ON (PC2/SO2)\r\n");
    RBI_Enable5VDCDC(0);
    HAL_GPIO_WritePin(GPIOC, AMP_5V_EN_PIN, GPIO_PIN_RESET);

    /* Explicitly configure 3.3V PA (PC2/SO2) for CW mode (3.3V rail is hardware always-on) */
    RBI_SetTxSwitchConfig(s_config.rfSwitchConfig);
    Satellite_SetRFSwitch(s_config.rfSwitchConfig);
    CPU2_Delay_Ms(2); /* Settle delay for 3.3V PA */

    SUBGRF_SetStandby(STDBY_RC);
    SUBGRF_SetDioIrqParams(IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE);
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);
    SUBGRF_SetPacketType(PACKET_TYPE_GFSK);
    SUBGRF_SetRfFrequency(s_config.radio.txFrequency);
    SUBGRF_SetTxParams(pa_sel, power, RADIO_RAMP_40_US);
    SUBGRF_WriteRegister(REG_DRV_CTRL, 0x7 << 1);
    SUBGRF_SetSwitch(pa_sel, RFSWITCH_TX);
}

void Satellite_CW_CarrierOn(void) {
    SUBGRF_SetTxContinuousWave();
}

void Satellite_CW_CarrierOff(void) {
    /* Return to STDBY_XOSC: cuts RF carrier immediately while keeping 32MHz TCXO running */
    SUBGRF_SetStandby(STDBY_XOSC);
}

void Satellite_CW_Finish(void) {
    SUBGRF_SetStandby(STDBY_RC);
    Radio.Standby();
    /* Power down 3.3V External PA, 5V DC/DC, and RF switch after CW session completes */
    Satellite_SetRFSwitch(RBI_SWITCH_OFF);
    SUBGRF_SetDioIrqParams(IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE);
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);
    tx_busy = 0;
}

void Satellite_CW_Abort_To_Rx(void) {
    Satellite_CW_CarrierOff();
    /* Power down CW 3.3V PA */
    Satellite_SetRFSwitch(RBI_SWITCH_OFF);
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
    uint8_t pa_sel = (s_config.rfSwitchConfig == RBI_SWITCH_RFO_HP) ? RFO_HP : RFO_LP;
    Radio.Standby();

    /* 1. Ensure 3.3V External PA (PC2) is OFF */
    HAL_GPIO_WritePin(GPIOC, AMP_3V3_EN_PIN, GPIO_PIN_RESET);

    /* 2. Enable 5V DC/DC Converter (PA0 = 1) */
    RBI_Enable5VDCDC(1);
    CPU2_Delay_Ms(10); /* 10 ms soft-start settle delay for 5V boost capacitor charging */

    /* 3. Configure RF switch and assert 5V External PA (PC3 = 1) */
    RBI_SetTxSwitchConfig(s_config.rfSwitchConfig5V);
    Satellite_SetRFSwitch(s_config.rfSwitchConfig5V);
    CPU2_Delay_Ms(2);  /* 2 ms PA bias settle delay */

    SUBGRF_SetStandby(STDBY_RC);
    SUBGRF_SetDioIrqParams(IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE);
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);
    SUBGRF_SetPacketType(PACKET_TYPE_GFSK);
    SUBGRF_SetRfFrequency(s_config.radio.txFrequency);
    SUBGRF_SetTxParams(pa_sel, power, RADIO_RAMP_40_US);
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

    /* Power down 5V PA PC3, 5V DC/DC PA0, and RF switch */
    Satellite_SetRFSwitch(RBI_SWITCH_OFF);
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
    uint8_t pa_sel = (s_config.rfSwitchConfig == RBI_SWITCH_RFO_HP) ? RFO_HP : RFO_LP;
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
    HAL_Init();
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
    uart1_puts("[INIT] 4. RF Dual-PA & 5V DC/DC Configured (Standby: ALL OFF) ... [OK]\r\n");
    uart1_puts("       - CW Mode   : 3.3V External PA (PC2/SO2) | 5V DC/DC (PA0): OFF\r\n");
    if (s_config.rfSwitchConfig5V == RBI_SWITCH_RFO_LP5V) {
        uart1_puts("       - GMSK Mode : 5V External PA (PC3/SI2)   | 5V DC/DC (PA0): ENABLED (Flight Mode)\r\n");
    } else {
        uart1_puts("       - GMSK Mode : 3.3V External PA (PC2/SO2) | 5V DC/DC (PA0): OFF (Safe Bench Mode)\r\n");
    }

    MX_SUBGHZ_Init();
    HAL_NVIC_SetPriority(SUBGHZ_Radio_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(SUBGHZ_Radio_IRQn);
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
    RadioApp_SetPaSelect((s_config.rfSwitchConfig == RBI_SWITCH_RFO_HP) ? RFO_HP : RFO_LP);
    uart1_puts("[INIT] 6. Radio Application Layer Configured:              [OK]\r\n");
    uart1_printf("       - Downlink Frequency : %lu.%03lu MHz\r\n",
                 (unsigned long)(s_config.radio.txFrequency / 1000000UL),
                 (unsigned long)((s_config.radio.txFrequency % 1000000UL) / 1000UL));
    uart1_printf("       - Transmit RF Power  : +%d dBm\r\n", tx_power);
    uart1_printf("       - RF Front-End Switch: %s\r\n",
                 (s_config.rfSwitchConfig == RBI_SWITCH_RFO_HP) ? "RBI_SWITCH_RFO_HP (High Power)" : "RBI_SWITCH_RFO_LP (Low Power)");
    uart1_printf("       - GMSK Bitrate       : %lu bps (Fdev: %lu Hz, BT: 0.5)\r\n",
                 (unsigned long)bitrate, (unsigned long)fdev);
    uart1_printf("       - Callsign Profile   : %s-%d -> %s-%d\r\n",
                 s_config.radio.sourceCallsign, s_config.radio.sourceSSID,
                 s_config.radio.destCallsign, s_config.radio.destSSID);
    uart1_puts("       - Supported Modes    : 1) CW Morse Beacon (Keying)\r\n");
    uart1_puts("                              2) GMSK AX.25 UI Frame + G3RUH\r\n");
    uart1_puts("======================================================================\r\n");
    uart1_puts("[INIT COMPLETE] Satellite Application Ready.\r\n");
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
    if (!callsign) callsign = "9NS2S2NEPAL";
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

    if (!callsign) callsign = "9NS2S2NEPAL";
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
    uint8_t pa_sel = (s_config.rfSwitchConfig == RBI_SWITCH_RFO_HP) ? RFO_HP : RFO_LP;
    Radio.Standby();

    if (s_config.rfSwitchConfig5V == RBI_SWITCH_RFO_LP5V) {
        /*
         * 5V External PA Mode (Flight Mode):
         * 1. Ensure 3.3V External PA (PC2) is completely OFF
         * 2. Enable 5V DC/DC Converter (PA0 = 1)
         * 3. Settle delay: 50 ms soft-start for 5V boost capacitors to reach steady state,
         *    preventing input rail collapse / BOR reset on cold boots.
         * 4. Configure RF switch (PC4=0, PC5=1) and assert 5V External PA (PC3 = 1)
         * 5. Settle delay: 15 ms for PA DC bias stabilization before RF transmission.
         */
        HAL_GPIO_WritePin(GPIOC, AMP_3V3_EN_PIN, GPIO_PIN_RESET);
        RBI_Enable5VDCDC(1);
        CPU2_Delay_Ms(50); /* 50 ms robust capacitor soft-start settle delay */

        RBI_SetTxSwitchConfig(RBI_SWITCH_RFO_LP5V);
        Satellite_SetRFSwitch(RBI_SWITCH_RFO_LP5V);
        CPU2_Delay_Ms(15); /* 15 ms PA bias settle delay */
    } else {
        /*
         * 3.3V External PA Mode (Safe Bench / ST-Link USB Mode):
         * 1. Ensure 5V DC/DC (PA0) and 5V PA (PC3) are OFF
         * 2. Enable 3.3V External PA (PC2 = 1)
         * 3. Current draw remains <170 mA - zero brownout risk under any USB supply!
         */
        RBI_Enable5VDCDC(0);
        HAL_GPIO_WritePin(GPIOC, AMP_5V_EN_PIN, GPIO_PIN_RESET);

        RBI_SetTxSwitchConfig(s_config.rfSwitchConfig);
        Satellite_SetRFSwitch(s_config.rfSwitchConfig);
        CPU2_Delay_Ms(5);  /* 5 ms PA settle delay */
    }

    SUBGRF_SetStandby(STDBY_RC);
    SUBGRF_SetPacketType(PACKET_TYPE_GFSK);
    RadioApp_SetTxPower(power);
    RadioApp_SetPaSelect(pa_sel);
    RadioApp_ForceOpenGfskParams();
}

bool Satellite_Send_Packet_Timeout(const uint8_t *payload, uint16_t len, uint32_t timeout_ms) {
    if (!payload || len == 0) return false;
    if (timeout_ms == 0) timeout_ms = (s_config.txTimeoutMs > 0) ? s_config.txTimeoutMs : 3000;

    /* First enable 5V DC/DC (PA0) and 5V PA (PC3/SI2) before GMSK transmission */
    Satellite_GMSK_Prepare();

    uint16_t frame_size = Protocol_CreatePacket(s_burst_scrambled, payload, len, &s_config.radio);
    if (frame_size == 0) {
        uart1_puts("ERROR: Failed to assemble packet!\r\n");
        Satellite_SetRFSwitch(RBI_SWITCH_OFF);
        return false;
    }

    bool res = Radio_Send_And_Wait(s_burst_scrambled, frame_size, timeout_ms);
    CPU2_Delay_Ms(2); /* Settle guard delay for PA ramp-down before shutting down RF switch & external PA */
    /* Only on while GMSK send PA0, otherwise OFF */
    Satellite_SetRFSwitch(RBI_SWITCH_OFF);
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
        SUBGRF_SetStandby(STDBY_RC);
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

    /* Guard delay: allow final packet PA ramp-down to finish cleanly before RF switch off */
    CPU2_Delay_Ms(2);
    Radio.Standby();
    /* Only on while GMSK send PA0, otherwise OFF: Power down 5V PA PC3, 5V DC/DC PA0, and RF switch */
    Satellite_SetRFSwitch(RBI_SWITCH_OFF);
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
  radio_log_write(RADIO_EVT_INFO,
                   "[M0+] Waiting for M4 Handshake via IPCC Channel 3 (up to 30s)...");

  uint32_t start_time = Get_Time_Ms();

  while ((Get_Time_Ms() - start_time) < 30000)
    {
      if (ipcc_m0_received(IPCC_CH_HANDSHAKE))
        {
          ipcc_m0_clear(IPCC_CH_HANDSHAKE);
          ipcc_m0_send(IPCC_CH_HANDSHAKE);   /* Acknowledge back to M4 */

          radio_log_write(RADIO_EVT_INFO,
                           "[M0+ RF] Handshake Synchronized with M4! Radio Link Active.");
          break;
        }

      CPU2_Delay_Ms(100);
    }
}
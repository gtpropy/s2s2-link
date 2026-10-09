#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "stm32wlxx_hal.h"
#include "radio.h"
#include "subghz.h"
#include "radio_board_if.h"
#include "uart_debug.h"

#include "config.h"
#include "radio_app.h"
#include "protocol.h"
#include "ax25.h"
#include "g3ruh.h"

const RadioConfig_t SatelliteProfile = {
    .txFrequency = DEFAULT_TX_FREQUENCY_HZ,
    .rxFrequency = DEFAULT_RX_FREQUENCY_HZ,
    .sourceCallsign = "9NS2S2",
    .sourceSSID = 1,
    .destCallsign = "GROUND",
    .destSSID = 0,
    .isSatelliteMode = true
};

#define BURST_PACKETS_PER_CYCLE  1   /* Number of burst packets per cycle: CW -> N x Burst -> CW ... */
#define MORSE_UNIT_MS            80  /* 15 WPM (dit=80ms, dah=240ms) */
#define CW_BEACON_TEXT           "9NS2S2 NEPAL"
#define BURST_PAYLOAD_TEXT       "Namaste everyone, Testing GMSK signal"

typedef enum {
    STATE_IDLE = 0,
    STATE_TRANSMIT_CW,
    STATE_TRANSMIT_BURST,
} AppState_t;

volatile AppState_t app_state = STATE_IDLE;
volatile uint8_t tx_busy = 0;
volatile uint8_t rx_frame_buffer[AX25_MAX_FRAME_SIZE];
volatile uint16_t rx_frame_size = 0;
volatile uint8_t rx_done_flag = 0;
volatile uint8_t rx_timeout_flag = 0;
volatile uint8_t rx_error_flag = 0;

/* Interrupt Status Callbacks */
void OnTxDone(void) { tx_busy = 0; }
void OnTxTimeout(void) { uart1_puts("TX TIMEOUT ERROR\r\n"); tx_busy = 0; }
void OnRxTimeout(void) { rx_timeout_flag = 1; }
void OnRxError(void) { rx_error_flag = 1; }

void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr) {
    (void)rssi;
    (void)snr;
    uint16_t copy_len = (size < AX25_MAX_FRAME_SIZE) ? size : AX25_MAX_FRAME_SIZE;
    memcpy((void *)rx_frame_buffer, payload, copy_len);
    rx_frame_size = copy_len;
    rx_done_flag = 1;
}

extern SUBGHZ_HandleTypeDef hsubghz;
void SUBGHZ_Radio_IRQHandler(void) { HAL_SUBGHZ_IRQHandler(&hsubghz); }

void HardFault_Handler(void) {
    (*(volatile uint32_t *)0x58000160UL) |= (1UL << 14); /* RCC_C2APB2ENR USART1 */
    (*(volatile uint32_t *)0x40013820UL) = 0xFFFFFFFFUL; /* USART1_ICR */
    (*(volatile uint32_t *)0x40013800UL) |= (1UL << 0) | (1UL << 3); /* UE | TE */
    uart1_puts("\r\n\r\n[CPU2 HARDFAULT TRAPPED!]\r\n");
    while (1);
}

static void Print_Hex_Bytes(const uint8_t *buf, uint16_t len) {
    char line[56]; uint16_t pos = 0; uint16_t on_this_line = 0;
    for (uint16_t i = 0; i < len; i++) {
        pos += snprintf(&line[pos], sizeof(line) - pos, "%02X ", buf[i]);
        on_this_line++;
        if (on_this_line == 16 || i == (uint16_t)(len - 1)) {
            uart1_puts(line); uart1_puts("\r\n"); pos = 0; on_this_line = 0;
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

    uint16_t payload_start = 17; uint16_t payload_end = (uint16_t)(len - 3);
    if (payload_end > payload_start) {
        uint16_t plen = (uint16_t)(payload_end - payload_start);
        snprintf(line, sizeof(line), "  Payload    : %u bytes\r\n", plen); uart1_puts(line);
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

extern void SysTick_Init_CPU2(uint32_t sys_freq_hz);
extern void CPU2_Delay_Ms(uint32_t ms);
extern volatile uint32_t g_system_tick_ms;

static uint32_t Get_Time_Ms(void) {
    return g_system_tick_ms;
}

/* All TX goes through RadioApp_Send() */
static bool Radio_Send_And_Wait(uint8_t *buffer, uint16_t size, uint32_t timeout_ms) {
    tx_busy = 1;
    RadioApp_Send(buffer, size);

    uint32_t start_ms = Get_Time_Ms();
    uint32_t max_loop = timeout_ms;
    while (tx_busy != 0) {
        uint16_t irq = SUBGRF_GetIrqStatus();
        if (irq & IRQ_TX_DONE) {
            SUBGRF_ClearIrqStatus(IRQ_TX_DONE | IRQ_RX_TX_TIMEOUT);
            tx_busy = 0;
            break;
        }
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

/* ==========================================================================
 * CW / MORSE CODE BEACON GENERATOR
 * ========================================================================== */

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
        default: return NULL;
    }
}

/* Static packet buffers to guarantee zero stack overflow */
static uint8_t s_raw_frame[AX25_MAX_FRAME_SIZE + 64];
static uint8_t s_burst_scrambled[RADIO_FIXED_PACKET_LEN + 64];

static void CW_Prepare(void) {
    Radio.Standby();
    RBI_ConfigRFSwitch(RBI_SWITCH_OFF);
    SUBGRF_SetDioIrqParams(IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE);
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);
    SUBGRF_SetPacketType(PACKET_TYPE_GFSK);
    SUBGRF_SetRfFrequency(SatelliteProfile.txFrequency);
    SUBGRF_SetRfTxPower(RADIO_TX_POWER_DBM);
    SUBGRF_WriteRegister(REG_DRV_CTRL, 0x7 << 1);
}

static void CW_Carrier_On(void) {
    RBI_SetAmpMode(RBI_AMP_MODE_CW);
    SUBGRF_SetRfFrequency(SatelliteProfile.txFrequency);
    uint8_t paSelect = SUBGRF_SetRfTxPower(RADIO_TX_POWER_DBM);
    SUBGRF_WriteRegister(REG_DRV_CTRL, 0x7 << 1);
    SUBGRF_SetSwitch(paSelect, RFSWITCH_TX);
    SUBGRF_SetTxContinuousWave();
}

static void CW_Carrier_Off(void) {
    SUBGRF_SetStandby(STDBY_RC);
    RBI_ConfigRFSwitch(RBI_SWITCH_OFF);
}

static void CW_Finish(void) {
    CW_Carrier_Off();
    Radio.Standby();
    RBI_ConfigRFSwitch(RBI_SWITCH_OFF);
    RBI_SetAmpMode(RBI_AMP_MODE_GMSK);
    SUBGRF_SetDioIrqParams(IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE);
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);
    tx_busy = 0;
}

static void CW_Send_Char(char c, uint32_t unit_ms) {
    if (c == ' ') {
        CPU2_Delay_Ms(unit_ms * 4);
        return;
    }

    const char *morse = Get_Morse_Code(c);
    if (!morse) return;

    for (int i = 0; morse[i] != '\0'; i++) {
        CW_Carrier_On();
        if (morse[i] == '.') {
            CPU2_Delay_Ms(unit_ms);
        } else if (morse[i] == '-') {
            CPU2_Delay_Ms(unit_ms * 3);
        }
        CW_Carrier_Off();
        CPU2_Delay_Ms(unit_ms); /* Intra-character element gap */
    }

    CPU2_Delay_Ms(unit_ms * 2); /* Inter-character gap (total 3 units) */
}

static void CW_Send_String(const char *str, uint32_t unit_ms) {
    while (*str) {
        CW_Send_Char(*str, unit_ms);
        str++;
    }
}

/* ==========================================================================
 * 1-MINUTE CONTINUOUS CW MORSE TRANSMISSION SESSION
 * ========================================================================== */

static void Run_CW_1Min_Session(uint32_t cycle_count) {
    uart1_puts("\r\n============================================================\r\n");
    uart1_printf(">>> [CYCLE #%lu: STARTING 1-MINUTE CONTINUOUS CW SESSION] <<<\r\n", (unsigned long)cycle_count);
    uart1_printf(" Downlink Frequency : %lu.%03lu MHz | Power: +%d dBm (Low-Power PA - JC2)\r\n",
                 (unsigned long)(SatelliteProfile.txFrequency / 1000000UL),
                 (unsigned long)((SatelliteProfile.txFrequency % 1000000UL) / 1000UL),
                 RADIO_TX_POWER_DBM);
    uart1_puts(" Modulation         : Continuous Wave (CW / Carrier Keying)\r\n");
    uart1_puts(" Morse Rate         : 15 WPM (80 ms dot, 240 ms dash)\r\n");
    uart1_puts(" Session Duration   : 60 Seconds (1 Minute)\r\n");
    uart1_puts("============================================================\r\n");

    CW_Prepare();
    const uint32_t morse_unit_ms = 80;
    uint32_t cw_start_time = Get_Time_Ms();
    uint32_t cw_duration_ms = 60000;

    /* Initial 2-second tuning carrier tone */
    uart1_puts("[CW] Emitting 2.0s continuous carrier tone...\r\n");
    CW_Carrier_On();
    CPU2_Delay_Ms(2000);
    CW_Carrier_Off();
    CPU2_Delay_Ms(1000);

    uint32_t morse_iter = 0;
    while ((Get_Time_Ms() - cw_start_time) < cw_duration_ms) {
        morse_iter++;
        uint32_t elapsed_s = (Get_Time_Ms() - cw_start_time) / 1000;
        uart1_printf("[CW #%lu] Keying \"9NS2S2NEPAL\" (Elapsed: %lu s / 60 s)\r\n",
                     (unsigned long)morse_iter, (unsigned long)elapsed_s);

        CW_Send_String("9NS2S2NEPAL", morse_unit_ms);

        CPU2_Delay_Ms(1000);
        if ((Get_Time_Ms() - cw_start_time) >= cw_duration_ms) break;

        uart1_puts("[CW] Keying \"Namaste everyone, Testing GMSK signal \"\r\n");
        CW_Send_String("Namaste everyone, Testing GMSK signal ", morse_unit_ms);

        CPU2_Delay_Ms(2000);
    }

    CW_Finish();

    uart1_printf(">>> 1-MINUTE CW TRANSMISSION COMPLETE (Cycle #%lu, %lu beacon iterations) <<<\r\n",
                 (unsigned long)cycle_count, (unsigned long)morse_iter);
    uart1_puts("------------------------------------------------------------\r\n\r\n");
}

/* ==========================================================================
 * 1-MINUTE CONTINUOUS GMSK BURST TRANSMISSION SESSION (9600 bps)
 * ========================================================================== */

static void Run_GMSK_1Min_Burst_Session(uint32_t cycle_count) {
    uart1_puts("============================================================\r\n");
    uart1_printf(">>> [CYCLE #%lu: STARTING 1-MINUTE GMSK BURST SESSION] <<<\r\n", (unsigned long)cycle_count);
    uart1_printf(" Downlink Frequency : %lu.%03lu MHz | Power: +%d dBm (Low-Power PA - JC2)\r\n",
                 (unsigned long)(SatelliteProfile.txFrequency / 1000000UL),
                 (unsigned long)((SatelliteProfile.txFrequency % 1000000UL) / 1000UL),
                 RADIO_TX_POWER_DBM);
    uart1_puts(" Protocol           : AX.25 UI Frame + G3RUH Scrambler\r\n");
    uart1_printf(" Bitrate            : %d bps GMSK (matches GS)\r\n", RADIO_BIT_RATE_BPS);
    uart1_puts(" Session Duration   : 60 Seconds (1 Minute continuous burst)\r\n");
    uart1_printf(" Destination        : %s-%d | Source: %s-%d\r\n",
                 SatelliteProfile.destCallsign, SatelliteProfile.destSSID,
                 SatelliteProfile.sourceCallsign, SatelliteProfile.sourceSSID);
    uart1_puts("============================================================\r\n");

    Radio.Standby();
    SUBGRF_SetStandby(STDBY_RC);
    SUBGRF_SetPacketType(PACKET_TYPE_GFSK);
    RadioApp_ForceOpenGfskParams();

    const char *payload_text = "Namaste everyone, Testing GMSK signal ";
    uint16_t raw_len = 0;

    /* Assemble raw frame once for verification display */
    AX25_BuildFrame(s_raw_frame, sizeof(s_raw_frame), &raw_len,
                    SatelliteProfile.destCallsign, SatelliteProfile.destSSID,
                    SatelliteProfile.sourceCallsign, SatelliteProfile.sourceSSID,
                    (const uint8_t *)payload_text, (uint16_t)strlen(payload_text));

    if (raw_len > 0) {
        Print_AX25_Fields("GMSK BURST AX.25 PACKET TEMPLATE", s_raw_frame, raw_len);
    }

    /* Encode G3RUH scrambled on-the-air packet */
    uint16_t frame_size = Protocol_CreatePacket(s_burst_scrambled,
                                                (const uint8_t *)payload_text,
                                                (uint16_t)strlen(payload_text),
                                                &SatelliteProfile);

    if (frame_size == 0) {
        uart1_puts("ERROR: Failed to assemble AX.25 GMSK frame!\r\n");
        return;
    }

    uart1_printf("[GMSK] Frame size: %u bytes encoded. Starting continuous 1-minute burst...\r\n", frame_size);

    uint32_t burst_start_time = Get_Time_Ms();
    uint32_t burst_duration_ms = 60000;
    uint32_t sent_ok = 0;
    uint32_t sent_timeout = 0;
    uint32_t pkt_num = 0;

    while ((Get_Time_Ms() - burst_start_time) < burst_duration_ms) {
        pkt_num++;
        bool ok = Radio_Send_And_Wait(s_burst_scrambled, frame_size, 3000);
        if (ok) sent_ok++; else sent_timeout++;

        uint32_t elapsed_s = (Get_Time_Ms() - burst_start_time) / 1000;
        uart1_printf(" [GMSK TX #%lu] %s (Elapsed: %lu s / 60 s)\r\n",
                     (unsigned long)pkt_num, ok ? "sent OK" : "TIMEOUT", (unsigned long)elapsed_s);

        /* 300 ms interval between burst packets */
        CPU2_Delay_Ms(300);
    }

    Radio.Standby();
    RBI_ConfigRFSwitch(RBI_SWITCH_OFF);
    SUBGRF_SetDioIrqParams(IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE, IRQ_RADIO_NONE);
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);
    tx_busy = 0;

    uart1_printf(">>> 1-MINUTE GMSK BURST COMPLETE: %lu sent OK, %lu timeouts <<<\r\n",
                 (unsigned long)sent_ok, (unsigned long)sent_timeout);
    uart1_puts("============================================================\r\n\r\n");
}

/* ==========================================================================
 * MAIN ENTRY POINT - STM32WL55 CORTEX-M0+
 * ========================================================================== */

int main(void) {
    /* 1. Core architecture & peripheral clock init */
    SystemInit();
    HAL_Init();
    SysTick_Init_CPU2(48000000UL);
    uart1_init();

    CPU2_Delay_Ms(50); /* Allow UART line voltage to settle */

    /* 2. Structured M0+ Initialization Logs */
    uart1_puts("\r\n\r\n");
    uart1_puts("======================================================================\r\n");
    uart1_puts("     STM32WL55 CORTEX-M0+ SATELLITE RADIO SYSTEM INITIALIZING         \r\n");
    uart1_puts("======================================================================\r\n");
    uart1_puts("[INIT] 1. SystemCoreClock & HAL Base Initialized .......... [OK]\r\n");
    uart1_puts("[INIT] 2. CPU2 SysTick Timer Started (48 MHz) ............. [OK]\r\n");
    uart1_puts("[INIT] 3. USART1 Console Initialized (PA9 TX / PA10 RX @ 115200) .. [OK]\r\n");

    RBI_Init();
    uart1_puts("[INIT] 4. RF Front-End Switch & Power Driver Configured ... [OK]\r\n");

    MX_SUBGHZ_Init();
    HAL_NVIC_SetPriority(SUBGHZ_Radio_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(SUBGHZ_Radio_IRQn);
    __enable_irq();
    uart1_puts("[INIT] 5. Sub-GHz Radio (SX1262) & NVIC IRQs Enabled ...... [OK]\r\n");

    RadioEvents_t events = {
        .TxDone = OnTxDone, .TxTimeout = OnTxTimeout,
        .RxDone = OnRxDone, .RxTimeout = OnRxTimeout, .RxError = OnRxError
    };

    RadioApp_Init(&SatelliteProfile, &events);
    uart1_puts("[INIT] 6. Radio Application Layer Configured:              [OK]\r\n");
    uart1_printf("       - Downlink Frequency : %lu.%03lu MHz\r\n",
                 (unsigned long)(SatelliteProfile.txFrequency / 1000000UL),
                 (unsigned long)((SatelliteProfile.txFrequency % 1000000UL) / 1000UL));
    uart1_printf("       - Transmit RF Power  : +%d dBm (Low-Power PA - JC2)\r\n", RADIO_TX_POWER_DBM);
    uart1_printf("       - GMSK Bitrate       : %d bps (Fdev: %d Hz, BT: 0.5)\r\n",
                 RADIO_BIT_RATE_BPS, RADIO_FDEV_HZ);
    uart1_printf("       - Callsign Profile   : %s-%d -> %s-%d\r\n",
                 SatelliteProfile.sourceCallsign, SatelliteProfile.sourceSSID,
                 SatelliteProfile.destCallsign, SatelliteProfile.destSSID);
    uart1_puts("       - Supported Modes    : 1) CW Morse Beacon (15 WPM Keying)\r\n");
    uart1_puts("                              2) GMSK AX.25 UI Frame + G3RUH\r\n");
    uart1_puts("======================================================================\r\n");
    uart1_puts("[INIT COMPLETE] Starting Continuous Alternating 1-Min CW <---> 1-Min Burst...\r\n");
    uart1_puts("======================================================================\r\n\r\n");

    uint32_t cycle = 0;
    while (1) {
        cycle++;

        /* 1. Exactly 1 Minute Continuous CW Morse Beacon */
        Run_CW_1Min_Session(cycle);

        /* Guard interval between CW and Burst */
        CPU2_Delay_Ms(1000);

        /* 2. Exactly 1 Minute Continuous GMSK Burst Data Stream */
        Run_GMSK_1Min_Burst_Session(cycle);

        /* Guard interval between Burst and CW */
        CPU2_Delay_Ms(1000);
    }

    return 0;
}
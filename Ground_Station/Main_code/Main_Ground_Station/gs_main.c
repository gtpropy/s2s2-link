#include "gs_app.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "stm32wlxx_hal.h"
#include "radio.h"
#include "subghz.h"
#include "radio_driver.h"
#include "radio_board_if.h"
#include "uart/uart_debug.h"
#include "Mission/radio_app.h"
#include "Mission/protocol.h"
#include "protocol/ax25/ax25.h"
#include "protocol/g3ruh/g3ruh.h"

/* ============================================================================
 * FREQUENCIES
 * ========================================================================== */
#define GS_UPLINK_FREQ_HZ    437375000UL   /* GS TX -> satellite RX */
#define GS_DOWNLINK_FREQ_HZ  435000000UL   /* satellite TX -> SDR   */

/* Uplink burst: 2 packets per operator command.
 * Sending 2 packets trains the receiver's G3RUH descrambler and guarantees
 * that at least one frame is completely inside an RX buffer boundary. */
#define GS_SEND_COUNT         2          /* 2 packets per button/command */
#define GS_BURST_INTERVAL_MS  40UL       /* 40 ms spacing between burst packets */
#define GS_TX_WAIT_MS         3000UL

/* ============================================================================
 * CONFIGURATION
 * ========================================================================== */
const GroundStationConfig_t GroundStationDefaultConfig = GROUND_STATION_CONFIG_DEFAULT;
static GroundStationConfig_t s_config = GROUND_STATION_CONFIG_DEFAULT;

/* ============================================================================
 * STATE
 * ========================================================================== */
volatile uint8_t  rx_frame_buffer[AX25_MAX_FRAME_SIZE];
volatile uint16_t rx_frame_size = 0;
volatile uint8_t  rx_done_flag = 0;
volatile uint8_t  rx_timeout_flag = 0;
volatile uint8_t  rx_error_flag = 0;
volatile uint8_t  tx_busy = 0;

static uint32_t stat_cmd_transmitted = 0;
static uint32_t stat_tx_timeouts     = 0;

static bool     s_auto_cmd_enabled = false;
static uint32_t s_last_auto_cmd_time_ms = 0;

#define MCU_ID_OBC  0x01
#define MCU_ID_CAM  0x04

static const uint8_t OPCODE_FLASH[3] = { 0x1D, 0xD1, 0xF2 };

/* Exact command frames (same bytes as the working RX/TX version) */
static const uint8_t CMD_HK1[13]  = { 0x53, 0x01, 0x1D, 0xD1, 0xF2, 0x00, 0xF2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05 };
static const uint8_t CMD_HK2[13]  = { 0x53, 0x01, 0x1D, 0xD2, 0xF2, 0x00, 0xF2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05 };
static const uint8_t CMD_PING[13] = { 0x53, MCU_ID_OBC, 0x1D, 0xD1, 0xF5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t CMD_CAMON[13]   = CMD_CAM_ON_COMMAND;
static const uint8_t CMD_CAMDOWN[13] = CMD_CAM_DOWNLOAD_COMMAND;
static const uint8_t CMD_ADCS[13]    = CMD_ADCD_COMMAND;
static const uint8_t CMD_EPDM[13]    = CMD_EPDM_COMMAND;

static uint8_t s_tx_frame[AX25_MAX_FRAME_SIZE];
static uint8_t s_hex_buf[AX25_MAX_FRAME_SIZE];

/* ============================================================================
 * RADIO IRQ LOCK
 * ========================================================================== */
static inline void GS_Radio_Lock(void)
{
    NVIC_DisableIRQ(SUBGHZ_Radio_IRQn);
    __DSB(); __ISB();
}

static inline void GS_Radio_Unlock(void)
{
    __DSB(); __ISB();
    NVIC_EnableIRQ(SUBGHZ_Radio_IRQn);
}

/* ============================================================================
 * RADIO CALLBACKS (ISR: latch only)
 * ========================================================================== */
static void OnTxDone(void)    { tx_busy = 0; }
static void OnTxTimeout(void) { tx_busy = 0; }
static void OnRxDone(uint8_t *p, uint16_t s, int16_t r, int8_t n) { (void)p; (void)s; (void)r; (void)n; }
static void OnRxTimeout(void) {}
static void OnRxError(void)   {}

static RadioEvents_t s_RadioEvents =
{
    .TxDone    = OnTxDone,
    .RxDone    = OnRxDone,
    .TxTimeout = OnTxTimeout,
    .RxTimeout = OnRxTimeout,
    .RxError   = OnRxError
};

static void GS_Radio_Idle(void)
{
    GS_Radio_Lock();
    Radio.Standby();
    SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);
    GS_Radio_Unlock();
}

/* RF switch pin states. Expected during RFO_HP TX: PA8=1 PC3=1 PC4=0 PC5=1 */
static void GS_Print_RF_Pins(void)
{
    uart2_printf("  [RF-PINS] PA8(PAEN)=%d PC3(CTRL3)=%d PC4(CTRL1)=%d PC5(CTRL2)=%d  (HP TX = 1,1,0,1)\r\n",
                 (int)HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8),
                 (int)HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_3),
                 (int)HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_4),
                 (int)HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_5));
}

/* ============================================================================
 * HARDWARE
 * ========================================================================== */
extern void SysTick_Init_CPU2(uint32_t sys_freq_hz);
extern void MX_SUBGHZ_Init(void);
extern void CPU2_Delay_Ms(uint32_t ms);

uint32_t GS_GetTimeMs(void) { return HAL_GetTick(); }
void GS_DelayMs(uint32_t ms) { CPU2_Delay_Ms(ms); }

/* ============================================================================
 * DEBUG PRINT HELPERS
 * ========================================================================== */
static void Print_Hex_Bytes(const uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        uart2_printf("%02X ", buf[i]);
        if ((i + 1) % 16 == 0) uart2_puts("\r\n");
    }
    if (len % 16 != 0) uart2_puts("\r\n");
}

static void Print_Payload_ASCII(const uint8_t *buf, uint16_t start, uint16_t end)
{
    if (end <= start) { uart2_puts("Payload (text) : (none)\r\n"); return; }
    char line[112];
    uint16_t n = (uint16_t)(end - start);
    uint16_t max_chars = (uint16_t)(sizeof(line) - 24);
    if (n > max_chars) n = max_chars;
    uint16_t pos = 0;
    pos += (uint16_t)snprintf(&line[pos], sizeof(line) - pos, "Payload (text): \"");
    for (uint16_t i = 0; i < n; i++) {
        uint8_t c = buf[start + i];
        line[pos++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    pos += (uint16_t)snprintf(&line[pos], sizeof(line) - pos, "\"\r\n");
    uart2_puts(line);
}

static void Print_AX25_Fields(const char *label, const uint8_t *buf, uint16_t len)
{
    uart2_printf("\r\n--- %s FIELD BREAKDOWN (%u bytes) ---\r\n", label, len);
    if (len < 20) { uart2_puts("(too short)\r\n"); return; }

    uart2_printf("Start Flag : %02X %s\r\n", buf[0], (buf[0] == AX25_FLAG) ? "(OK, 0x7E)" : "(MISMATCH)");

    char dest_cs[7], src_cs[7];
    AX25_DecodeAddress(&buf[1], dest_cs);
    AX25_DecodeAddress(&buf[8], src_cs);
    uart2_printf("Dest       : \"%s\" (SSID %d)\r\n", dest_cs, AX25_DecodeSSID(&buf[1]));
    uart2_printf("Src        : \"%s\" (SSID %d)\r\n", src_cs, AX25_DecodeSSID(&buf[8]));
    uart2_printf("Control    : 0x%02X\r\n", buf[15]);
    uart2_printf("PID        : 0x%02X\r\n", buf[16]);

    uint16_t ps = 17, pe = (uint16_t)(len - 3);
    if (pe > ps) {
        uart2_printf("Payload    : %u bytes\r\n", (unsigned)(pe - ps));
        Print_Hex_Bytes(&buf[ps], (uint16_t)(pe - ps));
        Print_Payload_ASCII(buf, ps, pe);
    } else {
        uart2_puts("Payload    : (none)\r\n");
    }
    uart2_printf("FCS/CRC    : %02X %02X\r\n", buf[len - 3], buf[len - 2]);
    uart2_printf("End Flag   : %02X %s\r\n", buf[len - 1], (buf[len - 1] == AX25_FLAG) ? "(OK, 0x7E)" : "(MISMATCH)");
}

/* ============================================================================
 * INIT
 * ========================================================================== */
void GS_Init_VectorTable(void)
{
    SCB->VTOR = GS_CPU2_VECTOR_TABLE_ADDR;
    __DSB(); __ISB();
}

void GS_Init_PeripheralClocks(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_SUBGHZ_CLK_ENABLE();

    RCC->C2AHB2ENR |= RCC_C2AHB2ENR_GPIOAEN | RCC_C2AHB2ENR_GPIOBEN | RCC_C2AHB2ENR_GPIOCEN;
}

void GS_Init_IPCC_Isolation(void)
{
#ifdef IPCC_C2_RX_C2_TX_IRQn
    HAL_NVIC_DisableIRQ(IPCC_C2_RX_C2_TX_IRQn);
#endif
}

void GS_Hardware_Init(void)
{
    SystemInit();
    GS_Init_VectorTable();      /* AFTER SystemInit() */

    HAL_Init();
    SysTick_Init_CPU2(HAL_RCC_GetHCLK2Freq());

    GS_Init_PeripheralClocks();
    GS_Init_IPCC_Isolation();

    uart2_init();

    RBI_Init();
    RBI_ConfigRFSwitch(GS_CFG_DEFAULT_RF_SWITCH);

    MX_SUBGHZ_Init();

    HAL_NVIC_SetPriority(SUBGHZ_Radio_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(SUBGHZ_Radio_IRQn);

    __enable_irq();

    if (s_config.uartSettleDelayMs > 0)
        CPU2_Delay_Ms(s_config.uartSettleDelayMs);
}

void GS_Init(const GroundStationConfig_t *config)
{
    if (config != NULL) s_config = *config;
    else                s_config = GroundStationDefaultConfig;

    /* Force frequencies BEFORE RadioApp_Init() reads them */
    s_config.radio.txFrequency = GS_UPLINK_FREQ_HZ;
    s_config.radio.rxFrequency = GS_DOWNLINK_FREQ_HZ;

    RadioApp_Init(&s_config.radio, &s_RadioEvents);

    /* Force high-power PA path (RFO_HP, +22 dBm) and matching RF switch */
    RadioApp_SetPaSelect(RFO_HP);
    RadioApp_SetTxPower(22);
    GS_SetRFSwitch(RBI_SWITCH_RFO_HP);

    s_last_auto_cmd_time_ms = GS_GetTimeMs();

    uart2_puts("\r\n========================================================================\r\n");
    uart2_puts("  ANTARIKCHYA PRATISTHAN NEPAL\r\n");
    uart2_puts("  STM32WL55 GROUND STATION - GMSK UPLINK TELECOMMAND CONSOLE (TX ONLY)\r\n");
    uart2_puts("========================================================================\r\n");
    uart2_printf("  Uplink TX Frequency   : %lu Hz\r\n", (unsigned long)s_config.radio.txFrequency);
    uart2_printf("  Transmit RF Power     : %+d dBm (%s)\r\n",
                 (int)RadioApp_GetTxPower(),
                 (RadioApp_GetPaSelect() == RFO_HP) ? "RFO_HP High Power" : "RFO_LP Low Power");
    uart2_printf("  HCLK2                 : %lu Hz\r\n", (unsigned long)HAL_RCC_GetHCLK2Freq());
    uart2_puts("  Satellite downlink    : CW only (receive with SDR, not decoded here)\r\n");
    uart2_puts("========================================================================\r\n\r\n");

    GS_Radio_Idle();
}

void GS_SetConfig(const GroundStationConfig_t *config) { if (config != NULL) s_config = *config; }
const GroundStationConfig_t *GS_GetConfig(void) { return &s_config; }

void GS_SetRFSwitch(RBI_Switch_TypeDef rf_switch)
{
    s_config.rfSwitchConfig = rf_switch;
    RBI_ConfigRFSwitch(rf_switch);
}

RBI_Switch_TypeDef GS_GetRFSwitch(void) { return s_config.rfSwitchConfig; }

void GS_CW_Poll(void) {}

/* ============================================================================
 * UPLINK BURST
 * ========================================================================== */
/* ============================================================================
 * GS_Send_Burst_Packet – single-shot AX.25/G3RUH uplink.
 *
 * burst_count is accepted for API compatibility but clamped to 1.
 * ONE command click MUST produce exactly ONE on-air packet so the
 * satellite M0+ command pipeline is never confused by duplicates.
 * ========================================================================== */
bool GS_Send_Burst_Packet(const uint8_t *payload, uint16_t len, uint8_t burst_count)
{
    if (payload == NULL || len == 0) return false;
    /* Hard-clamp: never send more than GS_SEND_COUNT = 1 frame per call. */
    burst_count = GS_SEND_COUNT;

    uint16_t tx_frame_len = Protocol_CreatePacket(s_tx_frame, payload, len, &s_config.radio);
    if (tx_frame_len == 0) {
        uart2_puts("!!! [TX ERROR] Protocol_CreatePacket failed\r\n");
        return false;
    }

    /* Show unscrambled AX.25 frame for debug */
    uint8_t ax25_unscrambled[AX25_MAX_FRAME_SIZE];
    uint16_t ax25_len = Protocol_GetLastTxAx25Frame(ax25_unscrambled, sizeof(ax25_unscrambled));
    if (ax25_len > 0) {
        uart2_printf("\r\n>>> [AX.25 TX FRAME (%u Bytes, Unscrambled)] <<<\r\n", ax25_len);
        Print_Hex_Bytes(ax25_unscrambled, ax25_len);
        Print_AX25_Fields("TX AX.25 FRAME", ax25_unscrambled, ax25_len);
    }

    uart2_printf(
        "\r\n=======================================================\r\n"
        ">>> [UPLINK-TX] %u PACKET(S) @ %lu.%03lu MHz (%s %+d dBm) <<<\r\n"
        "=======================================================\r\n",
        (unsigned int)burst_count,
        (unsigned long)(s_config.radio.txFrequency / 1000000UL),
        (unsigned long)((s_config.radio.txFrequency % 1000000UL) / 1000UL),
        (RadioApp_GetPaSelect() == RFO_HP) ? "RFO_HP" : "RFO_LP",
        (int)RadioApp_GetTxPower());

    for (uint8_t i = 0; i < burst_count; i++) {
        GS_Radio_Idle();         /* standby + clear IRQs, re-enable */

        tx_busy = 1;             /* set BEFORE send so TxDone ISR can clear it */
        stat_cmd_transmitted++;
        RadioApp_Send(s_tx_frame, tx_frame_len);
        GS_Print_RF_Pins();      /* verify RF switch in HP TX state */

        uint32_t cnt = GS_TX_WAIT_MS;
        while (tx_busy != 0 && cnt > 0) {
            CPU2_Delay_Ms(1);
            cnt--;
        }

        if (tx_busy != 0) {
            uart2_puts("TX TIMEOUT\r\n");
            stat_tx_timeouts++;
            GS_Radio_Idle();
            tx_busy = 0;
        }

        uart2_printf("  [%u/%u] Uplink packet sent\r\n", (unsigned)(i + 1), (unsigned)burst_count);
        if (i + 1 < burst_count) {
            CPU2_Delay_Ms(GS_BURST_INTERVAL_MS);
        }
    }

    GS_Radio_Idle();
    uart2_puts(">>> UPLINK COMPLETE <<<\r\n");
    return true;
}

bool GS_Send_Packet(const uint8_t *payload, uint16_t len)
{
    return GS_Send_Burst_Packet(payload, len, 1);
}

/* ============================================================================
 * TELECOMMAND
 * ========================================================================== */
bool GS_Send_Telecommand(uint8_t mcu_id, const uint8_t opcode[3],
                         uint16_t data_id, uint32_t addr, uint16_t count)
{
    uint8_t cmd[13];

    cmd[0]  = 0x53;
    cmd[1]  = mcu_id;
    cmd[2]  = opcode[0];
    cmd[3]  = opcode[1];
    cmd[4]  = opcode[2];
    cmd[5]  = (uint8_t)((data_id >> 8) & 0xFF);
    cmd[6]  = (uint8_t)(data_id & 0xFF);
    cmd[7]  = (uint8_t)((addr >> 24) & 0xFF);
    cmd[8]  = (uint8_t)((addr >> 16) & 0xFF);
    cmd[9]  = (uint8_t)((addr >> 8) & 0xFF);
    cmd[10] = (uint8_t)(addr & 0xFF);
    cmd[11] = (uint8_t)((count >> 8) & 0xFF);
    cmd[12] = (uint8_t)(count & 0xFF);

    return GS_Send_Burst_Packet(cmd, sizeof(cmd), GS_SEND_COUNT);
}

/* Kept for API compatibility. Maps to the SAME frames the console uses. */
bool GS_Send_Telecommand_ByName(const char *name, uint16_t count)
{
    if (strcmp(name, "HK1") == 0)
        return GS_Send_Burst_Packet(CMD_HK1, sizeof(CMD_HK1), GS_SEND_COUNT);
    if (strcmp(name, "HK2") == 0)
        return GS_Send_Burst_Packet(CMD_HK2, sizeof(CMD_HK2), GS_SEND_COUNT);
    if (strcmp(name, "CAM") == 0)
        return GS_Send_Burst_Packet(CMD_CAMDOWN, sizeof(CMD_CAMDOWN), GS_SEND_COUNT);
    if (strcmp(name, "SNAP") == 0)
        return GS_Send_Burst_Packet(CMD_CAMON, sizeof(CMD_CAMON), GS_SEND_COUNT);
    if (strcmp(name, "PING") == 0)
        return GS_Send_Burst_Packet(CMD_PING, sizeof(CMD_PING), GS_SEND_COUNT);
    if (strcmp(name, "FLASH") == 0 || strcmp(name, "DOWNLINK") == 0 || strcmp(name, "D") == 0)
        return GS_Send_Telecommand(MCU_ID_OBC, OPCODE_FLASH, 0x00F2, 0x00000000, count ? count : 5);
    return false;
}

/* ============================================================================
 * AUTOMATION
 * ========================================================================== */
void GS_Auto_Send_Command(void)
{
    if (!s_auto_cmd_enabled) return;

    uint32_t now = GS_GetTimeMs();
    if ((now - s_last_auto_cmd_time_ms) >= s_config.autoCmdIntervalMs)
    {
        s_last_auto_cmd_time_ms = now;
        uart2_puts(">>> [AUTO-CMD] Triggering Scheduled Telecommand <<<\r\n");
        GS_Send_Burst_Packet(CMD_HK1, sizeof(CMD_HK1), GS_SEND_COUNT);
    }
}

/* ============================================================================
 * HELP / STATUS
 * ========================================================================== */
void GS_Print_Help(void)
{
    uart2_puts("\r\n--- GROUND STATION COMMANDS (uplink only, 1 packet per command) ---\r\n");
    uart2_puts("  1 / HK / HK1    : HK1 request\r\n");
    uart2_puts("  2 / HK2         : HK2 request\r\n");
    uart2_puts("  3 / CAM ON/SNAP : Camera ON / run mission\r\n");
    uart2_puts("  5 / CD / CAM    : Camera download\r\n");
    uart2_puts("  ADCS | EPDM     : Subsystem commands\r\n");
    uart2_puts("  4 / D [cnt]     : Flash data request (0xF2)\r\n");
    uart2_puts("  P / PING        : Ping OBC\r\n");
    uart2_puts("  BURST           : Telemetry burst request (0x01)\r\n");
    uart2_puts("  S               : Status\r\n");
    uart2_puts("  AUTO ON|OFF     : Periodic HK telecommand\r\n");
    uart2_puts("  HEX <bytes...>  : Transmit raw bytes (burst)\r\n");
    uart2_puts("  PA LP|HP        : Select PA\r\n");
    uart2_puts("  PWR <dbm>       : Set TX power\r\n");
    uart2_puts("  CARRIER <sec>   : Test carrier on uplink frequency\r\n");
    uart2_puts("---------------------------------------------\r\n");
}

void GS_Print_Status(void)
{
    uart2_puts("\r\n--- GROUND STATION STATUS ---\r\n");
    uart2_printf("  TX Frequency : %lu Hz\r\n", (unsigned long)s_config.radio.txFrequency);
    uart2_printf("  TX Power     : %+d dBm (%s)\r\n", (int)RadioApp_GetTxPower(),
                 (RadioApp_GetPaSelect() == RFO_HP) ? "RFO_HP" : "RFO_LP");
    uart2_printf("  TX Sent      : %lu | TX Timeouts: %lu\r\n",
                 (unsigned long)stat_cmd_transmitted, (unsigned long)stat_tx_timeouts);
    uart2_printf("  Auto-Command : %s\r\n", s_auto_cmd_enabled ? "ENABLED" : "DISABLED");
    uart2_puts("-----------------------------\r\n");
}

/* ============================================================================
 * COMMAND HANDLER  (case-insensitive: matches on an upper-cased copy)
 * ========================================================================== */
#define SEND13(arr)  GS_Send_Burst_Packet((arr), 13, GS_SEND_COUNT)

void GS_Handle_Command_Line(const char *p)
{
    char u[CMD_LINE_MAX];
    size_t n = strlen(p);
    if (n >= sizeof(u)) n = sizeof(u) - 1;
    for (size_t i = 0; i < n; i++) u[i] = (char)toupper((unsigned char)p[i]);
    u[n] = '\0';

    if (strcmp(u, "1") == 0 || strcmp(u, "HK") == 0 || strcmp(u, "HK1") == 0) {
        uart2_puts(">>> [UPLINK] HK1 request\r\n");
        SEND13(CMD_HK1);
    }
    else if (strcmp(u, "2") == 0 || strcmp(u, "HK2") == 0) {
        uart2_puts(">>> [UPLINK] HK2 request\r\n");
        SEND13(CMD_HK2);
    }
    else if (strcmp(u, "3") == 0 || strcmp(u, "CAM ON") == 0 || strcmp(u, "CAMON") == 0 ||
             strcmp(u, "SNAP") == 0) {
        uart2_puts(">>> [UPLINK] CAM ON / RUN MISSION\r\n");
        SEND13(CMD_CAMON);
    }
    /* Dummy camera test */
    else if (strcmp(u, "DUMMYCAM") == 0 || strcmp(u, "DC") == 0 ||
             strcmp(u, "TESTCAM") == 0  || strcmp(u, "TC") == 0 ||
             strcmp(u, "DUMMY") == 0    || strcmp(u, "DUMMY CAM") == 0 ||
             strcmp(u, "DUMMY CAMERA") == 0 || strcmp(u, "DUMMY CAMERA DOWNLOAD") == 0 ||
             strcmp(u, "DUMMY CAM DOWNLOAD") == 0) {
        uart2_puts(">>> [UPLINK] DUMMY CAMERA TEST (stored test image downlink)\r\n");
        uint8_t dummy_cmd[13] = { 0x53, 0x04, 0xCC, 0x5E, 0xBE,
                                  0x00, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x45 };
        SEND13(dummy_cmd);
    }
    else if (strcmp(u, "5") == 0 || strcmp(u, "CD") == 0 || strcmp(u, "CAM") == 0 ||
             strcmp(u, "CAMERA") == 0 || strcmp(u, "CAM DOWN") == 0 ||
             strcmp(u, "CAMDOWN") == 0 || strcmp(u, "CAM DOWNLOAD") == 0) {
        uart2_puts(">>> [UPLINK] CAM DOWNLOAD\r\n");
        SEND13(CMD_CAMDOWN);
    }
    else if (strcmp(u, "ADCS") == 0) {
        uart2_puts(">>> [UPLINK] ADCS\r\n");
        SEND13(CMD_ADCS);
    }
    else if (strcmp(u, "EPDM") == 0) {
        uart2_puts(">>> [UPLINK] EPDM\r\n");
        SEND13(CMD_EPDM);
    }
    else if (strcmp(u, "4") == 0 || strncmp(u, "D ", 2) == 0 || strcmp(u, "D") == 0 ||
             strncmp(u, "DOWNLINK", 8) == 0 || strncmp(u, "FLASH", 5) == 0)
    {
        const char *arg = u;
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;
        uint16_t cnt = 5;
        if (*arg != '\0') {
            cnt = (uint16_t)strtoul(arg, NULL, 0);
            if (cnt == 0) cnt = 5;
        }
        uart2_printf(">>> [UPLINK] Flash data request (%u chunks via 0xF2)\r\n", cnt);
        uint8_t flash_cmd[13] = { 0x53, MCU_ID_OBC, 0x1D, 0xD1, 0xF2, 0x00, 0xF2,
                                  0x00, 0x00, 0x00, 0x00,
                                  (uint8_t)(cnt >> 8), (uint8_t)(cnt & 0xFF) };
        SEND13(flash_cmd);
    }
    else if (strcmp(u, "P") == 0 || strcmp(u, "PING") == 0) {
        uart2_puts(">>> [UPLINK] PING\r\n");
        SEND13(CMD_PING);
    }
    else if (strcmp(u, "COMMAND") == 0 || strcmp(u, "BURST") == 0) {
        uint8_t burst_cmd = 0x01;
        uart2_puts(">>> [UPLINK] BURST REQUEST (0x01)\r\n");
        GS_Send_Burst_Packet(&burst_cmd, 1, GS_SEND_COUNT);
    }
    else if (strcmp(u, "S") == 0) {
        GS_Print_Status();
    }
    else if (strcmp(u, "?") == 0 || strcmp(u, "HELP") == 0) {
        GS_Print_Help();
    }
    else if (strncmp(u, "CARRIER", 7) == 0 || strncmp(u, "CW ", 3) == 0 || strncmp(u, "TONE ", 5) == 0)
    {
        const char *num_str = u;
        while (*num_str && *num_str != ' ') num_str++;
        while (*num_str == ' ') num_str++;
        uint32_t sec = strtoul(num_str, NULL, 10);
        if (sec == 0) sec = 5;
        uart2_printf(">>> [CARRIER] %lu s @ %lu Hz\r\n", (unsigned long)sec,
                     (unsigned long)s_config.radio.txFrequency);
        RadioApp_TransmitCarrier(s_config.radio.txFrequency, sec);
        GS_Radio_Idle();
    }
    else if (strcmp(u, "RF") == 0) {
        GS_Print_RF_Pins();
    }
    else if (strcmp(u, "PA LP") == 0) {
        RadioApp_SetPaSelect(RFO_LP);
        RadioApp_SetTxPower(14);
        GS_SetRFSwitch(RBI_SWITCH_RFO_LP);
        uart2_puts(">>> PA set to RFO_LP (+14 dBm) <<<\r\n");
    }
    else if (strcmp(u, "PA HP") == 0) {
        RadioApp_SetPaSelect(RFO_HP);
        RadioApp_SetTxPower(22);
        GS_SetRFSwitch(RBI_SWITCH_RFO_HP);
        uart2_puts(">>> PA set to RFO_HP (+22 dBm, JC2 only) <<<\r\n");
    }
    else if (strncmp(u, "PWR ", 4) == 0 || strncmp(u, "POWER ", 6) == 0)
    {
        const char *arg = u;
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;
        RadioApp_SetTxPower((int8_t)atoi(arg));
        uart2_printf(">>> Transmit Power set to %+d dBm <<<\r\n", (int)RadioApp_GetTxPower());
    }
    else if (strncmp(u, "AUTO ", 5) == 0) {
        s_auto_cmd_enabled = (strcmp(u + 5, "ON") == 0);
        uart2_printf("Auto-Command %s\r\n", s_auto_cmd_enabled ? "ENABLED" : "DISABLED");
    }
    else if (strncmp(u, "HEX ", 4) == 0 || strncmp(u, "HHEX ", 5) == 0 || strncmp(u, "HHHEX ", 6) == 0)
    {
        const char *hex_str = (strncmp(u, "HHHEX ", 6) == 0) ? (u + 6) :
                              (strncmp(u, "HHEX ", 5) == 0) ? (u + 5) : (u + 4);
        uint16_t hex_len = 0;
        char byte_str[3] = {0};

        while (*hex_str && hex_len < sizeof(s_hex_buf)) {
            while (*hex_str == ' ') hex_str++;
            if (!*hex_str) break;
            byte_str[0] = *hex_str++;
            byte_str[1] = *hex_str ? *hex_str++ : '0';
            s_hex_buf[hex_len++] = (uint8_t)strtoul(byte_str, NULL, 16);
        }

        if ((hex_len == 13 || hex_len == 8) && s_hex_buf[0] == 0x53) {
            uart2_printf(">>> [UPLINK] Transmitting %u-byte Telecommand (Opcode 0x%02X)\r\n", hex_len, s_hex_buf[1]);
            GS_Send_Burst_Packet(s_hex_buf, hex_len, GS_SEND_COUNT);
        } else if (hex_len == 1 && (s_hex_buf[0] == 0x01 || s_hex_buf[0] == 0x53)) {
            uart2_printf(">>> [UPLINK] Transmitting Burst Request (0x%02X)\r\n", s_hex_buf[0]);
            GS_Send_Burst_Packet(s_hex_buf, hex_len, GS_SEND_COUNT);
        } else {
            uart2_printf("!!! [BLOCKED] Payload of %u bytes is NOT a valid Telecommand. Communication is restricted to valid Telecommands (13B starting with 0x53) only!\r\n", hex_len);
        }
    }
    else {
        const char *hex_str = u;
        uint16_t hex_len = 0;
        char byte_str[3] = {0};
        while (*hex_str && hex_len < sizeof(s_hex_buf)) {
            while (*hex_str == ' ') hex_str++;
            if (!*hex_str) break;
            if (!isxdigit((int)*hex_str)) { hex_len = 0; break; }
            byte_str[0] = *hex_str++;
            if (!isxdigit((int)*hex_str)) { hex_len = 0; break; }
            byte_str[1] = *hex_str++;
            s_hex_buf[hex_len++] = (uint8_t)strtoul(byte_str, NULL, 16);
        }
        if ((hex_len == 13 || hex_len == 8) && s_hex_buf[0] == 0x53) {
            uart2_printf(">>> [UPLINK] Transmitting %u-byte Telecommand (Opcode 0x%02X)\r\n", hex_len, s_hex_buf[1]);
            GS_Send_Burst_Packet(s_hex_buf, hex_len, GS_SEND_COUNT);
        } else {
            uart2_printf("!!! [BLOCKED] \"%s\" is not a valid Telecommand. AX.25 G3RUH communication strictly allows only valid Telecommands (13B starting with 0x53)!\r\n", p);
        }
    }
}

/* ============================================================================
 * CONSOLE
 * ========================================================================== */
bool GS_Console_TryReadLine(char *out, uint8_t max)
{
    static char buffer[CMD_LINE_MAX];
    static uint8_t index = 0;
    uint8_t c;

    while (uart2_try_getc(&c)) {
        if (c == '\b' || c == 0x7F) {
            if (index > 0) { index--; }
            continue;
        }

        if (c == '\r' || c == '\n') {
            if (index == 0) continue;
            uint8_t copy_len = (index < (max - 1)) ? index : (uint8_t)(max - 1);
            memcpy(out, buffer, copy_len);
            out[copy_len] = '\0';
            index = 0;
            return true;
        }

        if (c >= 32 && c <= 126) {
            if (index < (sizeof(buffer) - 1)) {
                buffer[index++] = (char)c;
            }
        }
    }
    return false;
}

/* ==========================================================================
 * MAIN - STM32WL55 Ground Station (CPU2), uplink TX only @ 437.375 MHz
 * ========================================================================== */
int main(void)
{
    GS_Hardware_Init();
    GS_Init(NULL);

    char line_buf[CMD_LINE_MAX];

    uart2_puts("Ground Station Ready (uplink TX only, 437.375 MHz).\r\n");
    uart2_puts("Type '?' or 'help' for command list.\r\n> ");

    while (1)
    {
        GS_Auto_Send_Command();

        if (GS_Console_TryReadLine(line_buf, sizeof(line_buf)))
        {
            char *p = line_buf;
            while (*p == ' ' || *p == '\t') p++;

            if (*p != '\0')
                GS_Handle_Command_Line(p);

            uart2_puts("> ");
        }
    }

    return 0;
}
/*
 * STM32WL55 Ground Station
 *
 * GMSK physical layer, S2S2 header/payload carried inside real AX.25 UI
 * frames, each independently wrapped in FX.25 RS(255,223) FEC. See
 * core/Inc/cubus_ax25.h for why (interoperability with any AX.25/FX.25-
 * aware ground station, e.g. SatNOGS/gr-satellites/Direwolf, at the cost
 * of cross-fragment interleaving and AX.25's per-frame address overhead).
 *
 * TX (uplink):   437.375 MHz
 * RX (downlink): 435.000 MHz
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-21
*/

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stm32wlxx_hal.h"

#include "radio.h"
#include "subghz.h"
#include "radio_driver.h"
#include "radio_board_if.h"

#include "uart_debug.h"

#include "config.h"
#include "radio_app.h"
#include "s2s2com.h"
#include "s2s2_commands.h"
#include "s2s2_auth.h"
#include "cubus_ax25.h"

/*
=========================================================
GROUND STATION RADIO PROFILE
=========================================================
*/

const RadioConfig_t GroundStationProfile =
{
    /* Ground station transmit frequency, satellite uplink receiver */
    .txFrequency = 437375000UL,

    /* Ground station receive frequency, satellite downlink transmitter */
    .rxFrequency = 435000000UL,

    .nodeId = NODE_ID_GROUND,
    .peerId = NODE_ID_SATELLITE
};

static const cubus_ax25_link_t s_link = {
    .dest = { .callsign = AX25_SATELLITE_CALLSIGN, .ssid = AX25_SATELLITE_SSID },
    .src  = { .callsign = AX25_GROUND_CALLSIGN, .ssid = AX25_GROUND_SSID },
};



/*
=========================================================
SYSTEM CONFIGURATION
=========================================================
*/

#define CMD_LINE_MAX        1400 /* "TX " + up to MAX_PAYLOAD_LEN (659) bytes hex-encoded (1318 chars) + margin */
#define TX_TIMEOUT_MS       3000
#define RX_SESSION_MS       60000

#define RX_HEARTBEAT_MS     3000

#define GAP_REPORT_MAX_ENTRIES  256  /* fits well inside MAX_PAYLOAD_LEN (2 bytes/entry) */



/*
=========================================================
GLOBAL RADIO FLAGS
=========================================================
*/

volatile uint8_t tx_busy = 0;

volatile uint8_t rx_chunk_buffer[FX25_FRAME_LEN];
volatile uint16_t rx_chunk_size = 0;

volatile uint8_t rx_done_flag = 0;
volatile uint8_t rx_timeout_flag = 0;
volatile uint8_t rx_error_flag = 0;

static cubus_ax25_reassembler_t s_reassembler;
static gap_tracker_t s_gap_tracker;



/*
=========================================================
RX SESSION STATS
=========================================================
*/

static uint32_t stat_packets_ok = 0;
static uint32_t stat_packets_bad = 0;
static uint32_t stat_packets_misaddressed = 0;



/*
=========================================================
FORWARD DECLARATIONS
=========================================================
*/

static void Print_Hex_Bytes(const uint8_t *buf, uint16_t len);
static void Print_Payload_ASCII(const uint8_t *buf, uint16_t len);
static void Print_Reassembled_Fields(const char *label, const cubus_ax25_reassembler_t *r);
static void Process_Received_Frame(void);
static void Send_Gap_Report(void);
static void Listen_For_Response(uint32_t timeout_ms);



/*
=========================================================
DEBUG PRINT HELPERS
=========================================================
*/

static void Print_Hex_Bytes(const uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        uart2_printf("%02X ", buf[i]);
        if ((i + 1) % 16 == 0) uart2_puts("\r\n");
    }
    if (len % 16 != 0) uart2_puts("\r\n");
}

static void Print_Payload_ASCII(const uint8_t *buf, uint16_t len)
{
    if (len == 0)
    {
        uart2_puts("Payload (text) : (none)\r\n");
        return;
    }

    char line[112];
    uint16_t max_chars = (uint16_t)(sizeof(line) - 24);
    uint16_t n = (len > max_chars) ? max_chars : len;

    uint16_t pos = 0;
    pos += (uint16_t)snprintf(&line[pos], sizeof(line) - pos, "Payload (text): \"");
    for (uint16_t i = 0; i < n; i++)
    {
        uint8_t c = buf[i];
        line[pos++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    pos += (uint16_t)snprintf(&line[pos], sizeof(line) - pos, "\"\r\n");
    uart2_puts(line);
}

extern volatile int16_t rx_last_rssi;
extern volatile int8_t rx_last_snr;

static void Print_Reassembled_Fields(const char *label, const cubus_ax25_reassembler_t *r)
{
    uart2_printf("\r\n--- %s (%u byte payload) ---\r\n", label, r->payload_len);
    uart2_printf(
        "version=%u type=%u src=%u dst=%u seq=%u session=0x%04X flags=0x%02X "
        "rs_corrected=%d rssi=%d dBm snr=%d dB\r\n",
        r->version, r->type, r->src_id, r->dst_id, r->seq_num,
        r->session_id, r->flags, r->rs_bytes_corrected,
        (int)rx_last_rssi, (int)rx_last_snr);
    Print_Hex_Bytes(r->payload, r->payload_len);
    Print_Payload_ASCII(r->payload, r->payload_len);
}



/*
=========================================================
RADIO CALLBACK FUNCTIONS
=========================================================
*/

void OnTxDone(void)
{
    tx_busy = 0;
}

void OnTxTimeout(void)
{
    tx_busy = 0;
}

void OnRxTimeout(void)
{
    rx_timeout_flag = 1;
}

void OnRxError(void)
{
    rx_error_flag = 1;
}

volatile int16_t rx_last_rssi = 0;
volatile int8_t rx_last_snr = 0;

void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr)
{
    rx_last_rssi = rssi;
    rx_last_snr = snr;
    uint16_t copy_len = (size > FX25_FRAME_LEN) ? FX25_FRAME_LEN : size;
    memcpy((void *)rx_chunk_buffer, payload, copy_len);
    rx_chunk_size = copy_len;
    rx_done_flag = 1;
}

extern SUBGHZ_HandleTypeDef hsubghz;

void SUBGHZ_Radio_IRQHandler(void)
{
    HAL_SUBGHZ_IRQHandler(&hsubghz);
}



/*
=========================================================
PROCESS ONE RECEIVED FX.25 FRAME
=========================================================
*/

static void Process_Received_Frame(void)
{
    uint8_t frame_copy[FX25_FRAME_LEN];
    memset(frame_copy, 0, sizeof(frame_copy));
    memcpy(frame_copy, (const void *)rx_chunk_buffer, rx_chunk_size);

    int accept_rc = cubus_ax25_accept_frame(&s_reassembler, frame_copy);
    if (accept_rc < 0)
    {
        stat_packets_bad++;
        uart2_printf("\r\nFRAME REJECTED (bad FX.25 tag/RS, bad AX.25 FCS, or malformed) "
                     "rssi=%d dBm snr=%d dB\r\n", (int)rx_last_rssi, (int)rx_last_snr);
        return;
    }
    if (accept_rc == 0) return; /* message still incomplete */

    if (s_reassembler.dst_id != NODE_ID_GROUND)
    {
        stat_packets_misaddressed++;
        cubus_ax25_reassembler_init(&s_reassembler);
        return;
    }

    if (s_reassembler.type == TYPE_ACK)
    {
        cubus_ack_t ack;
        if (ack_unpack(s_reassembler.payload, s_reassembler.payload_len, &ack) == 0)
        {
            uart2_printf(
                "\r\nRX ACK: status=0x%02X (%s) data_id=0x%02X acked_seq=%u\r\n",
                ack.status, ack_status_str(ack.status), ack.data_id, ack.acked_seq);
        }
        else
        {
            uart2_puts("\r\nRX ACK: malformed payload\r\n");
        }
        cubus_ax25_reassembler_init(&s_reassembler);
        return;
    }

    if (s_reassembler.type != TYPE_TELEMETRY)
    {
        Print_Reassembled_Fields("RX (non-telemetry)", &s_reassembler);
        cubus_ax25_reassembler_init(&s_reassembler);
        return;
    }

    stat_packets_ok++;
    cubus_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.version = s_reassembler.version;
    pkt.type = s_reassembler.type;
    pkt.src_id = s_reassembler.src_id;
    pkt.dst_id = s_reassembler.dst_id;
    pkt.seq_num = s_reassembler.seq_num;
    pkt.session_id = s_reassembler.session_id;
    pkt.flags = s_reassembler.flags;
    gap_tracker_accept(&s_gap_tracker, &pkt);
    Print_Reassembled_Fields("RX TELEMETRY", &s_reassembler);

    uart2_printf(
        "SESSION STATS: %lu OK | %lu bad | %lu misaddressed\r\n",
        (unsigned long)stat_packets_ok, (unsigned long)stat_packets_bad,
        (unsigned long)stat_packets_misaddressed);

    bool last_in_burst = (s_reassembler.flags & FLAG_LAST_IN_BURST) != 0;
    cubus_ax25_reassembler_init(&s_reassembler);

    if (last_in_burst)
    {
        uart2_puts("LAST_IN_BURST seen - computing gap report\r\n");
        Send_Gap_Report();
    }
}



/*
=========================================================
SEND HELPERS
=========================================================
*/

extern void SysTick_Init_CPU2(uint32_t sys_freq_hz);
extern void CPU2_Delay_Ms(uint32_t ms);
extern volatile uint32_t g_system_tick_ms;

static uint32_t Get_Time_Ms(void)
{
    return g_system_tick_ms;
}

static bool Radio_Send_And_Wait(uint8_t *buffer, uint16_t size, uint32_t timeout_ms)
{
    tx_busy = 1;
    RadioApp_Send(buffer, size);

    uint32_t start_ms = Get_Time_Ms();
    while (tx_busy != 0)
    {
        if ((Get_Time_Ms() - start_ms) > timeout_ms)
        {
            uart2_puts("TX TIMEOUT\r\n");
            Radio.Standby();
            tx_busy = 0;
            return false;
        }
        CPU2_Delay_Ms(1);
    }
    return true;
}

/* Sends one complete FX25_FRAME_LEN-byte FX.25 frame (tag + shortened
   RS(255,223) codeword) through the SX126x's native packet engine,
   configured (radio_app.c) for fixed-length/no-CRC/no-whitening so the
   bytes that actually leave the antenna are real, standards-compliant
   AX.25/FX.25 -- decodable by SatNOGS/gr-satellites/Direwolf, not just
   this firmware. */
static bool Send_FX25_Frame(const uint8_t frame[FX25_FRAME_LEN])
{
    return Radio_Send_And_Wait((uint8_t *)frame, FX25_FRAME_LEN, TX_TIMEOUT_MS);
}

static uint16_t s_uplink_seq = 0;

/* Fragments one S2S2 message across AX.25/FX.25 frames and sends each. */
static bool Send_Message(uint8_t type, uint16_t seq_num, uint8_t flags,
                          const uint8_t *payload, uint16_t payload_len)
{
    uint8_t frames[CUBUS_AX25_MAX_FRAGMENTS][FX25_FRAME_LEN];
    int num_frames = 0;
    if (cubus_ax25_fragment(&s_link, 1, type, NODE_ID_GROUND, NODE_ID_SATELLITE,
                             seq_num, DEFAULT_SESSION_ID, flags,
                             payload, payload_len, frames, &num_frames) != 0)
    {
        uart2_puts("cubus_ax25_fragment FAILED\r\n");
        return false;
    }

    bool all_ok = true;
    for (int f = 0; f < num_frames; f++)
    {
        if (!Send_FX25_Frame(frames[f])) all_ok = false;
    }
    return all_ok;
}

/* Builds and uplinks one standard command packet (see s2s2_commands.h).
   address/num_packets are only meaningful for CMD_DOWNLOAD_FILE-style
   opcodes; pass 0 for commands that don't use them. */
static void Send_Command_Ex(uint8_t opcode, uint8_t subsystem, uint8_t data_id,
                             uint32_t address, uint16_t num_packets)
{
    cubus_command_t cmd = { .opcode = opcode, .subsystem = subsystem, .data_id = data_id,
                             .address = address, .num_packets = num_packets,
                             .counter = s2s2_auth_next_counter() };
    uint8_t payload[COMMAND_PAYLOAD_LEN];
    command_pack(&cmd, payload);
    /* Sign after packing so the MAC covers the exact wire bytes
       (opcode..counter, COMMAND_SIGNED_LEN of them), not the struct. */
    s2s2_auth_compute_mac(NODE_ID_GROUND, NODE_ID_SATELLITE,
                           payload, COMMAND_SIGNED_LEN, &payload[13]);

    uint16_t seq = s_uplink_seq++;
    uart2_printf("\r\nTX COMMAND 0x%02X (seq=%u)\r\n", opcode, seq);
    bool ok = Send_Message(TYPE_TELECOMMAND, seq, FLAG_ACK_REQUESTED, payload, sizeof(payload));
    uart2_printf("COMMAND %s - SWITCHING TO RX\r\n", ok ? "SENT" : "TIMED OUT");
}

static void Send_Command(uint8_t opcode)
{
    Send_Command_Ex(opcode, 0, DATA_ID_GENERAL, 0, 0);
}

#define ACK_WAIT_MS 5000

/* Pumps RX for a fixed window, printing whatever comes back (ACK or
   telemetry) via Process_Received_Frame. Used after single-shot commands
   that expect one short reply rather than a full telemetry burst. */
static void Listen_For_Response(uint32_t timeout_ms)
{
    rx_done_flag = 0;
    rx_timeout_flag = 0;
    rx_error_flag = 0;
    RadioApp_StartRx();

    uint32_t start = Get_Time_Ms();
    while ((Get_Time_Ms() - start) < timeout_ms)
    {
        if (rx_done_flag)
        {
            rx_done_flag = 0;
            Process_Received_Frame();
        }
        if (rx_timeout_flag || rx_error_flag)
        {
            rx_timeout_flag = 0;
            rx_error_flag = 0;
        }
    }
}

/* Uplinks the current gap tracker's missing-sequence list as a single
   TYPE_GAP_REPORT frame (payload = big-endian uint16 list). Satellite-side
   retransmit-from-buffer is not implemented yet - see the architecture
   README for that known limitation. */
static void Send_Gap_Report(void)
{
    uint16_t missing[GAP_REPORT_MAX_ENTRIES];
    int n = gap_tracker_report(&s_gap_tracker, missing, GAP_REPORT_MAX_ENTRIES);

    if (n == 0)
    {
        uart2_puts("GAP REPORT: no gaps - session complete, nothing to report\r\n");
        return;
    }

    uint8_t payload[MAX_PAYLOAD_LEN];
    uint16_t plen = 0;
    int max_entries = MAX_PAYLOAD_LEN / 2;
    int report_n = (n > max_entries) ? max_entries : n;
    for (int i = 0; i < report_n; i++)
    {
        payload[plen++] = (uint8_t)(missing[i] >> 8);
        payload[plen++] = (uint8_t)(missing[i] & 0xFF);
    }

    uint16_t seq = s_uplink_seq++;
    uart2_printf("GAP REPORT: %d missing seq number(s), uplinking...\r\n", n);
    Send_Message(TYPE_GAP_REPORT, seq, 0, payload, plen);
}



/*
=========================================================
CASE INSENSITIVE STRING COMPARE
=========================================================
*/

static bool str_ieq(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = *a;
        char cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
        a++;
        b++;
    }
    return (*a == 0 && *b == 0);
}

#ifdef DEBUG_BUILD
/* Case-insensitive prefix match, e.g. str_starts_ieq("TX deadbeef", "TX ").
   Only needed by the DEBUG_BUILD-only TX/POWER UART commands below -- not
   compiled into a production image at all, not just gated at runtime. */
static bool str_starts_ieq(const char *s, const char *prefix)
{
    while (*prefix)
    {
        char a = *s++;
        char b = *prefix++;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return false;
    }
    return true;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes an even-length hex string into bytes. Returns the byte count,
   or -1 on odd length, a non-hex character, or more bytes than max_out. */
static int hex_decode(const char *hex, uint8_t *out, int max_out)
{
    int len = (int)strlen(hex);
    if (len % 2 != 0) return -1;
    int n = len / 2;
    if (n > max_out) return -1;
    for (int i = 0; i < n; i++)
    {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}
#endif /* DEBUG_BUILD */



/*
=========================================================
UART COMMAND LINE READER
=========================================================
*/

static bool USART_TryReadLine(char *out, uint16_t max)
{
    static char buffer[CMD_LINE_MAX];
    static uint16_t index = 0;
    uint8_t c;

    if (!uart2_try_getc(&c)) return false;

    uart2_putc(c);

    if (c == '\r' || c == '\n')
    {
        if (index == 0) return false;
        buffer[index] = 0;
        uint16_t n = (index < (uint16_t)(max - 1)) ? index : (uint16_t)(max - 1);
        memcpy(out, buffer, n);
        out[n] = 0;
        index = 0;
        uart2_puts("\r\n");

        return true;
    }

    if (index < CMD_LINE_MAX - 1) buffer[index++] = (char)c;
    return false;
}



/*
=========================================================
MAIN FUNCTION
=========================================================
*/

int main(void)
{
    SystemInit();
    HAL_Init();
    SysTick_Init_CPU2(48000000UL);
    uart2_init();

    uart2_puts("\r\n");
    uart2_puts("================================\r\n");
    uart2_puts(" STM32WL55 GROUND STATION\r\n");
    uart2_puts(" GMSK, S2S2 over AX.25 + FX.25\r\n");
    uart2_puts(" UPLINK   : 437.375 MHz\r\n");
    uart2_puts(" DOWNLINK : 435.000 MHz\r\n");
#ifdef DEBUG_BUILD
    uart2_puts(" *** DEBUG BUILD - TX <hex> / POWER <dbm> ENABLED ***\r\n");
#else
    uart2_puts(" *** PRODUCTION BUILD ***\r\n");
#endif
    uart2_puts("================================\r\n");

    RBI_Init();
    MX_SUBGHZ_Init();

    HAL_NVIC_SetPriority(SUBGHZ_Radio_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(SUBGHZ_Radio_IRQn);
    __enable_irq();

    RadioEvents_t events =
    {
        .TxDone = OnTxDone,
        .TxTimeout = OnTxTimeout,
        .RxDone = OnRxDone,
        .RxTimeout = OnRxTimeout,
        .RxError = OnRxError
    };

    cubus_proto_init();
    RadioApp_Init(&GroundStationProfile, &events);
    cubus_ax25_reassembler_init(&s_reassembler);
    gap_tracker_init(&s_gap_tracker, DEFAULT_SESSION_ID);
    RadioApp_StartRx();

    uart2_puts("\r\nRADIO READY - Listening for satellite beacons on 435.000 MHz\r\n");
    uart2_puts("Standard commands:\r\n");
    uart2_puts("  COMMAND     - request a telemetry burst\r\n");
    uart2_puts("  HEARTBEAT   - liveness check\r\n");
    uart2_puts("  HEALTH      - get OBC health (CMD_GET_HEALTH)\r\n");
    uart2_puts("  HOUSEKEEPING- get housekeeping data\r\n");
    uart2_puts("  BEACON1     - get Beacon 1 (power/battery/thermal)\r\n");
    uart2_puts("  BEACON2     - get Beacon 2 (solar/ADCS)\r\n");
    uart2_puts("  ADCS        - get ADCS data\r\n");
    uart2_puts("  CAMERA      - run camera mission\r\n");
    uart2_puts("  DOWNLOAD    - download file from OBC\r\n");
    uart2_puts("  SATLOG      - get satellite log\r\n");
    uart2_puts("  DIGIPEATER  - digipeater mission\r\n");
    uart2_puts("  RETRANSMIT  - request retransmit of gapped sequence numbers\r\n");
    uart2_puts("  ABORT       - abort current mission\r\n");
#ifdef DEBUG_BUILD
    uart2_puts("  TX <hex>    - uplink arbitrary bytes as TYPE_RAW, e.g. TX 48656C6C6F\r\n");
    uart2_puts("  POWER <dbm> - override TX power for bench testing\r\n");
#endif
    uart2_puts("> ");

    char command[CMD_LINE_MAX];

    while (1)
    {
        if (rx_done_flag)
        {
            rx_done_flag = 0;
            Process_Received_Frame();
        }
        if (rx_timeout_flag || rx_error_flag)
        {
            rx_timeout_flag = 0;
            rx_error_flag = 0;
        }

        if (USART_TryReadLine(command, sizeof(command)))
        {
            if (str_ieq(command, "COMMAND"))
            {
                uart2_puts("\r\nCOMMAND ACCEPTED\r\n");

                stat_packets_ok = 0;
                stat_packets_bad = 0;
                stat_packets_misaddressed = 0;
                cubus_ax25_reassembler_init(&s_reassembler);
                gap_tracker_init(&s_gap_tracker, DEFAULT_SESSION_ID);

                Send_Command(CMD_REQUEST_BURST);

                uart2_puts("\r\nLISTENING 435 MHz FOR TELEMETRY BURST...\r\n");

                rx_done_flag = 0;
                rx_timeout_flag = 0;
                rx_error_flag = 0;

                RadioApp_StartRx();

                uint32_t start = Get_Time_Ms();
                uint32_t last_packet_time = start;
                uint32_t last_heartbeat = start;

                while ((Get_Time_Ms() - start) < RX_SESSION_MS)
                {
                    if (rx_done_flag)
                    {
                        rx_done_flag = 0;
                        last_packet_time = Get_Time_Ms();
                        last_heartbeat = last_packet_time;
                        Process_Received_Frame();
                    }

                    if (rx_timeout_flag || rx_error_flag)
                    {
                        rx_timeout_flag = 0;
                        rx_error_flag = 0;
                    }

                    /* If burst has started and no new packets have arrived
                       for 4 seconds, finish. */
                    if (stat_packets_ok > 0 && (Get_Time_Ms() - last_packet_time) > 4000)
                    {
                        break;
                    }

                    if ((Get_Time_Ms() - last_heartbeat) > RX_HEARTBEAT_MS)
                    {
                        last_heartbeat = Get_Time_Ms();
                        uart2_printf(
                            "... listening (%lus elapsed) [received %lu telemetry frames]\r\n",
                            (unsigned long)((Get_Time_Ms() - start) / 1000UL),
                            (unsigned long)stat_packets_ok);
                    }
                }

                uart2_printf(
                    "\r\n========================================\r\n"
                    "BURST RX COMPLETE: %lu telemetry frame(s) received cleanly\r\n"
                    "========================================\r\n",
                    (unsigned long)stat_packets_ok);

                RadioApp_StartRx();
            }
            else if (str_ieq(command, "HEARTBEAT"))
            {
                Send_Command(CMD_HEARTBEAT);
                Listen_For_Response(ACK_WAIT_MS);
            }
            else if (str_ieq(command, "HEALTH"))
            {
                Send_Command_Ex(CMD_GET_HEALTH, SUBSYS_OBC, DATA_ID_HOUSEKEEPING, 0, 0);
                Listen_For_Response(ACK_WAIT_MS);
            }
            else if (str_ieq(command, "HOUSEKEEPING"))
            {
                Send_Command_Ex(CMD_GET_HOUSEKEEPING, SUBSYS_OBC, DATA_ID_HOUSEKEEPING, 0, 0);
                Listen_For_Response(ACK_WAIT_MS);
            }
            else if (str_ieq(command, "BEACON1"))
            {
                Send_Command_Ex(CMD_GET_BEACON1, SUBSYS_OBC, DATA_ID_BEACON1, 0, 0);
                Listen_For_Response(ACK_WAIT_MS);
            }
            else if (str_ieq(command, "BEACON2"))
            {
                Send_Command_Ex(CMD_GET_BEACON2, SUBSYS_OBC, DATA_ID_BEACON2, 0, 0);
                Listen_For_Response(ACK_WAIT_MS);
            }
            else if (str_ieq(command, "ADCS"))
            {
                Send_Command_Ex(CMD_GET_ADCS, SUBSYS_ADCS, DATA_ID_ADCS, 0, 0);
                Listen_For_Response(ACK_WAIT_MS);
            }
            else if (str_ieq(command, "CAMERA"))
            {
                Send_Command_Ex(CMD_RUN_CAMERA, SUBSYS_CAM, DATA_ID_CAMERA_RGB, 0, 0);
                Listen_For_Response(ACK_WAIT_MS);
            }
            else if (str_ieq(command, "DOWNLOAD"))
            {
                Send_Command_Ex(CMD_DOWNLOAD_FILE, SUBSYS_OBC, DATA_ID_SAT_LOG, 0, 0);
                Listen_For_Response(ACK_WAIT_MS);
            }
            else if (str_ieq(command, "SATLOG"))
            {
                Send_Command_Ex(CMD_GET_SAT_LOG, SUBSYS_OBC, DATA_ID_SAT_LOG, 0, 0);
                Listen_For_Response(ACK_WAIT_MS);
            }
            else if (str_ieq(command, "DIGIPEATER"))
            {
                Send_Command_Ex(CMD_DIGIPEATER, SUBSYS_COM, DATA_ID_DIGIPEATER, 0, 0);
                Listen_For_Response(ACK_WAIT_MS);
            }
            else if (str_ieq(command, "RETRANSMIT"))
            {
                Send_Command(CMD_REQUEST_RETRANSMIT);
                Listen_For_Response(ACK_WAIT_MS);
            }
            else if (str_ieq(command, "ABORT"))
            {
                Send_Command(CMD_ABORT_MISSION);
                Listen_For_Response(ACK_WAIT_MS);
            }
#ifdef DEBUG_BUILD
            else if (str_starts_ieq(command, "TX "))
            {
                uint8_t payload[MAX_PAYLOAD_LEN];
                int n = hex_decode(command + 3, payload, sizeof(payload));
                if (n < 0)
                {
                    uart2_puts("ERR bad hex (need an even number of 0-9/a-f characters, "
                               "e.g. TX 48656C6C6F)\r\n");
                }
                else
                {
                    uint16_t seq = s_uplink_seq++;
                    bool ok = Send_Message(TYPE_RAW, seq, FLAG_LAST_IN_BURST, payload, (uint16_t)n);
                    uart2_printf("%s (%d byte%s)\r\n", ok ? "OK sent" : "ERR send failed",
                                 n, n == 1 ? "" : "s");
                    Listen_For_Response(ACK_WAIT_MS);
                }
            }
            else if (str_starts_ieq(command, "POWER "))
            {
                int dbm = atoi(command + 6);
                RadioApp_SetTxPower((int8_t)dbm);
            }
            else if (str_starts_ieq(command, "CMD "))
            {
                /* CMD <opcode_hex> <subsys_hex> <data_id_hex> <addr_hex> <num_packets_dec>
                   -- arbitrary authenticated command, any opcode/subsystem/
                   data_id/address/num_packets, not just the fixed presets
                   above. Signed on-board by the existing Send_Command_Ex
                   (real SipHash-2-4 key + counter) -- this is only a
                   parser in front of already-trusted code, not new
                   protocol/crypto logic. */
                char *p = command + 4;
                char *end;
                unsigned long opcode = strtoul(p, &end, 16);
                bool bad = (end == p);
                p = end;
                unsigned long subsystem = strtoul(p, &end, 16);
                bad = bad || (end == p);
                p = end;
                unsigned long data_id = strtoul(p, &end, 16);
                bad = bad || (end == p);
                p = end;
                unsigned long address = strtoul(p, &end, 16);
                bad = bad || (end == p);
                p = end;
                unsigned long num_packets = strtoul(p, &end, 10);
                bad = bad || (end == p);

                if (bad || opcode > 0xFF || subsystem > 0xFF || data_id > 0xFF ||
                    address > 0xFFFFFFFFUL || num_packets > 0xFFFF)
                {
                    uart2_puts("ERR usage: CMD <opcode_hex> <subsys_hex> <data_id_hex> "
                               "<addr_hex> <num_packets_dec>, e.g. CMD 12 01 01 0 0\r\n");
                }
                else
                {
                    Send_Command_Ex((uint8_t)opcode, (uint8_t)subsystem, (uint8_t)data_id,
                                     (uint32_t)address, (uint16_t)num_packets);
                    Listen_For_Response(ACK_WAIT_MS);
                }
            }
#endif
            else
            {
                uart2_printf("UNKNOWN COMMAND : %s (type COMMAND to request burst)\r\n", command);
            }

            uart2_puts("\r\n> ");
        }
    }
}

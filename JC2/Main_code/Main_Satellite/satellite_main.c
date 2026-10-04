#include "satellite_app.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "Sring_buffer.h"
#include "uart_debug.h"
#include "protocol.h"
#include "ax25.h"
#include "radio.h"
#include "radio_app.h"

#define ABS(x) ((x) < 0 ? -(x) : (x))

/*
 * ============================================================================
 * DEBUG BACKDOOR -- satellite's own debug UART (UART1) command console,
 * matching tools/console.py / tx_gui.py / satellite_gui.py's documented
 * protocol exactly (command strings + response log-line regexes). This
 * was previously only referenced/expected by those tools (the old,
 * pre-swap ipcc_m0plus_main.c implementation is where they got it from)
 * -- never actually implemented against the current codebase. No
 * DEBUG_BUILD compile-time strip here (the old codebase's production-
 * build concept doesn't exist in this Makefile) -- the real safety
 * lockout is s_satellite_mode (SATMODE ON/OFF), always compiled in,
 * runtime-toggled, reversible from this same local UART cable.
 * ============================================================================
 */

#define CMD_LINE_MAX 256

/* When true, every debug/test command below (TX/POWER/BEACON/TEST*) is
   refused so the unit behaves like a real deployed satellite -- only
   SATMODE OFF (from this same local cable) undoes it. */
static bool s_satellite_mode = false;
static bool s_beacon_enabled = true;
static const SatelliteConfig_t *s_cfg = NULL;

/* Case-insensitive prefix/exact match + hex decode -- same small helpers
   gs_main.c has its own copies of (separate compilation unit/target, no
   shared header for these -- duplicated on purpose rather than adding a
   cross-target dependency for three one-line functions). */
static bool str_starts_ieq(const char *s, const char *prefix) {
    while (*prefix) {
        char a = *s++;
        char b = *prefix++;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return false;
    }
    return true;
}

static bool str_ieq(const char *s, const char *other) {
    return str_starts_ieq(s, other) && s[strlen(other)] == '\0';
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *hex, uint8_t *out, int max_out) {
    int len = (int)strlen(hex);
    if (len % 2 != 0) return -1;
    int n = len / 2;
    if (n > max_out) return -1;
    for (int i = 0; i < n; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

/* Non-blocking line reader off UART1 -- echoes each typed character back
   (terminal feel over a raw serial console) and returns true once a
   full \r/\n-terminated line is ready in `out`. */
static bool USART_TryReadLine(char *out, uint16_t max) {
    static char buffer[CMD_LINE_MAX];
    static uint16_t index = 0;
    uint8_t c;

    if (!uart1_try_getc(&c)) return false;

    uart1_putc((char)c);

    if (c == '\r' || c == '\n') {
        if (index == 0) return false;
        buffer[index] = 0;
        uint16_t n = (index < (uint16_t)(max - 1)) ? index : (uint16_t)(max - 1);
        memcpy(out, buffer, n);
        out[n] = 0;
        index = 0;
        uart1_puts("\r\n");
        return true;
    }

    if (index < CMD_LINE_MAX - 1) buffer[index++] = (char)c;
    return false;
}

/* MEMSTAT -- real stack high-water-mark via the classic "paint the
   unused stack with a canary, scan for the deepest byte ever touched"
   technique. _ebss/_estack (scripts/wl55jc_cm0p.ld) bound the single
   stack region this bare-metal build uses (no heap) -- everything
   above .bss up to the top of RAM. Painted once, early in main(), so
   the baseline reflects actual boot-time stack depth, not a perfect
   from-reset-vector snapshot (some C-runtime startup call depth has
   already happened by the time main() runs) -- close enough to be a
   real, non-fabricated measurement, not exact to the instruction. */
extern uint32_t _estack;
extern uint32_t _ebss;
#define MEMSTAT_CANARY_BYTE 0xAAU
#define MEMSTAT_GUARD_BYTES 64U /* never paint the live stack under current SP */

static void MemStat_PaintStack(void) {
    uint32_t sp;
    __asm__ volatile ("mov %0, sp" : "=r" (sp));
    uint8_t *p = (uint8_t *)&_ebss;
    uint8_t *limit = (uint8_t *)sp - MEMSTAT_GUARD_BYTES;
    for (; p < limit; p++) {
        *p = (uint8_t)MEMSTAT_CANARY_BYTE;
    }
}

static void MemStat_Report(void) {
    uint8_t *p = (uint8_t *)&_ebss;
    uint8_t *stack_top = (uint8_t *)&_estack;
    while (p < stack_top && *p == (uint8_t)MEMSTAT_CANARY_BYTE) {
        p++;
    }
    uint32_t stack_total = (uint32_t)(stack_top - (uint8_t *)&_ebss);
    uint32_t stack_used = (uint32_t)(stack_top - p);
    uart1_printf("MEMSTAT stack_used=%lu stack_total=%lu\r\n",
                 (unsigned long)stack_used, (unsigned long)stack_total);
}

/*
 * ----------------------------------------------------------------------------
 * SATELLITE RX LISTENING DURATION CONFIGURATION
 * ----------------------------------------------------------------------------
 * User requirement: "once send then stuck so please one listen is every 30 min"
 * Standard CubeSat sequence: Complete CW1 -> Listen RX 30 min -> Complete CW2 -> Listen RX 30 min
 * 30 minutes = 30 * 60 * 1000 = 1,800,000 ms.
 * (For quick bench testing, can be temporarily set to e.g. 30UL * 1000UL for 30s)
 */
#ifndef SAT_LISTEN_DURATION_MS
#define SAT_LISTEN_DURATION_MS (30UL * 60UL * 1000UL)
#endif

static char cwBeaconlive_msg1[CW_BEACON_HEX_LEN + 1];
static char cwBeaconlive_msg2[CW_BEACON_HEX_LEN + 1];

/*
 * ============================================================================
 * BEACON 1/2 + CW1/2 + ACK + CAMERA FRAGMENT -- Packet Format sheet-exact
 * packing (moved here from satellite_app.c: this file owns the IPC ring-
 * buffer structs -- Sring_buffer.h's beacon1_ipc_s/beacon2_ipc_s -- these
 * pack from, and runs the mission loop that calls them). See Mission/
 * config.h for the Beacon1Packet_t/Beacon2Packet_t/CwBeacon1_t/CwBeacon2_t
 * field definitions.
 * ============================================================================
 */

static void CwBeacon1_Pack(const CwBeacon1_t *b, uint8_t out[CW_BEACON_PACKED_LEN]) {
    out[0] = (uint8_t)(((b->system_stat & 0x03) << 6) |
                        ((b->mission_status & 0x03) << 4) |
                        (b->time_after_reset & 0x0F));
    out[1] = b->sol_total_i;
    out[2] = (uint8_t)(b->sol_total_v >> 8);
    out[3] = (uint8_t)b->sol_total_v;
    out[4] = (uint8_t)(b->bat_v >> 8);
    out[5] = (uint8_t)b->bat_v;
    out[6] = (uint8_t)(b->bat_i >> 8);
    out[7] = (uint8_t)b->bat_i;
}

static void CwBeacon2_Pack(const CwBeacon2_t *b, uint8_t out[CW_BEACON_PACKED_LEN]) {
    out[0] = b->gyro_x;
    out[1] = b->gyro_y;
    out[2] = b->gyro_z;
    out[3] = b->mag_x;
    out[4] = b->mag_y;
    out[5] = b->mag_z;
    out[6] = (uint8_t)(((b->sol_plus_x  & 1) << 4) |
                        ((b->sol_minus_y & 1) << 3) |
                        ((b->sol_minus_z & 1) << 2) |
                        ((b->sol_plus_y  & 1) << 1) |
                        (b->sol_plus_z  & 1));
    out[7] = b->bat_temp;
}

static void CwBeacon_HexEncode(const uint8_t *bytes, int len, char *out) {
    static const char HEX[] = "0123456789ABCDEF";
    for (int i = 0; i < len; i++) {
        out[i * 2]     = HEX[(bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = HEX[bytes[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

/* response_type (out[0]) derived from status: ACCEPTED/COMPLETED ->
   ACK_RESPONSE_TYPE_ACK, every error code -> ACK_RESPONSE_TYPE_NACK. */
static void Ack_Pack(const AckPacket_t *ack, uint8_t out[ACK_PAYLOAD_LEN]) {
    out[0] = (ack->status == ACK_STATUS_ACCEPTED || ack->status == ACK_STATUS_COMPLETED)
                 ? ACK_RESPONSE_TYPE_ACK : ACK_RESPONSE_TYPE_NACK;
    out[1] = ack->status;
    out[2] = ack->data_id;
}

static bool __attribute__((unused)) Satellite_Send_Ack(const AckPacket_t *ack) {
    uint8_t payload[ACK_PAYLOAD_LEN];
    Ack_Pack(ack, payload);
    return Satellite_Send_Packet_Timeout(payload, sizeof(payload), 0);
}

/* out must have room for CAMERA_FRAGMENT_HDR_LEN + frag->data_len +
   CAMERA_FRAGMENT_CRC_LEN bytes. Returns bytes written, or -1 if
   data_len > CAMERA_FRAGMENT_MAX_DATA. No camera driver exists yet to
   call this -- see CameraFragment_t's comment in Mission/config.h. */
static int __attribute__((unused)) Camera_Fragment_Pack(const CameraFragment_t *frag, uint8_t *out) {
    if (frag->data_len > CAMERA_FRAGMENT_MAX_DATA) return -1;
    uint8_t *p = out;
    *p++ = CAMERA_FRAGMENT_PACKET_TYPE;
    *p++ = frag->image_id;
    *p++ = (uint8_t)(frag->fragment_index >> 8); *p++ = (uint8_t)frag->fragment_index;
    *p++ = (uint8_t)(frag->total_fragments >> 8); *p++ = (uint8_t)frag->total_fragments;
    *p++ = frag->image_type;
    *p++ = frag->ai_score;
    if (frag->data_len > 0) memcpy(p, frag->data, frag->data_len);
    p += frag->data_len;
    uint16_t total = (uint16_t)(CAMERA_FRAGMENT_HDR_LEN + frag->data_len);
    uint16_t crc = AX25_CalculateCRC(out, total);
    p[0] = (uint8_t)(crc >> 8);
    p[1] = (uint8_t)crc;
    return total + CAMERA_FRAGMENT_CRC_LEN;
}

/* Builds the 16-hex-digit string for one CW1/CW2 cycle -- caller feeds
   this straight into the existing Satellite_Send_CW_Beacon/CW_SendString
   Morse keyer, same as the plain-text message it replaces. */
static void Satellite_BuildCw1Hex(const CwBeacon1_t *b, char out[CW_BEACON_HEX_LEN + 1]) {
    uint8_t packed[CW_BEACON_PACKED_LEN];
    CwBeacon1_Pack(b, packed);
    CwBeacon_HexEncode(packed, CW_BEACON_PACKED_LEN, out);
}

static void Satellite_BuildCw2Hex(const CwBeacon2_t *b, char out[CW_BEACON_HEX_LEN + 1]) {
    uint8_t packed[CW_BEACON_PACKED_LEN];
    CwBeacon2_Pack(b, packed);
    CwBeacon_HexEncode(packed, CW_BEACON_PACKED_LEN, out);
}

/*
 * ============================================================================
 * TESTBEACON1/TESTBEACON2 -- debug-backdoor-only Beacon1/Beacon2 AX.25
 * send, re-added here (removed from the operational path) purely so the
 * sheet-compliant pack/send code can still be exercised/verified on
 * demand from the bench. Builds from whatever the ring buffer last
 * actually delivered (s_last_b1/s_last_b2 below, same honest-zero rule
 * as the CW path) -- not a parallel/fabricated data source.
 * ============================================================================
 */

static uint8_t Beacon_Crc8(const uint8_t *data, uint16_t len) {
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void Beacon1_Pack(const Beacon1Packet_t *b, uint8_t out[BEACON1_PAYLOAD_LEN]) {
    uint8_t *p = out;
    *p++ = BEACON1_HEAD;
    *p++ = b->type;
    *p++ = (uint8_t)(b->tim_day >> 8); *p++ = (uint8_t)b->tim_day;
    *p++ = b->tim_hour;
    *p++ = (uint8_t)(b->bat_v >> 8); *p++ = (uint8_t)b->bat_v;
    *p++ = (uint8_t)(b->bat_i >> 8); *p++ = (uint8_t)b->bat_i;
    *p++ = b->bat_temp;
    *p++ = b->raw_v;
    *p++ = b->raw_i;
    *p++ = (uint8_t)(b->sol_tot_v >> 8); *p++ = (uint8_t)b->sol_tot_v;
    *p++ = b->sol_tot_i;
    *p++ = b->unreg_i;
    *p++ = b->main_3v3_i;
    *p++ = b->msn_3v3_i;
    *p++ = b->v5_i;
    *p++ = b->adc_3v32_i;
    *p++ = b->temp1;
    *p++ = b->temp2;
    *p++ = b->temp3;
    *p++ = b->temp4;
    *p++ = b->temp5;
    *p++ = b->bpb_t;
    *p++ = b->ant_p_t;
    *p++ = b->obc_com_t;
    *p++ = b->system_stat;
    *p++ = b->mcu_stat;
    *p++ = b->adc_stat;
    *p = Beacon_Crc8(out, (uint16_t)(p - out));
}

static void Beacon2_Pack(const Beacon2Packet_t *b, uint8_t out[BEACON2_PAYLOAD_LEN]) {
    uint8_t *p = out;
    *p++ = BEACON2_HEAD;
    *p++ = b->type;
    *p++ = (uint8_t)(b->tim_day >> 8); *p++ = (uint8_t)b->tim_day;
    *p++ = b->sol_p1_v;
    *p++ = b->sol_p2_v;
    *p++ = b->sol_p3_v;
    *p++ = b->sol_p4_v;
    *p++ = b->sol_p5_v;
    *p++ = b->sol_p1_i;
    *p++ = b->sol_p2_i;
    *p++ = b->sol_p3_i;
    *p++ = b->sol_p4_i;
    *p++ = b->sol_p5_i;
    *p++ = b->gyro_x;
    *p++ = b->gyro_y;
    *p++ = b->gyro_z;
    *p++ = b->mag_x;
    *p++ = b->mag_y;
    *p++ = b->mag_z;
    *p++ = b->power_stat;
    *p = Beacon_Crc8(out, (uint16_t)(p - out));
}

static bool Satellite_Send_Beacon1(const Beacon1Packet_t *b) {
    uint8_t payload[BEACON1_PAYLOAD_LEN];
    Beacon1_Pack(b, payload);
    return Satellite_Send_Packet_Timeout(payload, sizeof(payload), 0);
}

static bool Satellite_Send_Beacon2(const Beacon2Packet_t *b) {
    uint8_t payload[BEACON2_PAYLOAD_LEN];
    Beacon2_Pack(b, payload);
    return Satellite_Send_Packet_Timeout(payload, sizeof(payload), 0);
}

/* Last telemetry M0_Drain_Telemetry actually delivered -- module-level
   so the debug-command handler (TESTBEACON1/2) can read the same state
   main()'s loop is already tracking, without a second IPC drain. */
static struct beacon1_ipc_s s_last_b1;
static struct beacon2_ipc_s s_last_b2;
static bool s_last_b1_valid = false;
static bool s_last_b2_valid = false;

static void TestBeacon1_Send(void) {
    Beacon1Packet_t b1pkt;
    memset(&b1pkt, 0, sizeof(b1pkt));
    b1pkt.type = 1;
    if (s_last_b1_valid) {
        b1pkt.tim_day     = s_last_b1.tim_day;
        b1pkt.tim_hour    = s_last_b1.tim_hour;
        b1pkt.bat_v       = s_last_b1.bat_v;
        b1pkt.bat_i       = s_last_b1.bat_i;
        b1pkt.bat_temp    = s_last_b1.bat_temp;
        b1pkt.raw_v       = s_last_b1.raw_v;
        b1pkt.raw_i       = s_last_b1.raw_i;
        b1pkt.sol_tot_v   = s_last_b1.sol_tot_v;
        b1pkt.sol_tot_i   = s_last_b1.sol_tot_i;
        b1pkt.unreg_i     = s_last_b1.unreg_i;
        b1pkt.main_3v3_i  = s_last_b1.main_3v3_i;
        b1pkt.msn_3v3_i   = s_last_b1.msn_3v3_i;
        b1pkt.v5_i        = s_last_b1.v5_i;
        b1pkt.adc_3v32_i  = s_last_b1.adc_3v32_i;
        b1pkt.temp1       = s_last_b1.temp1;
        b1pkt.temp2       = s_last_b1.temp2;
        b1pkt.temp3       = s_last_b1.temp3;
        b1pkt.temp4       = s_last_b1.temp4;
        b1pkt.temp5       = s_last_b1.temp5;
        b1pkt.bpb_t       = s_last_b1.bpb_t;
        b1pkt.ant_p_t     = s_last_b1.ant_p_t;
        b1pkt.obc_com_t   = s_last_b1.obc_com_t;
        b1pkt.system_stat = s_last_b1.system_stat;
        b1pkt.mcu_stat    = s_last_b1.mcu_stat;
        b1pkt.adc_stat    = s_last_b1.adc_stat;
    }
    uart1_puts("BEACON1 TX\r\n");
    Satellite_Send_Beacon1(&b1pkt);
}

static void TestBeacon2_Send(void) {
    Beacon2Packet_t b2pkt;
    memset(&b2pkt, 0, sizeof(b2pkt));
    b2pkt.type = 1;
    if (s_last_b2_valid) {
        b2pkt.tim_day    = s_last_b2.tim_day;
        b2pkt.sol_p1_v   = s_last_b2.sol_p1_v;
        b2pkt.sol_p2_v   = s_last_b2.sol_p2_v;
        b2pkt.sol_p3_v   = s_last_b2.sol_p3_v;
        b2pkt.sol_p4_v   = s_last_b2.sol_p4_v;
        b2pkt.sol_p5_v   = s_last_b2.sol_p5_v;
        b2pkt.sol_p1_i   = s_last_b2.sol_p1_i;
        b2pkt.sol_p2_i   = s_last_b2.sol_p2_i;
        b2pkt.sol_p3_i   = s_last_b2.sol_p3_i;
        b2pkt.sol_p4_i   = s_last_b2.sol_p4_i;
        b2pkt.sol_p5_i   = s_last_b2.sol_p5_i;
        b2pkt.gyro_x     = s_last_b2.gyro_x;
        b2pkt.gyro_y     = s_last_b2.gyro_y;
        b2pkt.gyro_z     = s_last_b2.gyro_z;
        b2pkt.mag_x      = s_last_b2.mag_x;
        b2pkt.mag_y      = s_last_b2.mag_y;
        b2pkt.mag_z      = s_last_b2.mag_z;
        b2pkt.power_stat = s_last_b2.power_stat;
    }
    uart1_puts("BEACON2 TX\r\n");
    Satellite_Send_Beacon2(&b2pkt);
}

/*
 * ============================================================================
 * TESTDIGI <hex> -- feeds host-supplied bytes through the REAL decode
 * pipeline (Protocol_ExtractFrame -> Protocol_VerifyPacket ->
 * Protocol_DecodePayload, the exact same light-FEC+auth check real RX
 * frames go through) and reports pass/fail via the DIGI: log lines
 * tools/console.py's activity counter already parses. This is a decode
 * self-test, NOT a multi-hop relay -- no such feature exists in this
 * firmware (the old pre-swap codebase's Try_Digipeat() this command
 * was originally written against doesn't apply here). rs_corrected is
 * always printed as 0: Protocol_DecodePayload reports pass/fail only,
 * no correction-count API exists to report a real number instead of a
 * fabricated-looking one.
 * ============================================================================
 */
static void TestDigi_Run(const uint8_t *frame, uint16_t n) {
    uint8_t decoded[AX25_MAX_FRAME_SIZE];
    uint16_t decoded_len = 0;

    if (!Protocol_ExtractFrame(frame, n, decoded, sizeof(decoded), &decoded_len) ||
        decoded_len < 17 ||
        !Protocol_VerifyPacket(decoded, decoded_len)) {
        uart1_puts("DIGI: relay TX failed\r\n");
        return;
    }

    char src_cs[7];
    AX25_DecodeAddress(&decoded[8], src_cs);

    uint16_t payload_start = 17;
    uint16_t payload_end = (uint16_t)(decoded_len - 3);
    uint8_t payload[256];
    uint16_t payload_len = 0;

    if (payload_end <= payload_start ||
        !Protocol_DecodePayload(&decoded[payload_start],
                                 (uint16_t)(payload_end - payload_start),
                                 payload, sizeof(payload), &payload_len)) {
        uart1_puts("DIGI: relay TX failed\r\n");
        return;
    }

    uart1_printf("DIGI: relayed via hop 0 (%s), %u info byte(s), rs_corrected=0\r\n",
                 src_cs, payload_len);
}

/*
 * ============================================================================
 * DEBUG BACKDOOR COMMAND DISPATCHER -- polled every main-loop iteration
 * (both the top-level while(1) and M0_Listening_Delay's inner wait loop)
 * regardless of mission phase. See the module-header comment above for
 * the s_satellite_mode safety-lockout rationale.
 * ============================================================================
 */
static void Process_UART_Command(void) {
    static char command[CMD_LINE_MAX];
    if (!USART_TryReadLine(command, sizeof(command))) return;

    if (str_ieq(command, "SATMODE ON")) {
        s_satellite_mode = true;
        uart1_puts("SATELLITE MODE ON - debug/test commands disabled\r\n");
        return;
    } else if (str_ieq(command, "SATMODE OFF")) {
        s_satellite_mode = false;
        uart1_puts("SATELLITE MODE OFF\r\n");
        return;
    } else if (str_ieq(command, "MEMSTAT")) {
        MemStat_Report();
        return;
    }

    if (s_satellite_mode) {
        if (command[0] != 0) {
            uart1_printf("UNKNOWN COMMAND : %s\r\n", command);
        }
        return;
    }

    if (str_starts_ieq(command, "TX ")) {
        uint8_t payload[AX25_MAX_FRAME_SIZE];
        int n = hex_decode(command + 3, payload, sizeof(payload));
        if (n < 0) {
            uart1_puts("ERR bad hex (need an even number of 0-9/a-f characters, "
                       "e.g. TX 48656C6C6F)\r\n");
            return;
        }
        bool ok = Satellite_Send_Packet_Timeout(payload, (uint16_t)n, 0);
        uart1_printf("%s (%d byte%s)\r\n", ok ? "OK sent" : "ERR send failed",
                     n, n == 1 ? "" : "s");
    } else if (str_starts_ieq(command, "POWER ")) {
        int dbm = atoi(command + 6);
        RadioApp_SetTxPower((int8_t)dbm);
        uart1_printf("POWER set to %d dBm\r\n", dbm);
    } else if (str_ieq(command, "BEACON ON")) {
        s_beacon_enabled = true;
        uart1_puts("BEACON ON\r\n");
    } else if (str_ieq(command, "BEACON OFF")) {
        s_beacon_enabled = false;
        uart1_puts("BEACON OFF\r\n");
    } else if (str_ieq(command, "TESTCW")) {
        uart1_puts("TESTCW: forcing an immediate CW beacon send\r\n");
        if (s_cfg != NULL) {
            uart1_puts("CW BEACON TX\r\n");
            Satellite_Send_CW_Beacon(0, "TEST", s_cfg->cwBeaconCallsign,
                                      cwBeaconlive_msg1, s_cfg->morseUnitMs,
                                      s_cfg->cwTuningCarrierDurationMs,
                                      s_cfg->cwTuningPostDelayMs,
                                      s_cfg->cwInterStringDelayMs);
        }
    } else if (str_ieq(command, "TESTBEACON1")) {
        uart1_puts("TESTBEACON1: forcing an immediate Beacon1 send\r\n");
        TestBeacon1_Send();
    } else if (str_ieq(command, "TESTBEACON2")) {
        uart1_puts("TESTBEACON2: forcing an immediate Beacon2 send\r\n");
        TestBeacon2_Send();
    } else if (str_starts_ieq(command, "TESTDIGI ")) {
        uint8_t frame[AX25_MAX_FRAME_SIZE];
        int n = hex_decode(command + 9, frame, sizeof(frame));
        if (n < 0) {
            uart1_puts("ERR bad hex (need an even number of 0-9/a-f characters)\r\n");
            return;
        }
        uart1_printf("TESTDIGI: feeding a %d-byte frame into the decode pipeline\r\n", n);
        TestDigi_Run(frame, (uint16_t)n);
    } else if (command[0] != 0) {
        uart1_printf("UNKNOWN COMMAND : %s\r\n", command);
    }
}

/*
 * One-stop builder: takes the sheet-exact Beacon1/Beacon2 IPC payloads
 * (ipc/Sring_buffer.h's beacon1_ipc_s/beacon2_ipc_s) + whether each has
 * EVER actually arrived from CPU1 (no fabricated defaults), copies every
 * real field 1:1 onto the CW1/2 structs (no scaling/guessing needed --
 * the IPC structs already share field names/meaning with the sheet), and
 * fills in the two CW hex strings for the caller to feed into
 * Satellite_Send_CW_Beacon. CW1/CW2 are the satellite's ONLY downlink
 * telemetry -- Beacon1/Beacon2 AX.25 binary packets are not transmitted
 * in the operational loop (TESTBEACON1/2 above are debug-backdoor-only).
 *
 * CW1's system_stat/mission_status (2-bit sheet value tables) have no
 * defined correspondence to Beacon1's generic system_stat/mcu_stat bytes
 * (the sheet never gives those a value table either) -- left at the
 * CPU2-uptime-derived time_after_reset only, same honest-zero rule as
 * everything else with no confirmed source. CW2's 5 solar-cell shadow/
 * sunshine bits ARE derived from Beacon2's real per-panel voltage bytes
 * (sunshine = nonzero reading) -- an honest derivation from a real
 * measurement, not a fabricated value, mapped P1..P5 -> +X/-Y/-Z/+Y/+Z
 * per BIRDS4's own reference order (Sheet4's own table repeats "+Y").
 */
static void Satellite_BuildAndSendTelemetry(const struct beacon1_ipc_s *b1, bool b1_valid,
                                             const struct beacon2_ipc_s *b2, bool b2_valid,
                                             char cw1_hex_out[CW_BEACON_HEX_LEN + 1],
                                             char cw2_hex_out[CW_BEACON_HEX_LEN + 1]) {
    uint32_t uptime_hours = Satellite_GetTimeMs() / 1000UL / 3600UL;

    CwBeacon1_t cw1;
    memset(&cw1, 0, sizeof(cw1));
    cw1.time_after_reset = (uint8_t)(uptime_hours % 16U);
    if (b1_valid) {
        cw1.sol_total_i = b1->sol_tot_i;
        cw1.sol_total_v = b1->sol_tot_v;
        cw1.bat_v       = b1->bat_v;
        cw1.bat_i       = b1->bat_i;
    }
    Satellite_BuildCw1Hex(&cw1, cw1_hex_out);

    CwBeacon2_t cw2;
    memset(&cw2, 0, sizeof(cw2));
    if (b1_valid) {
        cw2.bat_temp = b1->bat_temp;
    }
    if (b2_valid) {
        cw2.gyro_x = b2->gyro_x;
        cw2.gyro_y = b2->gyro_y;
        cw2.gyro_z = b2->gyro_z;
        cw2.mag_x  = b2->mag_x;
        cw2.mag_y  = b2->mag_y;
        cw2.mag_z  = b2->mag_z;
        cw2.sol_plus_x  = (b2->sol_p1_v > 0U) ? 1U : 0U;
        cw2.sol_minus_y = (b2->sol_p2_v > 0U) ? 1U : 0U;
        cw2.sol_minus_z = (b2->sol_p3_v > 0U) ? 1U : 0U;
        cw2.sol_plus_y  = (b2->sol_p4_v > 0U) ? 1U : 0U;
        cw2.sol_plus_z  = (b2->sol_p5_v > 0U) ? 1U : 0U;
    }
    Satellite_BuildCw2Hex(&cw2, cw2_hex_out);
}

/*
 * ============================================================================
 * HELPER: DRAIN TELEMETRY PACKETS FROM M4 VIA SHARED SRAM2 RING BUFFER
 *
 * b1/b2 are the sheet-exact IPC payloads (struct beacon1_ipc_s/beacon2_ipc_s,
 * ipc/Sring_buffer.h -- field-for-field the same as Beacon1Packet_t/
 * Beacon2Packet_t minus head/type/chk_crc). *_valid starts false and only
 * ever becomes true once CPU1 has actually sent a real packet for that
 * beacon -- no fabricated "looks plausible" defaults are substituted when
 * the ring buffer is empty (it used to be seeded with hardcoded
 * 4.00V/25.0C/1.00A values that would transmit forever if CPU1 never sent
 * anything at all). Beacon/CW builders zero the corresponding fields
 * instead until real data exists.
 * ============================================================================
 */
static void M0_Drain_Telemetry(struct beacon1_ipc_s *b1, bool *b1_valid,
                                struct beacon2_ipc_s *b2, bool *b2_valid)
{
    bool got_b1 = false;
    bool got_b2 = false;

    live_telem_drain_all(b1, &got_b1, b2, &got_b2);

    if (got_b1)
    {
        *b1_valid = true;
        s_last_b1 = *b1;
        s_last_b1_valid = true;
        uart1_printf("[M0+ IPC] Received B1: %u.%02uV %u.%02uV %u.%uC %u.%uC %u.%uC\r\n",
                     b1->bat_v / 100U, b1->bat_v % 100U,
                     b1->sol_tot_v / 100U, b1->sol_tot_v % 100U,
                     b1->ant_p_t / 10U, b1->ant_p_t % 10U,
                     b1->bat_temp / 10U, b1->bat_temp % 10U,
                     b1->bpb_t / 10U, b1->bpb_t % 10U);
    }

    if (got_b2)
    {
        *b2_valid = true;
        s_last_b2 = *b2;
        s_last_b2_valid = true;
        uart1_printf("[M0+ IPC] Received B2: SOL_P1-5_V=%u,%u,%u,%u,%u POWER_STAT=0x%02X\r\n",
                     b2->sol_p1_v, b2->sol_p2_v, b2->sol_p3_v,
                     b2->sol_p4_v, b2->sol_p5_v, b2->power_stat);
    }
}

/*
 * ============================================================================
 * HELPER: PROCESS AND FORWARD TELECOMMAND TO M4
 * ============================================================================
 */
static void M0_Check_And_Forward_Command(void)
{
    if (rx_done_flag)
    {
        rx_done_flag = 0;
        uart1_printf(">>> [M0+ RX] Telecommand Received (%u Bytes) <<<\r\n", rx_frame_size);
        if (rx_frame_size >= CMD_PAYLOAD_LEN)
        {
            struct rx_command_s rx_cmd;
            rx_cmd.len = CMD_PAYLOAD_LEN;
            memcpy(rx_cmd.cmd, (const void *)rx_frame_buffer, CMD_PAYLOAD_LEN);
            rx_cmd.crc16 = 0;
            rb_rx_write(&rx_cmd);
            ipcc_m0_send(IPCC_CH_COMMAND);
            uart1_puts(">>> [M0+ RX] Forwarded Telecommand to M4 via IPCC Channel 3 <<<\r\n");
        }
        RadioApp_StartRx();
    }
    else if (rx_timeout_flag || rx_error_flag)
    {
        rx_timeout_flag = 0;
        rx_error_flag = 0;
        RadioApp_StartRx();
    }
}

/*
 * ============================================================================
 * HELPER: ACTIVE RX LISTENING WITH TELECOMMAND FORWARDING TO M4
 * - Deterministic CPU2 cycle delay ensures the loop never hangs or gets stuck.
 * - Continuously drains M4 telemetry to prevent SRAM2 ring buffer overflow.
 * - Emits periodic status heartbeats so ground/test operator sees countdown.
 * ============================================================================
 */
static void M0_Listening_Delay(uint32_t delay_ms, struct beacon1_ipc_s *b1, bool *b1_valid,
                                struct beacon2_ipc_s *b2, bool *b2_valid)
{
    RadioApp_StartRx();
    uart1_printf("\r\n>>> [M0+] ENTERING RX LISTEN MODE (Duration: %lu min %lu s) <<<\r\n",
                 (unsigned long)(delay_ms / 60000UL),
                 (unsigned long)((delay_ms % 60000UL) / 1000UL));

    uint32_t remaining = delay_ms;
    uint32_t last_heartbeat = 0;
    const uint32_t chunk_ms = 50;
    const uint32_t heartbeat_interval = (delay_ms > 60000UL) ? 60000UL : 10000UL;

    while (remaining > 0)
    {
        Process_UART_Command();
        M0_Check_And_Forward_Command();
        M0_Drain_Telemetry(b1, b1_valid, b2, b2_valid);

        uint32_t step = (remaining > chunk_ms) ? chunk_ms : remaining;
        CPU2_Delay_Ms(step);
        remaining -= step;
        last_heartbeat += step;

        /* Periodic heartbeat countdown so operator knows receiver is actively listening */
        if (last_heartbeat >= heartbeat_interval)
        {
            last_heartbeat = 0;
            if (remaining > 0)
            {
                uart1_printf(">>> [M0+ RX] Active Listening for Telecommands... (%lu min %lu s remaining) <<<\r\n",
                             (unsigned long)(remaining / 60000UL),
                             (unsigned long)((remaining % 60000UL) / 1000UL));
            }
        }
    }

    uart1_puts(">>> [M0+] RX Listen Window Completed. Proceeding to next beacon transmission. <<<\r\n\r\n");
}

/*
 * ============================================================================
 * MAIN ENTRY POINT (CORTEX-M0+ / CPU2)
 * ============================================================================
 */
int main(void) {
    /* As early as possible, before any significant call depth -- see
       MemStat_PaintStack's own comment on why this isn't a perfect
       from-reset-vector baseline but is close enough to be real. */
    MemStat_PaintStack();

    /*
     * ------------------------------------------------------------------------
     * STAGE 1: Low-Level CPU2 Hardware & Bus Initialization
     * ------------------------------------------------------------------------
     */
    Satellite_Hardware_Init();

    /*
     * ------------------------------------------------------------------------
     * STAGE 2: Satellite Subsystem & Application Initialization
     * ------------------------------------------------------------------------
     */
    Satellite_Init(NULL);

    /* IPCC Initial Handshake with Cortex-M4 */
    IPCC_Handshake();

    /* Retrieve active configuration pointer for runtime parameters and delays */
    const SatelliteConfig_t *cfg = Satellite_GetConfig();
    s_cfg = cfg;   /* TESTCW (debug backdoor) reuses this same config */

    uint32_t cycle = 0;

    /* No real telemetry has arrived from CPU1 yet -- zeroed, not a
       plausible-looking fabricated reading (see M0_Drain_Telemetry). */
    struct beacon1_ipc_s b1_data;
    struct beacon2_ipc_s b2_data;
    memset(&b1_data, 0, sizeof(b1_data));
    memset(&b2_data, 0, sizeof(b2_data));
    bool b1_valid = false;
    bool b2_valid = false;

    /* Arm radio in RX listening mode immediately */
    RadioApp_StartRx();

    /*
     * ------------------------------------------------------------------------
     * STAGE 3: Satellite Mission Executive Loop
     * Sequence: CW1 -> listen -> CW2 -> listen (continuous repeat)
     * ------------------------------------------------------------------------
     */
    while (1) {
        cycle++;

        Process_UART_Command();

        /* 1. Update latest telemetry from M4 shared ring buffer */
        M0_Drain_Telemetry(&b1_data, &b1_valid, &b2_data, &b2_valid);
        M0_Check_And_Forward_Command();

        /* 2. Build CW1 hex string (Morse, bit-packed per Sheet4) from the
           real/zeroed telemetry -- see Satellite_BuildAndSendTelemetry. */
        uart1_puts("CYCLE PHASE: CW1\r\n");
        Satellite_BuildAndSendTelemetry(&b1_data, b1_valid, &b2_data, b2_valid,
                                         cwBeaconlive_msg1, cwBeaconlive_msg2);

        uart1_printf("\r\n>>> [CYCLE #%lu] TRANSMITTING CW1: \"%s\"%s <<<\r\n",
                     (unsigned long)cycle, cwBeaconlive_msg1,
                     (b1_valid || b2_valid) ? "" : " (no real telemetry yet)");
        if (s_beacon_enabled) {
            uart1_puts("CW BEACON TX\r\n");
            Satellite_Send_CW_Beacon(cycle,
                                     "CW1",
                                     cfg->cwBeaconCallsign,
                                     cwBeaconlive_msg1,
                                     cfg->morseUnitMs,
                                     cfg->cwTuningCarrierDurationMs,
                                     cfg->cwTuningPostDelayMs,
                                     cfg->cwInterStringDelayMs);
        }

        /* 3. Listen RX after CW1 (30 Minutes) */
        uart1_puts("CYCLE PHASE: MISSION1\r\n");
        uart1_puts(">>> [M0+] ENTERING RX LISTEN MODE AFTER CW1 <<<\r\n");
        M0_Listening_Delay(SAT_LISTEN_DURATION_MS, &b1_data, &b1_valid, &b2_data, &b2_valid);

        /* 4. Update latest telemetry from M4 shared ring buffer */
        M0_Drain_Telemetry(&b1_data, &b1_valid, &b2_data, &b2_valid);
        M0_Check_And_Forward_Command();

        /* 5. Refresh CW2 hex string with the latest data */
        uart1_puts("CYCLE PHASE: CW2\r\n");
        Satellite_BuildAndSendTelemetry(&b1_data, b1_valid, &b2_data, b2_valid,
                                         cwBeaconlive_msg1, cwBeaconlive_msg2);

        uart1_printf("\r\n>>> [CYCLE #%lu] TRANSMITTING CW2: \"%s\"%s <<<\r\n",
                     (unsigned long)cycle, cwBeaconlive_msg2,
                     (b1_valid || b2_valid) ? "" : " (no real telemetry yet)");
        if (s_beacon_enabled) {
            uart1_puts("CW BEACON TX\r\n");
            Satellite_Send_CW_Beacon(cycle,
                                     "CW2",
                                     cfg->cwBeaconCallsign,
                                     cwBeaconlive_msg2,
                                     cfg->morseUnitMs,
                                     cfg->cwTuningCarrierDurationMs,
                                     cfg->cwTuningPostDelayMs,
                                     cfg->cwInterStringDelayMs);
        }

        /* 6. Listen RX after CW2 (30 Minutes) */
        uart1_puts("CYCLE PHASE: MISSION2\r\n");
        uart1_puts(">>> [M0+] ENTERING RX LISTEN MODE AFTER CW2 <<<\r\n");
        M0_Listening_Delay(SAT_LISTEN_DURATION_MS, &b1_data, &b1_valid, &b2_data, &b2_valid);
    }

    return 0;
}
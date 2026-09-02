/*
 * ipcc_m0plus_main.c
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
#include "radio_board_if.h"
#include "uart_debug.h"

#include "config.h"
#include "radio_app.h"
#include "s2s2com.h"
#include "s2s2_commands.h"
#include "s2s2_auth.h"
#include "cubus_ax25.h"
#include "ax25_digi.h"
#include "tx_history.h"
#include "cw_beacon.h"

const RadioConfig_t SatelliteProfile = {
    .txFrequency = DEFAULT_TX_FREQUENCY_HZ,
    .rxFrequency = DEFAULT_RX_FREQUENCY_HZ,
    .nodeId = NODE_ID_SATELLITE,
    .peerId = NODE_ID_GROUND
};

static const cubus_ax25_link_t s_link = {
    .dest = { .callsign = AX25_GROUND_CALLSIGN, .ssid = AX25_GROUND_SSID },
    .src  = { .callsign = AX25_SATELLITE_CALLSIGN, .ssid = AX25_SATELLITE_SSID },
};

#define BURST_PACKET_COUNT  100
#define BEACON_INTERVAL_MS  5000
#define TX_TIMEOUT_MS       1000

/* Real CW Morse beacon and structured FX.25 housekeeping beacons --
   real amateur-CubeSat practice (see CPU2/README_GMSK_S2S2_ARCHITECTURE.md),
   all gated by the same s_beacon_enabled bench-test toggle as the 5s text
   ping above (DEBUG_BUILD only; always on in production, same as it).

   Sequenced as one strict cycle, not three independent interval timers:
   [CW] -> [Beacon1] -> [Mission 90s] -> [Beacon2] -> back to [CW]. CW/
   Beacon1/Beacon2 slot lengths are just however long their actual radio
   TX takes (Morse duration, FX.25 airtime, RX resume) -- only Mission
   gets a fixed, dedicated timer, started fresh the moment its phase
   begins, uninterrupted by any beacon. So the CW-to-CW gap is ~90s plus
   whatever the three TX phases actually took, not a fixed number. */
#define CYCLE_MISSION_MS   90000

#define CMD_LINE_MAX 1400 /* "TX " + up to MAX_PAYLOAD_LEN (659) bytes hex-encoded (1318 chars) + margin */

typedef enum {
    STATE_IDLE_RX = 0,
    STATE_PROCESS_COMMAND,
    STATE_TRANSMIT_BURST,
} AppState_t;

volatile AppState_t app_state = STATE_IDLE_RX;
volatile uint8_t tx_busy = 0;
volatile uint8_t rx_chunk_buffer[FX25_FRAME_LEN];
volatile uint16_t rx_chunk_size = 0;
volatile uint8_t rx_done_flag = 0;
volatile uint8_t rx_timeout_flag = 0;
volatile uint8_t rx_error_flag = 0;

static cubus_ax25_reassembler_t s_reassembler;
static uint16_t s_tx_seq = 0;
static uint32_t last_beacon_tick_ms = 0;
static uint32_t last_burst_finish_time = 0;

/* CW/Beacon1/Mission/Beacon2 cycle sequencer state -- see CYCLE_MISSION_MS
   comment above. */
typedef enum {
    CYCLE_PHASE_CW = 0,
    CYCLE_PHASE_BEACON1,
    CYCLE_PHASE_MISSION,
    CYCLE_PHASE_BEACON2,
} CyclePhase_t;

static CyclePhase_t s_cycle_phase = CYCLE_PHASE_CW;
static uint32_t s_mission_start_ms = 0;
static tx_history_t s_tx_history;
#ifdef DEBUG_BUILD
static bool s_beacon_enabled = true;
#endif

/* Bench/flight-mode switch: when true, every DEBUG_BUILD UART backdoor
   (TX/POWER/BEACON/TEST*) is refused (falls through to UNKNOWN COMMAND)
   regardless of build type, so the unit behaves like a real production
   satellite -- all further interaction has to go over real RF via the
   ground station. Togglable back off from the local bench UART cable
   (physical access = trusted), unlike a real flight unit. */
static bool s_satellite_mode = false;

/* Digipeater mission (CMD_DIGIPEATER/CMD_ABORT_MISSION), production
   feature (not DEBUG_BUILD-gated) -- see DPM_ARCHITECTURE.md. */
static bool s_digipeater_enabled = false;

/* Interrupt Status Callbacks */
void OnTxDone(void) { tx_busy = 0; }
void OnTxTimeout(void) { uart2_puts("TX TIMEOUT ERROR\r\n"); tx_busy = 0; }
void OnRxTimeout(void) { rx_timeout_flag = 1; }
void OnRxError(void) { rx_error_flag = 1; }

volatile int16_t rx_last_rssi = 0;
volatile int8_t rx_last_snr = 0;

void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr) {
    rx_last_rssi = rssi;
    rx_last_snr = snr;
    uint16_t copy_len = (size < FX25_FRAME_LEN) ? size : FX25_FRAME_LEN;
    memcpy((void *)rx_chunk_buffer, payload, copy_len);
    rx_chunk_size = copy_len;
    rx_done_flag = 1;
}

extern SUBGHZ_HandleTypeDef hsubghz;
void SUBGHZ_Radio_IRQHandler(void) { HAL_SUBGHZ_IRQHandler(&hsubghz); }

static void Print_Hex_Bytes(const uint8_t *buf, uint16_t len) {
    char line[56]; uint16_t pos = 0; uint16_t on_this_line = 0;
    for (uint16_t i = 0; i < len; i++) {
        pos += snprintf(&line[pos], sizeof(line) - pos, "%02X ", buf[i]);
        on_this_line++;
        if (on_this_line == 16 || i == (uint16_t)(len - 1)) {
            uart2_puts(line); uart2_puts("\r\n"); pos = 0; on_this_line = 0;
        }
    }
}

static void Print_Payload_ASCII(const uint8_t *buf, uint16_t len) {
    if (len == 0) {
        uart2_puts(" Payload (text) : (none)\r\n");
        return;
    }
    char line[96];
    uint16_t max_chars = (uint16_t)(sizeof(line) - 24);
    uint16_t n = (len > max_chars) ? max_chars : len;
    uint16_t pos = 0;
    pos += (uint16_t)snprintf(&line[pos], sizeof(line) - pos, " Payload (text): \"");
    for (uint16_t i = 0; i < n; i++) {
        uint8_t c = buf[i];
        line[pos++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    pos += (uint16_t)snprintf(&line[pos], sizeof(line) - pos, "\"\r\n");
    uart2_puts(line);
}

extern volatile int16_t rx_last_rssi;
extern volatile int8_t rx_last_snr;

static void Print_Reassembled_Fields(const char *label, const cubus_ax25_reassembler_t *r) {
    uart2_printf(" --- %s ---\r\n", label);
    uart2_printf(" version=%u type=%u src=%u dst=%u seq=%u session=0x%04X flags=0x%02X\r\n",
                 r->version, r->type, r->src_id, r->dst_id,
                 r->seq_num, r->session_id, r->flags);
    uart2_printf(" payload_len=%u rs_bytes_corrected=%d rssi=%d dBm snr=%d dB\r\n",
                 r->payload_len, r->rs_bytes_corrected, (int)rx_last_rssi, (int)rx_last_snr);
    Print_Hex_Bytes(r->payload, r->payload_len);
    Print_Payload_ASCII(r->payload, r->payload_len);
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
    while (tx_busy != 0) {
        if ((Get_Time_Ms() - start_ms) > timeout_ms) {
            uart2_puts("ERROR: TX timeout\r\n");
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
static bool Send_FX25_Frame(const uint8_t frame[FX25_FRAME_LEN]) {
    return Radio_Send_And_Wait((uint8_t *)frame, FX25_FRAME_LEN, TX_TIMEOUT_MS);
}

/* Fragments one S2S2 message across AX.25/FX.25 frames and sends each.
   Also records it in the tx history ring buffer (tx_history.h) so a later
   TYPE_GAP_REPORT or CMD_REQUEST_RETRANSMIT can actually resend it instead
   of dead-ending in ACK_STATUS_DATA_UNAVAILABLE. */
static bool Send_Message(uint8_t type, uint8_t dst_id, uint16_t seq_num, uint8_t flags,
                          const uint8_t *payload, uint16_t payload_len) {
    uint8_t frames[CUBUS_AX25_MAX_FRAGMENTS][FX25_FRAME_LEN];
    int num_frames = 0;
    if (cubus_ax25_fragment(&s_link, 1, type, NODE_ID_SATELLITE, dst_id,
                             seq_num, DEFAULT_SESSION_ID, flags,
                             payload, payload_len, frames, &num_frames) != 0) {
        uart2_puts("cubus_ax25_fragment FAILED\r\n");
        return false;
    }

    /* Don't re-store a retransmit: it's already a copy of an existing
       history entry, and storing it would advance the ring cursor while
       Retransmit_All_History() is iterating the very array it lives in. */
    if ((flags & FLAG_RETRANSMIT) == 0) {
        tx_history_store(&s_tx_history, seq_num, type, dst_id, flags, payload, payload_len);
    }

    bool all_ok = true;
    for (int f = 0; f < num_frames; f++) {
        if (!Send_FX25_Frame(frames[f])) all_ok = false;
    }
    return all_ok;
}

/* Resends one previously-sent message by its original seq_num, if it's
   still in the tx history buffer. Reuses the ORIGINAL seq_num (not a new
   one) so the ground station's gap tracker marks the right entry received;
   flags get FLAG_RETRANSMIT ORed in on top of whatever was originally set. */
static bool Retransmit_By_Seq(uint16_t seq_num) {
    const tx_history_entry_t *e = tx_history_find(&s_tx_history, seq_num);
    if (e == NULL) return false;
    return Send_Message(e->type, e->dst_id, e->seq_num, (uint8_t)(e->flags | FLAG_RETRANSMIT),
                         e->payload, e->payload_len);
}

/* CMD_REQUEST_RETRANSMIT carries no specific seq list (unlike
   TYPE_GAP_REPORT) -- it's a coarse "resend what you've still got" nudge.
   Resends every entry currently in the tx history buffer. Returns the
   count actually resent. */
static int Retransmit_All_History(void) {
    int count = 0;
    for (int i = 0; i < TX_HISTORY_LEN; i++) {
        if (!s_tx_history.entries[i].used) continue;
        const tx_history_entry_t *e = &s_tx_history.entries[i];
        if (Send_Message(e->type, e->dst_id, e->seq_num, (uint8_t)(e->flags | FLAG_RETRANSMIT),
                          e->payload, e->payload_len)) {
            count++;
        }
    }
    return count;
}

/* ---- Digipeater mission (DPM_ARCHITECTURE.md) ----
 *
 * Real amateur digipeating: relays a third-party ham's uplinked AX.25 UI
 * frame if (a) it lists our own callsign in its digipeater path with the
 * H-bit still clear, and (b) we haven't already relayed this exact frame
 * recently. Completely independent of the S2S2 command/telemetry link --
 * see Try_Digipeat's call site in STATE_PROCESS_COMMAND for why this only
 * ever runs on frames S2S2's own parsing has already given up on. */

#define DIGI_DUP_TABLE_LEN 16   /* mirrors TX_HISTORY_LEN's sizing logic */
#define DIGI_DUP_TTL_MS    8000

typedef struct {
    bool     used;
    uint32_t hash;
    uint32_t seen_at_ms;
} digi_dup_entry_t;

static digi_dup_entry_t s_digi_dup[DIGI_DUP_TABLE_LEN];
static int s_digi_dup_next = 0;

/* Cheap content fingerprint: the frame's own (already FCS-validated)
   trailing CRC combined with its length. Not a security mechanism --
   just enough to catch "the same transmission again" within a short
   window, same as any APRS digipeater's dup cache. */
static uint32_t Digi_Frame_Hash(const uint8_t *ax25_bytes, uint16_t len) {
    uint16_t fcs = (uint16_t)(ax25_bytes[len - 2] | (ax25_bytes[len - 1] << 8));
    return ((uint32_t)fcs << 16) ^ (uint32_t)len;
}

/* Returns true if this exact frame was already relayed within
   DIGI_DUP_TTL_MS; records it (evicting the oldest ring slot) either
   way, so the caller only ever needs to call this once per frame. */
static bool Digi_Is_Duplicate(uint32_t hash) {
    uint32_t now = Get_Time_Ms();
    for (int i = 0; i < DIGI_DUP_TABLE_LEN; i++) {
        if (s_digi_dup[i].used && s_digi_dup[i].hash == hash &&
            (now - s_digi_dup[i].seen_at_ms) < DIGI_DUP_TTL_MS) {
            return true;
        }
    }
    s_digi_dup[s_digi_dup_next].used = true;
    s_digi_dup[s_digi_dup_next].hash = hash;
    s_digi_dup[s_digi_dup_next].seen_at_ms = now;
    s_digi_dup_next = (s_digi_dup_next + 1) % DIGI_DUP_TABLE_LEN;
    return false;
}

/* Attempts to digipeat one already FX.25/RS-received frame that S2S2's
   own parsing rejected. Any failure at any stage (not real FX.25, not a
   digi-shaped AX.25 frame, no unused hop addressed to us, duplicate,
   re-encode/TX failure) just returns -- this path never reports an error
   to anyone, since a frame reaching here was never ours to complain
   about in the first place. */
static void Try_Digipeat(const uint8_t frame_copy[FX25_FRAME_LEN], uint16_t frame_len) {
    (void)frame_len; /* fx25_decode_frame reads a fixed FX25_FRAME_LEN, same as cubus_ax25_accept_frame does */

    uint8_t fx25_scratch[FX25_FRAME_LEN];
    memcpy(fx25_scratch, frame_copy, sizeof(fx25_scratch));

    uint8_t ax25_bytes[AX25_DIGI_MAX_FRAME_LEN];
    int rs_corrected = 0;
    int ax25_len = fx25_decode_frame(fx25_scratch, ax25_bytes, sizeof(ax25_bytes), &rs_corrected);
    if (ax25_len < 0) return; /* not even a valid FX.25 frame -- genuine noise, not ours to relay */

    ax25_full_frame_t f;
    if (ax25_digi_parse(ax25_bytes, (uint16_t)ax25_len, &f) != 0) return; /* not digi-shaped either */

    if (Digi_Is_Duplicate(Digi_Frame_Hash(ax25_bytes, (uint16_t)ax25_len))) {
        uart2_puts("DIGI: duplicate, not re-relaying\r\n");
        return;
    }

    int hop_index = -1;
    for (uint8_t i = 0; i < f.num_digis; i++) {
        if (!f.digis[i].has_been_repeated &&
            strcmp(f.digis[i].addr.callsign, AX25_SATELLITE_CALLSIGN) == 0 &&
            f.digis[i].addr.ssid == AX25_SATELLITE_SSID) {
            hop_index = i;
            break;
        }
    }
    if (hop_index < 0) return; /* not addressed to us as a digipeater hop */

    f.digis[hop_index].has_been_repeated = true;

    uint8_t rebuilt[AX25_DIGI_MAX_FRAME_LEN];
    uint16_t rebuilt_len = 0;
    if (ax25_digi_rebuild(&f, rebuilt, sizeof(rebuilt), &rebuilt_len) != 0) return;

    uint8_t out_frame[FX25_FRAME_LEN];
    if (fx25_encode_frame(rebuilt, rebuilt_len, out_frame) != 0) return;

    if (Send_FX25_Frame(out_frame)) {
        uart2_printf("DIGI: relayed via hop %d (%s-%u), %u info byte(s), rs_corrected=%d\r\n",
                      hop_index, f.digis[hop_index].addr.callsign, f.digis[hop_index].addr.ssid,
                      f.info_len, rs_corrected);
    } else {
        uart2_puts("DIGI: relay TX failed\r\n");
    }
}

/* Replies to a telecommand with a TYPE_ACK frame carrying the standard
   status code (see s2s2_commands.h) and the seq_num of the command it's
   acknowledging. */
static void Send_Ack(uint8_t status, uint8_t data_id, uint16_t acked_seq) {
    cubus_ack_t ack = { .status = status, .data_id = data_id, .acked_seq = acked_seq };
    uint8_t payload[ACK_PAYLOAD_LEN];
    ack_pack(&ack, payload);

    uint16_t seq = s_tx_seq++;
    uart2_printf("TX ACK: status=0x%02X (%s) for seq=%u\r\n", status, ack_status_str(status), acked_seq);
    Send_Message(TYPE_ACK, NODE_ID_GROUND, seq, 0, payload, sizeof(payload));
}

/* BURST_PACKET_COUNT-packet Telemetry Burst Stream, sequenced with a
   shared session_id and the last packet flagged FLAG_LAST_IN_BURST. */
static void Transmit_Burst(void) {
    uart2_printf("BURST START: %d telemetry frames, session=0x%04X\r\n",
                 BURST_PACKET_COUNT, DEFAULT_SESSION_ID);

    uint32_t sent_ok = 0, sent_fail = 0;
    for (int i = 0; i < BURST_PACKET_COUNT; i++) {
        char text[MAX_PAYLOAD_LEN];
        int text_len = snprintf(text, sizeof(text),
            "S2S2 telemetry #%03d - Namaste Everyone! From Antarikchya Nepal. CubeSat Beacon Test",
            i);
        if (text_len < 0) text_len = 0;
        if ((uint16_t)text_len > MAX_PAYLOAD_LEN) text_len = MAX_PAYLOAD_LEN;

        uint8_t flags = (i == BURST_PACKET_COUNT - 1) ? FLAG_LAST_IN_BURST : 0;
        uint16_t seq = s_tx_seq++;

        bool ok = Send_Message(TYPE_TELEMETRY, NODE_ID_GROUND, seq, flags,
                                (const uint8_t *)text, (uint16_t)text_len);
        if (ok) sent_ok++; else sent_fail++;
        uart2_printf(" TX frame %3d/%d : seq=%u %s\r\n", i + 1, BURST_PACKET_COUNT, seq,
                     ok ? "sent OK" : "TIMEOUT");
        CPU2_Delay_Ms(25);
    }

    uart2_printf("BURST COMPLETE (%lu/%d sent cleanly, %lu failed)\r\n",
                 (unsigned long)sent_ok, BURST_PACKET_COUNT, (unsigned long)sent_fail);
}

/* Asynchronous Housekeeping Beacon Timer Loop */
static void Send_Beacon_If_Due(void) {
    if (app_state != STATE_IDLE_RX) {
        return;
    }
#ifdef DEBUG_BUILD
    if (!s_beacon_enabled) {
        return;
    }
#endif
    uint32_t now = Get_Time_Ms();
    if ((now - last_beacon_tick_ms) < BEACON_INTERVAL_MS) {
        return;
    }
    last_beacon_tick_ms = now;

    const char *beacon_text = "NEPSAT BEACON";
    uint16_t plen = (uint16_t)strlen(beacon_text);
    uint16_t seq = s_tx_seq++;

    uart2_puts("\r\nBEACON TX (5s tick)\r\n");
    /* TYPE_RAW, not TYPE_TELEMETRY: this is a plain ASCII placeholder, not
       a [data_id][struct] telemetry payload (see s2s2_commands.h) -- a
       receiver that follows that spec would otherwise eat the first byte
       as a bogus data_id (e.g. showing "EPSAT BEACON" instead of "NEPSAT
       BEACON"). */
    Send_Message(TYPE_RAW, NODE_ID_GROUND, seq, FLAG_LAST_IN_BURST,
                 (const uint8_t *)beacon_text, plen);

    /* Back to continuous RX listening on 437.375 MHz */
    rx_done_flag = 0;
    rx_timeout_flag = 0;
    rx_error_flag = 0;
    RadioApp_StartRx();
}

/* Coarse single-digit (0-9) telemetry code for the CW beacon message --
   V, Gx, Gy, Gz, T, matching real amateur-CubeSat CW beacon practice
   (JY1SAT/AO-40 heritage). NO ADCS/EPDM sensor drivers are wired up on
   this Nucleo bench build -- same honest gap as CMD_GET_ADCS/
   CMD_GET_HOUSEKEEPING's "not available on this build" replies above.
   These five digits are fixed placeholders until real battery/gyro/
   thermal hardware exists; swapping in real ADC/IMU reads here is the
   entire integration point, nothing else about the beacon changes. */
static void Get_Cw_Telemetry_Digits(char out[6]) {
    out[0] = '0'; /* V  */
    out[1] = '0'; /* Gx */
    out[2] = '0'; /* Gy */
    out[3] = '0'; /* Gz */
    out[4] = '0'; /* T  */
    out[5] = '\0';
}

/* Real, listenable-by-ear Morse CW beacon: "DE <CALLSIGN> <5 digits>".
   Independent of the S2S2 AX.25/FX.25 link entirely -- keys the raw RF
   carrier directly (RadioApp_KeyCw/cw_beacon.h), not a packet of any
   kind, so it needs no seq_num/ACK/tx_history bookkeeping. */
static void Send_Cw_Beacon(void) {
    char digits[6];
    Get_Cw_Telemetry_Digits(digits);

    char message[32];
    snprintf(message, sizeof(message), "DE %s %s", AX25_SATELLITE_CALLSIGN, digits);

    uart2_printf("\r\nCW BEACON TX: \"%s\"\r\n", message);
    cw_beacon_send(message, CW_DEFAULT_WPM);

    /* cw_beacon_send leaves the radio in Standby, not Rx -- same
       requirement as RadioApp_Send, resume normal GMSK listening. */
    rx_done_flag = 0;
    rx_timeout_flag = 0;
    rx_error_flag = 0;
    RadioApp_StartRx();
}

/* Primary housekeeping beacon (structured, FX.25-framed, decodable by
   any AX.25/FX.25-aware ground station). tim_day/tim_hour are real
   (mission elapsed time since boot, from Get_Time_Ms()) -- everything
   else in beacon1_t is a placeholder pending real battery/thermal/EPDM
   hardware (same gap as CMD_GET_HEALTH/HOUSEKEEPING above), zeroed
   rather than fabricated. */
static void Send_Beacon1(void) {
    beacon1_t b;
    memset(&b, 0, sizeof(b));
    uint32_t uptime_s = Get_Time_Ms() / 1000;
    b.tim_day = (uint16_t)(uptime_s / 86400);
    b.tim_hour = (uint8_t)((uptime_s / 3600) % 24);

    uint8_t payload[1 + BEACON1_PAYLOAD_LEN];
    payload[0] = DATA_ID_BEACON1;
    beacon1_pack(&b, &payload[1]);

    uint16_t seq = s_tx_seq++;
    uart2_printf("\r\nBEACON1 TX (cycle): day=%u hour=%u\r\n", b.tim_day, b.tim_hour);
    Send_Message(TYPE_TELEMETRY, NODE_ID_GROUND, seq, FLAG_LAST_IN_BURST, payload, sizeof(payload));

    rx_done_flag = 0;
    rx_timeout_flag = 0;
    rx_error_flag = 0;
    RadioApp_StartRx();
}

/* Redundant beacon (solar panel + ADCS data) -- same structure/honesty
   note as Send_Beacon1; beacon2_t has no time field of its own to
   fill with a real value, so it's entirely placeholder until real
   solar-panel/gyro/magnetometer hardware exists. */
static void Send_Beacon2(void) {
    beacon2_t b;
    memset(&b, 0, sizeof(b));

    uint8_t payload[1 + BEACON2_PAYLOAD_LEN];
    payload[0] = DATA_ID_BEACON2;
    beacon2_pack(&b, &payload[1]);

    uint16_t seq = s_tx_seq++;
    uart2_puts("\r\nBEACON2 TX (cycle)\r\n");
    Send_Message(TYPE_TELEMETRY, NODE_ID_GROUND, seq, FLAG_LAST_IN_BURST, payload, sizeof(payload));

    rx_done_flag = 0;
    rx_timeout_flag = 0;
    rx_error_flag = 0;
    RadioApp_StartRx();
}

/* Drives the [CW]...[Beacon1][Mission 90s][Beacon2]...[CW] cycle, one
   phase-step per call. CW/Beacon1/Beacon2 each fire once then advance
   immediately (real TX duration is whatever it is); Mission blocks phase
   advance until its own dedicated CYCLE_MISSION_MS timer -- started the
   instant Mission begins, not from CW -- has run out uninterrupted.
   Ordinary command RX/processing (STATE_PROCESS_COMMAND) is untouched by
   this and keeps working normally through every phase, Mission included. */
static void Cycle_Tick(void) {
    if (app_state != STATE_IDLE_RX) {
        return;
    }

    switch (s_cycle_phase) {
        case CYCLE_PHASE_CW:
#ifdef DEBUG_BUILD
            if (s_beacon_enabled) {
#endif
                Send_Cw_Beacon();
#ifdef DEBUG_BUILD
            }
#endif
            s_cycle_phase = CYCLE_PHASE_BEACON1;
            uart2_puts("CYCLE PHASE: BEACON1\r\n");
            break;

        case CYCLE_PHASE_BEACON1:
#ifdef DEBUG_BUILD
            if (s_beacon_enabled) {
#endif
                Send_Beacon1();
#ifdef DEBUG_BUILD
            }
#endif
            s_mission_start_ms = Get_Time_Ms();
            s_cycle_phase = CYCLE_PHASE_MISSION;
            uart2_puts("CYCLE PHASE: MISSION\r\n");
            break;

        case CYCLE_PHASE_MISSION:
            if ((Get_Time_Ms() - s_mission_start_ms) < CYCLE_MISSION_MS) {
                return;
            }
            s_cycle_phase = CYCLE_PHASE_BEACON2;
            uart2_puts("CYCLE PHASE: BEACON2\r\n");
            break;

        case CYCLE_PHASE_BEACON2:
#ifdef DEBUG_BUILD
            if (s_beacon_enabled) {
#endif
                Send_Beacon2();
#ifdef DEBUG_BUILD
            }
#endif
            s_cycle_phase = CYCLE_PHASE_CW;
            uart2_puts("CYCLE PHASE: CW\r\n");
            break;
    }
}

#ifdef DEBUG_BUILD
/* Case-insensitive prefix match, e.g. str_starts_ieq("TX deadbeef", "TX ").
   Only needed by the DEBUG_BUILD-only TX/POWER UART commands below -- not
   compiled into a production image at all, not just gated at runtime. */
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

/* Case-insensitive exact match, e.g. str_ieq("beacon on", "BEACON ON"). */
static bool str_ieq(const char *s, const char *other) {
    return str_starts_ieq(s, other) && s[strlen(other)] == '\0';
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes an even-length hex string into bytes. Returns the byte count,
   or -1 on odd length, a non-hex character, or more bytes than max_out. */
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
#endif /* DEBUG_BUILD */

static bool USART_TryReadLine(char *out, uint16_t max) {
    static char buffer[CMD_LINE_MAX];
    static uint16_t index = 0;
    uint8_t c;

    if (!uart2_try_getc(&c)) return false;

    uart2_putc((char)c);

    if (c == '\r' || c == '\n') {
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

/* Polled every main-loop iteration regardless of app_state.
   DEBUG_BUILD only: "TX <hex>" transmits arbitrary host-supplied bytes
   downlink as TYPE_RAW (so a host GUI/terminal can key any test payload
   out over RF), "POWER <dbm>" overrides the TX power for bench
   testing (see RadioApp_SetTxPower / config.h's BENCH_TX_POWER_DBM), and
   "BEACON ON"/"BEACON OFF" toggles the 5s housekeeping beacon (useful for
   isolating RF captures to only manually-triggered traffic). None of this
   is compiled into a production build - see Makefile's BUILD=production. */
static void Process_UART_Command(void) {
    static char command[CMD_LINE_MAX];
    if (!USART_TryReadLine(command, sizeof(command))) return;

    /* Always available, any build: locks/unlocks every debug backdoor
       below regardless of build type, so a DEBUG_BUILD unit on the
       bench can be made to behave like real flight hardware. Reversible
       from this same local UART cable (physical access = trusted). */
    if (str_ieq(command, "SATMODE ON")) {
        s_satellite_mode = true;
        uart2_puts("SATELLITE MODE ON - debug/test commands disabled\r\n");
        return;
    } else if (str_ieq(command, "SATMODE OFF")) {
        s_satellite_mode = false;
        uart2_puts("SATELLITE MODE OFF\r\n");
        return;
    }

#ifdef DEBUG_BUILD
    if (!s_satellite_mode && str_starts_ieq(command, "TX ")) {
        uint8_t payload[MAX_PAYLOAD_LEN];
        int n = hex_decode(command + 3, payload, sizeof(payload));
        if (n < 0) {
            uart2_puts("ERR bad hex (need an even number of 0-9/a-f characters, "
                       "e.g. TX 48656C6C6F)\r\n");
            return;
        }
        uint16_t seq = s_tx_seq++;
        bool ok = Send_Message(TYPE_RAW, NODE_ID_GROUND, seq, FLAG_LAST_IN_BURST,
                                payload, (uint16_t)n);
        uart2_printf("%s (%d byte%s)\r\n", ok ? "OK sent" : "ERR send failed",
                     n, n == 1 ? "" : "s");
    } else if (!s_satellite_mode && str_starts_ieq(command, "POWER ")) {
        int dbm = atoi(command + 6);
        RadioApp_SetTxPower((int8_t)dbm);
    } else if (!s_satellite_mode && str_ieq(command, "BEACON ON")) {
        s_beacon_enabled = true;
        uart2_puts("BEACON ON\r\n");
    } else if (!s_satellite_mode && str_ieq(command, "BEACON OFF")) {
        s_beacon_enabled = false;
        uart2_puts("BEACON OFF\r\n");
    } else if (!s_satellite_mode && str_ieq(command, "TESTCW")) {
        uart2_puts("TESTCW: forcing an immediate CW beacon send\r\n");
        Send_Cw_Beacon();
    } else if (!s_satellite_mode && str_ieq(command, "TESTBEACON1")) {
        uart2_puts("TESTBEACON1: forcing an immediate Beacon1 send\r\n");
        Send_Beacon1();
    } else if (!s_satellite_mode && str_ieq(command, "TESTBEACON2")) {
        uart2_puts("TESTBEACON2: forcing an immediate Beacon2 send\r\n");
        Send_Beacon2();
    } else if (!s_satellite_mode && str_starts_ieq(command, "TESTDIGI ")) {
        uint8_t frame[FX25_FRAME_LEN];
        int n = hex_decode(command + 9, frame, sizeof(frame));
        if (n != FX25_FRAME_LEN) {
            uart2_printf("ERR TESTDIGI needs exactly %d hex-encoded bytes (%d given)\r\n",
                         FX25_FRAME_LEN, n < 0 ? -1 : n);
            return;
        }
        uart2_printf("TESTDIGI: feeding a %d-byte frame into Try_Digipeat() "
                     "(digipeater currently %s for real traffic)\r\n",
                     FX25_FRAME_LEN, s_digipeater_enabled ? "enabled" : "disabled");
        Try_Digipeat(frame, (uint16_t)n);
    } else
#endif
    if (command[0] != 0) {
        uart2_printf("UNKNOWN COMMAND : %s\r\n", command);
    }
}

int main(void) {
    SystemInit();
    HAL_Init();
    SysTick_Init_CPU2(48000000UL);
    uart2_puts("HAL_Init done\r\n");
    uart2_init();
    uart2_puts("\r\n=== STM32WL55 GMSK / S2S2 over AX.25+FX.25 [satellite] ===\r\n");
#ifdef DEBUG_BUILD
    uart2_puts("*** DEBUG BUILD - TX <hex> / POWER <dbm> UART backdoors ENABLED ***\r\n");
#else
    uart2_puts("*** PRODUCTION BUILD ***\r\n");
#endif

    RBI_Init();
    uart2_puts("RF FRONT END OK\r\n");

    MX_SUBGHZ_Init();
    HAL_NVIC_SetPriority(SUBGHZ_Radio_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(SUBGHZ_Radio_IRQn);
    __enable_irq();

    RadioEvents_t events = {
        .TxDone = OnTxDone, .TxTimeout = OnTxTimeout,
        .RxDone = OnRxDone, .RxTimeout = OnRxTimeout, .RxError = OnRxError
    };

    cubus_proto_init();
    RadioApp_Init(&SatelliteProfile, &events);
    cubus_ax25_reassembler_init(&s_reassembler);
    tx_history_init(&s_tx_history);
    RadioApp_StartRx();

    uart2_puts("LISTENING FOR COMMAND (and beaconing every 5s)...\r\n");
#ifdef DEBUG_BUILD
    uart2_puts("Type TX <hex> to transmit arbitrary bytes downlink, e.g. TX 48656C6C6F\r\n");
    uart2_puts("Type POWER <dbm> to override TX power for bench testing\r\n");
    uart2_puts("Type BEACON ON / BEACON OFF to toggle all beaconing "
               "(5s text, plus the CW/Beacon1/Mission/Beacon2 cycle)\r\n");
#endif

    app_state = STATE_IDLE_RX;
    last_beacon_tick_ms = Get_Time_Ms();
    s_cycle_phase = CYCLE_PHASE_CW;

    while (1) {
        switch (app_state) {
            case STATE_IDLE_RX: {
                if (rx_timeout_flag || rx_error_flag) {
                    rx_timeout_flag = 0;
                    rx_error_flag = 0;
                }
                if (rx_done_flag) {
                    rx_done_flag = 0;
                    app_state = STATE_PROCESS_COMMAND;
                }
                break;
            }

            case STATE_PROCESS_COMMAND: {
                uint8_t frame_copy[FX25_FRAME_LEN];
                memset(frame_copy, 0, sizeof(frame_copy));
                memcpy(frame_copy, (const void *)rx_chunk_buffer, rx_chunk_size);

                int accept_rc = cubus_ax25_accept_frame(&s_reassembler, frame_copy);

                if (accept_rc < 0) {
                    uart2_printf("frame rejected (bad FX.25 tag/RS, bad AX.25 FCS, or malformed) "
                                 "rssi=%d dBm snr=%d dB - still listening\r\n",
                                 (int)rx_last_rssi, (int)rx_last_snr);
                    /* Not a valid S2S2 fragment for us -- if the digipeater
                       mission is active, give the same raw bytes a chance at
                       the digipeat path before giving up on the frame (see
                       DPM_ARCHITECTURE.md Section 1: the two paths fork
                       below cubus_ax25.c, not above it). S2S2 traffic always
                       gets first look, unconditionally, right above. */
                    if (s_digipeater_enabled) {
                        Try_Digipeat(frame_copy, rx_chunk_size);
                    }
                    app_state = STATE_IDLE_RX;
                    break;
                }
                if (accept_rc == 0) {
                    /* Message still incomplete - keep listening for more fragments. */
                    app_state = STATE_IDLE_RX;
                    break;
                }

                uart2_puts("\r\n-----------------------------------\r\n");
                uart2_puts("VALID S2S2 MESSAGE REASSEMBLED - PROCESSING COMMAND...\r\n");
                Print_Reassembled_Fields("RX", &s_reassembler);

                if (s_reassembler.type == TYPE_GAP_REPORT && s_reassembler.dst_id == NODE_ID_SATELLITE) {
                    /* Payload = big-endian uint16 list of missing seq numbers
                       (see gs_main.c's Send_Gap_Report / format.md). Resend
                       whichever ones are still in the tx history buffer. */
                    uint16_t n_missing = (uint16_t)(s_reassembler.payload_len / 2);
                    uart2_printf("GAP REPORT RECEIVED: %u missing seq number(s)\r\n", n_missing);
                    int resent = 0, unavailable = 0;
                    for (uint16_t i = 0; i < n_missing; i++) {
                        uint16_t seq = (uint16_t)((s_reassembler.payload[i * 2] << 8) |
                                                   s_reassembler.payload[i * 2 + 1]);
                        if (Retransmit_By_Seq(seq)) {
                            resent++;
                        } else {
                            unavailable++;
                            uart2_printf(" seq=%u no longer in tx history - cannot retransmit\r\n", seq);
                        }
                    }
                    uart2_printf("GAP REPORT: resent %d, %d unavailable\r\n", resent, unavailable);
                    app_state = STATE_IDLE_RX;
                    break;
                }

                if (s_reassembler.type != TYPE_TELECOMMAND || s_reassembler.dst_id != NODE_ID_SATELLITE) {
                    uart2_puts(" *** COMMAND RECEIVED: NO (not a telecommand addressed to us) ***\r\n");
                    app_state = STATE_IDLE_RX;
                    break;
                }
                cubus_command_t cmd;
                if (command_unpack(s_reassembler.payload, s_reassembler.payload_len, &cmd) != 0) {
                    uart2_puts(" *** COMMAND RECEIVED: NO (payload too short for a command) ***\r\n");
                    if (s_reassembler.flags & FLAG_ACK_REQUESTED)
                        Send_Ack(ACK_STATUS_PACKET_ERROR, DATA_ID_GENERAL, s_reassembler.seq_num);
                    app_state = STATE_IDLE_RX;
                    break;
                }

                uart2_printf(" *** COMMAND RECEIVED: YES (from node %u, opcode 0x%02X) ***\r\n",
                             s_reassembler.src_id, cmd.opcode);
                bool want_ack = (s_reassembler.flags & FLAG_ACK_REQUESTED) != 0;
                uint16_t cmd_seq = s_reassembler.seq_num;

                /* Authenticate before dispatching anything (see s2s2_auth.h):
                   wrong MAC -> forged/tampered command, reject outright.
                   Right MAC but stale/replayed counter -> reject too, and
                   in both cases do NOT advance the replay floor. */
                uint8_t expected_mac[S2S2_AUTH_MAC_LEN];
                s2s2_auth_compute_mac(s_reassembler.src_id, s_reassembler.dst_id,
                                       s_reassembler.payload, COMMAND_SIGNED_LEN, expected_mac);
                if (memcmp(expected_mac, cmd.mac, S2S2_AUTH_MAC_LEN) != 0) {
                    uart2_puts(" *** COMMAND RECEIVED: NO (bad MAC - forged or corrupted) ***\r\n");
                    if (want_ack) Send_Ack(ACK_STATUS_AUTH_FAILED, DATA_ID_GENERAL, cmd_seq);
                    app_state = STATE_IDLE_RX;
                    break;
                }
                if (!s2s2_auth_check_and_update_counter(cmd.counter)) {
                    uart2_printf(" *** COMMAND RECEIVED: NO (counter=%lu not newer than last "
                                 "accepted - replay) ***\r\n", (unsigned long)cmd.counter);
                    if (want_ack) Send_Ack(ACK_STATUS_AUTH_FAILED, DATA_ID_GENERAL, cmd_seq);
                    app_state = STATE_IDLE_RX;
                    break;
                }

                switch (cmd.opcode) {
                    case CMD_REQUEST_BURST:
                        if ((Get_Time_Ms() - last_burst_finish_time) < 4000) {
                            uart2_puts("CMD_REQUEST_BURST duplicate packet - ignoring\r\n");
                            if (want_ack) Send_Ack(ACK_STATUS_BUSY, DATA_ID_GENERAL, cmd_seq);
                            app_state = STATE_IDLE_RX;
                        } else {
                            uart2_puts("CMD_REQUEST_BURST ACCEPTED - STARTING BURST\r\n");
                            if (want_ack) Send_Ack(ACK_STATUS_ACCEPTED, DATA_ID_GENERAL, cmd_seq);
                            app_state = STATE_TRANSMIT_BURST;
                        }
                        break;

                    case CMD_HEARTBEAT:
                        uart2_puts("CMD_HEARTBEAT\r\n");
                        if (want_ack) Send_Ack(ACK_STATUS_COMPLETED, DATA_ID_GENERAL, cmd_seq);
                        app_state = STATE_IDLE_RX;
                        break;

                    case CMD_ABORT_MISSION:
                        s_digipeater_enabled = false;
                        uart2_puts("CMD_ABORT_MISSION\r\n");
                        if (want_ack) Send_Ack(ACK_STATUS_COMPLETED, DATA_ID_GENERAL, cmd_seq);
                        app_state = STATE_IDLE_RX;
                        break;

                    case CMD_GET_HEALTH:
                    case CMD_GET_HOUSEKEEPING:
                    case CMD_GET_BEACON1:
                    case CMD_GET_BEACON2:
                    case CMD_GET_ADCS:
                    case CMD_GET_SAT_LOG:
                        /* Subsystem sensor drivers (ADCS/EPDM/OBC file store) aren't
                           wired up in this firmware yet - reply honestly instead of
                           fabricating data. */
                        uart2_printf("opcode 0x%02X: requested data not available on this build\r\n", cmd.opcode);
                        if (want_ack) Send_Ack(ACK_STATUS_DATA_UNAVAILABLE, cmd.data_id, cmd_seq);
                        app_state = STATE_IDLE_RX;
                        break;

                    case CMD_RUN_CAMERA:
                    case CMD_DOWNLOAD_FILE:
                        /* Camera/file missions not implemented in this build. */
                        uart2_printf("opcode 0x%02X: mission not implemented on this build\r\n", cmd.opcode);
                        if (want_ack) Send_Ack(ACK_STATUS_MISSION_INACTIVE, cmd.data_id, cmd_seq);
                        app_state = STATE_IDLE_RX;
                        break;

                    case CMD_DIGIPEATER:
                        /* Real amateur digipeating (DPM_ARCHITECTURE.md) --
                           lowest-priority, opportunistic-only: the fork in
                           STATE_PROCESS_COMMAND only tries Try_Digipeat()
                           after S2S2's own command/telemetry/gap-report
                           parsing has already failed, so this never delays
                           or competes with scheduled S2S2 traffic. */
                        s_digipeater_enabled = true;
                        uart2_puts("CMD_DIGIPEATER: digipeater mission ACTIVE\r\n");
                        if (want_ack) Send_Ack(ACK_STATUS_ACCEPTED, cmd.data_id, cmd_seq);
                        app_state = STATE_IDLE_RX;
                        break;

                    case CMD_REQUEST_RETRANSMIT: {
                        /* Carries no specific seq list (that's what
                           TYPE_GAP_REPORT is for, handled above) - this is a
                           coarse "resend what you've still got" nudge, so
                           just replay everything currently in the tx history
                           ring buffer (tx_history.h). */
                        int available = 0;
                        for (int hi = 0; hi < TX_HISTORY_LEN; hi++) {
                            if (s_tx_history.entries[hi].used) available++;
                        }
                        if (want_ack) {
                            Send_Ack(available > 0 ? ACK_STATUS_ACCEPTED : ACK_STATUS_DATA_UNAVAILABLE,
                                     DATA_ID_GENERAL, cmd_seq);
                        }
                        int resent = Retransmit_All_History();
                        uart2_printf("CMD_REQUEST_RETRANSMIT: resent %d buffered message(s)\r\n", resent);
                        app_state = STATE_IDLE_RX;
                        break;
                    }

                    default:
                        uart2_printf("UNKNOWN OPCODE 0x%02X - IGNORING\r\n", cmd.opcode);
                        if (want_ack) Send_Ack(ACK_STATUS_INVALID_COMMAND, DATA_ID_GENERAL, cmd_seq);
                        app_state = STATE_IDLE_RX;
                        break;
                }
                cubus_ax25_reassembler_init(&s_reassembler);
                break;
            }

            case STATE_TRANSMIT_BURST:
                Transmit_Burst();
                last_burst_finish_time = Get_Time_Ms();
                rx_done_flag = 0;
                rx_timeout_flag = 0;
                rx_error_flag = 0;
                cubus_ax25_reassembler_init(&s_reassembler);
                last_beacon_tick_ms = Get_Time_Ms();
                RadioApp_StartRx();
                uart2_puts("LISTENING FOR NEXT COMMAND (and beaconing every 5s)...\r\n");
                app_state = STATE_IDLE_RX;
                break;
        }
        Send_Beacon_If_Due();
        Cycle_Tick();
        Process_UART_Command();
    }
    return 0;
  }

/*
 * s2s2com.h -- S2S2 Communications Subsystem, unified public header.
 *
 * Single include for the comms stack:
 *   1. Reed-Solomon RS(255,223) FEC
 *   2. Packet header, packet types/flags, gap tracking
 *   3. (see ax25.h / fx25.h / cubus_ax25.h) the on-air transport --
 *      S2S2's header+payload riding inside real AX.25 UI frames, each
 *      independently wrapped in FX.25 RS(255,223) FEC. Split out of this
 *      file because fx25.h already depends on the RS section below.
 *   4. STM32WL55 GMSK radio configuration
 *   5. S2S2 link budget parameters (frequency, power, adaptive baud/FEC)
 *
 * Implementation lives in rs.c, cubus_proto.c (header pack/unpack + gap
 * tracking only), ax25.c, fx25.c, cubus_ax25.c. Tested end-to-end: RS at
 * its 16-error/block limit (200/200 trials), AX.25 address/FCS/bit-
 * stuffing round trips, FX.25 encode/decode with injected errors up to
 * and past the 16-byte correction limit, and full message fragmentation/
 * reassembly across multiple independent FX.25 frames (including one
 * frame dropped past repair while the rest still deliver). TODOs below
 * still need real S2S2 numbers.
 *
 * Bare C: no malloc, no libc dependency beyond stdint.h / string.h.
 */
#ifndef S2S2COM_H
#define S2S2COM_H

#include <stdint.h>

/* =====================================================================
 * 1. Reed-Solomon RS(255,223)
 * ===================================================================== */

#define RS_K       223   /* message symbols */
#define RS_PARITY  32    /* parity symbols */
#define RS_N       255   /* codeword symbols (RS_K + RS_PARITY) */
#define RS_T       16    /* correction capability = RS_PARITY / 2 */

/* Must be called once before any encode/decode call (also called by
   cubus_proto_init() below, so you don't need to call it twice). */
void rs_init(void);

/* Systematic encode: msg[RS_K] -> codeword[RS_N] (msg followed by parity). */
void rs_encode(const uint8_t msg[RS_K], uint8_t codeword[RS_N]);

/*
 * Decode in place. Returns:
 *   >= 0 : number of symbol errors corrected, codeword[] now holds the
 *          corrected codeword (message is codeword[0..RS_K-1]).
 *   -1   : uncorrectable (more than RS_T errors) -- codeword[] undefined.
 */
int rs_decode(uint8_t codeword[RS_N]);

/* =====================================================================
 * 2. Packet header, packet types/flags, gap tracking
 *
 * The on-air transport (fragmenting a header+payload across AX.25/FX.25
 * frames) lives in ax25.h/fx25.h/cubus_ax25.h -- pulled apart from this
 * file so fx25.h (which needs RS_K/RS_N/rs_encode/rs_decode above) isn't
 * a circular include.
 * ===================================================================== */

#define MAX_PAYLOAD_LEN  659   /* one S2S2 message; fragmented across
                                   multiple AX.25/FX.25 frames -- see
                                   cubus_ax25.h */

/* Packet types */
#define TYPE_TELEMETRY    0x0
#define TYPE_TELECOMMAND  0x1
#define TYPE_ACK          0x2
#define TYPE_GAP_REPORT   0x3
#define TYPE_RAW          0x4 /* arbitrary host-supplied payload, no structure assumed --
                                  used by the "TX <hex>" UART command in both main.c files */

/* Flags */
#define FLAG_ACK_REQUESTED 0x01
#define FLAG_LAST_IN_BURST 0x02
#define FLAG_PRIORITY      0x04
#define FLAG_RETRANSMIT    0x08

typedef struct {
    uint8_t  version;
    uint8_t  type;
    uint8_t  src_id;
    uint8_t  dst_id;
    uint16_t seq_num;
    uint16_t payload_len;
    uint8_t  flags;
    uint16_t session_id;
    uint8_t  payload[MAX_PAYLOAD_LEN];
    int      rs_bytes_corrected; /* diagnostic: total symbol errors fixed across all fragments */
} cubus_packet_t;

/* Call once at startup -- initializes GF(256) tables and the RS
   generator polynomial. Covers rs_init() internally. */
void cubus_proto_init(void);

/* ---- Ground-station sequence-gap tracking (no dynamic allocation) ---- */

#define GAP_TRACKER_MAX_SEQ 65536

typedef struct {
    uint16_t session_id;
    int32_t  lowest_seen;                        /* -1 = none yet */
    int32_t  highest_seen;                       /* -1 = none yet */
    uint8_t  received[GAP_TRACKER_MAX_SEQ / 8];   /* bitset */
} gap_tracker_t;

void gap_tracker_init(gap_tracker_t *t, uint16_t session_id);
void gap_tracker_accept(gap_tracker_t *t, const cubus_packet_t *pkt);

/* Fills missing_out with missing sequence numbers in [lowest_seen,
   highest_seen] not yet received, up to max_out entries. Returns the
   number written. Deliberately NOT [0, highest_seen]: seq_num on both
   satellite and ground firmware is a single counter shared across every
   message type (beacons, ACKs, telecommands, telemetry bursts), so a
   burst practically never starts at 0 -- scanning from 0 would report
   every sequence number used by unrelated prior traffic as "missing". */
int gap_tracker_report(const gap_tracker_t *t, uint16_t *missing_out, int max_out);

/* =====================================================================
 * 4. STM32WL55 GFSK Radio Configuration (Nucleo-WL55JC1 TX side)
 *
 * Maps to STM32CubeWL's Radio.SetTxConfig() / Radio.SetRxConfig() calls
 * (see ST's SubGHz_Phy_PingPong or SubGHz_Phy_Per example projects).
 * Starting-point values for bench testing -- confirm against the real
 * S2S2 link budget (section 5) before flight.
 * ===================================================================== */

#define WL55_MODEM_TYPE        "GFSK"     /* MODEM_FSK in STM32CubeWL, Gaussian-filtered */
#define WL55_GAUSSIAN_BT       0.5        /* standard choice, tightens spectrum vs plain FSK */

#define WL55_BITRATE_BPS       9600UL     /* raise for bench throughput; confirm vs real link margin before flight */
#define WL55_FDEV_HZ           4800UL     /* Fdev ~= bitrate/2 -> modulation index ~1 */

/* RX bandwidth (Carson's rule, ST-confirmed formula):
   BW[DSB] > bitrate + 2*Fdev + frequency_error > 19200 Hz minimum */
#define WL55_RXBW_MIN_HZ       19200UL
#define WL55_RXBW_HZ           19500UL    /* TODO: confirm exact table entry against RM0453 for your CubeWL version */

#define WL55_PREAMBLE_LEN_BYTES        16 /* generous for reliable CDR lock at low SNR near-horizon */
#define WL55_PREAMBLE_MIN_DETECT_BITS  16

/* NOTE: the AX.25/FX.25 transport (ax25.h/fx25.h/cubus_ax25.h) does its
   own HDLC flag/bit-stuffing framing and its own FX.25 RS FEC. It rides
   over the SX126x's native GMSK packet engine same as any other packet
   here (radio_app.c), just configured for FIXED length with CRC and
   whitening OFF -- see radio_app.c's RadioApp_Send()/
   RadioApp_ForceRadioParams() for the actual PacketParams used. The
   values below (WL55_PACKET_TYPE/WL55_CRC_TYPE/WL55_WHITENING_ENABLED)
   describe an earlier variable-length/CRC-on/whitening-on design that's
   no longer what this firmware sends -- kept as historical reference for
   the sync word pattern (still used, purely for the chip's own RX FIFO
   triggering; it's transmitted ahead of the real AX.25/FX.25 content and
   a stock decoder just treats it as pre-frame noise before the first
   real HDLC flag, same as any other preamble). */

#define WL55_SYNCWORD_LEN_BYTES  4
#define WL55_SYNCWORD_BYTES      { 0x1A, 0xCF, 0xFC, 0x1D }

#define WL55_PACKET_TYPE          "Fixed length" /* radio_app.c: RADIO_PACKET_FIXED_LENGTH, PayloadLength = FX25_FRAME_LEN */
#define WL55_ADDRESS_FILTERING    0       /* off -- src/dst addressing is handled inside the logical protocol header */

#define WL55_CRC_TYPE             "off" /* radio_app.c: RADIO_CRC_OFF -- AX.25's FCS + FX.25's RS FEC (fx25.h)
                                            already cover integrity/correction */

#define WL55_WHITENING_ENABLED    0       /* radio_app.c: RADIO_DC_FREE_OFF -- HDLC bit-stuffing
                                              (fx25_pack_data_field) provides transition density instead */

#define WL55_TX_POWER_DBM         14      /* TODO: set from actual link budget; LP PA up to +15 dBm,
                                              HP PA up to +22 dBm depending on your board/antenna path */

/* Matching RTL-SDR / GNU Radio receive chain (ground station side).
   250 ksps-1 Msps is typical, comfortably oversampled vs the ~19.5 kHz
   occupied bandwidth above -- gives the GFSK demod / clock-recovery block
   plenty of margin. GNU Radio side also needs: matching bitrate (9600),
   matching Gaussian BT (0.5), the same 4-byte sync word for its Correlate
   Access Code / Sync block, and a symbol-timing recovery block (Polyphase
   Clock Sync or M&M) tuned to WL55_BITRATE_BPS. */
#define GS_RTLSDR_SAMPLE_RATE_HZ   250000UL   /* TODO: tune against your actual RTL-SDR + demod chain */

/* Frame time-on-air at a given baud rate for a frame of frame_len_bytes
   (e.g. FX25_FRAME_LEN from fx25.h for the AX.25/FX.25 transport). */
static inline double s2s2_frame_time_on_air_sec(uint32_t baud_rate, uint16_t frame_len_bytes) {
    return (frame_len_bytes * 8.0) / (double)baud_rate;
}

/* =====================================================================
 * 5. S2S2 Link Budget Parameters
 *
 * Everything marked TODO needs a real number from the actual S2S2 link
 * budget calculation -- these are placeholders only, typical values for
 * a 1U amateur UHF CubeSat digipeater (same class as S2S, S2S2's
 * predecessor), NOT numbers pulled from S2S2's actual design.
 * ===================================================================== */

#define S2S2_DOWNLINK_FREQ_HZ         436500000UL  /* TODO: confirm assigned UHF freq */
#define S2S2_UPLINK_FREQ_HZ           145900000UL  /* TODO: confirm assigned VHF freq (if split-band) */

#define S2S2_SAT_ANTENNA_GAIN_DBI     0            /* TODO: confirm (monopole/dipole ~0 dBi typical for 1U) */
#define S2S2_GROUND_ANTENNA_GAIN_DBI  12           /* TODO: confirm ground station antenna (yagi ~10-15 dBi typical) */
#define S2S2_GROUND_SYSTEM_NOISE_TEMP_K 500        /* TODO: confirm receiver noise figure -> system temp */

#define S2S2_ORBIT_ALTITUDE_KM        500          /* TODO: confirm from TLE once deployed, or design target */
#define S2S2_MIN_ELEVATION_DEG        10           /* TODO: confirm ground station elevation mask */
#define S2S2_TYPICAL_PASS_DURATION_S  480          /* TODO: derive from actual pass predictions for your station */

/* Adaptive baud rate / FEC strength by pass phase. Near AOS/LOS (horizon)
   has the least margin -- lowest baud, full FEC. Near TCA (zenith) has the
   most margin -- push baud up. TODO: tune against real Eb/N0-vs-elevation
   numbers once the link budget is done; values below are placeholders. */
typedef enum {
    S2S2_PHASE_HORIZON = 0,  /* low elevation, worst SNR + worst Doppler rate */
    S2S2_PHASE_MID,
    S2S2_PHASE_ZENITH,       /* high elevation, best SNR */
} s2s2_pass_phase_t;

typedef struct {
    uint32_t baud_rate;
    int      use_inner_convolutional_code;  /* 1 = add inner FEC layer, 0 = RS-only */
} s2s2_link_mode_t;

static const s2s2_link_mode_t S2S2_LINK_MODE_TABLE[3] = {
    [S2S2_PHASE_HORIZON] = { .baud_rate = 1200, .use_inner_convolutional_code = 1 },
    [S2S2_PHASE_MID]     = { .baud_rate = 4800, .use_inner_convolutional_code = 0 },
    [S2S2_PHASE_ZENITH]  = { .baud_rate = 9600, .use_inner_convolutional_code = 0 },
};

/* Rough max frames deliverable in one pass at a given phase's baud rate.
   Starting-point estimate only -- real usage should split pass duration
   across phases rather than assuming one rate for the whole pass. */
static inline long s2s2_max_frames_per_pass(uint32_t baud_rate, uint16_t frame_len_bytes, long pass_duration_s) {
    double t_frame = s2s2_frame_time_on_air_sec(baud_rate, frame_len_bytes);
    return (long)(pass_duration_s / t_frame);
}

#endif /* S2S2COM_H */
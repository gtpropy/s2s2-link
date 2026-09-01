/*
 * cubus_ax25.h -- S2S2's own header + payload, fragmented across real
 * AX.25/FX.25 frames.
 *
 * This replaces the old ASM + CRC32 + depth-3-interleaved-RS + 4-way
 * radio_chunk.c transport. That scheme spread one RS(255,223) block's
 * worth of protection across the whole 773-byte logical frame; losing
 * one native radio chunk took out ~84 interleaved bytes per RS block --
 * well past the RS_T=16 correction limit -- so a single dropped chunk
 * actually killed the entire frame regardless of the interleaving.
 *
 * Here, each fragment IS one independently RS(255,223)-shortened FX.25
 * frame (t=16, same strength as before) wrapped around a real AX.25 UI
 * frame. Losing one fragment costs only that fragment's slice of the
 * payload -- not the whole message -- and any FX.25-aware ground
 * station (SatNOGS, gr-satellites, Direwolf) can receive and forward
 * the raw bytes without any S2S2-specific software. The cost, as
 * intended: no more cross-fragment interleaving, AX.25's per-frame
 * address/control/pid/FCS overhead (18 bytes) plus a 12-byte S2S2
 * fragment header eaten out of each frame's ~80-byte Info-field budget,
 * and more, smaller fragments per message than the old 4-way chunking.
 *
 * Wire layout of one fragment's AX.25 Info field (all multi-byte fields
 * big-endian), 12 bytes header + up to CUBUS_AX25_FRAG_MAX_DATA payload:
 *
 *   [version][type][src_id][dst_id][seq_num(2)][session_id(2)][flags]
 *   [frag_index][frag_total][frag_len][ ...frag_len payload bytes... ]
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-30
*/
#ifndef CUBUS_AX25_H
#define CUBUS_AX25_H

#include <stdint.h>
#include "ax25.h"
#include "fx25.h"
#include "s2s2com.h" /* MAX_PAYLOAD_LEN, packet types/flags */

#ifdef __cplusplus
extern "C" {
#endif

#define CUBUS_AX25_FRAG_HDR_LEN  12
#define CUBUS_AX25_FRAG_MAX_DATA (AX25_MAX_INFO_LEN - CUBUS_AX25_FRAG_HDR_LEN) /* 68 */
#define CUBUS_AX25_MAX_FRAGMENTS ((MAX_PAYLOAD_LEN + CUBUS_AX25_FRAG_MAX_DATA - 1) / CUBUS_AX25_FRAG_MAX_DATA)

/* The AX.25 addresses used for one link direction. Configure once at
   boot from config.h. NEPSAT/GROUND are the same placeholder callsigns
   the original AX.25 design doc used for bench testing -- swap in a
   real assigned amateur-radio callsign before any actual RF transmit. */
typedef struct {
    ax25_addr_t dest;
    ax25_addr_t src;
} cubus_ax25_link_t;

/* Fragments one S2S2 message into up to CUBUS_AX25_MAX_FRAGMENTS
   complete, ready-to-transmit FX25_FRAME_LEN-byte FX.25 frames. Returns 0 on
   success (num_frames_out set), -1 if payload_len > MAX_PAYLOAD_LEN or
   any fragment failed to encode. */
int cubus_ax25_fragment(const cubus_ax25_link_t *link,
                         uint8_t version, uint8_t type, uint8_t src_id, uint8_t dst_id,
                         uint16_t seq_num, uint16_t session_id, uint8_t flags,
                         const uint8_t *payload, uint16_t payload_len,
                         uint8_t frames_out[CUBUS_AX25_MAX_FRAGMENTS][FX25_FRAME_LEN],
                         int *num_frames_out);

typedef struct {
    int      in_progress;
    uint16_t seq_num;
    uint16_t session_id;
    uint8_t  version;
    uint8_t  type;
    uint8_t  src_id;
    uint8_t  dst_id;
    uint8_t  flags;
    uint8_t  frag_total;
    uint8_t  last_frag_len;
    uint32_t received_mask;      /* bit i = fragment i received (frag_total <= 32) */
    uint8_t  payload[MAX_PAYLOAD_LEN];
    uint16_t payload_len;        /* valid only once complete */
    int      rs_bytes_corrected; /* summed across all fragments' FX.25 RS correction */
} cubus_ax25_reassembler_t;

void cubus_ax25_reassembler_init(cubus_ax25_reassembler_t *r);

/* Feed one received FX25_FRAME_LEN-byte FX.25 frame. Returns:
     1  -> message complete (r->payload/payload_len/header fields valid)
     0  -> fragment accepted, message still incomplete
    -1  -> frame rejected (bad FX.25 tag/RS, bad AX.25 FCS, or a
           malformed/inconsistent fragment header) */
int cubus_ax25_accept_frame(cubus_ax25_reassembler_t *r, uint8_t frame[FX25_FRAME_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* CUBUS_AX25_H */

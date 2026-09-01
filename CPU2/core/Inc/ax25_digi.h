/*
 * ax25_digi.h -- Variable-address-count AX.25 UI frame parse/rebuild, for
 * real digipeating (Section 4 of the S2S2 master architecture: "ax25.c
 * only builds 2-address (dest/src) frames today. Digipeating needs a
 * third address field (the repeater/digipeater field) plus the H-bit for
 * loop prevention. Not yet implemented" -- this file is that piece).
 *
 * Deliberately separate from ax25.h/ax25_build_ui_frame/ax25_parse_ui_frame,
 * which stay exactly as they are: S2S2's own command/telemetry link never
 * uses more than 2 addresses (dest+src, no repeaters), so nothing here
 * changes that path. This file exists only for frames carrying a real
 * AX.25 digipeater path (uplinked by a third-party ham station, not by
 * our own ground station).
 *
 * AX.25 spec allows up to 8 total address fields (dest + src + up to 6
 * digipeaters). Each digipeater address field's SSID byte bit 7 is the
 * "H-bit" (has-been-repeated) -- distinct from the C/R command bit dest/
 * src use that same bit position for. A digipeater looks for its own
 * callsign among the digi fields with H=0, sets that one H-bit to 1, and
 * retransmits everything else byte-identical -- see ax25_digi_rebuild.
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-31
 */
#ifndef AX25_DIGI_H
#define AX25_DIGI_H

#include <stdint.h>
#include <stdbool.h>
#include "ax25.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AX25_MAX_DIGIS       6                       /* AX.25 spec max: 8 total addr fields - dest - src */
#define AX25_MAX_ADDR_FIELDS (2 + AX25_MAX_DIGIS)

/* Sized like AX25_MAX_FRAME_LEN (ax25.h) but for up to 8 address fields
   instead of 2 -- still comfortably inside one FX.25 tag-0x06 data field
   (128 bytes) since a foreign digipeated frame carries no S2S2 fragment
   header, unlike our own traffic. */
#define AX25_DIGI_MAX_FRAME_LEN \
    (AX25_ADDR_LEN * AX25_MAX_ADDR_FIELDS + 1 + 1 + AX25_MAX_INFO_LEN + 2)

typedef struct {
    ax25_addr_t addr;
    bool        has_been_repeated;   /* H-bit */
} ax25_digi_addr_t;

typedef struct {
    ax25_addr_t      dest;
    int              dest_command_bit; /* preserved byte-identical, not reinterpreted */
    ax25_addr_t      src;
    int              src_command_bit;
    ax25_digi_addr_t digis[AX25_MAX_DIGIS];
    uint8_t          num_digis;
    uint8_t          control;
    uint8_t          pid;
    uint8_t          info[AX25_MAX_INFO_LEN];
    uint16_t         info_len;
} ax25_full_frame_t;

/* Parses a 2..8-address-field UI frame (dest+src+0..6 digis, control,
   pid, info, FCS -- no HDLC flags, same convention as
   ax25_parse_ui_frame). Rejects anything that isn't AX25_CTRL_UI /
   AX25_PID_NO_L3, or whose FCS doesn't check out, or that doesn't
   terminate its address chain within AX25_MAX_ADDR_FIELDS. Returns 0 on
   success, -1 otherwise. */
int ax25_digi_parse(const uint8_t *frame, uint16_t frame_len, ax25_full_frame_t *out);

/* Re-serializes f back into a transmittable frame (address fields with
   fresh extension/H-bits derived from f's own fields, control, pid,
   info, freshly computed FCS). Returns 0 on success (len_out set), -1 if
   it doesn't fit in max_out. */
int ax25_digi_rebuild(const ax25_full_frame_t *f, uint8_t *out, uint16_t max_out, uint16_t *len_out);

#ifdef __cplusplus
}
#endif

#endif /* AX25_DIGI_H */

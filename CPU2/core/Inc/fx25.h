/*
 * fx25.h -- FX.25 (AX.25 + Reed-Solomon FEC), per the Stensat Group /
 * K9DPD spec, as reverse-verified against Direwolf's own fx25_send.c
 * (github.com/wb2osz/direwolf, src/fx25_send.c). A real Direwolf,
 * UZ7HO Soundmodem, gr-satellites, or SatNOGS FX.25 receiver can decode
 * this without any S2S2-specific software -- that's the whole point:
 * a global, redundant, already-deployed ground-station network for free.
 *
 * S2S2 uses correlation tag 0x06 exclusively:
 *
 *   Tag 0x06 = 0xFF94DC634F1CFF4E, K=128 data bytes, 32 check bytes,
 *   RS(255,128), t=16.
 *
 * This is a *shortened* RS(255,223) code -- the standard, well-known
 * technique (also how CCSDS gets RS(255,223) down to shorter block
 * sizes): virtually zero-pad the 128 real data bytes up to RS_K (223)
 * bytes, RS(255,223)-encode that with our existing rs.c unchanged, then
 * transmit only the real 128 data bytes + the 32 parity bytes (never the
 * zero padding -- both ends already know it's there). Same generator
 * polynomial, same t=16 correction strength as tag 0x05 would give;
 * just a smaller block so the whole 8+128+32=168-byte frame fits inside
 * the SX126x's 255-byte native packet-engine buffer, unlike tag 0x05's
 * 263-byte frame. See fx25.c for the zero-pad/strip wrapper around
 * rs_encode()/rs_decode().
 *
 * Wire format (bytes, in transmission order):
 *
 *   [correlation tag, 8 bytes, little-endian of the 64-bit constant]
 *   [128-byte "data" field, built by fx25_pack_data_field below]
 *   [32 RS parity bytes]
 *
 * The "data" field itself is NOT an AX.25 frame with flags around it in
 * the way a stock TNC would send one -- it's HDLC bit-stuffed and starts
 * with one raw (unstuffed) leading flag byte and a stuffed closing flag,
 * then the remaining space is filled with literal flag bytes (a
 * deliberately simpler, still spec-correct choice than Direwolf's exact
 * bit-phase-continued fill -- see fx25.c for why that's safe).
 *
 * On the radio: this whole 168-byte frame is sent through the SX126x's
 * native packet engine configured for real AX.25 compatibility --
 * RADIO_PACKET_FIXED_LENGTH (no chip-inserted length byte -- AX.25/FX.25
 * frame boundaries come from HDLC flags only), CrcLength = RADIO_CRC_OFF
 * (no extra hardware CRC a stock decoder wouldn't expect), and
 * DcFree = RADIO_DC_FREE_OFF (whitening OFF -- HDLC bit-stuffing already
 * provides transition density, and a real ground station has no way to
 * de-whiten Semtech's proprietary whitening sequence). See radio_app.c.
 */
#ifndef FX25_H
#define FX25_H

#include <stdint.h>
#include "s2s2com.h" /* RS_K, RS_N, RS_PARITY, rs_encode/rs_decode */

#ifdef __cplusplus
extern "C" {
#endif

#define FX25_TAG_LEN     8
#define FX25_DATA_LEN    128        /* tag 0x06's K */
#define FX25_CHECK_LEN   RS_PARITY  /* 32 -- same mother code as tag 0x05 */
#define FX25_BLOCK_LEN   (FX25_DATA_LEN + FX25_CHECK_LEN) /* 160 */
#define FX25_FRAME_LEN   (FX25_TAG_LEN + FX25_BLOCK_LEN)  /* 168 -- fits the SX126x's 255B buffer */

/* How many virtual zero bytes precede FX25_DATA_LEN's real bytes in the
   RS(255,223) mother code (shortened-RS technique -- see fx25.c). */
#define FX25_ZERO_PAD (RS_K - FX25_DATA_LEN) /* 95 */

/* The only correlation tag S2S2 transmits (Direwolf/Stensat tag 0x06). */
#define FX25_TAG06_VALUE 0xFF94DC634F1CFF4EULL

/* Emits the 8-byte correlation tag in wire (little-endian) order. */
void fx25_tag_encode(uint8_t out[FX25_TAG_LEN]);

/* Returns 1 if the 8 bytes match the S2S2 tag, 0 otherwise -- use this to
   resync on a received bitstream / reject frames using a tag we don't
   support. */
int fx25_tag_matches(const uint8_t in[FX25_TAG_LEN]);

/* HDLC bit-stuffs `ax25_frame` (as built by ax25_build_ui_frame, i.e.
   already including its FCS, NOT including flags) into a FX25_DATA_LEN
   (128) byte field: one raw leading flag byte, the stuffed frame bits,
   one stuffed closing flag, and literal 0x7E fill for the remainder.
   Returns 0 on success, -1 if the stuffed frame doesn't fit in
   FX25_DATA_LEN bytes (caller should use a smaller AX.25 Info field and
   retry). */
int fx25_pack_data_field(const uint8_t *ax25_frame, uint16_t frame_len,
                          uint8_t out[FX25_DATA_LEN]);

/* Inverse: destuffs a (already RS-corrected) FX25_DATA_LEN-byte data
   field back into AX.25 frame bytes (including FCS). Returns the frame
   length, or -1 if no valid opening+closing flag pair could be found. */
int fx25_unpack_data_field(const uint8_t data[FX25_DATA_LEN],
                            uint8_t *ax25_frame_out, uint16_t max_len);

/* Builds one complete FX25_FRAME_LEN-byte FX.25 frame (tag + RS-encoded
   data+check) from an unstuffed AX.25 frame (with FCS, no flags).
   Returns 0 on success, -1 if it doesn't fit in the data field. */
int fx25_encode_frame(const uint8_t *ax25_frame, uint16_t frame_len,
                       uint8_t out[FX25_FRAME_LEN]);

/* Decodes one received FX25_FRAME_LEN-byte FX.25 frame (tag + codeword).
   Returns the AX.25 frame length in ax25_frame_out (>= 0, with
   rs_bytes_corrected set to the number of symbols the RS step fixed), or
   -1 if the tag doesn't match, RS correction fails (more than 16 byte
   errors), or no valid flag pair is found after destuffing. */
int fx25_decode_frame(uint8_t in_frame[FX25_FRAME_LEN],
                       uint8_t *ax25_frame_out, uint16_t max_len,
                       int *rs_bytes_corrected);

#ifdef __cplusplus
}
#endif

#endif /* FX25_H */

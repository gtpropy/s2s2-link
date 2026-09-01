/*
 * ax25.h -- Minimal AX.25 UI-frame framing (address fields, FCS, frame
 * build/parse). No flags, no bit-stuffing here -- that's fx25.h's job,
 * since FX.25 needs the bit-stuffed *unstuffed* frame bytes as input to
 * its own leading/trailing-flag + stuffing pass.
 *
 * This is what makes S2S2 receivable by any AX.25/FX.25-aware ground
 * station (SatNOGS, gr-satellites, Direwolf) without custom software --
 * see fx25.h for the tradeoff this buys and what it costs.
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-21
*/
#ifndef AX25_H
#define AX25_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AX25_ADDR_LEN     7   /* one address field: 6 callsign chars + SSID byte */
#define AX25_CALLSIGN_MAX 6

#define AX25_CTRL_UI      0x03  /* Unnumbered Information */
#define AX25_PID_NO_L3    0xF0  /* no layer-3 protocol */

typedef struct {
    char    callsign[AX25_CALLSIGN_MAX + 1]; /* up to 6 chars + NUL, upper-case */
    uint8_t ssid;                             /* 0-15 */
} ax25_addr_t;

/* Packs one 7-byte AX.25 address field. is_last=1 marks the final address
   in the chain (source, since S2S2 uses no repeaters); command_bit sets
   bit 7 (conventionally 1 on the destination of a command, 0 on the
   source -- for our own point-to-point UI traffic either convention
   works as long as both ends agree, so callers just pick one). */
void ax25_addr_encode(const ax25_addr_t *addr, int is_last, int command_bit,
                       uint8_t out[AX25_ADDR_LEN]);

/* Decodes one address field. Trailing spaces in the callsign are trimmed. */
void ax25_addr_decode(const uint8_t in[AX25_ADDR_LEN], ax25_addr_t *addr,
                       int *is_last, int *command_bit);

/* Standard AX.25 FCS: CRC-16/X-25 (poly 0x8408 reflected, init 0xFFFF,
   xorout 0xFFFF). Check value for ASCII "123456789" is 0x906E. */
uint16_t ax25_fcs(const uint8_t *data, uint16_t len);

/* Max UI frame size this module will build/parse: 2 addresses + control +
   pid + info + fcs. Capped at 80 (not AX.25's full 256-byte Info field
   allowance) so the frame -- even in the pathological worst case where
   every bit needs HDLC stuffing -- always fits fx25.h's 128-byte FX.25
   data field (tag 0x06): 18 bytes of address/control/pid/fcs overhead +
   80 info = 98 bytes unstuffed = 784 bits; worst-case stuffing (all-1s
   content) inflates that to ceil(784*6/5) = 941 bits, plus 16 bits for
   the leading+closing flags = 957 bits <= 128*8 = 1024 bits available. */
#define AX25_MAX_INFO_LEN 80
#define AX25_MAX_FRAME_LEN (AX25_ADDR_LEN * 2 + 1 + 1 + AX25_MAX_INFO_LEN + 2)

/* Builds dest_addr(7) + src_addr(7) + control(0x03) + pid(0xF0) + info +
   fcs(2, low byte first) -- NOT including HDLC flags or bit-stuffing.
   Returns the frame length, or -1 if info_len > AX25_MAX_INFO_LEN. */
int ax25_build_ui_frame(const ax25_addr_t *dest, const ax25_addr_t *src,
                         const uint8_t *info, uint16_t info_len,
                         uint8_t out[AX25_MAX_FRAME_LEN]);

/* Parses and validates a frame built by ax25_build_ui_frame (checks
   control == UI, pid == no-L3, and the FCS). Returns 0 on success (dest/
   src/info_out populated), -1 on any validation failure. */
int ax25_parse_ui_frame(const uint8_t *frame, uint16_t frame_len,
                         ax25_addr_t *dest, ax25_addr_t *src,
                         uint8_t *info_out, uint16_t *info_len_out, uint16_t max_info_len);

#ifdef __cplusplus
}
#endif

#endif /* AX25_H */

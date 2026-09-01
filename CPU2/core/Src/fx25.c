/*
 * fx25.c
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-30
 */

#include "fx25.h"
#include <string.h>

void fx25_tag_encode(uint8_t out[FX25_TAG_LEN]) {
    uint64_t v = FX25_TAG06_VALUE;
    for (int i = 0; i < FX25_TAG_LEN; i++) {
        out[i] = (uint8_t)(v >> (i * 8));
    }
}

int fx25_tag_matches(const uint8_t in[FX25_TAG_LEN]) {
    uint8_t expect[FX25_TAG_LEN];
    fx25_tag_encode(expect);
    return memcmp(in, expect, FX25_TAG_LEN) == 0;
}

static void put_bit(uint8_t *out, int *olen, int cap_bits, int value, int *overflow) {
    if (*olen >= cap_bits) { *overflow = 1; return; }
    if (value) out[*olen >> 3] |= (uint8_t)(1 << (*olen & 7));
    (*olen)++;
}

int fx25_pack_data_field(const uint8_t *ax25_frame, uint16_t frame_len,
                          uint8_t out[FX25_DATA_LEN]) {
    memset(out, 0, FX25_DATA_LEN);
    const int cap_bits = FX25_DATA_LEN * 8;
    int olen = 0;
    int overflow = 0;

    /* Leading flag: one raw, unstuffed byte. */
    out[0] = 0x7E;
    olen = 8;

    int ones = 0;
    for (uint16_t i = 0; i < frame_len; i++) {
        for (int b = 0; b < 8; b++) {
            int v = (ax25_frame[i] >> b) & 1;
            put_bit(out, &olen, cap_bits, v, &overflow);
            if (overflow) return -1;
            if (v) {
                ones++;
                if (ones == 5) {
                    put_bit(out, &olen, cap_bits, 0, &overflow); /* stuffed 0 */
                    if (overflow) return -1;
                    ones = 0;
                }
            } else {
                ones = 0;
            }
        }
    }

    /* Closing flag: literal bits, not run through the stuffing counter. */
    for (int b = 0; b < 8; b++) {
        put_bit(out, &olen, cap_bits, (0x7E >> b) & 1, &overflow);
        if (overflow) return -1;
    }

    /* Fill the rest with literal (byte-aligned) flag bytes. Any leftover
       bits in the still-partial last byte are already 0 from the memset
       above; a few stray zero bits ahead of the next real flag byte are
       harmless idle filler to any HDLC bit destuffer -- see fx25.h. */
    int used_bytes = (olen + 7) / 8;
    for (int i = used_bytes; i < FX25_DATA_LEN; i++) out[i] = 0x7E;

    return 0;
}

static int get_bit(const uint8_t *buf, int bitpos) {
    return (buf[bitpos >> 3] >> (bitpos & 7)) & 1;
}

int fx25_unpack_data_field(const uint8_t data[FX25_DATA_LEN],
                            uint8_t *ax25_frame_out, uint16_t max_len) {
    if (data[0] != 0x7E) return -1;

    /* Emit into a full-capacity scratch buffer, not directly into the
       caller's (possibly exactly-max_len-sized) buffer: the flag-
       detection retraction below always overshoots by up to 6 bits
       before correcting itself (see the comment at the retraction), and
       destuffed content can never exceed the stuffed input's own
       capacity, so FX25_DATA_LEN bytes of scratch is always enough. */
    uint8_t scratch[FX25_DATA_LEN];
    memset(scratch, 0, sizeof(scratch));

    const int total_bits = FX25_DATA_LEN * 8;
    int i = 8; /* skip the raw leading flag byte */
    int ones = 0;
    int out_bits = 0;

    while (i < total_bits) {
        int bit = get_bit(data, i++);

        if (ones == 5) {
            ones = 0;
            if (bit == 0) {
                continue; /* stuffed bit, discard */
            }
            /* bit == 1: sixth consecutive one -> must be a flag (real
               stuffed data can never produce six raw ones in a row).
               The closing flag's own literal bits (0,1,1,1,1,1,1,0) were
               never run through the stuffing counter on encode, so this
               decoder's ones-tracking spuriously "emitted" all 6 bits
               leading up to and including this one (the flag's own
               leading 0 always resets `ones` to 0 first, then its five
               more 1-bits climb back up to 5 before this 6th one is
               caught here) -- retract them now that the flag is
               confirmed. */
            if (i >= total_bits) return -1;
            int bit2 = get_bit(data, i++);
            if (bit2 == 0) {
                out_bits -= 6; /* undo the flag's own leading 0 + first five 1s */
                if (out_bits < 0) return -1;
                int frame_len = (out_bits + 7) / 8;
                if (frame_len > max_len) return -1;
                memset(ax25_frame_out, 0, max_len);
                memcpy(ax25_frame_out, scratch, (size_t)frame_len);
                return frame_len; /* closing flag found */
            }
            return -1; /* abort condition (7+ ones) */
        }

        if (bit) scratch[out_bits >> 3] |= (uint8_t)(1 << (out_bits & 7));
        out_bits++;

        if (bit) ones++;
        else ones = 0;
    }
    return -1; /* ran off the end without finding a closing flag */
}

/* Tag 0x06 is a shortened RS(255,223): virtually zero-pad the
   FX25_DATA_LEN (128) real data bytes with FX25_ZERO_PAD (95) leading
   zero bytes to make a full RS_K (223) byte message, RS(255,223)-encode
   that with the unmodified mother-code rs.c, then keep only the last
   FX25_DATA_LEN bytes of the systematic message part (== the real data,
   since the zero padding contributes nothing) plus the parity -- never
   transmit the zero padding itself, both ends already know it's there. */
int fx25_encode_frame(const uint8_t *ax25_frame, uint16_t frame_len,
                       uint8_t out[FX25_FRAME_LEN]) {
    uint8_t data[FX25_DATA_LEN];
    if (fx25_pack_data_field(ax25_frame, frame_len, data) != 0) return -1;

    uint8_t rs_msg[RS_K];
    memset(rs_msg, 0, FX25_ZERO_PAD);
    memcpy(&rs_msg[FX25_ZERO_PAD], data, FX25_DATA_LEN);

    uint8_t codeword[RS_N];
    rs_encode(rs_msg, codeword);

    fx25_tag_encode(out);
    memcpy(&out[FX25_TAG_LEN], data, FX25_DATA_LEN);                     /* real data bytes */
    memcpy(&out[FX25_TAG_LEN + FX25_DATA_LEN], &codeword[RS_K], FX25_CHECK_LEN); /* parity */
    return 0;
}

int fx25_decode_frame(uint8_t in_frame[FX25_FRAME_LEN],
                       uint8_t *ax25_frame_out, uint16_t max_len,
                       int *rs_bytes_corrected) {
    if (!fx25_tag_matches(in_frame)) return -1;

    /* Reconstruct the full RS(255,223) codeword: zero-pad + real data +
       parity, then run the unmodified mother-code decoder on it. */
    uint8_t codeword[RS_N];
    memset(codeword, 0, FX25_ZERO_PAD);
    memcpy(&codeword[FX25_ZERO_PAD], &in_frame[FX25_TAG_LEN], FX25_DATA_LEN);
    memcpy(&codeword[RS_K], &in_frame[FX25_TAG_LEN + FX25_DATA_LEN], FX25_CHECK_LEN);

    int corrected = rs_decode(codeword);
    if (corrected < 0) return -1;
    if (rs_bytes_corrected) *rs_bytes_corrected = corrected;

    int frame_len = fx25_unpack_data_field(&codeword[FX25_ZERO_PAD], ax25_frame_out, max_len);
    return frame_len;
}

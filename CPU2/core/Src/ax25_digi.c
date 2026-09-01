/*
 * ax25_digi.c -- see ax25_digi.h.
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-31
 */
#include "ax25_digi.h"
#include <string.h>

int ax25_digi_parse(const uint8_t *frame, uint16_t frame_len, ax25_full_frame_t *out) {
    ax25_addr_t addrs[AX25_MAX_ADDR_FIELDS];
    int is_last_flags[AX25_MAX_ADDR_FIELDS];
    int bit7_flags[AX25_MAX_ADDR_FIELDS];
    uint8_t addr_count = 0;
    uint16_t pos = 0;

    while (addr_count < AX25_MAX_ADDR_FIELDS) {
        if ((uint16_t)(pos + AX25_ADDR_LEN) > frame_len) return -1;
        int is_last = 0, bit7 = 0;
        ax25_addr_decode(&frame[pos], &addrs[addr_count], &is_last, &bit7);
        is_last_flags[addr_count] = is_last;
        bit7_flags[addr_count] = bit7;
        pos = (uint16_t)(pos + AX25_ADDR_LEN);
        addr_count++;
        if (is_last) break;
    }
    /* Need at least dest+src, and the chain must have actually
       terminated (not just run out of AX25_MAX_ADDR_FIELDS slots). */
    if (addr_count < 2 || !is_last_flags[addr_count - 1]) return -1;

    if ((uint16_t)(pos + 2) > frame_len) return -1;
    uint8_t control = frame[pos++];
    uint8_t pid = frame[pos++];
    if (control != AX25_CTRL_UI || pid != AX25_PID_NO_L3) return -1;

    if (frame_len < (uint16_t)(pos + 2)) return -1;
    uint16_t info_len = (uint16_t)(frame_len - pos - 2);
    if (info_len > AX25_MAX_INFO_LEN) return -1;

    uint16_t computed_fcs = ax25_fcs(frame, (uint16_t)(pos + info_len));
    uint16_t received_fcs = (uint16_t)(frame[pos + info_len] | (frame[pos + info_len + 1] << 8));
    if (computed_fcs != received_fcs) return -1;

    out->dest = addrs[0];
    out->dest_command_bit = bit7_flags[0];
    out->src = addrs[1];
    out->src_command_bit = bit7_flags[1];
    out->num_digis = (uint8_t)(addr_count - 2);
    for (uint8_t i = 0; i < out->num_digis; i++) {
        out->digis[i].addr = addrs[2 + i];
        out->digis[i].has_been_repeated = bit7_flags[2 + i] ? true : false;
    }
    out->control = control;
    out->pid = pid;
    out->info_len = info_len;
    if (info_len > 0) memcpy(out->info, &frame[pos], info_len);

    return 0;
}

int ax25_digi_rebuild(const ax25_full_frame_t *f, uint8_t *out, uint16_t max_out, uint16_t *len_out) {
    if (f->num_digis > AX25_MAX_DIGIS) return -1;

    uint16_t need = (uint16_t)(AX25_ADDR_LEN * (2 + f->num_digis) + 1 + 1 + f->info_len + 2);
    if (need > max_out) return -1;

    uint16_t pos = 0;
    /* Dest is never the chain terminator (src or a digi always follows). */
    ax25_addr_encode(&f->dest, 0, f->dest_command_bit, &out[pos]);
    pos = (uint16_t)(pos + AX25_ADDR_LEN);

    int src_is_last = (f->num_digis == 0) ? 1 : 0;
    ax25_addr_encode(&f->src, src_is_last, f->src_command_bit, &out[pos]);
    pos = (uint16_t)(pos + AX25_ADDR_LEN);

    for (uint8_t i = 0; i < f->num_digis; i++) {
        int is_last = (i == (uint8_t)(f->num_digis - 1)) ? 1 : 0;
        ax25_addr_encode(&f->digis[i].addr, is_last, f->digis[i].has_been_repeated ? 1 : 0, &out[pos]);
        pos = (uint16_t)(pos + AX25_ADDR_LEN);
    }

    out[pos++] = f->control;
    out[pos++] = f->pid;
    if (f->info_len > 0) memcpy(&out[pos], f->info, f->info_len);
    pos = (uint16_t)(pos + f->info_len);

    uint16_t fcs = ax25_fcs(out, pos);
    out[pos++] = (uint8_t)(fcs & 0xFF);
    out[pos++] = (uint8_t)((fcs >> 8) & 0xFF);

    *len_out = pos;
    return 0;
}

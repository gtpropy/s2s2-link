/*
 * cubus_ax25.c
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-30
 */

#include "cubus_ax25.h"
#include <string.h>

static void pack_frag_header(uint8_t out[CUBUS_AX25_FRAG_HDR_LEN],
                              uint8_t version, uint8_t type, uint8_t src_id, uint8_t dst_id,
                              uint16_t seq_num, uint16_t session_id, uint8_t flags,
                              uint8_t frag_index, uint8_t frag_total, uint8_t frag_len)
{
    out[0] = version;
    out[1] = type;
    out[2] = src_id;
    out[3] = dst_id;
    out[4] = (uint8_t)(seq_num >> 8);
    out[5] = (uint8_t)(seq_num & 0xFF);
    out[6] = (uint8_t)(session_id >> 8);
    out[7] = (uint8_t)(session_id & 0xFF);
    out[8] = flags;
    out[9] = frag_index;
    out[10] = frag_total;
    out[11] = frag_len;
}

int cubus_ax25_fragment(const cubus_ax25_link_t *link,
                         uint8_t version, uint8_t type, uint8_t src_id, uint8_t dst_id,
                         uint16_t seq_num, uint16_t session_id, uint8_t flags,
                         const uint8_t *payload, uint16_t payload_len,
                         uint8_t frames_out[CUBUS_AX25_MAX_FRAGMENTS][FX25_FRAME_LEN],
                         int *num_frames_out)
{
    if (payload_len > MAX_PAYLOAD_LEN) return -1;

    int frag_total = (payload_len + CUBUS_AX25_FRAG_MAX_DATA - 1) / CUBUS_AX25_FRAG_MAX_DATA;
    if (frag_total == 0) frag_total = 1; /* zero-length payload still gets one fragment */
    if (frag_total > CUBUS_AX25_MAX_FRAGMENTS) return -1;

    for (int i = 0; i < frag_total; i++) {
        uint16_t offset = (uint16_t)(i * CUBUS_AX25_FRAG_MAX_DATA);
        uint16_t remaining = (uint16_t)(payload_len - offset);
        uint8_t frag_len = (uint8_t)((remaining < CUBUS_AX25_FRAG_MAX_DATA) ? remaining : CUBUS_AX25_FRAG_MAX_DATA);

        uint8_t info[CUBUS_AX25_FRAG_HDR_LEN + CUBUS_AX25_FRAG_MAX_DATA];
        pack_frag_header(info, version, type, src_id, dst_id, seq_num, session_id, flags,
                          (uint8_t)i, (uint8_t)frag_total, frag_len);
        if (frag_len > 0) memcpy(&info[CUBUS_AX25_FRAG_HDR_LEN], &payload[offset], frag_len);

        uint8_t ax25_frame[AX25_MAX_FRAME_LEN];
        int frame_len = ax25_build_ui_frame(&link->dest, &link->src, info,
                                             (uint16_t)(CUBUS_AX25_FRAG_HDR_LEN + frag_len), ax25_frame);
        if (frame_len < 0) return -1;

        if (fx25_encode_frame(ax25_frame, (uint16_t)frame_len, frames_out[i]) != 0) return -1;
    }

    *num_frames_out = frag_total;
    return 0;
}

void cubus_ax25_reassembler_init(cubus_ax25_reassembler_t *r)
{
    memset(r, 0, sizeof(*r));
    r->in_progress = 0;
}

int cubus_ax25_accept_frame(cubus_ax25_reassembler_t *r, uint8_t frame[FX25_FRAME_LEN])
{
    uint8_t ax25_frame[AX25_MAX_FRAME_LEN];
    int rs_corrected = 0;
    int frame_len = fx25_decode_frame(frame, ax25_frame, AX25_MAX_FRAME_LEN, &rs_corrected);
    if (frame_len < 0) return -1;

    ax25_addr_t dest, src;
    uint8_t info[AX25_MAX_INFO_LEN];
    uint16_t info_len = 0;
    if (ax25_parse_ui_frame(ax25_frame, (uint16_t)frame_len, &dest, &src,
                             info, &info_len, AX25_MAX_INFO_LEN) != 0) {
        return -1;
    }
    if (info_len < CUBUS_AX25_FRAG_HDR_LEN) return -1;

    uint8_t version = info[0];
    uint8_t type = info[1];
    uint8_t src_id = info[2];
    uint8_t dst_id = info[3];
    uint16_t seq_num = (uint16_t)((info[4] << 8) | info[5]);
    uint16_t session_id = (uint16_t)((info[6] << 8) | info[7]);
    uint8_t flags = info[8];
    uint8_t frag_index = info[9];
    uint8_t frag_total = info[10];
    uint8_t frag_len = info[11];

    if (frag_total == 0 || frag_total > CUBUS_AX25_MAX_FRAGMENTS) return -1;
    if (frag_index >= frag_total) return -1;
    if (info_len != (uint16_t)(CUBUS_AX25_FRAG_HDR_LEN + frag_len)) return -1;

    int is_new = !r->in_progress || r->seq_num != seq_num || r->session_id != session_id;
    if (is_new) {
        cubus_ax25_reassembler_init(r);
        r->in_progress = 1;
        r->version = version;
        r->type = type;
        r->src_id = src_id;
        r->dst_id = dst_id;
        r->seq_num = seq_num;
        r->session_id = session_id;
        r->flags = flags;
        r->frag_total = frag_total;
    } else if (r->frag_total != frag_total) {
        return -1; /* inconsistent with the in-progress message */
    }

    uint16_t offset = (uint16_t)(frag_index * CUBUS_AX25_FRAG_MAX_DATA);
    if ((uint32_t)offset + frag_len > MAX_PAYLOAD_LEN) return -1;
    memcpy(&r->payload[offset], &info[CUBUS_AX25_FRAG_HDR_LEN], frag_len);
    r->received_mask |= (1UL << frag_index);
    r->rs_bytes_corrected += rs_corrected;
    if (frag_index == (uint8_t)(frag_total - 1)) r->last_frag_len = frag_len;

    uint32_t complete_mask = (frag_total >= 32) ? 0xFFFFFFFFUL : ((1UL << frag_total) - 1UL);
    if ((r->received_mask & complete_mask) != complete_mask) return 0;

    r->payload_len = (uint16_t)((frag_total - 1) * CUBUS_AX25_FRAG_MAX_DATA + r->last_frag_len);
    return 1;
}

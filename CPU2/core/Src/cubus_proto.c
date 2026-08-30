/*
 * cubus_proto.c -- stack init + ground-station sequence-gap tracking.
 *
 * The header/CRC32/framing/RS-interleave that used to live here has been
 * superseded by the AX.25/FX.25 transport (ax25.c/fx25.c/cubus_ax25.c) --
 * see cubus_ax25.h's header comment for why. What's left is generic:
 * one-time stack init and gap tracking, both independent of which
 * transport carried the packet.
 */
#include <string.h>
#include "s2s2com.h"

void cubus_proto_init(void)
{
    rs_init();
}

/* ---- Ground-station sequence-gap tracking ---- */

void gap_tracker_init(gap_tracker_t *t, uint16_t session_id)
{
    t->session_id = session_id;
    t->lowest_seen = -1;
    t->highest_seen = -1;
    memset(t->received, 0, sizeof(t->received));
}

void gap_tracker_accept(gap_tracker_t *t, const cubus_packet_t *pkt)
{
    if (pkt->session_id != t->session_id) return;
    uint16_t seq = pkt->seq_num;
    t->received[seq / 8] |= (uint8_t)(1U << (seq % 8));
    if ((int32_t)seq > t->highest_seen) t->highest_seen = (int32_t)seq;
    if (t->lowest_seen < 0 || (int32_t)seq < t->lowest_seen) t->lowest_seen = (int32_t)seq;
}

int gap_tracker_report(const gap_tracker_t *t, uint16_t *missing_out, int max_out)
{
    int n = 0;
    if (t->highest_seen < 0) return 0;
    for (int32_t seq = t->lowest_seen; seq <= t->highest_seen && n < max_out; seq++) {
        if (!(t->received[seq / 8] & (1U << (seq % 8)))) {
            missing_out[n++] = (uint16_t)seq;
        }
    }
    return n;
}

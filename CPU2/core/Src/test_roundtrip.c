/*
 * Host-buildable (no HAL dependency) correctness test for the S2S2 stack:
 * RS(255,223) at its error limit, AX.25 address/FCS/bit-stuffing, FX.25
 * encode/decode under injected errors, gap tracking, full message
 * fragmentation/reassembly across independent AX.25/FX.25 frames, and the
 * satellite-side tx_history retransmit ring buffer.
 *
 * Build and run:
 *   clang -std=c11 -Wall -Wextra -O2 -I ../Inc -o /tmp/test_s2s2 \
 *         rs.c ax25.c fx25.c cubus_ax25.c cubus_proto.c tx_history.c test_roundtrip.c
 *   /tmp/test_s2s2
 */
#include "s2s2com.h"
#include "ax25.h"
#include "fx25.h"
#include "cubus_ax25.h"
#include "tx_history.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static unsigned rng_state = 0xC0FFEEu;
static unsigned rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state; }

/* ---------------------------------------------------------------
 * RS(255,223) at its correction limit
 * --------------------------------------------------------------- */
static void test_rs_limit(void) {
    int exact = 0, rejected = 0;
    for (int trial = 0; trial < 200; trial++) {
        uint8_t msg[RS_K], codeword[RS_N], original[RS_N];
        for (int i = 0; i < RS_K; i++) msg[i] = (uint8_t)rnd();
        rs_encode(msg, codeword);
        memcpy(original, codeword, RS_N);

        int positions[RS_N];
        for (int i = 0; i < RS_N; i++) positions[i] = i;
        for (int i = RS_N - 1; i > 0; i--) {
            int j = rnd() % (i + 1);
            int t = positions[i]; positions[i] = positions[j]; positions[j] = t;
        }
        for (int e = 0; e < RS_T; e++) {
            uint8_t bad; do { bad = (uint8_t)rnd(); } while (bad == original[positions[e]]);
            codeword[positions[e]] = bad;
        }
        int corrected = rs_decode(codeword);
        if (corrected == RS_T && memcmp(codeword, original, RS_N) == 0) exact++;
    }
    printf("RS(255,223) exactly at 16-error/block limit: %d/200 exact recovery\n", exact);
    CHECK(exact == 200);

    for (int trial = 0; trial < 200; trial++) {
        uint8_t msg[RS_K], codeword[RS_N], original[RS_N];
        for (int i = 0; i < RS_K; i++) msg[i] = (uint8_t)rnd();
        rs_encode(msg, codeword);
        memcpy(original, codeword, RS_N);

        int positions[RS_N];
        for (int i = 0; i < RS_N; i++) positions[i] = i;
        for (int i = RS_N - 1; i > 0; i--) {
            int j = rnd() % (i + 1);
            int t = positions[i]; positions[i] = positions[j]; positions[j] = t;
        }
        for (int e = 0; e < RS_T + 1; e++) {
            uint8_t bad; do { bad = (uint8_t)rnd(); } while (bad == original[positions[e]]);
            codeword[positions[e]] = bad;
        }
        int corrected = rs_decode(codeword);
        if (corrected < 0 || memcmp(codeword, original, RS_N) != 0) rejected++;
    }
    printf("RS(255,223) beyond limit (17 errors): %d/200 correctly rejected/not-silently-wrong\n", rejected);
    CHECK(rejected == 200);
}

/* ---------------------------------------------------------------
 * AX.25 address encode/decode, FCS, UI frame build/parse
 * --------------------------------------------------------------- */
static void test_ax25(void) {
    ax25_addr_t addr = { .callsign = "NEPSAT", .ssid = 3 };
    uint8_t enc[AX25_ADDR_LEN];
    ax25_addr_encode(&addr, 1, 1, enc);
    ax25_addr_t dec; int is_last = 0, cmd = 0;
    ax25_addr_decode(enc, &dec, &is_last, &cmd);
    CHECK(strcmp(dec.callsign, "NEPSAT") == 0);
    CHECK(dec.ssid == 3);
    CHECK(is_last == 1);
    CHECK(cmd == 1);

    ax25_addr_t short_addr = { .callsign = "GND", .ssid = 0 };
    uint8_t enc2[AX25_ADDR_LEN];
    ax25_addr_encode(&short_addr, 0, 0, enc2);
    ax25_addr_t dec2; int is_last2 = 1;
    ax25_addr_decode(enc2, &dec2, &is_last2, NULL);
    CHECK(strcmp(dec2.callsign, "GND") == 0); /* trailing pad spaces trimmed */
    CHECK(is_last2 == 0);

    /* Known CRC-16/X-25 check value. */
    CHECK(ax25_fcs((const uint8_t *)"123456789", 9) == 0x906E);

    ax25_addr_t dest = { .callsign = "NEPSAT", .ssid = 0 };
    ax25_addr_t src  = { .callsign = "GROUND", .ssid = 1 };
    const uint8_t info[] = "Namaste Everyone!";
    uint8_t frame[AX25_MAX_FRAME_LEN];
    int flen = ax25_build_ui_frame(&dest, &src, info, (uint16_t)(sizeof(info) - 1), frame);
    CHECK(flen > 0);

    ax25_addr_t pdest, psrc;
    uint8_t pinfo[AX25_MAX_INFO_LEN];
    uint16_t pinfo_len = 0;
    CHECK(ax25_parse_ui_frame(frame, (uint16_t)flen, &pdest, &psrc, pinfo, &pinfo_len, AX25_MAX_INFO_LEN) == 0);
    CHECK(strcmp(pdest.callsign, "NEPSAT") == 0);
    CHECK(strcmp(psrc.callsign, "GROUND") == 0 && psrc.ssid == 1);
    CHECK(pinfo_len == sizeof(info) - 1);
    CHECK(memcmp(pinfo, info, pinfo_len) == 0);

    /* Corrupted FCS must be rejected. */
    uint8_t bad_frame[AX25_MAX_FRAME_LEN];
    memcpy(bad_frame, frame, (size_t)flen);
    bad_frame[flen - 1] ^= 0xFF;
    CHECK(ax25_parse_ui_frame(bad_frame, (uint16_t)flen, &pdest, &psrc, pinfo, &pinfo_len, AX25_MAX_INFO_LEN) != 0);

    /* Oversized info must be rejected at build time. */
    uint8_t huge[AX25_MAX_INFO_LEN + 1] = {0};
    CHECK(ax25_build_ui_frame(&dest, &src, huge, sizeof(huge), frame) == -1);
}

/* ---------------------------------------------------------------
 * FX.25 bit-stuffing + encode/decode under injected byte errors
 * --------------------------------------------------------------- */
static void test_fx25(void) {
    rs_init();

    /* Round trip a few payload shapes, including all-0xFF (worst case
       for bit-stuffing density) and empty info. */
    uint8_t payloads_lens[] = { 0, 1, 50, AX25_MAX_INFO_LEN };
    for (size_t p = 0; p < sizeof(payloads_lens); p++) {
        uint16_t info_len = payloads_lens[p];
        uint8_t info[AX25_MAX_INFO_LEN];
        memset(info, 0xFF, info_len);

        ax25_addr_t dest = { .callsign = "NEPSAT", .ssid = 0 };
        ax25_addr_t src  = { .callsign = "GROUND", .ssid = 1 };
        uint8_t ax25_frame[AX25_MAX_FRAME_LEN];
        int flen = ax25_build_ui_frame(&dest, &src, info, info_len, ax25_frame);
        CHECK(flen > 0);

        uint8_t fx_frame[FX25_FRAME_LEN];
        CHECK(fx25_encode_frame(ax25_frame, (uint16_t)flen, fx_frame) == 0);
        CHECK(fx25_tag_matches(fx_frame) == 1);

        int rs_corrected = 0;
        uint8_t out_ax25[AX25_MAX_FRAME_LEN];
        int out_len = fx25_decode_frame(fx_frame, out_ax25, AX25_MAX_FRAME_LEN, &rs_corrected);
        CHECK(out_len == flen);
        CHECK(rs_corrected == 0);
        CHECK(memcmp(out_ax25, ax25_frame, (size_t)flen) == 0);
    }

    /* Inject exactly 16 byte errors into the RS-protected block -- must
       still recover exactly. */
    {
        ax25_addr_t dest = { .callsign = "NEPSAT", .ssid = 0 };
        ax25_addr_t src  = { .callsign = "GROUND", .ssid = 1 };
        const uint8_t info[] = "S2S2 telemetry via FX.25";
        uint8_t ax25_frame[AX25_MAX_FRAME_LEN];
        int flen = ax25_build_ui_frame(&dest, &src, info, (uint16_t)(sizeof(info) - 1), ax25_frame);

        uint8_t fx_frame[FX25_FRAME_LEN];
        CHECK(fx25_encode_frame(ax25_frame, (uint16_t)flen, fx_frame) == 0);

        int exact = 0;
        for (int trial = 0; trial < 50; trial++) {
            uint8_t corrupted[FX25_FRAME_LEN];
            memcpy(corrupted, fx_frame, FX25_FRAME_LEN);
            int positions[FX25_BLOCK_LEN];
            for (int i = 0; i < FX25_BLOCK_LEN; i++) positions[i] = i;
            for (int i = FX25_BLOCK_LEN - 1; i > 0; i--) {
                int j = rnd() % (i + 1);
                int t = positions[i]; positions[i] = positions[j]; positions[j] = t;
            }
            for (int e = 0; e < 16; e++) {
                corrupted[FX25_TAG_LEN + positions[e]] ^= (uint8_t)(1 + rnd() % 255);
            }
            int rs_corrected = 0;
            uint8_t out_ax25[AX25_MAX_FRAME_LEN];
            int out_len = fx25_decode_frame(corrupted, out_ax25, AX25_MAX_FRAME_LEN, &rs_corrected);
            if (out_len == flen && memcmp(out_ax25, ax25_frame, (size_t)flen) == 0) exact++;
        }
        printf("FX.25 exactly at 16-byte correction limit: %d/50 exact recovery\n", exact);
        CHECK(exact == 50);

        /* 17 errors: must not silently return a wrong frame. */
        int safe = 0;
        for (int trial = 0; trial < 50; trial++) {
            uint8_t corrupted[FX25_FRAME_LEN];
            memcpy(corrupted, fx_frame, FX25_FRAME_LEN);
            int positions[FX25_BLOCK_LEN];
            for (int i = 0; i < FX25_BLOCK_LEN; i++) positions[i] = i;
            for (int i = FX25_BLOCK_LEN - 1; i > 0; i--) {
                int j = rnd() % (i + 1);
                int t = positions[i]; positions[i] = positions[j]; positions[j] = t;
            }
            for (int e = 0; e < 17; e++) {
                corrupted[FX25_TAG_LEN + positions[e]] ^= (uint8_t)(1 + rnd() % 255);
            }
            int rs_corrected = 0;
            uint8_t out_ax25[AX25_MAX_FRAME_LEN];
            int out_len = fx25_decode_frame(corrupted, out_ax25, AX25_MAX_FRAME_LEN, &rs_corrected);
            if (out_len < 0 || memcmp(out_ax25, ax25_frame, (size_t)flen) != 0) safe++;
        }
        printf("FX.25 beyond limit (17 errors): %d/50 correctly rejected/not-silently-wrong\n", safe);
        CHECK(safe == 50);
    }

    /* Tag mismatch must be rejected outright. */
    {
        uint8_t frame[FX25_FRAME_LEN] = {0};
        int rs_corrected = 0;
        uint8_t out[AX25_MAX_FRAME_LEN];
        CHECK(fx25_decode_frame(frame, out, AX25_MAX_FRAME_LEN, &rs_corrected) == -1);
    }
}

/* ---------------------------------------------------------------
 * Full message fragmentation/reassembly, including the independence
 * tradeoff: one fragment corrupted past repair drops only that
 * fragment's data, not neighboring fragments.
 * --------------------------------------------------------------- */
static void test_cubus_ax25(void) {
    cubus_ax25_link_t link = {
        .dest = { .callsign = "NEPSAT", .ssid = 1 },
        .src  = { .callsign = "GROUND", .ssid = 0 },
    };

    uint8_t payload[400];
    for (int i = 0; i < 400; i++) payload[i] = (uint8_t)(i * 7 + 3);

    uint8_t frames[CUBUS_AX25_MAX_FRAGMENTS][FX25_FRAME_LEN];
    int num_frames = 0;
    CHECK(cubus_ax25_fragment(&link, 1, TYPE_TELEMETRY, 1, 0, 42, 0x1234, FLAG_LAST_IN_BURST,
                               payload, sizeof(payload), frames, &num_frames) == 0);
    CHECK(num_frames == (400 + CUBUS_AX25_FRAG_MAX_DATA - 1) / CUBUS_AX25_FRAG_MAX_DATA);
    CHECK(num_frames > 1); /* must actually need multiple frames to be a real test */

    /* In-order delivery, no corruption. */
    {
        cubus_ax25_reassembler_t r;
        cubus_ax25_reassembler_init(&r);
        int result = 0;
        for (int i = 0; i < num_frames; i++) {
            result = cubus_ax25_accept_frame(&r, frames[i]);
            CHECK(result == (i == num_frames - 1 ? 1 : 0));
        }
        CHECK(r.payload_len == sizeof(payload));
        CHECK(memcmp(r.payload, payload, sizeof(payload)) == 0);
        CHECK(r.type == TYPE_TELEMETRY && r.src_id == 1 && r.dst_id == 0);
        CHECK(r.seq_num == 42 && r.session_id == 0x1234 && r.flags == FLAG_LAST_IN_BURST);
        CHECK(r.rs_bytes_corrected == 0);
    }

    /* Out-of-order delivery still reassembles correctly. */
    {
        cubus_ax25_reassembler_t r;
        cubus_ax25_reassembler_init(&r);
        int result = 0;
        for (int i = num_frames - 1; i >= 0; i--) {
            result = cubus_ax25_accept_frame(&r, frames[i]);
        }
        CHECK(result == 1);
        CHECK(r.payload_len == sizeof(payload));
        CHECK(memcmp(r.payload, payload, sizeof(payload)) == 0);
    }

    /* Independence tradeoff: corrupt one fragment past FX.25's repair
       limit (17 byte errors). That fragment is dropped; the OTHER
       fragments -- unlike the old cross-chunk-interleaved design --
       are completely unaffected and still deliver their data. */
    {
        uint8_t frames_corrupted[CUBUS_AX25_MAX_FRAGMENTS][FX25_FRAME_LEN];
        memcpy(frames_corrupted, frames, sizeof(frames));
        int victim = 1;
        for (int e = 0; e < 17; e++) {
            frames_corrupted[victim][FX25_TAG_LEN + e] ^= (uint8_t)(0x55 + e);
        }

        cubus_ax25_reassembler_t r;
        cubus_ax25_reassembler_init(&r);
        int dropped = 0, accepted = 0;
        for (int i = 0; i < num_frames; i++) {
            int rc = cubus_ax25_accept_frame(&r, frames_corrupted[i]);
            if (rc == -1) dropped++;
            else accepted++;
        }
        CHECK(dropped == 1);
        CHECK(accepted == num_frames - 1);
        CHECK(r.payload_len == 0); /* never completed -- one fragment missing */
        for (int f = 0; f < num_frames; f++) {
            if (f == victim) continue;
            uint16_t off = (uint16_t)(f * CUBUS_AX25_FRAG_MAX_DATA);
            uint16_t len = (f == num_frames - 1)
                ? (uint16_t)(sizeof(payload) - off)
                : CUBUS_AX25_FRAG_MAX_DATA;
            CHECK(memcmp(&r.payload[off], &payload[off], len) == 0);
        }
    }

    /* New seq_num/session_id mid-reassembly resets cleanly. */
    {
        uint8_t frames2[CUBUS_AX25_MAX_FRAGMENTS][FX25_FRAME_LEN];
        int num_frames2 = 0;
        uint8_t payload2[10] = { 9,8,7,6,5,4,3,2,1,0 };
        CHECK(cubus_ax25_fragment(&link, 1, TYPE_ACK, 0, 1, 99, 0x1234, 0,
                                   payload2, sizeof(payload2), frames2, &num_frames2) == 0);
        CHECK(num_frames2 == 1);

        cubus_ax25_reassembler_t r;
        cubus_ax25_reassembler_init(&r);
        CHECK(cubus_ax25_accept_frame(&r, frames[0]) == 0); /* first fragment of the 400B message */
        CHECK(cubus_ax25_accept_frame(&r, frames2[0]) == 1); /* different seq_num -- fresh message */
        CHECK(r.payload_len == sizeof(payload2));
        CHECK(memcmp(r.payload, payload2, sizeof(payload2)) == 0);
    }
}

/* ---------------------------------------------------------------
 * Gap tracker (unchanged, transport-independent)
 * --------------------------------------------------------------- */
static void test_gap_tracker(void) {
    gap_tracker_t t;
    gap_tracker_init(&t, 0x1234);

    cubus_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.session_id = 0x1234;

    uint16_t seqs[] = { 0, 1, 3, 4, 7 };
    for (size_t i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        pkt.seq_num = seqs[i];
        gap_tracker_accept(&t, &pkt);
    }

    uint16_t missing[16];
    int n = gap_tracker_report(&t, missing, 16);
    CHECK(n == 3);
    CHECK(missing[0] == 2 && missing[1] == 5 && missing[2] == 6);

    /* Cross-session packets must not pollute this tracker. */
    pkt.session_id = 0x9999;
    pkt.seq_num = 2;
    gap_tracker_accept(&t, &pkt);
    n = gap_tracker_report(&t, missing, 16);
    CHECK(n == 3); /* unchanged */

    /* Real-world case: seq_num is a single counter shared across every
       message type (beacons, ACKs, telecommands, telemetry), so a burst
       essentially never starts at 0. The report must not claim every
       sequence number before the burst's actual start is "missing". */
    gap_tracker_t t2;
    gap_tracker_init(&t2, 0xABCD);
    cubus_packet_t pkt2;
    memset(&pkt2, 0, sizeof(pkt2));
    pkt2.session_id = 0xABCD;
    uint16_t seqs2[] = { 200, 201, 203, 204 }; /* 202 missing, burst starts at 200 not 0 */
    for (size_t i = 0; i < sizeof(seqs2) / sizeof(seqs2[0]); i++) {
        pkt2.seq_num = seqs2[i];
        gap_tracker_accept(&t2, &pkt2);
    }
    uint16_t missing2[16];
    int n2 = gap_tracker_report(&t2, missing2, 16);
    CHECK(n2 == 1);
    CHECK(missing2[0] == 202);
}

/* ---------------------------------------------------------------
 * tx_history -- satellite-side retransmit ring buffer
 * --------------------------------------------------------------- */
static void test_tx_history(void) {
    tx_history_t h;
    tx_history_init(&h);

    /* Never stored -> not found. */
    CHECK(tx_history_find(&h, 42) == NULL);

    /* Store and find. */
    const uint8_t payload_a[] = "hello";
    tx_history_store(&h, 10, TYPE_TELEMETRY, 0 /* dst_id, arbitrary for this test */, 0, payload_a, sizeof(payload_a));
    const tx_history_entry_t *e = tx_history_find(&h, 10);
    CHECK(e != NULL);
    CHECK(e->type == TYPE_TELEMETRY);
    CHECK(e->payload_len == sizeof(payload_a));
    CHECK(memcmp(e->payload, payload_a, sizeof(payload_a)) == 0);

    /* Oversized payload silently not stored (documented tradeoff). */
    uint8_t big[TX_HISTORY_MAX_PAYLOAD + 1];
    memset(big, 0xAB, sizeof(big));
    tx_history_store(&h, 11, TYPE_TELEMETRY, 0 /* dst_id, arbitrary for this test */, 0, big, sizeof(big));
    CHECK(tx_history_find(&h, 11) == NULL);

    /* Ring wraps: filling past TX_HISTORY_LEN evicts the oldest entry. */
    tx_history_t h2;
    tx_history_init(&h2);
    uint8_t tiny[] = { 0x01 };
    for (int i = 0; i < TX_HISTORY_LEN + 3; i++) {
        tx_history_store(&h2, (uint16_t)i, TYPE_TELEMETRY, 0 /* dst_id, arbitrary for this test */, 0, tiny, sizeof(tiny));
    }
    CHECK(tx_history_find(&h2, 0) == NULL);  /* evicted */
    CHECK(tx_history_find(&h2, 2) == NULL);  /* evicted */
    CHECK(tx_history_find(&h2, 3) != NULL);  /* still present */
    CHECK(tx_history_find(&h2, TX_HISTORY_LEN + 2) != NULL); /* most recent */
}

int main(void) {
    rs_init();

    printf("== RS(255,223) 16-error/block limit ==\n");
    test_rs_limit();
    printf("== AX.25 address/FCS/UI-frame ==\n");
    test_ax25();
    printf("== FX.25 encode/decode ==\n");
    test_fx25();
    printf("== cubus_ax25 fragmentation/reassembly ==\n");
    test_cubus_ax25();
    printf("== gap_tracker ==\n");
    test_gap_tracker();
    printf("== tx_history retransmit ring buffer ==\n");
    test_tx_history();

    printf("\n===== %d failure(s) =====\n", failures);
    return failures == 0 ? 0 : 1;
}

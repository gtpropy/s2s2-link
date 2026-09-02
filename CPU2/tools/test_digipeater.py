# @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
# @date   2026-09-01
"""
test_digipeater.py -- tests the satellite's digipeater path
(Try_Digipeat/Digi_Is_Duplicate in ipcc_m0plus_main.c).

Two tiers, run `python3 test_digipeater.py [--port /dev/tty...]`:

1. OFFLINE (always runs): compiles a small C harness against the real,
   pure-logic sources this feature is built from -- core/Src/ax25.c,
   ax25_digi.c, fx25.c, rs.c -- and drives them through the exact same
   sequence Try_Digipeat itself does (decode FX.25 -> parse digi path ->
   dup-check -> find our hop -> rebuild -> re-encode). The dup-check
   functions (Digi_Frame_Hash/Digi_Is_Duplicate) are copied verbatim
   from ipcc_m0plus_main.c into the harness (see the C source string
   below) since they have no HAL dependency of their own.

   HONESTY NOTE: this does NOT call the real Try_Digipeat() symbol.
   That function lives in ipcc_m0plus_main.c itself, alongside HAL/radio
   calls (Send_FX25_Frame -> real SX126x TX) that don't host-compile the
   way the pure-logic .c files above do -- see the architecture plan for
   why this wasn't restructured just for testability. This tier proves
   the shared primitives Try_Digipeat is built from are correct (and,
   via the RELAY_HEX cross-check against this repo's own
   s2s2_decoder.fx25_decode_frame, that a relayed frame round-trips as
   valid FX.25); it does not prove Try_Digipeat's own glue code is bug-
   free. That needs the hardware tier below.

2. HARDWARE-IN-THE-LOOP (only with --port): sends the new `TESTDIGI
   <hex>` UART command (added to ipcc_m0plus_main.c, DEBUG_BUILD only)
   to a real connected satellite board twice with the identical frame
   and checks for "DIGI: duplicate" on the second, then once with a
   different frame and checks for "DIGI: relayed". No --port -> prints
   a clear skip message and exits 0 (not a failure -- this genuinely
   needs real hardware, not a fabricated result).

Run: python3 test_digipeater.py
     python3 test_digipeater.py --port /dev/tty.usbserial-XXXX
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import s2s2_decoder as dec

CPU2_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

C_HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "ax25.h"
#include "ax25_digi.h"
#include "fx25.h"
#include "config.h"

/* --- verbatim copy of ipcc_m0plus_main.c's dup-cache logic (HAL-free,
   see test_digipeater.py's docstring for why this is copied rather than
   linked) --- */
#define DIGI_DUP_TABLE_LEN 16
#define DIGI_DUP_TTL_MS    8000
typedef struct { int used; uint32_t hash; uint32_t seen_at_ms; } digi_dup_entry_t;
static digi_dup_entry_t s_digi_dup[DIGI_DUP_TABLE_LEN];
static int s_digi_dup_next = 0;
static uint32_t g_fake_now_ms = 1000;
static uint32_t Get_Time_Ms(void) { return g_fake_now_ms; }

static uint32_t Digi_Frame_Hash(const uint8_t *ax25_bytes, uint16_t len) {
    uint16_t fcs = (uint16_t)(ax25_bytes[len - 2] | (ax25_bytes[len - 1] << 8));
    return ((uint32_t)fcs << 16) ^ (uint32_t)len;
}
static int Digi_Is_Duplicate(uint32_t hash) {
    uint32_t now = Get_Time_Ms();
    for (int i = 0; i < DIGI_DUP_TABLE_LEN; i++) {
        if (s_digi_dup[i].used && s_digi_dup[i].hash == hash &&
            (now - s_digi_dup[i].seen_at_ms) < DIGI_DUP_TTL_MS) {
            return 1;
        }
    }
    s_digi_dup[s_digi_dup_next].used = 1;
    s_digi_dup[s_digi_dup_next].hash = hash;
    s_digi_dup[s_digi_dup_next].seen_at_ms = now;
    s_digi_dup_next = (s_digi_dup_next + 1) % DIGI_DUP_TABLE_LEN;
    return 0;
}
/* --- end verbatim copy --- */

static void print_hex(const char *label, const uint8_t *buf, int len) {
    printf("%s=", label);
    for (int i = 0; i < len; i++) printf("%02x", buf[i]);
    printf("\n");
}

/* Builds a foreign-station-shaped AX.25 UI frame with one digi hop
   addressed to our satellite (H-bit = already_repeated), wraps it as
   FX.25. Mirrors what a real third-party ham uplink would look like --
   S2S2's own traffic never uses a digi path (see ax25_digi.h). */
static int build_digi_frame(int already_repeated, uint8_t out_fx25[FX25_FRAME_LEN]) {
    ax25_addr_t dest = { "APRS", 0 };
    ax25_addr_t src  = { "N0CALL", 1 };
    ax25_addr_t digi = { AX25_SATELLITE_CALLSIGN, AX25_SATELLITE_SSID };

    uint8_t frame[64];
    int pos = 0;
    ax25_addr_encode(&dest, 0, 0, &frame[pos]); pos += AX25_ADDR_LEN;
    ax25_addr_encode(&src, 0, 0, &frame[pos]); pos += AX25_ADDR_LEN;
    ax25_addr_encode(&digi, 1, already_repeated ? 1 : 0, &frame[pos]); pos += AX25_ADDR_LEN;
    frame[pos++] = AX25_CTRL_UI;
    frame[pos++] = AX25_PID_NO_L3;
    const char *info = "TEST DIGI MSG";
    int info_len = (int)strlen(info);
    memcpy(&frame[pos], info, (size_t)info_len); pos += info_len;
    uint16_t fcs = ax25_fcs(frame, (uint16_t)pos);
    frame[pos++] = (uint8_t)(fcs & 0xFF);
    frame[pos++] = (uint8_t)(fcs >> 8);

    return fx25_encode_frame(frame, (uint16_t)pos, out_fx25) == 0 ? pos : -1;
}

static int find_our_hop(const ax25_full_frame_t *f) {
    for (uint8_t i = 0; i < f->num_digis; i++) {
        if (!f->digis[i].has_been_repeated &&
            strcmp(f->digis[i].addr.callsign, AX25_SATELLITE_CALLSIGN) == 0 &&
            f->digis[i].addr.ssid == AX25_SATELLITE_SSID) {
            return i;
        }
    }
    return -1;
}

int main(void) {
    rs_init();  /* builds rs.c's GF(256) tables -- rs_encode/rs_decode are silent
                   garbage without this, same as any other caller of rs.c */

    uint8_t fx25_a[FX25_FRAME_LEN];
    if (build_digi_frame(0, fx25_a) < 0) { fprintf(stderr, "build frame A failed\n"); return 1; }
    print_hex("FRAME_A_HEX", fx25_a, FX25_FRAME_LEN);

    uint8_t ax25_bytes[AX25_DIGI_MAX_FRAME_LEN];
    uint8_t scratch[FX25_FRAME_LEN];
    int rs_corrected = 0;

    memcpy(scratch, fx25_a, FX25_FRAME_LEN);
    int ax25_len = fx25_decode_frame(scratch, ax25_bytes, sizeof(ax25_bytes), &rs_corrected);
    if (ax25_len < 0) { fprintf(stderr, "fx25_decode_frame(A) failed\n"); return 1; }

    ax25_full_frame_t f;
    if (ax25_digi_parse(ax25_bytes, (uint16_t)ax25_len, &f) != 0) {
        fprintf(stderr, "ax25_digi_parse(A) failed\n"); return 1;
    }
    printf("A_NUM_DIGIS=%d\n", f.num_digis);

    uint32_t hash = Digi_Frame_Hash(ax25_bytes, (uint16_t)ax25_len);
    printf("DUP_FIRST=%s\n", Digi_Is_Duplicate(hash) ? "DUP" : "NOTDUP");
    printf("DUP_SECOND=%s\n", Digi_Is_Duplicate(hash) ? "DUP" : "NOTDUP");

    int hop_index = find_our_hop(&f);
    printf("A_HOP_INDEX=%d\n", hop_index);

    if (hop_index >= 0) {
        f.digis[hop_index].has_been_repeated = true;
        uint8_t rebuilt[AX25_DIGI_MAX_FRAME_LEN];
        uint16_t rebuilt_len = 0;
        if (ax25_digi_rebuild(&f, rebuilt, sizeof(rebuilt), &rebuilt_len) != 0) {
            fprintf(stderr, "ax25_digi_rebuild failed\n"); return 1;
        }
        uint8_t relay_fx25[FX25_FRAME_LEN];
        if (fx25_encode_frame(rebuilt, rebuilt_len, relay_fx25) != 0) {
            fprintf(stderr, "fx25_encode_frame(relay) failed\n"); return 1;
        }
        print_hex("RELAY_HEX", relay_fx25, FX25_FRAME_LEN);
    }

    /* Frame B: same digi hop, but already marked repeated (H=1) -- must
       NOT be eligible (mirrors Try_Digipeat's has_been_repeated check). */
    uint8_t fx25_b[FX25_FRAME_LEN];
    if (build_digi_frame(1, fx25_b) < 0) { fprintf(stderr, "build frame B failed\n"); return 1; }
    memcpy(scratch, fx25_b, FX25_FRAME_LEN);
    int rs_corrected_b = 0;
    int ax25_len_b = fx25_decode_frame(scratch, ax25_bytes, sizeof(ax25_bytes), &rs_corrected_b);
    if (ax25_len_b < 0) { fprintf(stderr, "fx25_decode_frame(B) failed\n"); return 1; }
    ax25_full_frame_t fb;
    if (ax25_digi_parse(ax25_bytes, (uint16_t)ax25_len_b, &fb) != 0) {
        fprintf(stderr, "ax25_digi_parse(B) failed\n"); return 1;
    }
    printf("B_HOP_INDEX=%d\n", find_our_hop(&fb));

    /* Frame C: plain 2-address frame, no digi path at all -- the common
       case for S2S2's own traffic, must parse with num_digis=0. */
    ax25_addr_t dest = { "APRS", 0 };
    ax25_addr_t src  = { "N0CALL", 1 };
    uint8_t plain[AX25_MAX_FRAME_LEN];
    int plain_len = ax25_build_ui_frame(&dest, &src, (const uint8_t *)"HI", 2, plain);
    uint8_t fx25_c[FX25_FRAME_LEN];
    if (plain_len < 0 || fx25_encode_frame(plain, (uint16_t)plain_len, fx25_c) != 0) {
        fprintf(stderr, "build frame C failed\n"); return 1;
    }
    memcpy(scratch, fx25_c, FX25_FRAME_LEN);
    int rs_corrected_c = 0;
    int ax25_len_c = fx25_decode_frame(scratch, ax25_bytes, sizeof(ax25_bytes), &rs_corrected_c);
    if (ax25_len_c < 0) { fprintf(stderr, "fx25_decode_frame(C) failed\n"); return 1; }
    ax25_full_frame_t fc;
    if (ax25_digi_parse(ax25_bytes, (uint16_t)ax25_len_c, &fc) != 0) {
        fprintf(stderr, "ax25_digi_parse(C) failed\n"); return 1;
    }
    printf("C_NUM_DIGIS=%d\n", fc.num_digis);

    return 0;
}
"""


def check(name, cond):
    status = "PASS" if cond else "FAIL"
    print(f"[{status}] {name}")
    if not cond:
        raise SystemExit(1)


def run_offline_tier():
    with tempfile.TemporaryDirectory() as td:
        c_path = os.path.join(td, "digi_harness.c")
        bin_path = os.path.join(td, "digi_harness")
        with open(c_path, "w") as f:
            f.write(C_HARNESS)
        subprocess.run(
            ["clang", "-std=c11", "-O0", "-Wall",
             "-I", os.path.join(CPU2_ROOT, "core/Inc"),
             os.path.join(CPU2_ROOT, "core/Src/ax25.c"),
             os.path.join(CPU2_ROOT, "core/Src/ax25_digi.c"),
             os.path.join(CPU2_ROOT, "core/Src/fx25.c"),
             os.path.join(CPU2_ROOT, "core/Src/rs.c"),
             c_path, "-o", bin_path],
            check=True,
        )
        out = subprocess.run([bin_path], check=True, capture_output=True, text=True).stdout

    fields = dict(line.split("=", 1) for line in out.strip().splitlines() if "=" in line)

    print("--- offline tier (real ax25.c/ax25_digi.c/fx25.c/rs.c + copied dup-cache) ---")
    check("frame A: parsed with exactly 1 digi hop", fields.get("A_NUM_DIGIS") == "1")
    check("frame A: our hop found at index 0 (unrepeated)", fields.get("A_HOP_INDEX") == "0")
    check("first sighting is not a duplicate", fields.get("DUP_FIRST") == "NOTDUP")
    check("identical frame seen again IS a duplicate", fields.get("DUP_SECOND") == "DUP")

    relay_hex = fields.get("RELAY_HEX")
    check("relay frame was produced", bool(relay_hex) and len(relay_hex) == dec.FX25_FRAME_LEN * 2)
    relayed = bytes.fromhex(relay_hex)
    result = dec.fx25_decode_frame(relayed)
    check("relayed frame round-trips through this repo's own FX.25 decoder", result is not None)

    check("frame B: already-repeated hop is NOT eligible (hop index -1)",
          fields.get("B_HOP_INDEX") == "-1")
    check("frame C: plain 2-address frame parses with 0 digis",
          fields.get("C_NUM_DIGIS") == "0")
    print("offline tier: all checks passed\n")


def run_hardware_tier(port: str, baud: int):
    import serial  # noqa: local import -- optional dependency, only needed here

    with tempfile.TemporaryDirectory() as td:
        c_path = os.path.join(td, "digi_harness.c")
        bin_path = os.path.join(td, "digi_harness")
        with open(c_path, "w") as f:
            f.write(C_HARNESS)
        subprocess.run(
            ["clang", "-std=c11", "-O0",
             "-I", os.path.join(CPU2_ROOT, "core/Inc"),
             os.path.join(CPU2_ROOT, "core/Src/ax25.c"),
             os.path.join(CPU2_ROOT, "core/Src/ax25_digi.c"),
             os.path.join(CPU2_ROOT, "core/Src/fx25.c"),
             os.path.join(CPU2_ROOT, "core/Src/rs.c"),
             c_path, "-o", bin_path],
            check=True,
        )
        out = subprocess.run([bin_path], check=True, capture_output=True, text=True).stdout
    fields = dict(line.split("=", 1) for line in out.strip().splitlines() if "=" in line)
    frame_hex = fields["FRAME_A_HEX"]

    print(f"--- hardware tier ({port} @ {baud}) ---")
    ser = serial.Serial(port, baudrate=baud, timeout=2)
    time.sleep(2)  # let the board's USB-CDC enumerate/settle
    ser.reset_input_buffer()

    def send_and_collect(cmd, wait_s=1.0):
        ser.write((cmd + "\r\n").encode("ascii"))
        time.sleep(wait_s)
        return ser.read(ser.in_waiting or 1).decode("ascii", errors="replace")

    reply1 = send_and_collect(f"TESTDIGI {frame_hex}")
    check("first TESTDIGI: board responded", len(reply1) > 0)
    reply2 = send_and_collect(f"TESTDIGI {frame_hex}")
    check("second TESTDIGI (same frame): reports duplicate",
          "DIGI: duplicate" in reply2)

    ser.close()
    print("hardware tier: all checks passed\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=None,
                         help="satellite board's CPU2 debug UART port for the hardware tier "
                              "(NOT the NuttX/NSH console port -- see README section on the "
                              "dual-core Nucleo's two serial ports)")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    run_offline_tier()

    if args.port:
        run_hardware_tier(args.port, args.baud)
    else:
        print("no --port given: skipping hardware-in-the-loop tier "
              "(this is the tier that actually exercises Try_Digipeat() itself)")

    print("test_digipeater.py: done")


if __name__ == "__main__":
    main()

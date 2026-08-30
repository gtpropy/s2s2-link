#!/usr/bin/env python3
"""
make_demo_capture.py -- builds a synthetic .cfile (interleaved float32
I/Q, GNU Radio convention) containing a handful of real firmware-encoded
S2S2/FX.25 frames run through the synthetic GMSK modulator in
gmsk_demod.py, with AWGN added. Lets you test s2s2_rx_gui.py's "IQ file"
source and the whole demod/decode pipeline without an RTL-SDR or Nucleo.

Requires clang on PATH (compiles a tiny harness against the real
core/Src/{rs,ax25,fx25,cubus_ax25,cubus_proto,s2s2_commands}.c sources,
same as test_demod.py -- not a Python re-implementation of the encoder).

Usage:
    python3 make_demo_capture.py [output.cfile] [--sample-rate 240000] [--snr-db 15]
"""
import argparse
import os
import subprocess
import sys
import tempfile
import textwrap

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gmsk_demod as gd

CPU2_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def build_demo_frames():
    src = textwrap.dedent("""
        #include <stdio.h>
        #include <string.h>
        #include "s2s2com.h"
        #include "s2s2_commands.h"
        #include "cubus_ax25.h"

        static void emit(uint8_t frame[FX25_FRAME_LEN]) {
            for (int j = 0; j < FX25_FRAME_LEN; j++) printf("%02x", frame[j]);
            printf("\\n");
        }

        int main(void) {
            cubus_proto_init();
            cubus_ax25_link_t link;
            memset(&link, 0, sizeof(link));
            strcpy(link.dest.callsign, "GROUND"); link.dest.ssid = 0;
            strcpy(link.src.callsign, "NEPSAT"); link.src.ssid = 1;

            uint8_t frames[CUBUS_AX25_MAX_FRAGMENTS][FX25_FRAME_LEN];
            int n;

            beacon1_t b1; memset(&b1, 0, sizeof(b1));
            b1.type = 1; b1.tim_day = 100; b1.tim_hour = 6;
            b1.bat_v = 7600; b1.bat_i = 300; b1.bat_temp = 22;
            b1.sol_tot_v = 8300; b1.sol_tot_i = 120; b1.system_stat = 1;
            uint8_t p1[1 + BEACON1_PAYLOAD_LEN];
            p1[0] = DATA_ID_BEACON1;
            beacon1_pack(&b1, &p1[1]);
            cubus_ax25_fragment(&link, 1, TYPE_TELEMETRY, 1, 0, 1, 500,
                                 FLAG_LAST_IN_BURST, p1, sizeof(p1), frames, &n);
            for (int i = 0; i < n; i++) emit(frames[i]);

            const char *msg = "S2S2 demo capture -- hello from the ground!";
            cubus_ax25_fragment(&link, 1, TYPE_RAW, 0, 1, 2, 501,
                                 FLAG_LAST_IN_BURST, (const uint8_t *)msg,
                                 (uint16_t)strlen(msg), frames, &n);
            for (int i = 0; i < n; i++) emit(frames[i]);

            return 0;
        }
    """)
    with tempfile.TemporaryDirectory() as td:
        c_path = os.path.join(td, "gen.c")
        bin_path = os.path.join(td, "gen")
        with open(c_path, "w") as f:
            f.write(src)
        subprocess.run(
            ["clang", "-std=c11", "-O2", "-I", os.path.join(CPU2_ROOT, "core/Inc"),
             os.path.join(CPU2_ROOT, "core/Src/rs.c"),
             os.path.join(CPU2_ROOT, "core/Src/ax25.c"),
             os.path.join(CPU2_ROOT, "core/Src/fx25.c"),
             os.path.join(CPU2_ROOT, "core/Src/cubus_ax25.c"),
             os.path.join(CPU2_ROOT, "core/Src/cubus_proto.c"),
             os.path.join(CPU2_ROOT, "core/Src/s2s2_commands.c"),
             c_path, "-o", bin_path],
            check=True,
        )
        out = subprocess.run([bin_path], check=True, capture_output=True, text=True).stdout
    return [bytes.fromhex(line) for line in out.splitlines() if line.strip()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output", nargs="?", default="s2s2_demo.cfile")
    ap.add_argument("--sample-rate", type=float, default=240000)
    ap.add_argument("--snr-db", type=float, default=15.0)
    ap.add_argument("--gap-sec", type=float, default=0.3, help="silence between frames")
    args = ap.parse_args()

    frames = build_demo_frames()
    print(f"Built {len(frames)} real firmware-encoded frame(s)")

    pieces = []
    gap = np.zeros(int(args.sample_rate * args.gap_sec), dtype=np.complex64)
    for f in frames:
        pieces.append(gap)
        pieces.append(gd.gmsk_modulate(f, sample_rate=args.sample_rate, order="msb", invert=False))
    pieces.append(gap)
    iq = np.concatenate(pieces)

    sig_power = np.mean(np.abs(iq) ** 2)
    noise_power = sig_power / (10 ** (args.snr_db / 10.0))
    noise = np.sqrt(noise_power / 2) * (np.random.randn(*iq.shape) + 1j * np.random.randn(*iq.shape))
    iq = (iq + noise).astype(np.complex64)

    interleaved = np.empty(len(iq) * 2, dtype=np.float32)
    interleaved[0::2] = iq.real
    interleaved[1::2] = iq.imag
    interleaved.tofile(args.output)

    print(f"Wrote {args.output}: {len(iq)} samples @ {args.sample_rate:.0f} sps, "
          f"SNR={args.snr_db} dB. Load it in s2s2_rx_gui.py with source=IQ file, "
          f"format=cf32, sample rate={args.sample_rate:.0f}.")


if __name__ == "__main__":
    main()

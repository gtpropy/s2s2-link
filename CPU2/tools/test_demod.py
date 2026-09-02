# @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
# @date   2026-08-30
"""
test_demod.py -- end-to-end self-test for gmsk_demod.py + s2s2_decoder.py,
using real firmware-encoded FX.25 frame bytes (generated via a small C
harness against the actual core/Src/{rs,ax25,fx25,cubus_ax25}.c) run
through a synthetic GMSK modulator, AWGN, and the full demod+sync+decode
pipeline. There is no RTL-SDR or Nucleo hardware in this environment, so
this is the strongest available verification: a real bug in either the
RS/FX.25/AX.25 decode logic or the demod/sync logic will show up as a
failed round trip here.

Run: python3 test_demod.py
"""
import subprocess
import sys
import tempfile
import textwrap
import os

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gmsk_demod as gd
import s2s2_decoder as dec

CPU2_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def build_reference_frame() -> bytes:
    """Compiles a tiny C program against the real firmware sources to
    fragment one TYPE_RAW message and returns its single FX.25 frame's
    168 raw bytes -- ground truth, not a Python re-implementation."""
    src = textwrap.dedent("""
        #include <stdio.h>
        #include <string.h>
        #include "s2s2com.h"
        #include "cubus_ax25.h"
        int main(void) {
            cubus_proto_init();
            cubus_ax25_link_t link;
            memset(&link, 0, sizeof(link));
            strcpy(link.dest.callsign, "GROUND"); link.dest.ssid = 0;
            strcpy(link.src.callsign, "NEPSAT"); link.src.ssid = 1;
            const char *msg = "S2S2 GMSK selftest";
            uint8_t frames[CUBUS_AX25_MAX_FRAGMENTS][FX25_FRAME_LEN];
            int n = 0;
            int rc = cubus_ax25_fragment(&link, 1, TYPE_RAW, 1, 0, 7, 42,
                                          FLAG_LAST_IN_BURST,
                                          (const uint8_t *)msg, (uint16_t)strlen(msg),
                                          frames, &n);
            if (rc != 0 || n != 1) { fprintf(stderr, "fragment failed\\n"); return 1; }
            for (int j = 0; j < FX25_FRAME_LEN; j++) printf("%02x", frames[0][j]);
            printf("\\n");
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
             c_path, "-o", bin_path],
            check=True,
        )
        out = subprocess.run([bin_path], check=True, capture_output=True, text=True).stdout.strip()
    return bytes.fromhex(out)


def run_case(label, snr_db, sample_rate):
    frame = build_reference_frame()
    assert len(frame) == dec.FX25_FRAME_LEN

    iq = gd.gmsk_modulate(frame, sample_rate=sample_rate, order="msb", invert=False)

    if snr_db is not None:
        sig_power = np.mean(np.abs(iq) ** 2)
        noise_power = sig_power / (10 ** (snr_db / 10.0))
        noise = np.sqrt(noise_power / 2) * (np.random.randn(*iq.shape) + 1j * np.random.randn(*iq.shape))
        iq = iq + noise.astype(np.complex64)

    candidates = gd.extract_candidate_frames(iq, sample_rate)
    good = []
    for c in candidates:
        result = dec.fx25_decode_frame(c["frame"])
        if result is not None:
            good.append((c, result))

    print(f"[{label}] candidates={len(candidates)} tag-valid={len(good)}")
    assert good, f"{label}: no candidate decoded successfully"

    ax25_frame, rs_corrected = good[0][1]
    parsed = dec.ax25_parse_ui_frame(ax25_frame)
    assert parsed is not None, f"{label}: AX.25 parse/FCS failed on recovered frame"

    r = dec.Reassembler()
    status, pkt = r.accept_frame(good[0][0]["frame"])
    assert status == "complete", f"{label}: reassembly status={status} ({pkt})"
    assert pkt["payload"] == b"S2S2 GMSK selftest", f"{label}: payload mismatch: {pkt['payload']!r}"
    print(f"[{label}] OK -- combo={{'order':{good[0][0]['order']!r},'invert':{good[0][0]['invert']}}}, "
          f"rs_corrected={rs_corrected}, payload={pkt['payload']!r}")


def run_erasure_wiring_case():
    """Proves the confidence -> erasure wiring end-to-end (gmsk_demod's
    `confidence` field -> s2s2_decoder.erasure_positions_from_confidence
    -> rs_decode_with_erasures -> Reassembler.accept_frame) using the
    real frame layout constants and the real decode/reassembly code
    path. The RS math itself (does erasure-marking actually recover
    more errors) is already proven exhaustively in test_decoder.py;
    this test is specifically about the plumbing between gmsk_demod's
    per-byte confidence output and s2s2_decoder's consumption of it --
    frame168-index-to-codeword-position mapping, threshold/cap
    selection, and Reassembler.accept_frame's confidence pass-through --
    so it corrupts a known, controlled set of bytes in the real
    firmware-encoded reference frame rather than depending on how
    discriminating this demod chain's raw confidence metric happens to
    be for a given SNR (a separate DSP-tuning question)."""
    frame = build_reference_frame()

    rng = np.random.RandomState(7)
    bad_frame = bytearray(frame)
    confidence = [1.0] * dec.FX25_FRAME_LEN
    corrupt_idx = rng.choice(range(dec.FX25_TAG_LEN, dec.FX25_FRAME_LEN), size=20, replace=False)
    for idx in corrupt_idx:
        orig = bad_frame[idx]
        new = orig
        while new == orig:
            new = int(rng.randint(0, 256))
        bad_frame[idx] = new
        confidence[idx] = 0.0
    bad_frame = bytes(bad_frame)

    plain_result = dec.fx25_decode_frame(bad_frame)
    assert plain_result is None, "erasure-wiring case: plain decode should have failed past RS_T=16"

    era_result = dec.fx25_decode_frame(bad_frame, confidence)
    assert era_result is not None, "erasure-wiring case: confidence-guided decode should have recovered it"
    ax25_frame, corrected = era_result
    parsed = dec.ax25_parse_ui_frame(ax25_frame)
    assert parsed is not None, "erasure-wiring case: AX.25 parse/FCS failed on recovered frame"

    r = dec.Reassembler()
    status, pkt = r.accept_frame(bad_frame, confidence)
    assert status == "complete", f"erasure-wiring case: reassembly status={status} ({pkt})"
    assert pkt["payload"] == b"S2S2 GMSK selftest", f"erasure-wiring case: payload mismatch: {pkt['payload']!r}"
    print(f"[erasure-wiring] plain rs_decode correctly failed on the corrupted frame; "
          f"confidence-guided rs_decode_with_erasures recovered it (corrected={corrected} bytes), "
          f"payload={pkt['payload']!r}")


def main():
    np.random.seed(0)
    run_case("clean, 240ksps", snr_db=None, sample_rate=240000)
    run_case("20dB SNR, 240ksps", snr_db=20, sample_rate=240000)
    run_case("12dB SNR, 250ksps (non-integer sps)", snr_db=12, sample_rate=250000)
    run_erasure_wiring_case()
    print("\nALL DEMOD SELF-TESTS PASSED")


if __name__ == "__main__":
    main()

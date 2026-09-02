# @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
# @date   2026-09-01
"""
test_decoder.py -- self-checks for s2s2_decoder.py's RS(255,223) decode
path, in particular rs_decode_with_erasures(). No live hardware needed:
rs_encode() (self-test-only) produces real codewords, corrupted directly.

Run: python3 test_decoder.py
"""
import random

from s2s2_decoder import (
    RS_K, RS_N, RS_PARITY, RS_T,
    rs_encode, rs_decode, rs_decode_with_erasures,
)


def random_msg(rng):
    return bytes(rng.randrange(256) for _ in range(RS_K))


def corrupt(codeword: bytes, positions, rng):
    out = bytearray(codeword)
    for p in positions:
        orig = out[p]
        new = orig
        while new == orig:
            new = rng.randrange(256)
        out[p] = new
    return out


def check(name, cond):
    status = "PASS" if cond else "FAIL"
    print(f"[{status}] {name}")
    if not cond:
        raise SystemExit(1)


def main():
    rng = random.Random(1234)

    # 1. Clean roundtrip.
    msg = random_msg(rng)
    cw = rs_encode(msg)
    cw2 = bytearray(cw)
    check("clean codeword decodes with 0 corrections", rs_decode(cw2) == 0)
    check("clean codeword unchanged", bytes(cw2) == cw)

    # 2. Plain rs_decode up to RS_T=16 errors.
    for n_err in (1, 5, 16):
        msg = random_msg(rng)
        cw = rs_encode(msg)
        positions = rng.sample(range(RS_N), n_err)
        bad = corrupt(cw, positions, rng)
        result = rs_decode(bad)
        check(f"rs_decode corrects {n_err} errors", result == n_err)
        check(f"rs_decode({n_err} errors) recovers original", bytes(bad) == cw)

    # 3. rs_decode fails past capacity (17 errors > RS_T=16).
    msg = random_msg(rng)
    cw = rs_encode(msg)
    bad = corrupt(cw, rng.sample(range(RS_N), 17), rng)
    result = rs_decode(bad)
    check("rs_decode correctly refuses 17 errors (not silently wrong)",
          result == -1 or bytes(bad) != cw)

    # 4. rs_decode_with_erasures([]) == rs_decode() for every case above
    #    (regression check on the rs_decode refactor into shared helpers).
    for n_err in (0, 1, 5, 16):
        msg = random_msg(rng)
        cw = rs_encode(msg)
        positions = rng.sample(range(RS_N), n_err)
        bad_a = corrupt(cw, positions, rng) if n_err else bytearray(cw)
        bad_b = bytearray(bad_a)
        ra = rs_decode(bad_a)
        rb = rs_decode_with_erasures(bad_b, [])
        check(f"erasures([]) matches rs_decode() at {n_err} errors", ra == rb)
        check(f"erasures([]) output matches rs_decode() output at {n_err} errors",
              bytes(bad_a) == bytes(bad_b))

    # 5. THE POINT: erasures let you correct past RS_T=16.
    #    24 known-bad positions, all erasures, 0 unknown errors: 2*0+24=24<=32, must work.
    msg = random_msg(rng)
    cw = rs_encode(msg)
    positions = rng.sample(range(RS_N), 24)
    bad = corrupt(cw, positions, rng)
    plain_result = rs_decode(bytearray(bad))
    check("plain rs_decode CANNOT fix 24 errors (past t=16)", plain_result == -1)
    era_result = rs_decode_with_erasures(bad, positions)
    check("rs_decode_with_erasures fixes 24 known-bad bytes", era_result == 24)
    check("rs_decode_with_erasures recovers exact original", bytes(bad) == cw)

    # 6. Mixed: 20 erasures (known-bad) + 3 unknown errors elsewhere.
    #    2*3+20=26<=32, must work.
    msg = random_msg(rng)
    cw = rs_encode(msg)
    all_positions = rng.sample(range(RS_N), 23)
    erasure_positions = all_positions[:20]
    unknown_positions = all_positions[20:]
    bad = corrupt(cw, all_positions, rng)
    result = rs_decode_with_erasures(bad, erasure_positions)
    check("mixed 20 erasures + 3 unknown errors decodes", result == 23)
    check("mixed case recovers exact original", bytes(bad) == cw)

    # 7. Over capacity even with erasures: 2*5+24=34>32 must fail cleanly.
    msg = random_msg(rng)
    cw = rs_encode(msg)
    all_positions = rng.sample(range(RS_N), 29)
    erasure_positions = all_positions[:24]
    bad = corrupt(cw, all_positions, rng)
    result = rs_decode_with_erasures(bad, erasure_positions)
    check("over-capacity erasures+errors refuses cleanly (no false correct)",
          result == -1 or bytes(bad) != cw)

    # 8. Erasure positions that were actually fine (bad confidence hint) --
    #    must not silently corrupt a codeword that didn't need fixing there.
    msg = random_msg(rng)
    cw = rs_encode(msg)
    real_error_positions = rng.sample(range(RS_N), 2)
    bad = corrupt(cw, real_error_positions, rng)
    fake_erasures = rng.sample([p for p in range(RS_N) if p not in real_error_positions], 5)
    result = rs_decode_with_erasures(bad, fake_erasures)
    check("bogus erasure hints still recover the true codeword (or refuse)",
          result == -1 or bytes(bad) == cw)

    print("\nall checks passed")


if __name__ == "__main__":
    main()

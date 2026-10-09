import random, sys, time
import fec_model as M
from seal_emu import Seal, DATA as _D
DATA = _D + 0x1000

s = Seal(count=False)
rng = random.Random(2026)
CANARY = 0x5A
t0 = time.time()
cases = fixed_ok = corrected_total = uncorr = miscorr = 0

def run(rx, cw_expected, nerr, label):
    global cases, corrected_total, uncorr, miscorr
    # canaries around the codeword buffer: asm must not touch neighbours
    s.write(DATA - 64, bytes([CANARY]) * 64); s.write(DATA + 152, bytes([CANARY]) * 64)
    out, r = s.decode(bytes(rx), addr=DATA)
    assert s.read(DATA - 64, 64) == bytes([CANARY]) * 64 and s.read(DATA + 152, 64) == bytes([CANARY]) * 64, "buffer overrun (%s)" % label
    cases += 1
    if nerr <= 16:
        assert out == cw_expected and r == nerr, ("FAIL", label, nerr, r)
        corrected_total += nerr
    else:
        if r == -1:
            uncorr += 1
        else:
            # claimed success: output must be a valid codeword (zero syndromes) and match the model
            assert not any(M.syndromes(out)), ("claimed success but invalid codeword", label)
            miscorr += 1

def mkcw():
    data = bytes(rng.randrange(256) for _ in range(120))
    return data + M.encode(data)

# edge positions: first/last bytes, check bytes only, data bytes only, bursts
for label, positions in (
    ("first+last", [0, 151]),
    ("check only 16", list(range(120, 136))),
    ("check all 32 -> uncorrectable", list(range(120, 152))),
    ("data only 16", list(range(0, 16))),
    ("burst mid 16", list(range(60, 76))),
    ("burst end 16", list(range(136, 152))),
    ("spread 16", [i * 9 for i in range(16)]),
):
    for vals in ("rand", "ones", "ff"):
        cw = mkcw(); rx = bytearray(cw)
        for p in positions:
            rx[p] ^= {"rand": rng.randrange(1, 256), "ones": 1, "ff": 0xFF}[vals]
        run(rx, cw, len(positions), label + "/" + vals)

# special data patterns
for pat in (bytes(120), b"\xff" * 120, bytes(range(120)), b"\xaa\x55" * 60):
    cw = pat + M.encode(pat)
    for nerr in (0, 1, 8, 16):
        rx = bytearray(cw)
        for p in rng.sample(range(152), nerr): rx[p] ^= rng.randrange(1, 256)
        run(rx, cw, nerr, "pattern")

# random sweep: every error count 0..24, many trials each
N = int(sys.argv[1]) if len(sys.argv) > 1 else 60
for nerr in range(0, 25):
    for _ in range(N):
        cw = mkcw(); rx = bytearray(cw)
        for p in rng.sample(range(152), nerr): rx[p] ^= rng.randrange(1, 256)
        run(rx, cw, nerr, "rand%d" % nerr)

print("STRESS PASS: %d cases | %d bytes corrected | uncorrectable flagged: %d | claimed-success beyond t (valid codewords): %d | %.0f s"
      % (cases, corrected_total, uncorr, miscorr, time.time() - t0))

import random, sys
import fec_model as M
from seal_emu import Seal

s = Seal()
rng = random.Random(11)

# --- encode vs model
for t in range(40):
    data = bytes(rng.randrange(256) for _ in range(M.N_DATA))
    if t == 0: data = bytes(120)
    if t == 1: data = b"\xff" * 120
    if t == 2: data = bytes([1]) + bytes(119)
    assert s.encode(data) == M.encode(data), "encode mismatch t=%d" % t
print("encode: 40 vectors match model; instructions per frame: %d" % s.icount)

# --- syndromes vs model
for t in range(30):
    data = bytes(rng.randrange(256) for _ in range(M.N_DATA))
    cw = bytearray(data + M.encode(data))
    syn, nz = s.syndromes(bytes(cw))
    assert nz == 0 and not any(syn), "clean codeword must have zero syndromes"
    for _ in range(rng.randrange(1, 12)):
        cw[rng.randrange(152)] ^= rng.randrange(1, 256)
    syn, nz = s.syndromes(bytes(cw))
    assert syn == M.syndromes(bytes(cw)), "syndrome mismatch"
    assert (nz != 0) == any(syn)
print("syndromes: 30 clean + 30 corrupted match model; instructions per frame: %d" % s.icount)

if "decode" in sys.argv:
    n_ok = 0
    for t in range(int(sys.argv[sys.argv.index("decode") + 1]) if len(sys.argv) > sys.argv.index("decode") + 1 else 100):
        data = bytes(rng.randrange(256) for _ in range(M.N_DATA))
        cw = data + M.encode(data)
        nerr = rng.choice([0, 1, 2, 5, 8, 12, 15, 16, 16, 17, 20, 30])
        rx = bytearray(cw)
        for p in rng.sample(range(152), nerr):
            rx[p] ^= rng.randrange(1, 256)
        out, r = s.decode(bytes(rx))
        mo, mn = M.decode(bytes(rx))
        if nerr <= 16:
            assert out == cw and r == nerr, ("decode fail", t, nerr, r)
            n_ok += 1
        else:
            assert (r == -1) == (mn == -1), ("uncorrectable disagreement", t, nerr, r, mn)
            if r != -1: assert out == mo
    print("decode: ok (%d correctable verified), last instruction count %d" % (n_ok, s.icount))
print("ALL PASS")

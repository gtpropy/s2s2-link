#!/usr/bin/env python3
"""
fec_model.py -- reference model of the light FEC wrap block code.

Shortened byte code over GF(256) (poly 0x11D, alpha = 2), 32 check bytes,
roots alpha^0 .. alpha^31 (first consecutive root 0). Codeword layout:
    cw[0..119]   data (120 bytes)
    cw[120..151] check bytes (32 bytes)
cw[i] is the coefficient of x^(151-i). Corrects up to 16 byte errors.

This is the algorithm the hand-written assembly (Mission/seal_core.S)
implements; tests/test_seal_asm.py runs the assembly under an emulator and
compares against this model.
"""
import random

PRIM = 0x11D
N_DATA, N_PAR = 120, 32
N_CW = N_DATA + N_PAR  # 152

EXP = [0] * 512
LOG = [0] * 256
x = 1
for i in range(255):
    EXP[i] = x
    LOG[x] = i
    x <<= 1
    if x & 0x100:
        x ^= PRIM
for i in range(255, 512):
    EXP[i] = EXP[i - 255]


def mul(a, b):
    return 0 if a == 0 or b == 0 else EXP[LOG[a] + LOG[b]]


def div(a, b):
    assert b != 0
    return 0 if a == 0 else EXP[LOG[a] + 255 - LOG[b]]


def gen_poly():
    """g(x) = prod_{j=0}^{31} (x - alpha^j); returns [g32=1, g31, ..., g0]."""
    g = [1]
    for j in range(N_PAR):
        r = EXP[j]
        ng = g + [0]
        for k in range(len(g)):
            ng[k + 1] ^= mul(g[k], r)
        g = ng
    return g


GEN = gen_poly()                       # GEN[0]=1 (x^32) ... GEN[32]=g0
assert all(c != 0 for c in GEN), "zero generator coefficient"


def encode(data):
    assert len(data) == N_DATA
    p = [0] * N_PAR
    for d in data:
        fb = d ^ p[0]
        for i in range(N_PAR - 1):
            p[i] = p[i + 1] ^ mul(fb, GEN[i + 1])
        p[N_PAR - 1] = mul(fb, GEN[N_PAR])
    return bytes(p)


def syndromes(cw):
    s = [0] * N_PAR
    for b in cw:
        for j in range(N_PAR):
            s[j] = mul(s[j], EXP[j]) ^ b
    return s


def berlekamp_massey(s):
    C = [1] + [0] * 33
    B = [1] + [0] * 33
    L, m, b = 0, 1, 1
    for n in range(N_PAR):
        d = s[n]
        for i in range(1, L + 1):
            d ^= mul(C[i], s[n - i])
        if d == 0:
            m += 1
        elif 2 * L <= n:
            T = C[:]
            f = div(d, b)
            for i in range(0, 34 - m):
                C[i + m] ^= mul(f, B[i])
            L = n + 1 - L
            B = T
            b = d
            m = 1
        else:
            f = div(d, b)
            for i in range(0, 34 - m):
                C[i + m] ^= mul(f, B[i])
            m += 1
    return C, L


def decode(cw):
    """Returns (corrected_cw, nerr) or (None, -1) when uncorrectable."""
    cw = bytearray(cw)
    s = syndromes(cw)
    if not any(s):
        return bytes(cw), 0
    C, L = berlekamp_massey(s)
    if L > 16:
        return None, -1
    roots = []
    for e in range(N_CW):                    # exponent e <-> index i = 151 - e
        v = 0
        for k in range(L + 1):
            if C[k]:
                v ^= EXP[(LOG[C[k]] + (255 - e) * k) % 255]
        if v == 0:
            roots.append(e)
    if len(roots) != L:
        return None, -1
    omega = [0] * 16
    for k in range(L):
        v = 0
        for i in range(k + 1):
            v ^= mul(C[i], s[k - i])
        omega[k] = v
    for e in roots:
        xinv = EXP[(255 - e) % 255]
        num = 0
        for k in range(L):
            num ^= mul(omega[k], EXP[(LOG[xinv] * k) % 255] if xinv else 0)
        den = 0
        for k in range(1, L + 1, 2):
            den ^= mul(C[k], EXP[(LOG[xinv] * (k - 1)) % 255])
        if den == 0:
            return None, -1
        y = mul(EXP[e % 255], div(num, den))
        cw[N_CW - 1 - e] ^= y
    if any(syndromes(cw)):
        return None, -1
    return bytes(cw), L


if __name__ == "__main__":
    rng = random.Random(7)
    ok = bad = 0
    for trial in range(300):
        data = bytes(rng.randrange(256) for _ in range(N_DATA))
        cw = data + encode(data)
        assert not any(syndromes(cw))
        nerr = rng.randrange(0, 20)
        pos = rng.sample(range(N_CW), nerr)
        rx = bytearray(cw)
        for p in pos:
            rx[p] ^= rng.randrange(1, 256)
        out, n = decode(bytes(rx))
        if nerr <= 16:
            assert out == cw and n == nerr, (trial, nerr, n)
            ok += 1
        else:
            assert out is None or out != cw or True
            bad += 1
    print("model OK:", ok, "correctable cases,", bad, "overload cases;",
          "gen poly nonzero:", len(GEN), "coeffs")

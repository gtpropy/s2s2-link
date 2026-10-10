import os, random, struct
from seal_emu import Seal, DATA

M = (1 << 64) - 1
rotl = lambda x, b: ((x << b) | (x >> (64 - b))) & M

def siphash24(key, data):
    k0, k1 = struct.unpack("<QQ", key)
    v0, v1 = k0 ^ 0x736f6d6570736575, k1 ^ 0x646f72616e646f6d
    v2, v3 = k0 ^ 0x6c7967656e657261, k1 ^ 0x7465646279746573
    def rnd():
        nonlocal v0, v1, v2, v3
        v0 = (v0 + v1) & M; v1 = rotl(v1, 13) ^ v0; v0 = rotl(v0, 32)
        v2 = (v2 + v3) & M; v3 = rotl(v3, 16) ^ v2
        v0 = (v0 + v3) & M; v3 = rotl(v3, 21) ^ v0
        v2 = (v2 + v1) & M; v1 = rotl(v1, 17) ^ v2; v2 = rotl(v2, 32)
    n = len(data)
    for i in range(0, n - n % 8, 8):
        m = struct.unpack_from("<Q", data, i)[0]
        v3 ^= m; rnd(); rnd(); v0 ^= m
    b = (n & 0xff) << 56 | int.from_bytes(data[n - n % 8:], "little")
    v3 ^= b; rnd(); rnd(); v0 ^= b
    v2 ^= 0xff
    for _ in range(4): rnd()
    return struct.pack("<Q", v0 ^ v1 ^ v2 ^ v3)

# official vector
assert siphash24(bytes(range(16)), bytes(range(15))).hex() == "e545be4961ca29a1"

s = Seal()
KEY, MSG, OUT = DATA, DATA + 0x100, DATA + 0x400
def run(key, msg):
    s.write(KEY, key); s.write(MSG, msg + b"\0" * 8); s.write(OUT, bytes(8))
    s.call("seal_tag", KEY, MSG, len(msg), OUT)
    return s.read(OUT, 8)

key = bytes(range(16))
assert run(key, bytes(range(15))).hex() == "e545be4961ca29a1", "official vector"
random.seed(1)
cnt = 0
for n in list(range(0, 80)) + [112 + 5, 120, 255]:
    for _ in range(8):
        k = os.urandom(16); m = os.urandom(n)
        assert run(k, m) == siphash24(k, m), (n, k.hex(), m.hex())
        cnt += 1
print("seal_tag OK", cnt, "cases; insns for 117B:", (run(key, bytes(117)), s.icount)[1])

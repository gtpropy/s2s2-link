# @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
# @date   2026-08-30
"""
decoder.py -- pure-Python receive-side decode engine for the S2S2
AX.25/FX.25 link, ported line-for-line from the CPU2 firmware C sources
so it applies the exact same bit-level algorithms:

    core/Src/rs.c          -> rs_decode()          (RS(255,223) BM/Chien/Forney)
    core/Src/fx25.c        -> fx25_tag_matches(), fx25_unpack_data_field(),
                               fx25_decode_frame()  (shortened-RS + HDLC destuff)
    core/Src/ax25.c        -> ax25_addr_decode(), ax25_fcs(), ax25_parse_ui_frame()
    core/Src/cubus_ax25.c  -> cubus_ax25_accept_frame() (Reassembler class below)
    core/Inc/s2s2com.h, core/Inc/s2s2_commands.h -> constants/struct layouts

No third-party dependencies -- stdlib only. Feed it raw FX25_FRAME_LEN
(168)-byte frames recovered off the air (see gmsk_demod.py for that half);
this module only cares about bytes, not RF.

Verified against real firmware-generated frames -- see test_decoder.py.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

# =====================================================================
# Constants (mirrors s2s2com.h / fx25.h / ax25.h / cubus_ax25.h)
# =====================================================================

RS_K = 223
RS_PARITY = 32
RS_N = 255
RS_T = 16

FX25_TAG_LEN = 8
FX25_DATA_LEN = 128
FX25_CHECK_LEN = RS_PARITY
FX25_BLOCK_LEN = FX25_DATA_LEN + FX25_CHECK_LEN          # 160
FX25_FRAME_LEN = FX25_TAG_LEN + FX25_BLOCK_LEN            # 168
FX25_ZERO_PAD = RS_K - FX25_DATA_LEN                      # 95
FX25_TAG06_VALUE = 0xFF94DC634F1CFF4E

AX25_ADDR_LEN = 7
AX25_CALLSIGN_MAX = 6
AX25_CTRL_UI = 0x03
AX25_PID_NO_L3 = 0xF0
AX25_MAX_INFO_LEN = 80
AX25_MAX_FRAME_LEN = AX25_ADDR_LEN * 2 + 1 + 1 + AX25_MAX_INFO_LEN + 2

CUBUS_AX25_FRAG_HDR_LEN = 12
CUBUS_AX25_FRAG_MAX_DATA = AX25_MAX_INFO_LEN - CUBUS_AX25_FRAG_HDR_LEN   # 68
MAX_PAYLOAD_LEN = 659
CUBUS_AX25_MAX_FRAGMENTS = (MAX_PAYLOAD_LEN + CUBUS_AX25_FRAG_MAX_DATA - 1) // CUBUS_AX25_FRAG_MAX_DATA

TYPE_TELEMETRY = 0x0
TYPE_TELECOMMAND = 0x1
TYPE_ACK = 0x2
TYPE_GAP_REPORT = 0x3
TYPE_RAW = 0x4
TYPE_NAMES = {
    TYPE_TELEMETRY: "TELEMETRY", TYPE_TELECOMMAND: "TELECOMMAND",
    TYPE_ACK: "ACK", TYPE_GAP_REPORT: "GAP_REPORT", TYPE_RAW: "RAW",
}

FLAG_ACK_REQUESTED = 0x01
FLAG_LAST_IN_BURST = 0x02
FLAG_PRIORITY = 0x04
FLAG_RETRANSMIT = 0x08

CMD_NAMES = {
    0x00: "CMD_NONE", 0x01: "CMD_REQUEST_BURST", 0x02: "CMD_HEARTBEAT",
    0x03: "CMD_ABORT_MISSION", 0x10: "CMD_RUN_CAMERA", 0x11: "CMD_DOWNLOAD_FILE",
    0x12: "CMD_GET_HEALTH", 0x13: "CMD_GET_HOUSEKEEPING", 0x14: "CMD_GET_BEACON1",
    0x15: "CMD_GET_BEACON2", 0x16: "CMD_GET_ADCS", 0x17: "CMD_DIGIPEATER",
    0x18: "CMD_GET_SAT_LOG", 0x19: "CMD_REQUEST_RETRANSMIT",
}
SUBSYS_NAMES = {0x01: "OBC", 0x02: "COM", 0x03: "ADCS", 0x04: "CAM", 0x05: "EPDM"}
DATA_ID_NAMES = {
    0x01: "HOUSEKEEPING", 0x02: "BEACON1", 0x03: "BEACON2", 0x04: "ADCS",
    0x05: "CAMERA_RGB", 0x06: "CAMERA_NIR", 0x07: "NDWI", 0x08: "EPDM",
    0x09: "DIGIPEATER", 0x0A: "SAT_LOG", 0xFF: "GENERAL",
}
ACK_STATUS_NAMES = {
    0x00: "command accepted", 0x01: "command completed", 0x02: "packet error",
    0x03: "CRC/checksum error", 0x04: "incomplete packet", 0x05: "requested data missing",
    0x06: "requested data unavailable", 0x07: "satellite busy", 0x08: "mission inactive",
    0x09: "invalid command", 0x0A: "memory/file error", 0x0B: "transmission failed",
}

# =====================================================================
# GF(256), primitive polynomial 0x11D -- mirrors rs_init()/rs.c exactly
# =====================================================================

_GF_EXP = [0] * 512
_GF_LOG = [0] * 256


def _rs_init():
    x = 1
    for i in range(255):
        _GF_EXP[i] = x
        _GF_LOG[x] = i
        x <<= 1
        if x & 0x100:
            x ^= 0x11D
    for i in range(255, 512):
        _GF_EXP[i] = _GF_EXP[i - 255]
    _GF_LOG[0] = 0


_rs_init()


def gf_mul(a: int, b: int) -> int:
    if a == 0 or b == 0:
        return 0
    return _GF_EXP[_GF_LOG[a] + _GF_LOG[b]]


def gf_div(a: int, b: int) -> int:
    if a == 0:
        return 0
    d = _GF_LOG[a] - _GF_LOG[b]
    if d < 0:
        d += 255
    return _GF_EXP[d]


def gf_pow(a: int, n: int) -> int:
    if n == 0:
        return 1
    if a == 0:
        return 0
    e = (_GF_LOG[a] * n) % 255
    if e < 0:
        e += 255
    return _GF_EXP[e]


def gf_inverse(a: int) -> int:
    return _GF_EXP[255 - _GF_LOG[a]]


def _gf_poly_mul(a, b):
    out = [0] * (len(a) + len(b) - 1)
    for i, ai in enumerate(a):
        if not ai:
            continue
        for j, bj in enumerate(b):
            if bj:
                out[i + j] ^= gf_mul(ai, bj)
    return out


def _gf_poly_scale(a, scalar):
    return [gf_mul(x, scalar) for x in a]


def _gf_poly_add(a, b):
    lo = max(len(a), len(b))
    out = [0] * lo
    for i, ai in enumerate(a):
        out[i + lo - len(a)] = ai
    for i, bi in enumerate(b):
        out[i + lo - len(b)] ^= bi
    return out


def _gf_poly_eval_hl(poly, x):
    y = poly[0]
    for c in poly[1:]:
        y = gf_mul(y, x) ^ c
    return y


def _gf_poly_eval_lh(poly, x):
    y = poly[-1]
    for c in reversed(poly[:-1]):
        y = gf_mul(y, x) ^ c
    return y


# Generator polynomial, monic, roots alpha^1 .. alpha^RS_PARITY.
_GEN = [1]
for _i in range(RS_PARITY):
    _GEN = _gf_poly_mul(_GEN, [1, gf_pow(2, _i + 1)])


def rs_encode(msg: bytes) -> bytes:
    """Systematic RS(255,223) encode. Direct port of rs_encode() in rs.c.
    SELF-TEST / UTILITY USE ONLY -- the ground station never transmits;
    this exists so the decode paths below (rs_decode, rs_decode_with_erasures)
    can be verified against real codewords without needing a live radio
    link or firmware-generated capture."""
    assert len(msg) == RS_K
    data = bytearray(RS_N)
    data[:RS_K] = msg
    for i in range(RS_K):
        coef = data[i]
        if coef:
            for j in range(1, RS_PARITY + 1):
                if _GEN[j]:
                    data[i + j] ^= gf_mul(_GEN[j], coef)
    codeword = bytearray(RS_N)
    codeword[:RS_K] = msg
    codeword[RS_K:] = data[RS_K:]
    return bytes(codeword)


def _berlekamp_massey(synd_seq):
    """Massey's LFSR synthesis over the given syndrome sequence. Returns
    the (high-to-low, leading-zero-trimmed) locator polynomial. Shared by
    rs_decode (fed the full 32 syndromes) and rs_decode_with_erasures
    (fed a shorter, erasure-adjusted sequence -- see there) -- same
    algorithm either way, only the syndrome source and length differ."""
    err_loc = [1]
    old_loc = [1]
    le, lo = 1, 1

    for i in range(len(synd_seq)):
        delta = synd_seq[i]
        for j in range(1, le):
            idx = i - j
            if idx >= 0:
                delta ^= gf_mul(err_loc[le - 1 - j], synd_seq[idx])

        old_loc.append(0)
        lo += 1

        if delta != 0:
            if lo > le:
                le_before, lo_before = le, lo
                new_loc = _gf_poly_scale(old_loc, delta)
                dinv = gf_inverse(delta)
                new_old = _gf_poly_scale(err_loc, dinv)
                old_loc = new_old
                lo = le_before
                err_loc = new_loc
                le = lo_before
            scaled = _gf_poly_scale(old_loc, delta)
            err_loc = _gf_poly_add(err_loc, scaled)
            le = len(err_loc)

    start = 0
    while start < le - 1 and err_loc[start] == 0:
        start += 1
    err_loc = err_loc[start:]
    return err_loc, len(err_loc) - 1


def _chien_forney(codeword: bytearray, synd, err_loc, errs: int) -> int:
    """Chien search (root-find err_loc over all RS_N field elements) +
    Forney error-value formula, applied in place to `codeword`. `err_loc`
    is high-to-low (as _berlekamp_massey returns, or Gamma*sigma combined
    for the erasures case), `errs` = deg(err_loc). Returns errs corrected,
    or -1 if the locator doesn't factor cleanly (uncorrectable / erasure
    positions were wrong)."""
    if errs < 0 or errs > RS_PARITY:
        return -1

    err_pos = []
    for i in range(RS_N):
        if _gf_poly_eval_hl(err_loc, gf_pow(2, i)) == 0:
            if len(err_pos) > errs:
                return -1
            err_pos.append((RS_N - 1) if i == 0 else i - 1)
    if len(err_pos) != errs:
        return -1

    L = errs + 1
    Lam = [err_loc[L - 1 - k] for k in range(errs + 1)]

    Somega = [0] * RS_PARITY
    for k in range(RS_PARITY):
        acc = 0
        for j in range(errs + 1):
            a = k - j
            if 0 <= a < RS_PARITY:
                acc ^= gf_mul(synd[a], Lam[j])
        Somega[k] = acc

    Lamp = [(Lam[t + 1] if (t + 1) & 1 else 0) for t in range(errs)]

    for p in err_pos:
        m = RS_N - 1 - p
        Xk = gf_pow(2, m)
        Xk_inv = gf_inverse(Xk)
        omega_val = _gf_poly_eval_lh(Somega, Xk_inv)
        lamp_val = _gf_poly_eval_lh(Lamp, Xk_inv)
        if lamp_val == 0:
            return -1
        mag = gf_div(omega_val, lamp_val)
        codeword[p] ^= mag

    for i in range(RS_PARITY):
        if _gf_poly_eval_hl(codeword, gf_pow(2, i + 1)) != 0:
            return -1

    return errs


def rs_decode(codeword: bytearray) -> int:
    """In-place RS(255,223) decode. Returns symbols corrected (0..16), or
    -1 if uncorrectable. Direct port of rs_decode() in rs.c."""
    synd = [0] * RS_PARITY
    has_error = False
    for i in range(RS_PARITY):
        synd[i] = _gf_poly_eval_hl(codeword, gf_pow(2, i + 1))
        if synd[i]:
            has_error = True
    if not has_error:
        return 0

    err_loc, errs = _berlekamp_massey(synd)
    if errs < 1 or errs > RS_T:
        return -1
    return _chien_forney(codeword, synd, err_loc, errs)


def rs_decode_with_erasures(codeword: bytearray, erasure_positions) -> int:
    """Errors-and-erasures RS(255,223) decode: like rs_decode(), but the
    caller supplies byte positions (0..254 into `codeword`) it already
    knows are unreliable (e.g. low receiver-confidence bytes from the
    demod, see gmsk_demod.confidence_erasure_positions()). Every marked
    position gets its value recovered rather than merely checked, which
    trades decode capacity: each erasure costs half what an unknown error
    costs (2*errors + erasures <= 32, vs. errors <= 16 alone), so this
    can correct up to 32 known-bad bytes, or fewer erasures plus some
    number of genuinely unknown errors on top.

    Ground-truth relationship this implements (standard errors-and-
    erasures RS decoding, e.g. Wicker & Bhargava ch.5): with erasure
    locator Gamma(x) = prod (1 - X_j x) over the e known-bad positions
    and S(x) the syndrome polynomial, the v additional (unknown-position)
    errors satisfy the same key equation as the error-only case but
    against U(x) = Gamma(x)*S(x) mod x^32 with its lowest e coefficients
    dropped -- i.e. _berlekamp_massey() on a shorter syndrome sequence.
    The combined locator Lambda = Gamma*sigma and the ORIGINAL syndromes
    then go through the exact same Chien+Forney step rs_decode() uses,
    since that step doesn't care how the locator was derived.

    Behaves identically to rs_decode() when erasure_positions is empty
    (Gamma(x) = 1) -- verified in test_decoder.py.

    Returns total positions corrected (errors + erasures), or -1 if
    uncorrectable / an erasure position was actually fine (this can
    legitimately happen if the confidence heuristic was wrong -- it's
    just a hint, not a guarantee)."""
    erasure_positions = sorted(set(erasure_positions))
    e = len(erasure_positions)
    if e == 0:
        return rs_decode(codeword)
    if e > RS_PARITY or any(not (0 <= p < RS_N) for p in erasure_positions):
        return -1

    synd = [0] * RS_PARITY
    has_error = False
    for i in range(RS_PARITY):
        synd[i] = _gf_poly_eval_hl(codeword, gf_pow(2, i + 1))
        if synd[i]:
            has_error = True
    if not has_error:
        return 0

    # Gamma(x), low-to-high (index = power of x, Gamma[0] = 1 = constant
    # term) so it can be convolved directly against the syndrome
    # polynomial S(x) (synd[] is already low-to-high: synd[i] = S_{i+1}).
    gamma_lh = [1]
    for p in erasure_positions:
        location = gf_pow(2, RS_N - 1 - p)  # X_j = alpha^(x-power at position p)
        gamma_lh = _gf_poly_mul(gamma_lh, [1, location])

    if e > RS_PARITY:
        return -1
    u = _gf_poly_mul(gamma_lh, synd)[:RS_PARITY]  # Gamma(x)*S(x) mod x^32
    modsynd = u[e:]  # drop the e low-order coefficients (Forney-syndrome shift)

    sigma_hl, extra_errs = _berlekamp_massey(modsynd)
    if 2 * extra_errs + e > RS_PARITY:
        return -1

    gamma_hl = gamma_lh[::-1]  # same polynomial, high-to-low (poly[0] = leading term)
    combined = _gf_poly_mul(gamma_hl, sigma_hl)
    total_errs = e + extra_errs

    return _chien_forney(codeword, synd, combined, total_errs)


# =====================================================================
# FX.25 (fx25.c port)
# =====================================================================

def fx25_tag_matches(tag8: bytes) -> bool:
    v = FX25_TAG06_VALUE
    expect = bytes((v >> (i * 8)) & 0xFF for i in range(FX25_TAG_LEN))
    return bytes(tag8[:FX25_TAG_LEN]) == expect


def _get_bit(buf, bitpos):
    return (buf[bitpos >> 3] >> (bitpos & 7)) & 1


def _reverse8(v: int) -> int:
    v = ((v & 0xF0) >> 4) | ((v & 0x0F) << 4)
    v = ((v & 0xCC) >> 2) | ((v & 0x33) << 2)
    v = ((v & 0xAA) >> 1) | ((v & 0x55) << 1)
    return v


def g3ruh_scramble_frame(frame: bytes) -> bytes:
    """Python port of CPU2/core/Src/g3ruh.c's g3ruh_scramble_frame --
    bit-exact with Direwolf's gen_tone.c scrambler (self-synchronizing,
    17-bit LFSR, taps at bit 16/11) plus NRZI encode, then a per-byte
    bit-reversal to compensate the SX126x packet engine's native
    MSB-first shift-out (see g3ruh.h). Used to build synthetic on-air
    test signals (test_demod.py) matching what the real firmware now
    transmits -- g3ruh_descramble_frame is the RX-side inverse, wired
    into fx25_decode_frame below since real hardware always scrambles."""
    n = len(frame)
    lfsr = 0
    nrzi_prev = 1
    scratch = bytearray(n)
    for bitpos in range(n * 8):
        byte_idx, bit_idx = divmod(bitpos, 8)
        data_bit = (frame[byte_idx] >> bit_idx) & 1
        nrzi_bit = nrzi_prev if data_bit else (1 - nrzi_prev)
        nrzi_prev = nrzi_bit
        x = (nrzi_bit ^ (lfsr >> 16) ^ (lfsr >> 11)) & 1
        lfsr = ((lfsr << 1) | x) & 0xFFFFFFFF
        if x:
            scratch[byte_idx] |= 1 << bit_idx
    return bytes(_reverse8(b) for b in scratch)


def g3ruh_descramble_frame(frame: bytes) -> bytes:
    """Exact inverse of g3ruh_scramble_frame -- see there."""
    n = len(frame)
    scratch = bytes(_reverse8(b) for b in frame)
    lfsr = 0
    prev_descram = 1
    out = bytearray(n)
    for bitpos in range(n * 8):
        byte_idx, bit_idx = divmod(bitpos, 8)
        raw = (scratch[byte_idx] >> bit_idx) & 1
        descram = (raw ^ (lfsr >> 16) ^ (lfsr >> 11)) & 1
        lfsr = ((lfsr << 1) | raw) & 0xFFFFFFFF
        dbit = 1 if descram == prev_descram else 0
        prev_descram = descram
        if dbit:
            out[byte_idx] |= 1 << bit_idx
    return bytes(out)


def fx25_unpack_data_field(data: bytes):
    """Destuff a FX25_DATA_LEN-byte data field back to AX.25 frame bytes
    (with FCS, no flags). Returns bytes, or None if no valid flag pair."""
    if len(data) != FX25_DATA_LEN or data[0] != 0x7E:
        return None

    scratch = bytearray(FX25_DATA_LEN)
    total_bits = FX25_DATA_LEN * 8
    i = 8  # skip raw leading flag byte
    ones = 0
    out_bits = 0

    while i < total_bits:
        bit = _get_bit(data, i)
        i += 1

        if ones == 5:
            ones = 0
            if bit == 0:
                continue
            if i >= total_bits:
                return None
            bit2 = _get_bit(data, i)
            i += 1
            if bit2 == 0:
                out_bits -= 6
                if out_bits < 0:
                    return None
                frame_len = (out_bits + 7) // 8
                return bytes(scratch[:frame_len])
            return None

        if bit:
            scratch[out_bits >> 3] |= 1 << (out_bits & 7)
        out_bits += 1
        ones = ones + 1 if bit else 0

    return None


def _build_rs_codeword(frame168: bytes) -> bytearray:
    codeword = bytearray(FX25_ZERO_PAD)
    codeword += frame168[FX25_TAG_LEN:FX25_TAG_LEN + FX25_DATA_LEN]
    codeword += frame168[FX25_TAG_LEN + FX25_DATA_LEN:FX25_TAG_LEN + FX25_DATA_LEN + FX25_CHECK_LEN]
    assert len(codeword) == RS_N
    return codeword


def erasure_positions_from_confidence(confidence, max_erasures: int = 24):
    """Maps a per-frame168-byte receiver-confidence array (see
    gmsk_demod.extract_candidate_frames' `confidence` field -- higher =
    more reliable, e.g. discriminator-sample magnitude at the bit-slice
    instant) to RS codeword positions for rs_decode_with_erasures().

    Only frame168 bytes [FX25_TAG_LEN:FX25_FRAME_LEN) are RS codeword
    bytes (the first 8 are the hard-matched FX.25 tag, already verified
    exactly, not part of the RS block); the FX25_ZERO_PAD region is
    virtual (never transmitted, always 0) and never marked. Picks the
    `max_erasures` least-confident bytes -- capped well under RS_PARITY
    so there's headroom left for genuinely unknown errors on top (see
    fx25_decode_frame's fallback for what happens if this cap, or the
    confidence estimate itself, was wrong)."""
    if confidence is None or len(confidence) < FX25_FRAME_LEN:
        return []
    scored = sorted(
        range(FX25_TAG_LEN, FX25_FRAME_LEN),
        key=lambda idx: confidence[idx],
    )
    picked = scored[:max_erasures]
    return sorted((idx - FX25_TAG_LEN) + FX25_ZERO_PAD for idx in picked)


def fx25_decode_frame(frame168: bytes, confidence=None, max_erasures: int = 24):
    """Returns (ax25_frame: bytes, rs_bytes_corrected: int) or None.

    `confidence`, if given, is the per-byte receiver-confidence array
    from gmsk_demod.extract_candidate_frames (see there) -- the least
    confident bytes get marked as RS erasures (erasure_positions_from_
    confidence) and decoded via rs_decode_with_erasures for a shot at
    correcting more than RS_T=16 bad bytes. If that fails (the
    confidence heuristic guessed wrong, or there simply weren't that
    many actual errors), falls back to plain rs_decode on a fresh
    codeword copy -- erasure hints can only help or be a no-op, they
    can never make a frame that plain decode would've recovered fail,
    since rs_decode_with_erasures(codeword, []) is byte-identical to
    rs_decode(codeword) and this always tries both."""
    if len(frame168) != FX25_FRAME_LEN:
        return None
    frame168 = g3ruh_descramble_frame(frame168)
    if not fx25_tag_matches(frame168[:FX25_TAG_LEN]):
        return None

    erasures = erasure_positions_from_confidence(confidence, max_erasures) if confidence else []

    if erasures:
        codeword = _build_rs_codeword(frame168)
        corrected = rs_decode_with_erasures(codeword, erasures)
        if corrected < 0:
            codeword = _build_rs_codeword(frame168)  # erasure attempt may have partially mutated it
            corrected = rs_decode(codeword)
    else:
        codeword = _build_rs_codeword(frame168)
        corrected = rs_decode(codeword)

    if corrected < 0:
        return None

    ax25_frame = fx25_unpack_data_field(bytes(codeword[FX25_ZERO_PAD:FX25_ZERO_PAD + FX25_DATA_LEN]))
    if ax25_frame is None:
        return None
    return ax25_frame, corrected


# =====================================================================
# AX.25 (ax25.c port)
# =====================================================================

def ax25_addr_decode(addr7: bytes):
    callsign = "".join(chr(b >> 1) for b in addr7[:6]).rstrip(" ")
    ssid = (addr7[6] >> 1) & 0x0F
    is_last = addr7[6] & 0x01
    command_bit = (addr7[6] >> 7) & 0x01
    return callsign, ssid, bool(is_last), bool(command_bit)


def ax25_fcs(data: bytes) -> int:
    fcs = 0xFFFF
    for byte in data:
        fcs ^= byte
        for _ in range(8):
            fcs = (fcs >> 1) ^ 0x8408 if (fcs & 1) else (fcs >> 1)
    return fcs ^ 0xFFFF


def ax25_parse_ui_frame(frame: bytes):
    min_len = AX25_ADDR_LEN * 2 + 1 + 1 + 2
    if len(frame) < min_len:
        return None

    dest_cs, dest_ssid, _, _ = ax25_addr_decode(frame[0:7])
    src_cs, src_ssid, src_is_last, _ = ax25_addr_decode(frame[7:14])
    if not src_is_last:
        return None

    pos = 14
    if frame[pos] != AX25_CTRL_UI:
        return None
    pos += 1
    if frame[pos] != AX25_PID_NO_L3:
        return None
    pos += 1

    info_len = len(frame) - pos - 2
    if info_len < 0 or info_len > AX25_MAX_INFO_LEN:
        return None

    computed = ax25_fcs(frame[:pos + info_len])
    received = frame[pos + info_len] | (frame[pos + info_len + 1] << 8)
    if computed != received:
        return None

    return {
        "dest": (dest_cs, dest_ssid), "src": (src_cs, src_ssid),
        "info": bytes(frame[pos:pos + info_len]),
    }


# =====================================================================
# S2S2 fragment reassembly (cubus_ax25.c port)
# =====================================================================

class Reassembler:
    """Mirrors cubus_ax25_reassembler_t / cubus_ax25_accept_frame(). One
    in-flight message at a time, same as the firmware (single half-duplex
    link) -- a new (seq_num, session_id) resets state, matching real
    on-the-wire behavior exactly."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.in_progress = False
        self.seq_num = 0
        self.session_id = 0
        self.version = 0
        self.type = 0
        self.src_id = 0
        self.dst_id = 0
        self.flags = 0
        self.frag_total = 0
        self.last_frag_len = 0
        self.received_mask = 0
        self.payload = bytearray(MAX_PAYLOAD_LEN)
        self.rs_bytes_corrected = 0

    def accept_frame(self, frame168: bytes, confidence=None):
        """Returns ('complete', pkt_dict) | ('partial', info_dict) |
        ('rejected', reason_str). `confidence`: optional per-byte
        receiver-confidence array (see gmsk_demod.extract_candidate_
        frames' `confidence` field) -- passed straight through to
        fx25_decode_frame for erasure-assisted RS decode."""
        result = fx25_decode_frame(frame168, confidence)
        if result is None:
            return ("rejected", "fx25/rs decode failed")
        ax25_frame, rs_corrected = result

        parsed = ax25_parse_ui_frame(ax25_frame)
        if parsed is None:
            return ("rejected", "ax25 parse/FCS failed")
        info = parsed["info"]

        if len(info) < CUBUS_AX25_FRAG_HDR_LEN:
            return ("rejected", "fragment header too short")

        version, ptype, src_id, dst_id = info[0], info[1], info[2], info[3]
        seq_num = (info[4] << 8) | info[5]
        session_id = (info[6] << 8) | info[7]
        flags = info[8]
        frag_index = info[9]
        frag_total = info[10]
        frag_len = info[11]

        if frag_total == 0 or frag_total > CUBUS_AX25_MAX_FRAGMENTS:
            return ("rejected", "bad frag_total")
        if frag_index >= frag_total:
            return ("rejected", "bad frag_index")
        if len(info) != CUBUS_AX25_FRAG_HDR_LEN + frag_len:
            return ("rejected", "frag_len mismatch")

        is_new = (not self.in_progress) or self.seq_num != seq_num or self.session_id != session_id
        if is_new:
            self.reset()
            self.in_progress = True
            self.version, self.type = version, ptype
            self.src_id, self.dst_id = src_id, dst_id
            self.seq_num, self.session_id = seq_num, session_id
            self.flags, self.frag_total = flags, frag_total
        elif self.frag_total != frag_total:
            return ("rejected", "inconsistent frag_total mid-message")

        offset = frag_index * CUBUS_AX25_FRAG_MAX_DATA
        if offset + frag_len > MAX_PAYLOAD_LEN:
            return ("rejected", "payload overflow")
        self.payload[offset:offset + frag_len] = info[CUBUS_AX25_FRAG_HDR_LEN:CUBUS_AX25_FRAG_HDR_LEN + frag_len]
        self.received_mask |= 1 << frag_index
        self.rs_bytes_corrected += rs_corrected
        if frag_index == frag_total - 1:
            self.last_frag_len = frag_len

        complete_mask = (1 << frag_total) - 1
        if (self.received_mask & complete_mask) != complete_mask:
            return ("partial", {
                "seq_num": seq_num, "session_id": session_id,
                "frag_index": frag_index, "frag_total": frag_total,
                "rs_bytes_corrected": rs_corrected,
            })

        payload_len = (frag_total - 1) * CUBUS_AX25_FRAG_MAX_DATA + self.last_frag_len
        pkt = {
            "version": self.version, "type": self.type,
            "src_id": self.src_id, "dst_id": self.dst_id,
            "seq_num": self.seq_num, "session_id": self.session_id,
            "flags": self.flags,
            "payload": bytes(self.payload[:payload_len]),
            "payload_len": payload_len,
            "rs_bytes_corrected": self.rs_bytes_corrected,
            "src_addr": parsed["src"], "dest_addr": parsed["dest"],
        }
        return ("complete", pkt)


# =====================================================================
# S2S2 command/telemetry pretty-print (s2s2_commands.c port)
# =====================================================================

def _u16(b, o):
    return (b[o] << 8) | b[o + 1]


def _u32(b, o):
    return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]


def unpack_command(payload: bytes):
    if len(payload) < 9:
        return None
    return {
        "opcode": payload[0], "opcode_name": CMD_NAMES.get(payload[0], hex(payload[0])),
        "subsystem": payload[1], "subsystem_name": SUBSYS_NAMES.get(payload[1], "-"),
        "data_id": payload[2], "data_id_name": DATA_ID_NAMES.get(payload[2], "-"),
        "address": _u32(payload, 3), "num_packets": _u16(payload, 7),
    }


def unpack_ack(payload: bytes):
    if len(payload) < 4:
        return None
    return {
        "status": payload[0], "status_name": ACK_STATUS_NAMES.get(payload[0], "unknown status"),
        "data_id": payload[1], "data_id_name": DATA_ID_NAMES.get(payload[1], "-"),
        "acked_seq": _u16(payload, 2),
    }


def unpack_beacon1(b: bytes):
    if len(b) < 30:
        return None
    p = [0]

    def u8():
        v = b[p[0]]; p[0] += 1; return v

    def u16():
        v = _u16(b, p[0]); p[0] += 2; return v

    return {
        "type": u8(), "tim_day": u16(), "tim_hour": u8(),
        "bat_v": u16(), "bat_i": u16(), "bat_temp": u8(),
        "raw_v": u8(), "raw_i": u8(), "sol_tot_v": u16(), "sol_tot_i": u8(),
        "unreg_i": u8(), "main_3v3_i": u8(), "msn_3v3_i": u8(), "v5_i": u8(), "adc_3v32_i": u8(),
        "temp1": u8(), "temp2": u8(), "temp3": u8(), "temp4": u8(), "temp5": u8(),
        "bpb_t": u8(), "ant_p_t": u8(), "obc_com_t": u8(),
        "system_stat": u8(), "mcu_stat": u8(), "adc_stat": u8(),
    }


def unpack_beacon2(b: bytes):
    if len(b) < 25:
        return None
    p = [0]

    def u8():
        v = b[p[0]]; p[0] += 1; return v

    def u16():
        v = _u16(b, p[0]); p[0] += 2; return v

    return {
        "tim_day": u16(),
        "sol_p1_v": u8(), "sol_p2_v": u8(), "sol_p3_v": u8(), "sol_p4_v": u8(), "sol_p5_v": u8(),
        "sol_p1_i": u8(), "sol_p2_i": u8(), "sol_p3_i": u8(), "sol_p4_i": u8(), "sol_p5_i": u8(),
        "gyro_x": u16(), "gyro_y": u16(), "gyro_z": u16(),
        "mag_x": u16(), "mag_y": u16(), "mag_z": u16(),
        "power_stat": u8(),
    }


def unpack_camera_fragment(b: bytes):
    if len(b) < 7:
        return None
    return {
        "image_id": b[0], "fragment_index": _u16(b, 1), "total_fragments": _u16(b, 3),
        "image_type": b[5], "ai_score": b[6], "data_len": len(b) - 7, "data": b[7:],
    }


def decode_payload(pkt: dict) -> dict:
    """Given a complete Reassembler packet dict, return a type-specific
    breakdown under 'decoded'."""
    ptype = pkt["type"]
    payload = pkt["payload"]
    decoded = {"type_name": TYPE_NAMES.get(ptype, hex(ptype))}

    if ptype == TYPE_TELECOMMAND:
        decoded["command"] = unpack_command(payload)
    elif ptype == TYPE_ACK:
        decoded["ack"] = unpack_ack(payload)
    elif ptype == TYPE_TELEMETRY and len(payload) >= 1 and payload[0] in DATA_ID_NAMES:
        # Only treat byte 0 as a data_id if it's actually a recognized one --
        # some firmware TELEMETRY messages (e.g. Send_Beacon_If_Due's plain
        # "NEPSAT BEACON" text) don't follow the [data_id][struct] layout at
        # all, and blindly eating byte 0 as a data_id would silently drop
        # the first real payload byte (e.g. showing "EPSAT BEACON").
        data_id = payload[0]
        body = payload[1:]
        decoded["data_id"] = data_id
        decoded["data_id_name"] = DATA_ID_NAMES[data_id]
        if data_id == 0x02:
            decoded["beacon1"] = unpack_beacon1(body)
        elif data_id == 0x03:
            decoded["beacon2"] = unpack_beacon2(body)
        elif data_id in (0x05, 0x06, 0x07):
            decoded["camera_fragment"] = unpack_camera_fragment(body)
        else:
            decoded["raw_hex"] = body.hex()
    else:
        decoded["raw_hex"] = payload.hex()
        decoded["ascii"] = payload.decode("ascii", errors="replace")

    return decoded

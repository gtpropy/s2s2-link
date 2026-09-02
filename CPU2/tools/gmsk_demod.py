# @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
# @date   2026-08-30
"""
gmsk_demod.py -- GMSK demodulator + frame sync for the S2S2 downlink,
matching the STM32WL55 CPU2 firmware's exact PHY config (see
core/Src/radio_app.c / core/Inc/config.h):

    Modulation      : GMSK, h=0.5 (SX126x MODEM_FSK w/ Gaussian shaping)
    Bit rate        : 9600 bps
    Frequency dev   : 2400 Hz  (RADIO_FDEV_HZ)
    Gaussian BT     : 0.3      (MOD_SHAPING_G_BT_03)
    RX bandwidth    : 19500 Hz
    Sync word       : 0x1A 0xCF 0xFC 0x1D (4 bytes, hardware-correlated)
    Framing         : fixed length, 168 bytes/frame (FX25_FRAME_LEN),
                      CRC off, whitening off (see fx25.h / radio_app.c)

Two things about the on-air bit convention this receiver CANNOT know
without a real radio to check against: which frequency-deviation sign
means bit 0 vs bit 1 ("polarity"), and whether each payload byte is
serialized MSB-first or LSB-first ("bit order"). Rather than guess,
extract_candidate_frames() tries every combination (2 polarities x 2
bit orders x every sample-timing phase) and lets the caller keep only
the candidates whose FX.25 correlation tag actually matches -- the tag
is a hard-coded 8-byte constant, so it's a perfect self-check. See
test_demod.py for an end-to-end synthetic verification of this whole
pipeline (there's no live hardware in this environment to test against
real RF).

Depends on numpy + scipy only.
"""
from __future__ import annotations

from fractions import Fraction

import numpy as np
from scipy.signal import firwin, lfilter, resample_poly

from s2s2_decoder import FX25_FRAME_LEN

BAUD = 9600
FDEV_HZ = 2400
RX_BANDWIDTH_HZ = 19500
SYNC_WORD = bytes([0x1A, 0xCF, 0xFC, 0x1D])
SPS = 25  # samples/symbol used internally, after resampling


# =====================================================================
# IQ sources
# =====================================================================

def read_iq_file_u8(path: str) -> np.ndarray:
    """rtl_sdr's raw capture format: interleaved unsigned 8-bit I/Q,
    centered at 127.5 (matches `rtl_sdr -f <hz> -s <rate> capture.iq`)."""
    raw = np.fromfile(path, dtype=np.uint8).astype(np.float32)
    raw = (raw - 127.5) / 127.5
    return (raw[0::2] + 1j * raw[1::2]).astype(np.complex64)


def read_iq_file_cf32(path: str) -> np.ndarray:
    """Interleaved 32-bit float I/Q (GNU Radio's default .cfile format)."""
    raw = np.fromfile(path, dtype=np.float32)
    return (raw[0::2] + 1j * raw[1::2]).astype(np.complex64)


class RtlSdrSource:
    """Thin wrapper around pyrtlsdr. Import of `rtlsdr` is deferred so
    this module (and the decoder engine) works without the package or
    the hardware installed -- only actually opening a device needs it."""

    def __init__(self, center_freq_hz: float, sample_rate_hz: float, gain="auto"):
        import rtlsdr  # noqa: local import -- optional dependency
        self.dev = rtlsdr.RtlSdr()
        self.dev.center_freq = center_freq_hz
        self.dev.sample_rate = sample_rate_hz
        self.dev.gain = gain

    def read(self, num_samples: int) -> np.ndarray:
        # librtlsdr's synchronous read requires the byte count (2 bytes/
        # sample, 8-bit I/Q) to be a multiple of 512, or it raises
        # LIBUSB_ERROR_OVERFLOW -- round up to the nearest multiple of 256
        # samples so that always holds, regardless of what the caller asked for.
        aligned = ((num_samples + 255) // 256) * 256
        return self.dev.read_samples(aligned).astype(np.complex64)

    def set_gain(self, gain):
        """Live gain change (dB, or 'auto') -- takes effect on the next
        read(), no need to reopen the device."""
        self.dev.gain = gain

    def set_freq_correction_ppm(self, ppm: int):
        """Live crystal frequency-correction change, parts per million."""
        self.dev.freq_correction = int(ppm)

    def set_center_freq(self, freq_hz: float):
        self.dev.center_freq = freq_hz

    def close(self):
        self.dev.close()


# =====================================================================
# Demod chain
# =====================================================================

def fm_demod(iq: np.ndarray, sample_rate: float) -> np.ndarray:
    """Instantaneous-frequency discriminator, in Hz. GMSK/MSK is constant
    envelope, so a plain quadrature discriminator recovers the symbol
    stream directly -- no AM/limiter stage needed."""
    prod = iq[1:] * np.conj(iq[:-1])
    phase_diff = np.angle(prod)
    return phase_diff * (sample_rate / (2.0 * np.pi))


def dc_block(x: np.ndarray, pole: float = 0.999) -> np.ndarray:
    """Classic single-pole DC-blocking IIR filter: y[n] = x[n] - x[n-1] +
    pole*y[n-1]. Removes the frequency-offset bias an RTL-SDR's crystal
    error puts on the discriminator output (its ppm error alone can be
    tens of kHz at UHF -- far more than this link's +-2400 Hz deviation,
    which without this would push both bit values to the same sign and
    permanently defeat the sign-based bit slicer below)."""
    return lfilter([1.0, -1.0], [1.0, -pole], x)


def lowpass_filter(x: np.ndarray, sample_rate: float, cutoff_hz: float, numtaps: int = 101) -> np.ndarray:
    cutoff_hz = min(cutoff_hz, sample_rate / 2.0 * 0.98)
    taps = firwin(numtaps, cutoff_hz, fs=sample_rate)
    return lfilter(taps, 1.0, x)


def resample_to_sps(x: np.ndarray, in_rate: float, baud: float = BAUD, sps: int = SPS) -> np.ndarray:
    """Polyphase-resample the discriminator output so there are exactly
    `sps` samples per symbol, regardless of the RTL-SDR's actual capture
    rate."""
    target_rate = baud * sps
    frac = Fraction(target_rate).limit_denominator(1000) / Fraction(in_rate).limit_denominator(1000)
    frac = frac.limit_denominator(1000)
    return resample_poly(x, frac.numerator, frac.denominator)


def bits_of_byte(byte: int, order: str):
    if order == "msb":
        return [(byte >> i) & 1 for i in range(7, -1, -1)]
    return [(byte >> i) & 1 for i in range(8)]


def _sync_bits(order: str) -> np.ndarray:
    bits = []
    for b in SYNC_WORD:
        bits.extend(bits_of_byte(b, order))
    return np.array(bits, dtype=np.int8)


_SYNC_BITS = {"msb": _sync_bits("msb"), "lsb": _sync_bits("lsb")}


def pack_bits(bits: np.ndarray, order: str) -> bytes:
    assert len(bits) % 8 == 0
    out = bytearray(len(bits) // 8)
    for i in range(0, len(bits), 8):
        chunk = bits[i:i + 8]
        v = 0
        if order == "msb":
            for b in chunk:
                v = (v << 1) | int(b)
        else:
            for j, b in enumerate(chunk):
                v |= int(b) << j
        out[i // 8] = v
    return bytes(out)


def find_sync_positions(bits: np.ndarray, order: str, max_errors: int = 2):
    """Bit-index positions where SYNC_WORD starts, allowing up to
    max_errors bit errors (channel noise on the sync word itself)."""
    pat = _SYNC_BITS[order]
    if len(bits) < len(pat):
        return np.array([], dtype=np.int64)
    s = bits.astype(np.int8) * 2 - 1
    p = pat.astype(np.int8) * 2 - 1
    windows = np.lib.stride_tricks.sliding_window_view(s, len(p))
    scores = windows @ p
    threshold = len(p) - 2 * max_errors
    return np.where(scores >= threshold)[0]


def extract_candidate_frames(iq: np.ndarray, sample_rate: float, baud: float = BAUD,
                              sps: int = SPS, max_sync_errors: int = 2,
                              phases=None, orders=None, inverts=None, denoise_fn=None):
    """Runs the full demod chain and returns every raw FX25_FRAME_LEN
    (168)-byte candidate frame found, across the requested (sample-phase,
    polarity, bit-order) combinations -- most will be garbage; the caller
    (s2s2_decoder.fx25_decode_frame / Reassembler.accept_frame) is the
    real filter, since a correct decode is only possible once the right
    combination has been hit. Each result dict also reports which
    combination produced it.

    By default (phases/orders/inverts all None) this brute-forces the
    full search space (sps phases x 2 polarities x 2 bit-orders = 100
    combinations at the default sps=25) -- needed for initial acquisition
    when nothing is known about the link yet. Once a caller has seen a
    real decode succeed at a given combination, passing that combination
    back in via `phases=[locked_phase]` (or a small window around it),
    `orders=[locked_order]`, `inverts=[locked_invert]` cuts the search to
    a handful of candidates instead of 100 -- much cheaper, and it stops
    flooding a log with rejections from combinations that were never
    going to be right in the first place.

    Each result also carries `confidence`: a 168-entry list, one per
    frame byte, of how far that byte's least-certain bit's symbol sample
    sat from the slicer's zero threshold (min |decim| over its 8 bits --
    a byte is only as trustworthy as its worst bit). Not calibrated to
    any physical unit, just a relative reliability score for feeding
    s2s2_decoder.erasure_positions_from_confidence() / rs_decode_with_
    erasures() ahead of the usual rs_decode() -- see fx25_decode_frame.

    `denoise_fn`, if given, is applied to the discriminator output right
    after the existing lowpass filter, before symbol-rate resampling --
    e.g. `lambda x: ml_denoise.denoise(model, x)` (see ml_denoise.py).
    Not imported here: this module stays numpy+scipy only, ml_denoise is
    an optional add-on the caller wires in.
    """
    phases = range(sps) if phases is None else phases
    orders = ("msb", "lsb") if orders is None else orders
    inverts = (False, True) if inverts is None else inverts

    freq = fm_demod(iq, sample_rate)
    freq = dc_block(freq)
    freq = lowpass_filter(freq, sample_rate, cutoff_hz=baud * 0.9)
    if denoise_fn is not None:
        freq = denoise_fn(freq)
    sym = resample_to_sps(freq, sample_rate, baud, sps)

    frame_bits = FX25_FRAME_LEN * 8
    results = []
    for phase in phases:
        decim = sym[phase::sps]
        mag = np.abs(decim)
        for invert in inverts:
            bits = (decim < 0).astype(np.uint8) if invert else (decim > 0).astype(np.uint8)
            for order in orders:
                for start in find_sync_positions(bits, order, max_sync_errors):
                    fstart = int(start) + 32
                    fend = fstart + frame_bits
                    if fend > len(bits):
                        continue
                    frame = pack_bits(bits[fstart:fend], order)
                    confidence = mag[fstart:fend].reshape(-1, 8).min(axis=1).tolist()
                    results.append({
                        "phase": phase, "invert": invert, "order": order,
                        "bit_pos": int(start), "frame": frame,
                        "confidence": confidence,
                    })
    return results


def signal_power_dbfs(iq: np.ndarray) -> float:
    """Mean signal power relative to full-scale, in dB -- a rough,
    RSSI-like readout for tuning antenna placement/gain. Not calibrated
    to any absolute dBm reference, just useful as a relative indicator."""
    power = float(np.mean(np.abs(iq) ** 2))
    if power <= 0:
        return float("-inf")
    return 10.0 * np.log10(power)


_WINDOW_FUNCS = {
    "hann": np.hanning,
    "hamming": np.hamming,
    "blackman": np.blackman,
    "rect": lambda n: np.ones(n),
}


def spectrum_db(iq: np.ndarray, nfft: int = 4096, window: str = "hann") -> np.ndarray:
    """Windowed FFT magnitude of the most recent `nfft` samples, in dB,
    DC-centered (index 0 = -sample_rate/2, index -1 = +sample_rate/2 minus
    one bin). Cheap enough to call once per GUI chunk for a live spectrum
    analyzer / waterfall -- a single representative snapshot per chunk is
    plenty for a real-time display, no need to FFT every sample.
    Zero-padded (not tiled) if `iq` is shorter than `nfft`.

    window: "hann" (default, good general-purpose sidelobe suppression),
    "hamming" (narrower main lobe, higher sidelobes -- better for
    resolving two close signals), "blackman" (opposite tradeoff, best
    sidelobe suppression, widest main lobe), or "rect" (no window at all
    -- maximum frequency resolution, worst spectral leakage)."""
    n = min(len(iq), nfft)
    seg = iq[-n:] if n > 0 else iq
    win_fn = _WINDOW_FUNCS.get(window, np.hanning)
    windowed = seg * win_fn(n) if n > 0 else seg
    spec = np.fft.fftshift(np.fft.fft(windowed, n=nfft))
    return 20.0 * np.log10(np.abs(spec) + 1e-12)


# =====================================================================
# Synthetic GMSK modulator -- SELF-TEST USE ONLY. Not used for TX; the
# firmware's SX126x hardware does real on-air modulation. This exists so
# the demod chain above can be verified end-to-end without real RF
# hardware (see test_demod.py).
# =====================================================================

def _gaussian_taps(bt: float, sps: int, span_symbols: int = 4) -> np.ndarray:
    t = np.arange(-span_symbols * sps, span_symbols * sps + 1) / sps
    alpha = np.sqrt(np.log(2)) / (2 * np.pi * bt)
    g = np.exp(-(t ** 2) / (2 * alpha ** 2))
    return (g / np.sum(g)).astype(np.float64)


def gmsk_modulate(frame_bytes: bytes, sample_rate: float, baud: float = BAUD,
                   fdev: float = FDEV_HZ, bt: float = 0.3, order: str = "msb",
                   invert: bool = False, preamble_bits: int = 128) -> np.ndarray:
    """Synthesizes a baseband complex-exponential GMSK signal for one
    frame, preceded by an alternating-bit preamble, for self-test only."""
    sps_exact = sample_rate / baud
    sps = max(4, int(round(sps_exact)))
    bits = [1, 0] * (preamble_bits // 2)
    for byte in SYNC_WORD:
        bits.extend(bits_of_byte(byte, order))
    for byte in frame_bytes:
        bits.extend(bits_of_byte(byte, order))
    bits.extend([1, 0] * 32)  # trailing pad -- real captures always have samples
    bits = np.array(bits, dtype=np.float64)  # after the frame too; keeps filter group delay in-bounds
    if invert:
        bits = 1.0 - bits
    nrz = bits * 2.0 - 1.0

    # Exact-baud sample boundaries (cumulative rounding against the real
    # sample_rate/baud ratio, not a rounded integer sps) -- otherwise a
    # sample_rate that isn't an exact multiple of `baud` (e.g. a
    # 250 ksps RTL-SDR capture at 9600 baud, ratio 26.04) quantizes to
    # the nearest integer sps and silently emulates the wrong baud rate,
    # bit-slipping over a long frame. Real hardware has no such
    # quantization -- only genuine (much smaller, few-ppm) clock drift.
    n = len(nrz)
    starts = np.floor(np.arange(n) * sps_exact).astype(np.int64)
    ends = np.floor(np.arange(1, n + 1) * sps_exact).astype(np.int64)
    upsampled = np.repeat(nrz, ends - starts)
    taps = _gaussian_taps(bt, sps)
    shaped = np.convolve(upsampled, taps, mode="same")

    freq_hz = shaped * fdev
    phase = 2.0 * np.pi * np.cumsum(freq_hz) / sample_rate
    return np.exp(1j * phase).astype(np.complex64)

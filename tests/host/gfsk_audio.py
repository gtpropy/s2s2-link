"""Turns radio packets (as the SX126x would send them) into 48 kHz baseband audio for Direwolf.
On-air bit order: hardware preamble (alternating 1010..), 32-bit sync word, 200-byte payload MSB first.
Modulation: NRZ +-1 filtered by a Gaussian (BT 0.5) at 10 samples per bit (4800 baud)."""
import numpy as np

FS, BAUD = 48000, 4800
SPB = FS // BAUD
SYNC = bytes([0x93, 0x0B, 0x51, 0xDE])


def bits_of(data):
    return np.unpackbits(np.frombuffer(data, dtype=np.uint8))          # MSB first


def packet_bits(payload, preamble_bits=128, sync=SYNC):
    pre = np.tile(np.array([1, 0], dtype=np.uint8), (preamble_bits + 1) // 2)[:preamble_bits]
    return np.concatenate([pre, bits_of(sync), bits_of(payload)])


def gaussian_kernel(bt=0.5):
    sigma = np.sqrt(np.log(2)) / (2 * np.pi * bt) * SPB
    k = np.arange(-int(4 * sigma), int(4 * sigma) + 1)
    h = np.exp(-(k ** 2) / (2 * sigma ** 2))
    return h / h.sum()


def modulate(bits, bt=0.5):
    nrz = np.repeat(bits.astype(np.float64) * 2 - 1, SPB)
    return np.convolve(nrz, gaussian_kernel(bt), mode="same")


def stream(packets, preamble_bits=128, gap_s=0.5, snr_db=None, seed=1, lead_s=0.3):
    """packets: list of 200-byte payloads. Returns int16 audio."""
    rng = np.random.default_rng(seed)
    chunks = [np.zeros(int(lead_s * FS))]
    for p in packets:
        chunks.append(modulate(packet_bits(p, preamble_bits)))
        chunks.append(np.zeros(int(gap_s * FS)))
    x = np.concatenate(chunks)
    if snr_db is not None:
        sig_power = 1.0                                               # +-1 NRZ, filtered ~0.8 rms
        noise = rng.normal(0, np.sqrt(sig_power / (10 ** (snr_db / 10))), x.shape)
        x = x + noise
    x = np.clip(x * 0.45, -1, 1)
    return (x * 32767).astype(np.int16)

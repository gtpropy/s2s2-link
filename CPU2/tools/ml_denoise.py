# @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
# @date   2026-09-01
"""
ml_denoise.py -- small windowed MLP that denoises the FM-discriminator
output ahead of gmsk_demod's bit slicer, trained entirely on synthetic
data from gmsk_modulate() (there's no live hardware in this environment
-- see gmsk_demod.py's module docstring). Pure numpy, no ML framework:
this repo declares "numpy + scipy only" as its dependency policy (see
gmsk_demod.py), and a laptop running the ground station GUI shouldn't
need a multi-gigabyte torch/tensorflow install for an 11-tap window
regressor.

Model: TinyMLP, a single-hidden-layer perceptron (W-tap window of noisy
discriminator samples -> tanh hidden layer -> 1 denoised center sample),
trained by plain numpy forward/backward + Adam, minimizing MSE against
the *clean* discriminator output of the same synthetic frame (available
here only because gmsk_modulate() can synthesize both).

This is a genuine "is it worth it" question, not a foregone conclusion --
run this file directly to train a model and see the actual before/after
BER and FX.25-decode-success numbers it measures on held-out synthetic
data (see benchmark() / __main__ below) before deciding whether to wire
it into a live receiver.

Integration point: gmsk_demod.extract_candidate_frames(..., denoise_fn=...)
takes any callable(np.ndarray)->np.ndarray applied to the discriminator
output right after the existing lowpass filter, before symbol-rate
resampling -- denoise() below is exactly that shape, so
`denoise_fn=lambda x: ml_denoise.denoise(model, x)` plugs straight in.
"""
from __future__ import annotations

import numpy as np

import gmsk_demod as gd
import s2s2_decoder as dec


# =====================================================================
# TinyMLP -- manual numpy MLP, no framework
# =====================================================================

class TinyMLP:
    """window_len noisy samples -> hidden tanh layer -> 1 denoised
    sample. Inputs/outputs are normalized by `scale` (a physical unit,
    e.g. FDEV_HZ) so gradients stay well-conditioned regardless of the
    discriminator's raw Hz range."""

    def __init__(self, window_len: int = 11, hidden: int = 16, scale: float = gd.FDEV_HZ, seed: int = 0):
        assert window_len % 2 == 1, "window_len must be odd (centered prediction)"
        self.window_len = window_len
        self.hidden = hidden
        self.scale = float(scale)
        rng = np.random.RandomState(seed)
        # He-ish init, small enough tanh doesn't saturate immediately.
        self.W1 = rng.randn(window_len, hidden).astype(np.float64) * np.sqrt(2.0 / window_len)
        self.b1 = np.zeros(hidden, dtype=np.float64)
        self.W2 = rng.randn(hidden, 1).astype(np.float64) * np.sqrt(2.0 / hidden)
        self.b2 = np.zeros(1, dtype=np.float64)

    def _forward(self, X):
        """X: (N, window_len) normalized. Returns (y_hat, cache)."""
        z1 = X @ self.W1 + self.b1
        a1 = np.tanh(z1)
        z2 = a1 @ self.W2 + self.b2
        return z2[:, 0], (X, z1, a1)

    def predict(self, windows: np.ndarray) -> np.ndarray:
        """windows: (N, window_len) raw (un-normalized) noisy samples.
        Returns N denoised center-sample predictions, raw scale."""
        X = windows / self.scale
        y_hat, _ = self._forward(X)
        return y_hat * self.scale

    def train(self, X_raw, y_raw, epochs=40, batch_size=512, lr=1e-3, val_frac=0.1, seed=0, verbose=True):
        """X_raw: (N, window_len) raw noisy windows. y_raw: (N,) raw clean
        center samples. Adam optimizer, MSE loss on normalized values.
        Returns list of (epoch, train_mse, val_mse) for inspection."""
        rng = np.random.RandomState(seed)
        X = (X_raw / self.scale).astype(np.float64)
        y = (y_raw / self.scale).astype(np.float64)

        n = len(X)
        idx = rng.permutation(n)
        n_val = max(1, int(n * val_frac))
        val_idx, train_idx = idx[:n_val], idx[n_val:]
        Xtr, ytr = X[train_idx], y[train_idx]
        Xval, yval = X[val_idx], y[val_idx]

        params = [self.W1, self.b1, self.W2, self.b2]
        m = [np.zeros_like(p) for p in params]
        v = [np.zeros_like(p) for p in params]
        beta1, beta2, eps = 0.9, 0.999, 1e-8
        t = 0

        history = []
        for epoch in range(epochs):
            perm = rng.permutation(len(Xtr))
            for start in range(0, len(Xtr), batch_size):
                b = perm[start:start + batch_size]
                xb, yb = Xtr[b], ytr[b]
                n_b = len(xb)

                y_hat, (Xc, z1, a1) = self._forward(xb)
                err = (y_hat - yb)  # dL/dy_hat for MSE, up to 2/n_b const folded into lr

                dW2 = a1.T @ err[:, None] / n_b
                db2 = err.mean(axis=0, keepdims=True).reshape(-1)
                da1 = err[:, None] @ self.W2.T
                dz1 = da1 * (1.0 - a1 ** 2)
                dW1 = Xc.T @ dz1 / n_b
                db1 = dz1.mean(axis=0)

                grads = [dW1, db1, dW2, db2]
                t += 1
                for i, (p, g) in enumerate(zip(params, grads)):
                    m[i] = beta1 * m[i] + (1 - beta1) * g
                    v[i] = beta2 * v[i] + (1 - beta2) * (g * g)
                    mhat = m[i] / (1 - beta1 ** t)
                    vhat = v[i] / (1 - beta2 ** t)
                    p -= lr * mhat / (np.sqrt(vhat) + eps)

            train_pred, _ = self._forward(Xtr)
            val_pred, _ = self._forward(Xval)
            train_mse = float(np.mean((train_pred - ytr) ** 2))
            val_mse = float(np.mean((val_pred - yval) ** 2))
            history.append((epoch, train_mse, val_mse))
            if verbose and (epoch % 5 == 0 or epoch == epochs - 1):
                print(f"  epoch {epoch:3d}  train_mse={train_mse:.5f}  val_mse={val_mse:.5f}")
        return history

    def save(self, path: str):
        np.savez(path, W1=self.W1, b1=self.b1, W2=self.W2, b2=self.b2,
                  window_len=self.window_len, hidden=self.hidden, scale=self.scale)

    @classmethod
    def load(cls, path: str) -> "TinyMLP":
        d = np.load(path)
        m = cls(window_len=int(d["window_len"]), hidden=int(d["hidden"]), scale=float(d["scale"]))
        m.W1, m.b1, m.W2, m.b2 = d["W1"], d["b1"], d["W2"], d["b2"]
        return m


def denoise(model: TinyMLP, freq: np.ndarray) -> np.ndarray:
    """Applies `model` to a full discriminator-output array, matching
    the shape gmsk_demod.extract_candidate_frames' `denoise_fn` param
    expects (array in, same-length array out). Edge samples (within
    window_len//2 of either end) pass through unmodified -- not enough
    context for a window there, and it's a handful of samples out of a
    whole capture."""
    w = model.window_len
    half = w // 2
    if len(freq) < w:
        return freq
    windows = np.lib.stride_tricks.sliding_window_view(freq, w)
    centers = model.predict(windows)
    out = freq.copy()
    out[half:half + len(centers)] = centers
    return out


# =====================================================================
# Synthetic training-data generation (gmsk_modulate is the only source
# of ground truth here -- no live hardware, see module docstring)
# =====================================================================

def _clean_and_noisy_discriminator(payload: bytes, sample_rate: float, snr_db: float, rng: np.random.RandomState):
    """Returns (clean_freq, noisy_freq) discriminator-output arrays for
    one synthetic frame -- same preprocessing chain (fm_demod, dc_block,
    lowpass) gmsk_demod.extract_candidate_frames uses, so a model trained
    here sees exactly what it'll be applied to at inference time."""
    iq_clean = gd.gmsk_modulate(payload, sample_rate=sample_rate, order="msb", invert=False)
    sig_power = float(np.mean(np.abs(iq_clean) ** 2))
    noise_power = sig_power / (10 ** (snr_db / 10.0))
    noise = np.sqrt(noise_power / 2) * (rng.randn(*iq_clean.shape) + 1j * rng.randn(*iq_clean.shape))
    iq_noisy = (iq_clean + noise).astype(np.complex64)

    def chain(iq):
        freq = gd.fm_demod(iq, sample_rate)
        freq = gd.dc_block(freq)
        freq = gd.lowpass_filter(freq, sample_rate, cutoff_hz=gd.BAUD * 0.9)
        return freq

    return chain(iq_clean), chain(iq_noisy)


def generate_training_pairs(n_frames: int = 60, window_len: int = 11, sample_rate: float = 240000,
                             snr_range_db=(3, 18), seed: int = 0):
    """Builds (X, y) sliding-window arrays across n_frames synthetic
    frames (random payload bytes, random SNR per frame within
    snr_range_db) -- diverse bit patterns and noise levels so the model
    doesn't just memorize one message."""
    rng = np.random.RandomState(seed)
    half = window_len // 2
    X_parts, y_parts = [], []
    for _ in range(n_frames):
        payload = bytes(rng.randint(0, 256, size=dec.FX25_FRAME_LEN, dtype=np.uint8))
        snr_db = rng.uniform(*snr_range_db)
        clean, noisy = _clean_and_noisy_discriminator(payload, sample_rate, snr_db, rng)
        n = min(len(clean), len(noisy))
        clean, noisy = clean[:n], noisy[:n]
        if n <= window_len:
            continue
        windows = np.lib.stride_tricks.sliding_window_view(noisy, window_len)
        targets = clean[half:half + len(windows)]
        X_parts.append(windows)
        y_parts.append(targets)
    return np.concatenate(X_parts, axis=0), np.concatenate(y_parts, axis=0)


# =====================================================================
# Benchmark: does this actually help? (BER + real FX.25 decode success)
# =====================================================================

def _ber(a_bits: np.ndarray, b_bits: np.ndarray) -> float:
    n = min(len(a_bits), len(b_bits))
    return float(np.mean(a_bits[:n] != b_bits[:n]))


def benchmark(model: TinyMLP, n_frames: int = 40, sample_rate: float = 240000,
              snr_range_db=(3, 12), seed: int = 12345, reference_frame: bytes = None):
    """Held-out synthetic evaluation (different seed than training):
    for each frame, decode WITHOUT and WITH the denoiser, report bit
    error rate at the slicer and FX.25 decode success count for both.
    Prints a plain before/after table -- the honest answer, not assumed.

    `reference_frame` must be a real, validly RS/FX.25-encoded 168-byte
    frame (e.g. test_demod.build_reference_frame()) -- random bytes have
    no valid RS parity, so fx25_decode_frame would fail on them
    regardless of channel noise or denoising, making the decode-success
    comparison meaningless. Bit-level BER (which doesn't care about RS
    validity) still uses random per-frame payloads for pattern
    diversity; only the decode-success half needs a real frame."""
    rng = np.random.RandomState(seed)
    half = model.window_len // 2
    if reference_frame is None:
        import test_demod
        reference_frame = test_demod.build_reference_frame()

    plain_decodes = 0
    denoised_decodes = 0
    plain_ber_sum = 0.0
    denoised_ber_sum = 0.0
    n_eval = 0

    for _ in range(n_frames):
        msg = bytes(rng.randint(0, 256, size=dec.FX25_FRAME_LEN, dtype=np.uint8))
        snr_db = rng.uniform(*snr_range_db)
        clean, noisy = _clean_and_noisy_discriminator(msg, sample_rate, snr_db, rng)
        n = min(len(clean), len(noisy))
        clean, noisy = clean[:n], noisy[:n]
        if n <= model.window_len:
            continue

        denoised = denoise(model, noisy)

        clean_bits = (clean > 0).astype(np.uint8)
        plain_bits = (noisy > 0).astype(np.uint8)
        denoised_bits = (denoised > 0).astype(np.uint8)
        plain_ber_sum += _ber(clean_bits, plain_bits)
        denoised_ber_sum += _ber(clean_bits, denoised_bits)
        n_eval += 1

        iq = gd.gmsk_modulate(reference_frame, sample_rate=sample_rate, order="msb", invert=False)
        sig_power = float(np.mean(np.abs(iq) ** 2))
        noise_power = sig_power / (10 ** (snr_db / 10.0))
        noise = np.sqrt(noise_power / 2) * (rng.randn(*iq.shape) + 1j * rng.randn(*iq.shape))
        iq_noisy = (iq + noise).astype(np.complex64)

        plain_ok = any(
            dec.fx25_decode_frame(c["frame"]) is not None
            for c in gd.extract_candidate_frames(iq_noisy, sample_rate)
        )
        denoised_ok = any(
            dec.fx25_decode_frame(c["frame"]) is not None
            for c in gd.extract_candidate_frames(iq_noisy, sample_rate,
                                                  denoise_fn=lambda x: denoise(model, x))
        )
        plain_decodes += int(plain_ok)
        denoised_decodes += int(denoised_ok)

    print(f"\nheld-out benchmark, {n_eval} synthetic frames, SNR {snr_range_db[0]}-{snr_range_db[1]} dB:")
    print(f"  slicer BER   plain={plain_ber_sum / n_eval:.4f}   denoised={denoised_ber_sum / n_eval:.4f}")
    print(f"  FX.25 decode plain={plain_decodes}/{n_eval}   denoised={denoised_decodes}/{n_eval}")


if __name__ == "__main__":
    import os

    print("generating synthetic training pairs...")
    X, y = generate_training_pairs(n_frames=80, window_len=11, seed=0)
    print(f"{len(X)} training windows")

    model = TinyMLP(window_len=11, hidden=16, seed=0)
    print("training...")
    model.train(X, y, epochs=40, lr=2e-3)

    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "denoiser.npz")
    model.save(out_path)
    print(f"saved model to {out_path}")

    # 6-10dB is roughly this link's decode "knee" (SNR where plain decode
    # sometimes succeeds and sometimes doesn't) -- the benefit shows up
    # most clearly there; well above or below it neither side is
    # distinguishable (both always succeed, or both always fail).
    benchmark(model, n_frames=30, snr_range_db=(6, 10))

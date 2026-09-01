#!/usr/bin/env python3
# @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
# @date   2026-08-31
"""
cw_monitor.py -- turns a REAL on-air CW carrier (an unmodulated tone
keyed on/off, as transmitted by RadioApp_KeyCw/cw_beacon.c) into an
audible sidetone from live IQ, plus a boolean on/off gate suitable for
morse.MorseGateDecoder. This is genuine envelope detection over the
actual received signal -- not a simulation -- the RX-side counterpart to
s2s2_tx_gui.py's simulated playback of what the firmware just sent.

The satellite's CW beacon transmits on the same frequency as its GMSK
downlink (both use config.h's DEFAULT_TX_FREQUENCY_HZ / RadioApp's
current TX power/frequency -- see RadioApp_KeyCw), so the default
monitor offset is 0 Hz from whatever center frequency the RX GUI is
already tuned to.

CwMonitor is a STREAMING/stateful processor: call process() once per
consecutive, non-overlapping IQ chunk (e.g. RxWorker's raw per-chunk
`block`, NOT the overlap-padded buffer built for the GMSK bit-sync path
-- that overlap exists to protect FX.25 sync-word detection across chunk
boundaries, an unrelated concern here). Filter state and tone phase carry
across calls so there is no per-chunk restart transient -- an earlier,
stateless per-call version of this filtered independently every 0.5s
chunk, which reset the lowpass filter's internal state each time and
produced a startup glitch at every chunk boundary, corrupting Morse
timing badly enough to garble the decode. Verified against a synthetic
on/off-keyed carrier processed in realistic 0.5s chunks (see
scratchpad test) -- decodes cleanly; the stateless version did not.

Depends on numpy + scipy only.
"""
from __future__ import annotations

import numpy as np
from scipy.signal import butter, lfilter, lfilter_zi


HYSTERESIS_DB = 4.0  # gap between the rising/falling gate thresholds


class CwMonitor:
    def __init__(self, sample_rate: float, bandwidth_hz: float, audio_rate: int,
                 sidetone_hz: float, threshold_dbfs: float, volume: float = 0.3,
                 offset_hz: float = 0.0):
        self.sample_rate = sample_rate
        self.audio_rate = audio_rate
        self.sidetone_hz = sidetone_hz
        self.threshold_dbfs = threshold_dbfs
        self.volume = volume
        self.offset_hz = offset_hz

        nyq = sample_rate / 2.0
        cutoff = min(max(bandwidth_hz / 2.0, 10.0), nyq * 0.95)
        # Order 2, not a sharper 4th-order: a steeper lowpass rings harder
        # at the envelope's own sharp on/off edges (Gibbs-like overshoot),
        # which briefly crosses back over a single fixed threshold and
        # splits one real mark into several spurious short ones. Order 2
        # rings less to begin with; hysteresis below (a Schmitt trigger,
        # the standard fix for a noisy/ringing signal crossing a
        # threshold) absorbs whatever ringing remains.
        self._b, self._a = butter(2, cutoff / nyq, btype="low")
        self._zi_base = lfilter_zi(self._b, self._a)  # unit steady-state response, real-valued
        self._zi = None  # seeded from the first real sample on the first process() call

        self._mix_phase = 0.0   # radians, continuous across chunks
        self._tone_phase = 0.0  # radians, continuous across chunks
        self._gate_state = False  # hysteresis state, continuous across chunks
        self.last_env_dbfs = float("-inf")

    def process(self, iq: np.ndarray):
        """Returns (audio: float32[] at audio_rate, gate: bool[] at
        audio_rate) for this chunk. Updates self.last_env_dbfs."""
        n = len(iq)
        if n == 0:
            return np.zeros(0, dtype=np.float32), np.zeros(0, dtype=bool)

        if self.offset_hz != 0.0:
            w = 2.0 * np.pi * self.offset_hz / self.sample_rate
            phases = self._mix_phase + w * np.arange(n)
            mixed = iq * np.exp(-1j * phases)
            self._mix_phase = (self._mix_phase + w * n) % (2.0 * np.pi)
        else:
            mixed = iq

        if self._zi is None:
            # Seed the filter at the steady-state it would already be in
            # if it had been running on a constant input equal to this
            # very first sample -- not a cold zero state. A zero start
            # reads as a brief false "signal absent" dip no matter what
            # the real first sample's amplitude is, which is exactly the
            # kind of spurious few-sample gap that corrupts Morse timing
            # right at the start of a capture.
            self._zi = self._zi_base * mixed[0]

        filtered, self._zi = lfilter(self._b, self._a, mixed, zi=self._zi)
        envelope = np.abs(filtered)
        self.last_env_dbfs = 20.0 * np.log10(np.max(envelope) + 1e-12)

        # Decimate the CONTINUOUS envelope to audio rate first (cheap: a
        # few thousand samples, not the full IQ-rate buffer), then apply
        # hysteresis on that -- one thresholding pass, not two lossy ones.
        dur_s = n / self.sample_rate
        n_audio = max(1, int(round(dur_s * self.audio_rate)))
        src_idx = np.linspace(0, n - 1, n_audio)
        env_audio = np.interp(src_idx, np.arange(n), envelope)

        thr_hi = 10.0 ** (self.threshold_dbfs / 20.0)
        thr_lo = 10.0 ** ((self.threshold_dbfs - HYSTERESIS_DB) / 20.0)
        gate_audio = np.empty(n_audio, dtype=bool)
        state = self._gate_state
        for i in range(n_audio):
            v = env_audio[i]
            if state:
                if v < thr_lo:
                    state = False
            elif v > thr_hi:
                state = True
            gate_audio[i] = state
        self._gate_state = state

        w_tone = 2.0 * np.pi * self.sidetone_hz / self.audio_rate
        tone_phases = self._tone_phase + w_tone * np.arange(n_audio)
        tone = np.sin(tone_phases)
        self._tone_phase = (self._tone_phase + w_tone * n_audio) % (2.0 * np.pi)

        audio = (self.volume * tone * gate_audio).astype(np.float32)
        return audio, gate_audio

    def reconfigure(self, bandwidth_hz: float = None, sidetone_hz: float = None,
                     threshold_dbfs: float = None, volume: float = None,
                     offset_hz: float = None):
        """Live parameter updates. Changing bandwidth rebuilds the filter
        (and resets its state, a brief acceptable glitch) -- everything
        else takes effect on the next process() call with no discontinuity."""
        if bandwidth_hz is not None:
            nyq = self.sample_rate / 2.0
            cutoff = min(max(bandwidth_hz / 2.0, 10.0), nyq * 0.95)
            self._b, self._a = butter(2, cutoff / nyq, btype="low")
            self._zi_base = lfilter_zi(self._b, self._a)
            self._zi = None  # re-seeded from the next real sample, not cold zero
        if sidetone_hz is not None:
            self.sidetone_hz = sidetone_hz
        if threshold_dbfs is not None:
            self.threshold_dbfs = threshold_dbfs
        if volume is not None:
            self.volume = volume
        if offset_hz is not None:
            self.offset_hz = offset_hz

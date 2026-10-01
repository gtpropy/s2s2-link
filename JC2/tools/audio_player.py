#!/usr/bin/env python3
# @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
# @date   2026-08-31
"""
audio_player.py -- shared streaming audio output for both GUIs (TX GUI's
simulated CW sidetone, RX GUI's real envelope-detected CW monitor tone).

A single sounddevice.OutputStream pulled by a callback from a small ring
buffer, fed by push() from whichever thread has new audio (a Qt signal
handler on the GUI thread for TX, or the RxWorker QThread for RX) --
NOT one sd.play() call per chunk, which would click/gap at every chunk
boundary instead of playing one continuous tone.

Optional dependency: sounddevice (PortAudio). If it's not importable,
AudioPlayer.start() raises RuntimeError with an install hint and no
audio plays -- callers should catch that and just skip audio, not crash
the GUI over a missing optional feature.
"""
from __future__ import annotations

import threading

import numpy as np


class AudioPlayer:
    def __init__(self, sample_rate: int = 8000, max_buffer_seconds: float = 2.0):
        self.sample_rate = sample_rate
        self._max_buffer_len = int(sample_rate * max_buffer_seconds)
        self._buffer = np.zeros(0, dtype=np.float32)
        self._lock = threading.Lock()
        self._stream = None

    def start(self):
        if self._stream is not None:
            return
        try:
            import sounddevice as sd
        except ImportError as e:
            raise RuntimeError(
                "sounddevice not installed -- run: pip install sounddevice") from e

        def callback(outdata, frames, time_info, status):  # noqa: unused args required by PortAudio's callback signature
            with self._lock:
                n = len(self._buffer)
                if n >= frames:
                    outdata[:, 0] = self._buffer[:frames]
                    self._buffer = self._buffer[frames:]
                else:
                    outdata[:n, 0] = self._buffer
                    outdata[n:, 0] = 0.0
                    self._buffer = np.zeros(0, dtype=np.float32)

        self._stream = sd.OutputStream(
            samplerate=self.sample_rate, channels=1, dtype="float32", callback=callback)
        self._stream.start()

    def push(self, samples: np.ndarray):
        """Appends audio to the playback ring buffer. Safe to call from
        any thread. Silently drops the oldest audio if the buffer is
        backing up faster than the output device drains it, rather than
        growing unbounded (a slow/blocked audio device should not leak
        memory or add unbounded latency)."""
        if self._stream is None or len(samples) == 0:
            return
        with self._lock:
            self._buffer = np.concatenate([self._buffer, samples.astype(np.float32)])
            if len(self._buffer) > self._max_buffer_len:
                self._buffer = self._buffer[-self._max_buffer_len:]

    def stop(self):
        if self._stream is not None:
            self._stream.stop()
            self._stream.close()
            self._stream = None
        with self._lock:
            self._buffer = np.zeros(0, dtype=np.float32)

    @property
    def is_running(self) -> bool:
        return self._stream is not None

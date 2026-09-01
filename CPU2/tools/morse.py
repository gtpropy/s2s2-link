#!/usr/bin/env python3
# @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
# @date   2026-08-31
"""
morse.py -- shared International Morse table + audio synthesis/decode,
used by both GUIs:

    s2s2_tx_gui.py  -- synthesizes a sidetone reproduction of a CW beacon
                        the satellite just sent (parsed from its own log
                        line), so the operator can hear it, not just read
                        the digits. This is a SIMULATED tone matching the
                        firmware's own timing table (core/Src/cw_beacon.c)
                        -- not a live over-the-air capture.
    s2s2_rx_gui.py  -- via cw_monitor.py, turns a REAL envelope-detected
                        carrier (from live IQ) into audio, and can decode
                        it back to text using the streaming decoder below.

Timing matches core/Src/cw_beacon.c exactly: PARIS-standard WPM, dit=1
unit, dah=3 units, intra-character gap=1 unit (rendered as 2 extra units
after each character), inter-word gap=7 units. unit_ms = 1200/wpm.
"""
from __future__ import annotations

import numpy as np

MORSE_TABLE = {
    'A': ".-",    'B': "-...",  'C': "-.-.",  'D': "-..",
    'E': ".",     'F': "..-.",  'G': "--.",   'H': "....",
    'I': "..",    'J': ".---",  'K': "-.-",   'L': ".-..",
    'M': "--",    'N': "-.",    'O': "---",   'P': ".--.",
    'Q': "--.-",  'R': ".-.",   'S': "...",   'T': "-",
    'U': "..-",   'V': "...-",  'W': ".--",   'X': "-..-",
    'Y': "-.--",  'Z': "--..",
    '0': "-----", '1': ".----", '2': "..---", '3': "...--",
    '4': "....-", '5': ".....", '6': "-....", '7': "--...",
    '8': "---..", '9': "----.",
}
REVERSE_MORSE = {v: k for k, v in MORSE_TABLE.items()}

DEFAULT_WPM = 15          # matches CW_DEFAULT_WPM in core/Inc/cw_beacon.h
DEFAULT_SIDETONE_HZ = 600  # a conventional ham CW sidetone pitch


def unit_seconds(wpm: float) -> float:
    """PARIS-standard: unit_ms = 1200/wpm."""
    return 1.2 / wpm if wpm > 0 else 1.2 / DEFAULT_WPM


def message_to_segments(message: str, wpm: float = DEFAULT_WPM):
    """Returns a list of (is_mark: bool, duration_s) segments, exactly
    matching cw_beacon.c's timing (see that file's comments)."""
    u = unit_seconds(wpm)
    segments = []
    for ch in message.upper():
        if ch == ' ':
            segments.append((False, u * 7))
            continue
        code = MORSE_TABLE.get(ch)
        if code is None:
            continue  # unsupported character, silently skipped (matches firmware)
        for sym in code:
            segments.append((True, u * 3 if sym == '-' else u))
            segments.append((False, u))
        segments.append((False, u * 2))  # 1 (above) + 2 = 3-unit inter-character gap
    return segments


def message_to_audio(message: str, wpm: float = DEFAULT_WPM,
                      sidetone_hz: float = DEFAULT_SIDETONE_HZ,
                      sample_rate: int = 8000, volume: float = 0.3) -> np.ndarray:
    """Renders `message` as a mono float32 sidetone waveform. A short
    linear ramp at each mark's edges avoids audible clicks."""
    segments = message_to_segments(message, wpm)
    if not segments:
        return np.zeros(0, dtype=np.float32)

    chunks = []
    for is_mark, dur in segments:
        n = max(1, int(round(dur * sample_rate)))
        if is_mark:
            t = np.arange(n) / sample_rate
            tone = (volume * np.sin(2.0 * np.pi * sidetone_hz * t)).astype(np.float32)
            ramp = min(n // 8, int(0.003 * sample_rate) + 1)
            if ramp > 0:
                tone[:ramp] *= np.linspace(0.0, 1.0, ramp, dtype=np.float32)
                tone[-ramp:] *= np.linspace(1.0, 0.0, ramp, dtype=np.float32)
            chunks.append(tone)
        else:
            chunks.append(np.zeros(n, dtype=np.float32))
    return np.concatenate(chunks)


class MorseGateDecoder:
    """Streaming decoder: feed it a boolean on/off "gate" signal (True =
    carrier/tone present) at a known sample rate, in any-sized chunks, and
    it reconstructs Morse characters as timing evidence accumulates.

    Classification is deferred one character at a time: all of a
    character's mark lengths are buffered until its closing gap is seen,
    then split dit/dah two ways --
      - if the character's own marks span a real range (e.g. "K" = -.-,
        one dah + two dits), it self-calibrates off its own min/max --
        this is what lets the decoder track a WPM that drifts over time.
      - if every mark in the character is the same length (e.g. "E"=.,
        "T"=-, "M"=--), there's nothing to self-calibrate against, so it
        falls back to `self._known_dit_samples`, the shortest dit-length
        confirmed by any prior self-calibrated character this session.
    Before any character has self-calibrated (the very first symbol, if
    it happens to be uniform-length on its own), the only honest answer
    is a guess -- dit is the tie-break, since it's both the Morse default
    (no signal yet implies "shortest") and the more common letter (E).
    """

    def __init__(self, sample_rate: float):
        self.sample_rate = sample_rate
        self._last_state = False
        self._run_len = 0
        self._char_marks = []       # mark lengths (samples) for the in-progress character
        self.text = ""
        self._known_dit_samples = unit_seconds(DEFAULT_WPM) * sample_rate

    def _classify_char(self):
        if not self._char_marks:
            return
        lo, hi = min(self._char_marks), max(self._char_marks)
        if hi > lo * 1.8:
            # Self-calibrating: real dit/dah contrast within this one
            # character. Threshold at the midpoint; the shortest marks
            # here update our running dit-length estimate for later
            # uniform-length characters.
            threshold = (lo + hi) / 2.0
            self._known_dit_samples = float(np.mean([m for m in self._char_marks if m <= threshold]))
        else:
            threshold = self._known_dit_samples * 1.8
        code = "".join("-" if m > threshold else "." for m in self._char_marks)
        ch = REVERSE_MORSE.get(code, "")
        self.text += ch if ch else "?"
        self._char_marks = []

    def _flush_mark(self, run_samples: int):
        self._char_marks.append(run_samples)

    def _flush_space(self, run_samples: int):
        is_boundary = run_samples > self._known_dit_samples * 1.8
        if not is_boundary:
            # A 1-unit intra-character gap (between two symbols of the
            # SAME character, e.g. the gap in "A" = .-) is unconditionally
            # exactly 1 unit by construction -- unlike marks, there's no
            # dit/dah ambiguity to wait out. Tightening the estimate
            # toward the shortest gap actually seen (rather than trusting
            # a fixed assumed-WPM default) is what lets a single
            # multi-symbol character self-calibrate the correct WPM
            # before its own closing gap even arrives, regardless of how
            # far the true WPM is from the DEFAULT_WPM starting guess.
            self._known_dit_samples = min(self._known_dit_samples, run_samples)
        elif self._char_marks:
            self._classify_char()
        if run_samples > self._known_dit_samples * 5:
            self.text += " "  # inter-word gap

    def feed(self, gate: np.ndarray):
        for sample in gate:
            state = bool(sample)
            if state == self._last_state:
                self._run_len += 1
                continue
            # Transition: close out the run that just ended -- except the
            # very first transition ever seen, where _run_len is still its
            # initial 0 (no real run happened yet, just the decoder's
            # starting assumption of silence). Flushing that phantom
            # zero-length "gap" would tighten the unit estimate to zero
            # and misclassify everything after it.
            if self._run_len > 0:
                if self._last_state:
                    self._flush_mark(self._run_len)
                else:
                    self._flush_space(self._run_len)
            self._last_state = state
            self._run_len = 1

    def flush(self):
        """Call once at end-of-stream (or when idle) to classify any
        character still buffered without waiting for a trailing gap."""
        self._classify_char()

    def reset(self):
        self._last_state = False
        self._run_len = 0
        self._char_marks = []
        self.text = ""
        self._known_dit_samples = unit_seconds(DEFAULT_WPM) * self.sample_rate

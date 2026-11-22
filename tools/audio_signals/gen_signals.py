#!/usr/bin/env python3
"""Generate the Phase 5C audio test signals (44.1 kHz, 16-bit mono WAV) plus a JSON schedule sidecar per file.

    python3 -I tools/audio_signals/gen_signals.py [--out probes/audio]

Stdlib only, deterministic (fixed seeds): re-running produces identical files. The WAVs are git-ignored and
regenerated on demand; the sidecars (<name>.json) are small and describe every segment in file time (seconds).

Every file starts with the same lead-in so G-Force and reforce timelines can be aligned on the audio itself:
    0.0 .. 0.5 s  silence
    0.5 .. 0.55 s SYNC click: 2 kHz tone burst, Hann-windowed, -6 dBFS peak
    0.55 .. 2.0 s silence
    2.0 s ..      content (CONTENT_START), followed by 2 s of silence (tail)

Levels: tones/sweeps/kicks are dBFS *peak* (0 dBFS = full-scale sine); noise is dBFS *RMS*.
"""
import argparse
import cmath
import json
import math
import os
import random
import struct
import wave

SR = 44100
SYNC_T, SYNC_DUR, SYNC_HZ, SYNC_DB = 0.5, 0.05, 2000.0, -6.0
CONTENT_START = 2.0
TAIL = 2.0
EDGE = 0.005  # raised-cosine fade at segment edges (s), avoids spectral splatter


def db(x):
    return 10 ** (x / 20.0)


class Track:
    def __init__(self, name, description):
        self.name, self.description = name, description
        self.s = []
        self.segments = []
        self.add_silence(SYNC_T)
        n = int(SYNC_DUR * SR)
        a = db(SYNC_DB)
        self.s += [a * math.sin(2 * math.pi * SYNC_HZ * i / SR) * (0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1)))
                   for i in range(n)]
        self.add_silence(CONTENT_START - len(self.s) / SR)
        self.sync = {"t": SYNC_T, "duration": SYNC_DUR, "freq_hz": SYNC_HZ, "peak_dbfs": SYNC_DB}

    @property
    def t(self):
        return len(self.s) / SR

    def add_silence(self, seconds, label=None):
        t0 = self.t
        self.s += [0.0] * int(round(seconds * SR))
        if label:
            self.segments.append({"t0": round(t0, 6), "t1": round(self.t, 6), "kind": "silence", "label": label})

    def add(self, samples, seg, fade=True):
        if fade:
            samples = faded(samples)
        t0 = self.t
        self.s += samples
        seg = dict(seg)
        seg["t0"], seg["t1"] = round(t0, 6), round(self.t, 6)
        self.segments.append(seg)

    def write(self, out):
        self.add_silence(TAIL)
        path = os.path.join(out, self.name + ".wav")
        peak = max(abs(v) for v in self.s)
        if peak > 1.0:
            raise SystemExit("%s clips (peak %.3f)" % (self.name, peak))
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(b"".join(struct.pack("<h", int(round(max(-1.0, min(1.0, v)) * 32767))) for v in self.s))
        meta = {"file": self.name + ".wav", "description": self.description, "sample_rate": SR, "channels": 1,
                "sample_format": "s16", "duration": round(self.t, 6), "samples": len(self.s),
                "sync": self.sync, "content_start": CONTENT_START, "segments": self.segments,
                "levels": "tones/sweep/kick: dBFS peak (0 dBFS = full-scale sine); noise: dBFS RMS",
                "generator": "tools/audio_signals/gen_signals.py"}
        with open(os.path.join(out, self.name + ".json"), "w") as f:
            json.dump(meta, f, indent=1)
            f.write("\n")
        print("%-16s %6.2f s  peak %.3f  %d segments" % (self.name + ".wav", self.t, peak, len(self.segments)))


def faded(x, edge=EDGE):
    n = len(x)
    k = min(int(edge * SR), n // 2)
    x = list(x)
    for i in range(k):
        g = 0.5 - 0.5 * math.cos(math.pi * i / k)
        x[i] *= g
        x[n - 1 - i] *= g
    return x


def tone(freq, seconds, peak_db, phase=0.0):
    a = db(peak_db)
    w = 2 * math.pi * freq / SR
    return [a * math.sin(phase + w * i) for i in range(int(round(seconds * SR)))]


# ---- noise via inverse FFT (pure Python radix-2) ------------------------------------------------------------
def _ifft(a):
    n = len(a)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    size = 2
    while size <= n:
        half = size // 2
        wn = cmath.exp(2j * math.pi / size)
        tw = [wn ** k for k in range(half)]
        for start in range(0, n, size):
            for k in range(half):
                u = a[start + k]
                v = a[start + k + half] * tw[k]
                a[start + k] = u + v
                a[start + k + half] = u - v
        size *= 2
    return a


def noise(seconds, lo_hz, hi_hz, rms_db, seed, pink=False):
    """Random-phase flat-magnitude (or 1/sqrt(f) for pink) spectrum between lo and hi, scaled to an exact RMS."""
    n_out = int(round(seconds * SR))
    n = 1
    while n < n_out:
        n *= 2
    rng = random.Random(seed)
    spec = [0j] * n
    for k in range(1, n // 2):
        f = k * SR / n
        if lo_hz <= f <= hi_hz:
            mag = 1.0 / math.sqrt(f) if pink else 1.0
            ph = rng.uniform(0, 2 * math.pi)
            spec[k] = mag * cmath.exp(1j * ph)
            spec[n - k] = spec[k].conjugate()
    x = [v.real for v in _ifft(spec)][:n_out]
    r = math.sqrt(sum(v * v for v in x) / len(x))
    g = db(rms_db) / r
    x = [v * g for v in x]
    # Gaussian-like noise at -12 dBFS RMS has rare peaks above full scale (crest ~14 dB): hard-limit those few
    # samples at +-0.999 (the count is recorded in the sidecar as "clipped_samples").
    clipped = sum(1 for v in x if abs(v) > 0.999)
    return [max(-0.999, min(0.999, v)) for v in x], clipped


def kick(peak_db):
    """Synthetic kick: sine gliding 150 -> 55 Hz (tau 30 ms), amplitude decay tau 150 ms, 0.45 s long."""
    out, ph = [], 0.0
    a = db(peak_db)
    for i in range(int(0.45 * SR)):
        t = i / SR
        f = 55.0 + 95.0 * math.exp(-t / 0.03)
        ph += 2 * math.pi * f / SR
        env = math.exp(-t / 0.15) * min(1.0, t / 0.002)
        out.append(a * env * math.sin(ph))
    return out


# ---- the signal set ----------------------------------------------------------------------------------------
def build(out):
    tr = Track("silence", "Lead-in + 10 s digital silence: baseline / noise floor of every signal.")
    tr.add_silence(10.0, "silence")
    tr.write(out)

    tr = Track("tone_steps", "1 kHz sine, phase-continuous, level steps -40/-30/-20/-10/-6/0 dBFS peak, 2 s each.")
    ph = 0.0
    for lv in (-40, -30, -20, -10, -6, 0):
        seg = tone(1000.0, 2.0, lv, ph)
        ph += 2 * math.pi * 1000.0 * len(seg) / SR
        # 1 ms level ramps only at the very first/last edge; inner steps are instantaneous (amplitude only).
        tr.add(seg, {"kind": "tone", "freq_hz": 1000.0, "peak_dbfs": lv}, fade=False)
    # fade the whole block's outer edges
    n = int(round(12.0 * SR))
    blk = faded(tr.s[-n:])
    tr.s[-n:] = blk
    tr.write(out)

    tr = Track("sweep", "Exponential (log) sine sweep 20 Hz -> 20 kHz over 20 s at -6 dBFS peak. "
                        "Instantaneous frequency f(t) = 20 * 1000^((t - t0)/20).")
    f1, f2, T = 20.0, 20000.0, 20.0
    L = T / math.log(f2 / f1)
    a = db(-6)
    sw = [a * math.sin(2 * math.pi * f1 * L * (math.exp((i / SR) / L) - 1)) for i in range(int(T * SR))]
    tr.add(sw, {"kind": "log_sweep", "f0_hz": f1, "f1_hz": f2, "peak_dbfs": -6,
                "freq_of_t": "f0 * (f1/f0)^((t - t0)/(t1 - t0))"})
    tr.write(out)

    tr = Track("noise_bands", "Band-limited flat-spectrum noise at -15 dBFS RMS: low 40-150 Hz, mid 400-2000 Hz, "
                              "high 4-12 kHz; 3 s each with 1 s silent gaps.")
    for i, (name, lo, hi) in enumerate((("low", 40, 150), ("mid", 400, 2000), ("high", 4000, 12000))):
        if i:
            tr.add_silence(1.0, "gap")
        x, clipped = noise(3.0, lo, hi, -15, seed=100 + i)
        tr.add(x, {"kind": "noise", "band": name, "lo_hz": lo, "hi_hz": hi, "rms_dbfs": -15,
                   "clipped_samples": clipped})
    tr.write(out)

    tr = Track("kick120", "Synthetic kick (150->55 Hz glide, 150 ms decay, -3 dBFS peak) at 120 BPM for 16 s "
                          "(32 hits, one every 0.5 s; each segment is one hit + its silence).")
    k = kick(-3)
    period = int(0.5 * SR)
    for i in range(32):
        tr.add(k + [0.0] * (period - len(k)), {"kind": "kick", "index": i, "peak_dbfs": -3, "hit_len": 0.45},
               fade=False)
    tr.write(out)

    tr = Track("attack_release", "Attack/release step x3: 0.5 s silence -> 2 s pink noise (20 Hz-20 kHz, "
                                 "-12 dBFS RMS, instantaneous on/off) -> 2 s silence.")
    for i in range(3):
        tr.add_silence(0.5, "pre")
        x, clipped = noise(2.0, 20, 20000, -12, seed=200 + i, pink=True)
        tr.add(x, {"kind": "pink_noise", "index": i, "rms_dbfs": -12, "lo_hz": 20, "hi_hz": 20000,
                   "clipped_samples": clipped}, fade=False)
        tr.add_silence(2.0, "release")
    tr.write(out)

    # Follow-up run (5C, gaps 1-4 of docs/audio-probe-results.md): separates time-since-onset, level and frequency.
    tr = Track("tone_freqs", "Steady sines at -12 dBFS peak: 125/250/500/1k/2k/4k Hz, 6 s each with 2 s silent "
                             "gaps; 2 s gap; 1 kHz for 20 s (drift); 2 s gap; falling log sweep 20 kHz -> 20 Hz "
                             "over 20 s at -6 dBFS peak.")
    for i, f in enumerate((125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0)):
        if i:
            tr.add_silence(2.0, "gap")
        tr.add(tone(f, 6.0, -12), {"kind": "tone", "freq_hz": f, "peak_dbfs": -12})
    tr.add_silence(2.0, "gap")
    tr.add(tone(1000.0, 20.0, -12), {"kind": "tone_long", "freq_hz": 1000.0, "peak_dbfs": -12})
    tr.add_silence(2.0, "gap")
    f1, f2, T = 20000.0, 20.0, 20.0
    L = T / math.log(f2 / f1)  # negative: falling
    a = db(-6)
    sw = [a * math.sin(2 * math.pi * f1 * L * (math.exp((i / SR) / L) - 1)) for i in range(int(T * SR))]
    tr.add(sw, {"kind": "log_sweep_down", "f0_hz": f1, "f1_hz": f2, "peak_dbfs": -6,
                "freq_of_t": "f0 * (f1/f0)^((t - t0)/(t1 - t0))"})
    tr.write(out)

    # Two/three simultaneous tones: two known frequencies pin both ends of G-Force's fft() axis per snapshot.
    tr = Track("tone_pairs", "Simultaneous sines, -18 dBFS peak each: 300+2400 Hz 10 s; gap 2 s; 200+400+3200 Hz "
                             "10 s; gap 2 s; 1 kHz 6 s -> 1k+4k 6 s -> 4 kHz 6 s (no gaps); gap 2 s; 250+1000 Hz 6 s.")

    def chord(freqs, seconds, fade=True):
        parts = [tone(f, seconds, -18) for f in freqs]
        tr.add([sum(v) for v in zip(*parts)], {"kind": "tones", "freqs_hz": list(freqs), "peak_dbfs_each": -18},
               fade=fade)

    chord((300.0, 2400.0), 10.0)
    tr.add_silence(2.0, "gap")
    chord((200.0, 400.0, 3200.0), 10.0)
    tr.add_silence(2.0, "gap")
    n0 = len(tr.s)
    chord((1000.0,), 6.0, fade=False)
    chord((1000.0, 4000.0), 6.0, fade=False)  # 6 s = whole periods: the 1 kHz stays phase-continuous
    chord((4000.0,), 6.0, fade=False)
    tr.s[n0:] = faded(tr.s[n0:])
    tr.add_silence(2.0, "gap")
    chord((250.0, 1000.0), 6.0)
    tr.write(out)

    tr = Track("test_click", "Lead-in only + 1 s: the sync click, for route/latency checks of the player.")
    tr.add_silence(1.0, "silence")
    tr.write(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=os.path.join(here, "..", "..", "probes", "audio"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    build(os.path.abspath(a.out))


if __name__ == "__main__":
    main()

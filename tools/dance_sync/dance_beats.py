#!/usr/bin/env python3
"""Finds the BPM and beat grid of a dance video from the dancers' motion (no audio needed).

    tools/dance_sync/run dance_beats.py CLIP.mov [CLIP2.mov | FOLDER ...] [options]

    --preview          render CLIP.beats.mp4: the clip with the beat grid drawn on it, a motion strip, and a click
                       track (accented downbeat), then open it in QuickTime (--no-open to skip)
    --write            save BPM / BeatStart / BeatsPerBar into the movie's "<stem>.reforce.txt" (for an interpolated
                       "<stem>.rife4x.mov", the original's: analyse the smooth file, keep one options file)
    --bpm 124          skip the tempo search; only fit the phase at about this tempo (a tempo in the file name,
                       "_128bpm", narrows the search the same way)
    --range 70-180     tempo search range (default 70-180)
    --downbeat N       beat N of the fitted grid (0 = first beat in the clip) is "1" of the bar
    --offset-ms X      shift the grid later by X ms (calibration: dancers' hits vs the musical beat)
    --offset-beats 0.5 shift the grid by a fraction of a beat (e.g. the beat is the top of the bounce, not the bottom)
    --signal S         auto (default: fits bounce and hits, keeps the more confident; on a silhouette, bounce
                       unless hits leads by 0.2) | bounce (70% bounce + 30% hits) | hits | bounce-only
    --start / --end    analyse only this part of the clip (seconds or m:ss)

How it works:
  1. Frames are scaled to 256 px wide. Repeated frames are skipped (many clips are 15 fps padded to 24 by
     repeating frames; a repeat looks like the dancer freezing), and every real frame keeps its true time.
  2. Dense optical flow (OpenCV DIS) between consecutive real frames, minus the median flow (camera pan), as
     velocity per second. Two motion signals, resampled to 50 Hz:
       bounce  the body's vertical velocity, measured in three bands relative to the body's own height (upper
               body 20%, hips 40%, legs 40%) so zooms don't matter. Its onset envelope is the upward acceleration,
               which peaks at the bottom of each bounce; the velocity's own fundamental also votes for the tempo.
               Silhouette clips (a solid white or black figure on a plain background) use the figure as the
               body mask, others the moving region.
       hits    a directogram (flow summed in 8 direction bins; Davis & Agrawala, "Visual Rhythm and Beat", 2018)
               and its summed per-direction deceleration: a movement stopping or reversing.
     Each is normalised by its local level (quiet passages count) and silenced around shot cuts.
  3. Tempo: a Fourier tempogram of the combined envelope (tempo + 2nd/3rd harmonics) plus the bounce
     fundamental, with a mild prior around 115 BPM. The winning frequency's phase gives a first beat grid.
  5. Seamless loops (the last frame flows into the first, like VJ loops) whose length is a whole number of bars at
     the found tempo get the tempo from the length exactly (N beats in the clip's frame time), so they loop in sync.
  4. Refinement: the tempogram peak is refined to 0.002 BPM (a whole-clip estimate, unbiased by coarse frame
     timing). The phase comes from the envelope peak nearest each predicted beat (sub-sample, parabolic): their
     robust weighted mean offset places the grid t_k = BeatStart + k * 60/BPM.

Results: a JSON record in ~/Library/Caches/reforce/dance_sync/ (read by the preview and the review UI) and,
with --write, three keys in "<stem>.reforce.txt" (engine/MediaOptions.hpp): BPM, BeatStart (seconds of a
downbeat) and BeatsPerBar. Nothing else beside the movie is touched.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import subprocess
import sys
import wave
from pathlib import Path

import cv2
import numpy as np
from scipy.ndimage import gaussian_filter1d, median_filter, uniform_filter1d

MOVIE_EXT = {".mov", ".mp4", ".m4v", ".mpg", ".mpeg", ".avi", ".qt", ".3gp"}
CACHE = Path.home() / "Library/Caches/reforce/dance_sync"
ANALYSIS_W = 256
NBINS = 8
PRIOR_BPM = 115.0
PRIOR_OCTAVES = 0.8  # std dev of the log2 tempo prior
RATE = 50.0  # Hz of the resampled motion signals
BAND_WEIGHTS = np.array([0.2, 0.4, 0.4])  # upper body, hips, legs
VERSION = 2
RIFE_NAME = re.compile(r"\.rife\d+x$", re.I)  # "<stem>.rife4x.mov" from interpolate.py


# ---------------------------------------------------------------- media io

def probe(path: Path) -> dict:
    r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                        "stream=width,height,r_frame_rate:format=duration", "-of", "json", str(path)],
                       capture_output=True, text=True, check=True)
    j = json.loads(r.stdout)
    s = j["streams"][0]
    num, den = (int(x) for x in s["r_frame_rate"].split("/"))
    return {"w": int(s["width"]), "h": int(s["height"]), "fps": num / den,
            "duration": float(j["format"]["duration"])}


def frames(path: Path, w: int, h: int, gray: bool, start: float = 0.0, end: float | None = None):
    """Yields frames scaled to w x h (gray uint8 or BGR) via an ffmpeg pipe."""
    cmd = ["ffmpeg", "-v", "error"]
    if start > 0:
        cmd += ["-ss", f"{start:.4f}"]
    cmd += ["-i", str(path)]
    if end is not None:
        cmd += ["-t", f"{end - start:.4f}"]
    cmd += ["-vf", f"scale={w}:{h}:flags=area", "-pix_fmt", "gray" if gray else "bgr24", "-f", "rawvideo", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    n = w * h * (1 if gray else 3)
    shape = (h, w) if gray else (h, w, 3)
    try:
        while True:
            buf = p.stdout.read(n)
            if len(buf) < n:
                break
            yield np.frombuffer(buf, np.uint8).reshape(shape)
    finally:
        p.stdout.close()
        p.wait()


def even(x: float) -> int:
    return max(2, int(round(x / 2)) * 2)


# ---------------------------------------------------------------- motion -> onset envelopes

def is_silhouette(path: Path, info: dict) -> tuple[str, float] | None:
    """A solid figure on a plain background, either polarity: ("light", threshold) for a white figure on black,
    ("dark", threshold) for a black figure on a light background, else None. Per sampled frame, Otsu's threshold
    splits the image in two; the smaller part is the figure when it is near-solid black (mean < 30) or white
    (> 225) and covers 2-35% of the frame, with the same polarity in most samples."""
    w = 128
    h = even(info["h"] * w / info["w"])
    rows = []
    for i, f in enumerate(frames(path, w, h, gray=True, end=min(info["duration"], 30.0))):
        if i % 10:
            continue
        T, _ = cv2.threshold(f, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
        lo, hi = f[f <= T], f[f > T]
        if not len(lo) or not len(hi):
            continue
        dark = len(lo) < len(hi)
        fig = lo if dark else hi
        rows.append((dark, len(fig) / f.size, float(fig.mean()), float(T)))
    if len(rows) < 3:
        return None
    r = np.array(rows, float)
    dark_share = r[:, 0].mean()
    if max(dark_share, 1 - dark_share) < 0.8:
        return None
    dark = dark_share >= 0.5
    sel = r[:, 0] == (1.0 if dark else 0.0)
    frac, mean, T = (float(np.median(r[sel, j])) for j in (1, 2, 3))
    if not 0.02 <= frac <= 0.35:
        return None
    if dark and mean < 30:
        return ("dark", T)
    if not dark and mean > 225:
        return ("light", T)
    return None


def motion_features(path: Path, info: dict, start: float, end: float | None, silhouette,
                    retime: bool = True) -> dict:
    """Optical flow between consecutive *distinct* frames (repeated frames are skipped and real timestamps kept).
    Per pair, at the pair's mid time: directogram velocity (NBINS, image widths/s) and the body's vertical velocity
    in three bands relative to the body's own extent (upper, hips, legs; body heights/s, + = down)."""
    fps = info["fps"]
    w = ANALYSIS_W
    h = even(info["h"] * w / info["w"])
    dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
    rows = np.arange(h, dtype=np.float32)[:, None]
    t_mid, dirs, bands, cuts, dts, pairs = [], [], [], [], [], []
    prev = prev_hist = None
    prev_i = 0
    recent: list[float] = []
    dups = 0
    n = 0
    for i, f in enumerate(frames(path, w, h, gray=True, start=start, end=end)):
        n = i + 1
        hist = cv2.calcHist([f], [0], None, [32], [0, 256])
        cv2.normalize(hist, hist)
        if prev is None:
            prev, prev_hist, prev_i = f, hist, i
            continue
        diff = float(cv2.absdiff(prev, f).mean())
        if diff < max(0.25, 0.08 * (float(np.median(recent)) if recent else 0.0)):
            dups += 1
            continue  # a repeated frame: no new motion, keep waiting for the next real one
        recent = (recent + [diff])[-25:]
        dt = (i - prev_i) / fps
        cut = (cv2.compareHist(prev_hist, hist, cv2.HISTCMP_CORREL) < 0.7
               or (len(recent) > 8 and diff > 6 * float(np.median(recent[:-1]))))
        flow = dis.calc(prev, f, None)
        fx = flow[..., 0] - np.median(flow[..., 0])
        fy = flow[..., 1] - np.median(flow[..., 1])
        mag = np.hypot(fx, fy)
        b = ((np.arctan2(fy, fx) + math.pi) * (NBINS / (2 * math.pi))).astype(np.int32) % NBINS
        dirs.append(np.bincount(b.ravel(), weights=mag.ravel(), minlength=NBINS) / mag.size / w / dt)
        if silhouette:  # ("light" | "dark", threshold): the figure itself is the body mask
            kind, thr = silhouette
            mask = ((f > thr) | (prev > thr)) if kind == "light" else ((f < thr) | (prev < thr))
        else:  # where things move: a rough body mask
            mask = mag > max(0.15, float(np.percentile(mag, 75)))
            mask = cv2.dilate(mask.astype(np.uint8), np.ones((5, 5), np.uint8)) > 0
        vy = [0.0, 0.0, 0.0]
        ys = np.flatnonzero(mask.any(axis=1))
        if len(ys) > 3 and mask.sum() > 50 and not cut:
            top, bot = ys[0], ys[-1]
            span = float(max(8, bot - top))
            rel = (rows - top) / span
            for j, (lo, hi) in enumerate(((0.0, 0.33), (0.33, 0.66), (0.66, 1.01))):
                sel = mask & (rel >= lo) & (rel < hi)
                if sel.sum() > 10:
                    vy[j] = float(fy[sel].mean()) / span / dt
        bands.append(vy)
        cuts.append(cut)
        t_mid.append(start + (prev_i + i) / 2 / fps)
        dts.append(dt)
        pairs.append((prev_i, i))
        prev, prev_hist, prev_i = f, hist, i
    t_mid = np.array(t_mid)
    dts = np.array(dts)
    dirs = np.array(dirs).reshape(-1, NBINS)
    bands = np.array(bands).reshape(-1, 3)
    cuts = np.array(cuts, bool)
    pairs = np.array(pairs).reshape(-1, 2)
    content_fps, cadence, gap_units = None, None, None
    # content frame k first appears at c[k]; pair k spans c[k]..c[k+1]
    c = (start + np.concatenate([[pairs[0][0]], pairs[:, 1]]) / fps) if len(pairs) else np.zeros(1)
    raw = {"t_raw": t_mid.copy(), "dts": dts.copy(), "disp": dirs.sum(axis=1) * dts, "pairs": pairs}
    if retime and len(t_mid) > 30:
        # 1. Repeated frames (15 -> 23.976, 25 -> 29.97, ...): first-appearance times wobble by up to a frame in a
        #    repeating pattern. Where the real frames lie on a line locally (a sliding window that never crosses a
        #    cut or a long hold), they were evenly spaced: use the line. Elsewhere keep the raw times.
        slope = None
        if dups > 0.05 * n:
            c, slope, ok = local_even(c, cuts, fps)
            if ok.mean() > 0.5:
                content_fps = 1.0 / float(np.median(slope[ok[:-1]]))
            else:
                slope = None
        # 2. Uneven spacing inside the content itself (e.g. 25 -> 15 keeps 3 of 5 frames: gaps 2,2,1): smooth motion
        #    moves in proportion to the gap, so a strong repeating per-frame motion pattern gives the true gaps.
        k = np.arange(len(t_mid))
        disp = dirs.sum(axis=1) * dts
        rel = disp / (uniform_filter1d(disp, 9, mode="nearest") + 1e-12)
        # A real cadence looks the same in every quarter of the clip; motion that happens to repeat every P frames
        # (e.g. a bounce of ~8 frames) drifts against the frame grid and doesn't.
        best = None
        quarters = np.array_split(np.arange(len(rel)), 4)
        for P in range(2, 9):
            prof = np.array([np.median(rel[(k % P) == j]) for j in range(P)])
            contrast = prof.max() / max(1e-6, prof.min())
            if contrast <= 1.35 or len(rel) < 16 * P:
                continue
            steady = all(np.corrcoef(prof, [np.median(rel[q][(q % P) == j]) for j in range(P)])[0, 1] > 0.8
                         for q in quarters)
            if steady and (best is None or contrast > 1.1 * best[1]):
                best = (P, contrast, prof)
        if best is not None:
            P, _, prof = best
            prof = snap_cadence(prof / prof.mean())
            cadence = [round(float(x), 2) for x in prof]
            gap_units = np.rint(prof / prof.min()).astype(int)
            # true position of content frame k on an even grid: sum of the relative gaps before it
            dev = np.concatenate([[0.0], np.cumsum(prof)[:-1]]) - np.arange(P)
            dev -= dev.mean()
            step = np.concatenate([slope, slope[-1:]]) if slope is not None else np.gradient(c)
            c = c + step * dev[np.arange(len(c)) % P]
        if slope is not None or best is not None:
            t_mid = 0.5 * (c[:-1] + c[1:])
            true_dt = np.maximum(np.diff(c), 1e-4)
            scale = (dts / true_dt)[:, None]  # velocities were displacement / measured gap
            dirs = dirs * scale
            bands = bands * scale
    return {"t": t_mid, "dirs": dirs, "bands": bands, "cuts": cuts, "dups": dups, "frames": n,
            "silhouette": silhouette, "content_fps": content_fps, "cadence": cadence, "gap_units": gap_units,
            "c": c, **raw}


def local_even(c: np.ndarray, cuts: np.ndarray, fps: float, win: int = 31, tol_frames: float = 0.6):
    """Content frame times c (first appearances) -> locally linear times. Segments break at cuts and at holds
    (gaps > 3 output frames); in each, a sliding window of `win` frames is fitted with a line, and frame i takes the
    line of the window centred on it (clamped inside the segment) when that window's residual std is below
    tol_frames. Returns (times, per-pair spacing, per-frame ok)."""
    n = len(c)
    out = c.copy()
    ok = np.zeros(n, bool)
    slope = np.full(n, np.nan)
    gaps = np.diff(c)
    brk = np.flatnonzero(cuts[: n - 1] | (gaps > 3.0 / fps)) + 1  # a segment starts at each break
    starts = np.concatenate([[0], brk])
    ends = np.concatenate([brk, [n]])
    for s0, s1 in zip(starts, ends):
        L = s1 - s0
        if L < 8:
            continue
        W = min(win, L)
        y = c[s0:s1]
        x = np.arange(W, dtype=float)
        xm = x.mean()
        sxx = ((x - xm) ** 2).sum()
        Y = np.lib.stride_tricks.sliding_window_view(y, W)  # (L-W+1, W)
        ym = Y.mean(axis=1)
        b = ((Y - ym[:, None]) * (x - xm)).sum(axis=1) / sxx
        a = ym - b * xm
        res = np.sqrt(((Y - (a[:, None] + b[:, None] * x)) ** 2).mean(axis=1))
        i = np.arange(L)
        w0 = np.clip(i - W // 2, 0, L - W)
        good = res[w0] < tol_frames / fps
        fit = a[w0] + b[w0] * (i - w0)
        out[s0:s1] = np.where(good, fit, y)
        ok[s0:s1] = good
        slope[s0:s1] = b[w0]
    pair_slope = np.diff(out)
    med = np.nanmedian(slope) if np.any(np.isfinite(slope)) else 1.0 / fps
    pair_slope = np.where(ok[:-1] & ok[1:], np.nan_to_num(slope[:-1], nan=med), pair_slope)
    return out, pair_slope, ok


def snap_cadence(prof: np.ndarray) -> np.ndarray:
    """Rate conversions drop whole source frames, so true gaps are small whole numbers (25 -> 15 fps: 2,2,1).
    Snap a measured relative-gap profile to the closest pattern of 1- and 2-frame gaps; keep it if none is close."""
    import itertools
    best, err = prof, 0.15
    for gaps in itertools.product((1, 2), repeat=len(prof)):  # (P <= 8: 256 patterns)
        g = np.array(gaps, float)
        g /= g.mean()
        e = float(np.sqrt(np.mean((g - prof) ** 2)))
        if e < err:
            best, err = g, e
    return best


def resample(t: np.ndarray, x: np.ndarray, grid: np.ndarray) -> np.ndarray:
    if x.ndim == 1:
        return np.interp(grid, t, x)
    return np.stack([np.interp(grid, t, x[:, j]) for j in range(x.shape[1])], 1)


def _norm(e: np.ndarray, rate: float) -> np.ndarray:
    """Divide by the local level (2 s) so quiet passages count, then scale so the 99th percentile is 1."""
    local = uniform_filter1d(e, size=max(3, int(2 * rate)), mode="nearest")
    e = e / (local + 0.1 * (np.mean(e) + 1e-12) + 1e-12)
    e = np.maximum(0.0, e - median_filter(e, size=int(rate * 0.5) | 1, mode="nearest"))
    top = np.percentile(e, 99) if np.any(e > 0) else 1.0
    return np.clip(e / (top + 1e-12), 0.0, 1.5)


def envelopes(m: dict, rate: float, start: float, dur: float) -> dict:
    """Two onset envelopes on a uniform grid (rate Hz from start):
    hits   - summed per-direction deceleration (a movement stopping or reversing), Davis & Agrawala style
    bounce - upward acceleration of the body's vertical motion (the bottom of a bounce), legs/hips weighted
    plus the bounce velocity itself (its fundamental is a clean tempo cue)."""
    grid = start + np.arange(int(dur * rate)) / rate
    if len(m["t"]) < 4:
        z = np.zeros(len(grid))
        return {"grid": grid, "hits": z, "bounce": z, "vel": z, "cutmask": z.astype(bool)}
    dirs = gaussian_filter1d(resample(m["t"], m["dirs"], grid), 1.5, axis=0)
    hits = np.maximum(0.0, -np.gradient(dirs, axis=0) * rate).sum(axis=1)
    vel = resample(m["t"], m["bands"] @ BAND_WEIGHTS, grid)
    vel = gaussian_filter1d(vel, 2.0)
    bounce = np.maximum(0.0, -np.gradient(vel) * rate)  # + = moving down, so -dv/dt > 0 at the bottom
    cutmask = np.zeros(len(grid), bool)
    for tc in m["t"][m["cuts"]]:
        cutmask |= np.abs(grid - tc) < 0.12
    hits[cutmask] = 0.0
    bounce[cutmask] = 0.0
    vel[cutmask] = 0.0
    return {"grid": grid, "hits": _norm(hits, rate), "bounce": _norm(bounce, rate), "vel": vel, "cutmask": cutmask}


# ---------------------------------------------------------------- tempo + grid

def tempogram(env: np.ndarray, rate: float, bpms: np.ndarray) -> np.ndarray:
    """Complex Fourier coefficient of the envelope at each tempo (and the frequency's phase)."""
    t = np.arange(len(env)) / rate
    e = env - env.mean()
    out = np.empty(len(bpms), complex)
    for i0 in range(0, len(bpms), 128):  # chunked to bound memory
        f = bpms[i0:i0 + 128, None] / 60.0
        out[i0:i0 + 128] = np.exp(-2j * math.pi * f * t[None, :]) @ e
    return out / max(1, len(env))


def tempo_scores(env: np.ndarray, vel: np.ndarray | None, rate: float, lo: float, hi: float, step: float = 0.05):
    bpms = np.arange(lo, hi + 1e-9, step)
    s1 = np.abs(tempogram(env, rate, bpms))
    s2 = np.abs(tempogram(env, rate, bpms * 2))
    s3 = np.abs(tempogram(env, rate, bpms * 3))
    raw = s1 + 0.5 * s2 + 0.33 * s3
    raw = raw / (np.median(raw) + 1e-12)
    if vel is not None and np.any(vel):
        v = np.abs(tempogram(vel, rate, bpms))  # one bounce per beat: the velocity's fundamental
        raw = raw + 0.7 * v / (np.median(v) + 1e-12)
    prior = np.exp(-0.5 * (np.log2(bpms / PRIOR_BPM) / PRIOR_OCTAVES) ** 2)
    return bpms, raw, raw * prior


def peaks_of(x: np.ndarray, bpms: np.ndarray, count: int) -> list[int]:
    idx = [i for i in range(1, len(x) - 1) if x[i] >= x[i - 1] and x[i] > x[i + 1]]
    idx.sort(key=lambda i: -x[i])
    picked: list[int] = []
    for i in idx:  # skip near-duplicates (within 1.5 BPM)
        if all(abs(bpms[i] - bpms[j]) > 1.5 for j in picked):
            picked.append(i)
        if len(picked) == count:
            break
    return picked


def first_phase(env: np.ndarray, rate: float, bpm: float) -> float:
    """Time of the first beat (0 <= t0 < period) of the cosine that best matches the envelope at this tempo."""
    c = tempogram(env, rate, np.array([bpm]))[0]
    period = 60.0 / bpm
    return (-math.atan2(c.imag, c.real) / (2 * math.pi) * period) % period


def sub_sample_peak(env: np.ndarray, i: int) -> float:
    if 0 < i < len(env) - 1:
        a, b, c = env[i - 1], env[i], env[i + 1]
        d = a - 2 * b + c
        if d < 0:
            return i + 0.5 * (a - c) / d
    return float(i)


def _peak_near(env: np.ndarray, rate: float, tp: float, period: float) -> tuple[float, float] | None:
    n = len(env)
    lo = max(0, int(math.floor((tp - 0.3 * period) * rate)))
    hi = min(n - 1, int(math.ceil((tp + 0.3 * period) * rate)))
    if hi <= lo:
        return None
    i = lo + int(np.argmax(env[lo:hi + 1]))
    if env[i] < 0.12:
        return None
    return sub_sample_peak(env, i) / rate, float(env[i])


def fit_grid(env: np.ndarray, rate: float, bpm: float, t0: float) -> dict:
    """Tempo is fixed (from the whole-clip tempogram, which uses every sample and so isn't biased by coarse frame
    timing); this only sets the phase: snap each predicted beat to the nearest envelope peak and take the robust
    weighted mean offset."""
    period = 60.0 / bpm
    dur = len(env) / rate
    for _ in range(3):
        res, ws = [], []
        for k in range(math.ceil(-t0 / period), math.floor((dur - t0) / period) + 1):
            tp = t0 + k * period
            pk = _peak_near(env, rate, tp, period)
            if pk:
                res.append(pk[0] - tp)
                ws.append(pk[1])
        if len(res) < 4:
            break
        r = np.array(res)
        w = np.array(ws)
        keep = np.abs(r - np.median(r)) < 0.15 * period
        if keep.sum() < 4:
            break
        shift = float(np.sum(r[keep] * w[keep]) / np.sum(w[keep]))
        t0 = (t0 + shift) % period
        if abs(shift) < 1e-4:
            break
    predicted = int(math.floor((dur - t0) / period)) + 1
    res, peaks = [], []
    for k in range(predicted):
        tp = t0 + k * period
        pk = _peak_near(env, rate, tp, period)
        if pk:
            peaks.append(round(pk[0], 4))
            if abs(pk[0] - tp) < 0.15 * period:
                res.append(pk[0] - tp)
    return {"bpm": bpm, "t0": t0, "hit_rate": len(res) / max(1, predicted),
            "jitter_ms": float(np.std(res)) * 1000 if res else None, "peaks": peaks}


def local_phase(env: np.ndarray, rate: float, bpm: float, t0: float, win: float = 8.0, hop: float = 1.0):
    """Per window: how far (in beats, -0.5..0.5) the local best phase sits from the global grid, and strength."""
    period = 60.0 / bpm
    w = int(round(win * rate))
    if len(env) < w:
        return []
    basis = np.exp(-2j * math.pi * (bpm / 60.0) * np.arange(w) / rate) * np.hanning(w)
    out = []
    for s in range(0, len(env) - w + 1, max(1, int(round(hop * rate)))):
        seg = env[s:s + w] - env[s:s + w].mean()
        c = basis @ seg
        tl = s / rate + (-math.atan2(c.imag, c.real) / (2 * math.pi)) * period
        d = ((tl - t0) / period + 0.5) % 1.0 - 0.5
        out.append({"t": round((s + w / 2) / rate, 2), "drift_beats": round(float(d), 3),
                    "strength": round(float(abs(c) / w), 4)})
    return out


# Signal mixes (bounce, hits). auto fits both "bounce" (bounce-led) and "hits" and keeps the more confident (on a
# silhouette, bounce unless hits leads by SILHOUETTE_BOUNCE_LEAD): smooth
# swaying dancers (luma) are carried by the bounce, sharp hit-driven ones (silhouette, static) by the hits.
MIXES = {"bounce": (0.7, 0.3), "hits": (0.0, 1.0), "bounce-only": (1.0, 0.0)}
SIGNALS = ("auto", "bounce", "hits", "bounce-only")
SILHOUETTE_BOUNCE_LEAD = 0.2  # hits must beat bounce by this much on a silhouette (auto)


def fit_tempo(ev: dict, wb: float, wh: float, rng: tuple[float, float]) -> dict:
    """Tempo, grid and confidence for one signal mix."""
    env = np.clip(wb * ev["bounce"] + wh * ev["hits"], 0.0, 1.5)
    vel = ev["vel"] if wb > 0 else None
    lo, hi = rng
    bpms, raw, scored = tempo_scores(env, vel, RATE, lo, hi)
    top = peaks_of(scored, bpms, 5) or [int(np.argmax(scored))]
    floor = float(np.median(scored)) + 1e-12
    candidates = [{"bpm": round(float(bpms[i]), 2), "score": round(float(scored[i] / floor), 2)} for i in top]
    coarse = float(bpms[top[0]])
    fine_b, _, fine_s = tempo_scores(env, vel, RATE, coarse - 0.1, coarse + 0.1, 0.002)
    best = float(fine_b[int(np.argmax(fine_s))])
    g = fit_grid(env, RATE, best, first_phase(env, RATE, best))
    period = 60.0 / g["bpm"]
    # Sharpness: best score vs the best candidate that is not a simple ratio (1/2, 2/3, 1, 3/2, 2) of it.
    second = next((c["score"] for c in candidates[1:]
                   if min(abs(math.log2(c["bpm"] / best) - r) for r in (-1, 0, 1, math.log2(1.5), -math.log2(1.5))) > 0.05), 1.0)
    sharp = candidates[0]["score"] / max(1e-9, second)
    jit = g["jitter_ms"] if g["jitter_ms"] is not None else 999.0
    confidence = max(0.0, min(1.0, 0.5 * g["hit_rate"] + 0.25 * min(1.0, (sharp - 1) / 0.6)
                              + 0.25 * (1.0 - min(1.0, jit / (0.25 * period * 1000)))))
    return {"env": env, "g": g, "candidates": candidates, "confidence": confidence}


BPM_IN_NAME = re.compile(r"(\d{2,3}(?:\.\d+)?)\s*bpm", re.I)  # "beeple_harvest_128bpm"


def loop_seam(path: Path, info: dict) -> float:
    """How the jump from the last frame back to the first compares with the clip's own frame-to-frame steps
    (95th percentile): <= ~1.5 is a seamless loop (VJ loops), much more is a cut."""
    w = 96
    h = even(info["h"] * w / info["w"])
    first = prev = None
    steps = []
    for f in frames(path, w, h, gray=True):
        f = f.astype(np.int16)
        if first is None:
            first = f
        elif prev is not None:
            steps.append(float(np.abs(f - prev).mean()))
        prev = f
    if first is None or len(steps) < 10:
        return float("inf")
    typical = float(np.percentile(steps, 95)) + 0.05
    return float(np.abs(prev - first).mean()) / typical


def analyse(path: Path, args) -> dict:
    info = probe(path)
    start = args.start or 0.0
    end = args.end if args.end else None
    sil = is_silhouette(path, info)
    m = motion_features(path, info, start, end, sil)
    dur = (min(end, info["duration"]) if end else info["duration"]) - start
    ev = envelopes(m, RATE, start, dur)
    hint = BPM_IN_NAME.search(path.stem)
    hint = float(hint.group(1)) if hint and 40 <= float(hint.group(1)) <= 250 else None
    rng = (args.bpm * 0.96, args.bpm * 1.04) if args.bpm else (hint * 0.97, hint * 1.03) if hint else args.range
    names = ["bounce", "hits"] if args.signal == "auto" else [args.signal]
    fits = {n: fit_tempo(ev, *MIXES[n], rng) for n in names}
    chosen = max(fits, key=lambda n: fits[n]["confidence"])
    # On a silhouette the bounce is measured on the real figure, so it keeps the clip unless hits is clearly more
    # confident (luma_5: a stray hits fit at 105.2, conf .56, vs the right bounce 90.35, .46; silhouette's real hits
    # fit leads by .39). Elsewhere the body mask is approximate and the more confident fit wins (static's right hits
    # fit leads by only .085).
    if sil and len(fits) == 2 and chosen == "hits" \
            and fits["hits"]["confidence"] - fits["bounce"]["confidence"] < SILHOUETTE_BOUNCE_LEAD:
        chosen = "bounce"
    f = fits[chosen]
    env, g, candidates, confidence = f["env"], f["g"], f["candidates"], f["confidence"]
    # A seamless loop made to a tempo (VJ loops): when the clip lasts a whole number of bars at the found tempo,
    # the length gives the tempo exactly (N beats in the clip's exact frame time), so it loops in sync forever.
    loop = {"seamless": False, "wrap": None, "beats": None, "locked": False, "bpm_in_name": hint}
    if not args.start and not args.end and not args.bpm and info["duration"] < 180:
        wrap = loop_seam(path, info)
        loop["wrap"] = round(wrap, 2) if np.isfinite(wrap) else None
        loop["seamless"] = bool(wrap <= 1.5)
        exact = m["frames"] / info["fps"]
        beats = g["bpm"] * exact / 60.0
        n = int(round(beats / 4.0)) * 4
        if loop["seamless"] and n >= 4 and abs(beats - n) < 0.25:
            locked = n * 60.0 / exact
            g = fit_grid(f["env"], RATE, locked, first_phase(f["env"], RATE, locked))
            loop.update(beats=n, locked=True)
    bpm = g["bpm"]
    period = 60.0 / bpm
    t0 = (g["t0"] + args.offset_ms / 1000.0 + args.offset_beats * period) % period
    beat_start = t0 + (args.downbeat % args.beats_per_bar) * period if args.downbeat is not None else t0
    grid = [round(start + t0 + k * period, 4) for k in range(int((dur - t0) / period) + 1)]
    return {
        "version": VERSION,
        "file": str(path),
        "fps": info["fps"],
        "duration": info["duration"],
        "analysed": [start, start + dur],
        "silhouette": sil[0] if sil else None,
        "duplicate_frames": m["dups"],
        "content_fps": round(m["content_fps"], 3) if m["content_fps"] else None,
        "cadence": m["cadence"],
        "loop": loop,
        "frames": m["frames"],
        "signal": chosen if args.signal != "auto" else f"auto:{chosen}",
        "alternatives": {n: {"bpm": round(fits[n]["g"]["bpm"], 3), "confidence": round(fits[n]["confidence"], 3)}
                         for n in fits if n != chosen},
        "bpm": round(bpm, 3),
        "beat_start": round(start + beat_start, 4),
        "beats_per_bar": args.beats_per_bar,
        "offset_ms": args.offset_ms,
        "offset_beats": args.offset_beats,
        "confidence": round(confidence, 3),
        "hit_rate": round(g["hit_rate"], 3),
        "jitter_ms": round(g["jitter_ms"], 1) if g["jitter_ms"] is not None else None,
        "candidates": candidates,
        "beats": grid,
        "peaks": [round(start + p, 4) for p in g["peaks"]],
        "cuts": [round(float(t), 3) for t in m["t"][m["cuts"]]],
        "local_phase": local_phase(env, RATE, bpm, t0),
        "envelope_start": start,
        "envelope_rate": RATE,
        "envelope": [round(float(x), 3) for x in env],
        "bounce": [round(float(x), 3) for x in ev["bounce"]],
        "hits": [round(float(x), 3) for x in ev["hits"]],
    }


# ---------------------------------------------------------------- outputs

def cache_json(path: Path) -> Path:
    return CACHE / f"{path.stem.strip()}.beats.json"


def options_path(movie: Path) -> Path:
    """An interpolated "<stem>.rife4x.mov" shares its original's "<stem>.reforce.txt"."""
    return movie.with_name(RIFE_NAME.sub("", movie.stem) + ".reforce.txt")


def write_options(movie: Path, values: dict[str, str]) -> Path:
    """Sets keys in "<stem>.reforce.txt", keeping every other line (same format as tools/media_review)."""
    f = options_path(movie)
    lines = f.read_text(errors="replace").splitlines() if f.exists() else []
    lower = {k.lower() for k in values}
    keep = [l for l in lines if l.split("//", 1)[0].split("=", 1)[0].strip().lower() not in lower]
    notes = {"BPM": "dancers' tempo (tools/dance_sync)", "BeatStart": "seconds of a downbeat",
             "BeatsPerBar": "beats per bar"}
    for k, v in values.items():
        keep.append(f"{k} = {v}  // {notes.get(k, 'reforce')}")
    f.write_text("\n".join(keep) + "\n")
    return f


def click_track(beats: list[float], downbeat: float, period: float, bpb: int, dur: float, wav: Path) -> None:
    sr = 44100
    out = np.zeros(int(dur * sr) + sr, np.float32)
    n = int(0.03 * sr)
    tt = np.arange(n) / sr
    env = np.exp(-tt * 140)
    for t in beats:
        accent = round((t - downbeat) / period) % bpb == 0
        tone = np.sin(2 * math.pi * (2000 if accent else 1200) * tt) * env * (0.9 if accent else 0.55)
        i = int(t * sr)
        if 0 <= i < len(out) - n:
            out[i:i + n] += tone
    pcm = (np.clip(out, -1, 1) * 32000).astype(np.int16)
    with wave.open(str(wav), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())


def render_preview(path: Path, r: dict, out: Path) -> None:
    info = probe(path)
    fps = info["fps"]
    vw = min(960, even(info["w"]))
    vh = even(info["h"] * vw / info["w"])
    strip = 110
    H = vh + strip
    bpm, period, bpb = r["bpm"], 60.0 / r["bpm"], r["beats_per_bar"]
    down = r["beat_start"]
    beats = np.array(r["beats"])
    peaks = np.array(r["peaks"])
    curves = [(np.array(r["hits"]), (120, 230, 120)), (np.array(r["bounce"]), (60, 160, 255))]
    e0, rate = r["envelope_start"], r["envelope_rate"]
    wav = out.with_suffix(".click.wav")
    click_track(r["beats"], down, period, bpb, info["duration"], wav)
    enc = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "bgr24", "-s", f"{vw}x{H}",
         "-r", f"{fps:.6f}", "-i", "-", "-i", str(wav), "-map", "0:v", "-map", "1:a",
         "-c:v", "h264_videotoolbox", "-b:v", "8M", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "128k",
         "-shortest", "-movflags", "+faststart", str(out)],
        stdin=subprocess.PIPE)
    span = 2.0  # seconds either side of the playhead in the strip
    font = cv2.FONT_HERSHEY_SIMPLEX
    flash_len = min(0.18, 0.4 * period)
    for i, f in enumerate(frames(path, vw, vh, gray=False)):
        t = i / fps
        canvas = np.zeros((H, vw, 3), np.uint8)
        canvas[:vh] = f
        # beat state
        k = math.floor((t - down) / period + 1e-6)
        since = t - (down + k * period)
        num = k % bpb + 1
        is_down = num == 1
        inside = r["analysed"][0] - 1e-6 <= t <= r["analysed"][1] + 1e-6
        if inside and since < flash_len:
            a = 1.0 - since / flash_len
            col = np.array((40, 40, 255) if is_down else (255, 220, 60), float)
            th = max(6, vw // 60)
            for sl in ((slice(0, th), slice(None)), (slice(vh - th, vh), slice(None)),
                       (slice(0, vh), slice(0, th)), (slice(0, vh), slice(vw - th, vw))):
                region = canvas[sl].astype(float)
                canvas[sl] = (region * (1 - a) + col * a).astype(np.uint8)
        # beat counter: 4 dots, current one lit
        for j in range(bpb):
            cx = 30 + j * 34
            lit = inside and j == num - 1
            c = ((40, 40, 255) if j == 0 else (255, 220, 60)) if lit else (90, 90, 90)
            cv2.circle(canvas, (cx, 34), 13 if lit else 9, c, -1, cv2.LINE_AA)
        label = f"{bpm:.2f} BPM  conf {r['confidence']:.2f}  matched {r['hit_rate'] * 100:.0f}%  [{r['signal']}]"
        cv2.putText(canvas, label, (18 + bpb * 34, 42), font, 0.6, (0, 0, 0), 4, cv2.LINE_AA)
        cv2.putText(canvas, label, (18 + bpb * 34, 42), font, 0.6, (255, 255, 255), 1, cv2.LINE_AA)
        # motion strip
        y0, y1 = vh + 8, H - 8
        sx = lambda tt: int(round((tt - t + span) / (2 * span) * (vw - 1)))
        sy = lambda v: int(round(y1 - min(1.2, v) / 1.2 * (y1 - y0)))
        for b in beats[(beats > t - span) & (beats < t + span)]:
            kb = round((b - down) / period)
            dn = kb % bpb == 0
            cv2.line(canvas, (sx(b), y0), (sx(b), y1), (40, 40, 220) if dn else (110, 95, 40), 2 if dn else 1)
        lo = max(0, int((t - span - e0) * rate))
        for env, col in curves:
            hi = min(len(env), int((t + span - e0) * rate) + 2)
            if hi - lo > 1:
                pts = np.array([[sx(e0 + j / rate), sy(env[j])] for j in range(lo, hi)], np.int32)
                cv2.polylines(canvas, [pts], False, col, 1, cv2.LINE_AA)
        for p in peaks[(peaks > t - span) & (peaks < t + span)]:
            x = sx(p)
            cv2.fillConvexPoly(canvas, np.array([[x - 5, y0 - 6], [x + 5, y0 - 6], [x, y0 + 2]], np.int32),
                               (255, 255, 255), cv2.LINE_AA)
        cv2.line(canvas, (vw // 2, y0 - 4), (vw // 2, y1 + 4), (255, 255, 255), 1)
        cv2.putText(canvas, "bounce (orange), hits (green), matched peaks (white), beat grid (lines; red = downbeat)",
                    (8, H - 4), font, 0.38, (160, 160, 160), 1, cv2.LINE_AA)
        enc.stdin.write(canvas.tobytes())
    enc.stdin.close()
    enc.wait()
    wav.unlink(missing_ok=True)


# ---------------------------------------------------------------- cli

def parse_time(v: str) -> float:
    total = 0.0
    for part in v.split(":"):
        total = total * 60 + float(part)
    return total


def parse_range(v: str) -> tuple[float, float]:
    a, b = (float(x) for x in v.split("-"))
    return min(a, b), max(a, b)


def collect(paths: list[str]) -> list[Path]:
    out = []
    for p in map(Path, paths):
        p = p.expanduser().absolute()  # absolute: names like "- bg_x.mov" would look like options to ffmpeg
        if p.is_dir():
            out += sorted(q for q in p.iterdir() if q.suffix.lower() in MOVIE_EXT and not q.name.startswith("._"))
        elif p.exists():
            out.append(p)
        else:
            print(f"not found: {p}", file=sys.stderr)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--preview", action="store_true")
    ap.add_argument("--no-open", action="store_true")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--bpm", type=float)
    ap.add_argument("--range", type=parse_range, default=(70.0, 180.0))
    ap.add_argument("--downbeat", type=int)
    ap.add_argument("--beats-per-bar", type=int, default=4)
    ap.add_argument("--offset-ms", type=float, default=0.0)
    ap.add_argument("--offset-beats", type=float, default=0.0)
    ap.add_argument("--signal", choices=SIGNALS, default="auto")
    ap.add_argument("--start", type=parse_time)
    ap.add_argument("--end", type=parse_time)
    args = ap.parse_args()
    CACHE.mkdir(parents=True, exist_ok=True)
    movies = collect(args.paths)
    if not movies:
        return 1
    print(f"{'clip':40} {'BPM':>8} {'conf':>5} {'hits':>5} {'jitter':>7}  alternatives")
    for m in movies:
        r = analyse(m, args)
        cache_json(m).write_text(json.dumps(r, separators=(",", ":")))
        alts = ", ".join(f"{c['bpm']:g}" for c in r["candidates"][1:4])
        jit = f"{r['jitter_ms']:.0f}ms" if r["jitter_ms"] is not None else "-"
        print(f"{m.name.strip()[:40]:40} {r['bpm']:8.2f} {r['confidence']:5.2f} {r['hit_rate'] * 100:4.0f}% {jit:>7}  {alts}",
              flush=True)
        if args.write:
            f = write_options(m, {"BPM": f"{r['bpm']:.3f}", "BeatStart": f"{r['beat_start']:.3f}",
                                  "BeatsPerBar": str(r["beats_per_bar"])})
            print(f"  wrote {f}")
        if args.preview:
            out = CACHE / f"{m.stem.strip()}.beats.mp4"
            render_preview(m, r, out)
            print(f"  preview {out}")
            if not args.no_open:
                subprocess.run(["open", "-a", "QuickTime Player", str(out)])
    return 0


if __name__ == "__main__":
    sys.exit(main())

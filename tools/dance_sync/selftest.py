#!/usr/bin/env python3
"""Synthetic check for dance_beats.py: renders clips of "dancers" that hit poses on a known beat grid
(with camera pan, sensor noise, a shot cut, and odd frame rates), runs the detector, and compares.

    tools/dance_sync/run selftest.py [--keep]

Pass: tempo within 0.1 BPM and grid phase within 12 ms (45 ms for the 15 fps bouncers), with no octave fixing.
"""
from __future__ import annotations

import argparse
import math
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import dance_beats as db  # noqa: E402

OUT = db.CACHE / "selftest"


def render(path: Path, bpm: float, t0: float, fps: float, dur: float, seed: int, cut_at: float | None) -> None:
    rng = np.random.default_rng(seed)
    w, h = 640, 360
    period = 60.0 / bpm
    n_limbs = 6
    poses = rng.uniform(-1, 1, (int(dur / period) + 4, n_limbs, 2)) * 45
    anchors = np.stack([rng.uniform(150, 490, n_limbs), rng.uniform(100, 260, n_limbs)], 1)
    bg = cv2.GaussianBlur((rng.random((h + 200, w + 300)) * 255).astype(np.uint8), (0, 0), 6)
    bg = cv2.normalize(bg, None, 40, 120, cv2.NORM_MINMAX)
    enc = subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "gray", "-s", f"{w}x{h}",
                            "-r", f"{fps:.6f}", "-i", "-", "-c:v", "libx264", "-crf", "20", "-pix_fmt", "yuv420p",
                            str(path)], stdin=subprocess.PIPE)
    for i in range(int(dur * fps)):
        t = i / fps
        b = (t - t0) / period
        k = math.floor(b)
        u = b - k
        ease = u * u  # accelerate into the pose, stop dead on the beat (a kinematic "hit")
        p0, p1 = poses[k + 2], poses[k + 3]
        pos = anchors + p0 + (p1 - p0) * ease
        pan = int(60 * math.sin(t * 0.4)) + 100
        if cut_at is not None and t >= cut_at:
            pan += 70
            pos = pos + np.array([40, -20])
        frame = bg[100:100 + h, pan:pan + w].copy()
        for (x, y) in pos:
            cv2.circle(frame, (int(round(x)), int(round(y))), 18, 235, -1, cv2.LINE_AA)
        frame = np.clip(frame.astype(np.int16) + rng.normal(0, 6, frame.shape), 0, 255).astype(np.uint8)
        enc.stdin.write(frame.tobytes())
    enc.stdin.close()
    enc.wait()


def render_bouncer(path: Path, bpm: float, t0: float, fps: float, dur: float, seed: int, cut_at: float | None) -> None:
    """A white silhouette on black that bounces (lowest point on each beat) and sways, shot at 25 fps, cut down to
    15 fps by keeping 3 of every 5 frames, then padded to the output rate by repeating frames (like the luma clips)."""
    rng = np.random.default_rng(seed)
    w, h = 640, 360
    period = 60.0 / bpm
    kept = [j for j in range(int(dur * 30) + 30) if j % 5 in (0, 2, 4)]  # gaps 2, 2, 1
    enc = subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "gray", "-s", f"{w}x{h}",
                            "-r", f"{fps:.6f}", "-i", "-", "-c:v", "libx264", "-crf", "18", "-pix_fmt", "yuv420p",
                            str(path)], stdin=subprocess.PIPE)
    arm = rng.uniform(0.5, 1.5)
    for i in range(int(dur * fps)):
        t_out = i / fps
        t = kept[int(t_out * 15 + 1e-9)] / 25  # the 15 fps file shows kept frame m at m/15 s
        ph = 2 * math.pi * (t - t0) / period
        scale = 1.0 if cut_at is None or t < cut_at else 1.4  # a zoom cut
        dy = 14 * math.cos(ph)  # + = down; lowest on the beat
        dx = 25 * math.sin(ph / 2)  # slow side-to-side sway, one way per beat
        frame = np.zeros((h, w), np.uint8)
        cx, cy = 320 + dx, 150 + dy

        def P(x, y):
            return (int(round(320 + (x - 320) * scale)), int(round(180 + (y - 180) * scale)))
        cv2.circle(frame, P(cx, cy - 70), int(16 * scale), 255, -1, cv2.LINE_AA)
        cv2.ellipse(frame, P(cx, cy), (int(26 * scale), int(52 * scale)), 0, 0, 360, 255, -1, cv2.LINE_AA)
        knee = 18 + dy * 0.8  # knees bend as the body drops
        for side in (-1, 1):
            cv2.line(frame, P(cx + side * 12, cy + 40), P(cx + side * (18 + knee * 0.3), cy + 95), 255, int(12 * scale))
            cv2.line(frame, P(cx + side * (18 + knee * 0.3), cy + 95), P(320 + side * 22, 300), 255, int(11 * scale))
            cv2.line(frame, P(cx + side * 20, cy - 35), P(cx + side * 60, cy - 10 + side * 20 * math.sin(ph * arm)),
                     255, int(9 * scale))
        enc.stdin.write(frame.tobytes())
    enc.stdin.close()
    enc.wait()


CASES = [  # renderer, bpm, first beat, output fps, seconds, cut, phase tolerance (ms)
    (render, 124.0, 0.21, 29.97, 40, None, 12),
    (render, 97.5, 0.40, 23.976, 45, 20.0, 12),
    (render, 140.0, 0.05, 25.0, 30, None, 12),
    (render, 110.3, 0.33, 30.0, 50, 31.0, 12),
    # content timing is quantised to 15 fps and shown up to a frame late, so allow a frame of phase error
    (render_bouncer, 90.0, 0.30, 2997 / 125, 60, None, 45),
    (render_bouncer, 113.0, 0.12, 2997 / 125, 60, 33.0, 45),
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="keep the rendered clips")
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    args = SimpleNamespace(start=None, end=None, range=(70.0, 180.0), bpm=None, offset_ms=0.0, offset_beats=0.0,
                           downbeat=None, beats_per_bar=4, signal="auto")
    ok = True
    for n, (rend, bpm, t0, fps, dur, cut, tol) in enumerate(CASES):
        p = OUT / f"synthetic_{n}.mp4"
        rend(p, bpm, t0, fps, dur, seed=n, cut_at=cut)
        r = db.analyse(p, args)
        period = 60 / bpm
        phase_ms = ((r["beat_start"] - t0) / period + 0.5) % 1.0 - 0.5
        phase_ms *= period * 1000
        good = abs(r["bpm"] - bpm) < 0.1 and abs(phase_ms) < tol
        ok &= good
        print(f"{'ok  ' if good else 'FAIL'} {bpm:6.1f} BPM @ {fps:6.3f} fps -> {r['bpm']:7.2f} BPM, "
              f"phase {phase_ms:+5.1f} ms, conf {r['confidence']:.2f}, matched {r['hit_rate'] * 100:.0f}%"
              f"{', silhouette' if r['silhouette'] else ''}{', cadence ' + str(r['cadence']) if r['cadence'] else ''}")
        if not a.keep:
            p.unlink()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

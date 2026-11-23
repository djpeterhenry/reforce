#!/usr/bin/env python3
"""Restores a clip's real frame timing and interpolates it to a higher frame rate with RIFE, for smooth slow playback.

    tools/dance_sync/run interpolate.py CLIP.mov [--factor 4] [--out DIR] [--compare] [--keep-temp]

Steps (each only when the clip needs it; dance_beats.py's analysis finds out):
  1. Content frames: repeated frames are dropped (e.g. 15 fps padded to 23.976 by repeating frames).
  2. Base timeline: an uneven pattern inside the content (e.g. 25 -> 15 fps kept 3 of every 5 frames: gaps 2,2,1)
     is undone by having RIFE make each dropped frame, the midpoint of a 2-gap, so the clip is back on its
     original even timeline (25 fps for the luma clips).
  3. --factor N: RIFE makes N-1 in-betweens per base frame (25 fps x 4 = 100 fps).
The output starts at the same moment and lasts as long as the original, so BPM / BeatStart stay valid.

Output: "<stem>.rife<N>x.mov" (HEVC 10-bit, high quality) beside the original (or in --out). reforce plays it in place of the
original and leaves it out of its movie lists (engine: interpolatedMovie). --compare also renders
"<stem>.compare.mp4" into the analysis cache: original | restored, side by side at half speed.
Originals are never modified.

RIFE: rife-ncnn-vulkan 20221029 (Vulkan via MoltenVK; model rife-v4.6) in tools/dance_sync/bin (git-ignored),
fetched by `tools/dance_sync/run interpolate.py --install`.
"""
from __future__ import annotations

import argparse
import hashlib
import math
import shutil
import subprocess
import sys
import tempfile
from fractions import Fraction
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import dance_beats as db  # noqa: E402

HERE = Path(__file__).parent
RIFE_TAG = "20221029"
RIFE_ZIP = f"rife-ncnn-vulkan-{RIFE_TAG}-macos.zip"
RIFE_SHA256 = "4a63a1f3c9c715773c57d2ee51df1b315ed20cd6c63103e45c483ecc4400b595"
RIFE_DIR = HERE / "bin" / f"rife-ncnn-vulkan-{RIFE_TAG}-macos"
MODEL = "rife-v4.6"
# Output: HEVC 10-bit, VideoToolbox (hardware) constant quality 85: ~10x smaller than ProRes LT at SSIM 0.999 against
# it (tested on dance_silhouette); 10-bit keeps smooth gradients from banding; "hvc1" so AVFoundation plays it.
HEVC = ["-c:v", "hevc_videotoolbox", "-profile:v", "main10", "-pix_fmt", "p010le", "-q:v", "85", "-tag:v", "hvc1"]
COMMON_RATES = [Fraction(24000, 1001), Fraction(24), Fraction(25), Fraction(30000, 1001), Fraction(30),
                Fraction(50), Fraction(60000, 1001), Fraction(60), Fraction(15000, 1001), Fraction(15), Fraction(12),
                Fraction(25, 2), Fraction(20)]


def install() -> None:
    if (RIFE_DIR / "rife-ncnn-vulkan").exists():
        print(f"already installed: {RIFE_DIR}")
        return
    dl = HERE / "bin" / "dl"
    dl.mkdir(parents=True, exist_ok=True)
    z = dl / RIFE_ZIP
    if not z.exists():
        subprocess.run(["gh", "release", "download", RIFE_TAG, "--repo", "nihui/rife-ncnn-vulkan", "--pattern",
                        RIFE_ZIP, "--dir", str(dl)], check=True)
    sha = hashlib.sha256(z.read_bytes()).hexdigest()
    if sha != RIFE_SHA256:
        sys.exit(f"checksum mismatch for {z}: {sha}")
    subprocess.run(["unzip", "-q", "-o", str(z), "-d", str(HERE / "bin")], check=True)
    print(f"installed {RIFE_DIR}")


def rife(src: Path, dst: Path, factor: int) -> int:
    """factor-1 in-betweens per input frame. rife-ncnn-vulkan puts output i at input position i * inputs / count
    (measured), so asking for exactly factor * inputs frames puts output i at i / factor; the frames past the last
    input are copies of it and are not used. Returns how many outputs are used: factor * (inputs - 1) + 1."""
    n_in = len(list(src.glob("*.png")))
    count = factor * n_in
    exe = RIFE_DIR / "rife-ncnn-vulkan"
    if not exe.exists():
        sys.exit("RIFE missing: run  tools/dance_sync/run interpolate.py --install")
    dst.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(exe), "-i", str(src), "-o", str(dst), "-m", str(RIFE_DIR / MODEL), "-n", str(count),
                    "-j", "2:4:4", "-f", "%08d.png"], check=True, stderr=subprocess.DEVNULL)
    return factor * (n_in - 1) + 1


def snap_rate(r: float) -> Fraction:
    """The closest common frame rate within 0.6% (measured rates are a little off: holds, slips), else r itself."""
    best = min(COMMON_RATES, key=lambda c: abs(float(c) - r))
    if abs(float(best) - r) / r < 0.006:
        return best
    return Fraction(r).limit_denominator(1001)


def plan(path: Path, info: dict) -> dict:
    """Which source frames are real content, the base timeline (real frames + frames a cadence dropped) and when
    each base frame belongs (dance_beats.motion_features: repeats skipped, locally even spacing, cadence undone)."""
    sil = db.is_silhouette(path, info)
    m = db.motion_features(path, info, 0.0, None, sil)
    pairs = m["pairs"]
    content = [int(pairs[0][0])] + [int(b) for _, b in pairs]  # source frame index of each real frame
    c = np.asarray(m["c"], float)  # when each real frame belongs
    gaps = np.ones(len(pairs), int)
    if m["gap_units"] is not None:
        units = np.asarray(m["gap_units"])
        gaps = units[np.arange(len(pairs)) % len(units)]
    times = []
    for k in range(len(content)):
        times.append(c[k])
        if k < len(pairs) and gaps[k] == 2:
            times.append(0.5 * (c[k] + c[k + 1]))  # the frame the cadence dropped
    # A seamless loop (VJ loops): the first frame follows the last one period later, so RIFE also fills the jump
    # back to the start and the seam plays as smoothly as the rest.
    seamless = info["duration"] < 180 and db.loop_seam(path, info) <= 1.5
    if seamless:
        times.append(c[0] + m["frames"] / info["fps"])
        gaps = np.append(gaps, 1)
    times = np.array(times)
    # the base timeline's rate: from the content rate (a whole-clip estimate) when known, else the median spacing
    if m["content_fps"]:
        units = gaps.mean() if len(gaps) else 1.0
        base_rate = snap_rate(m["content_fps"] * units)
    else:
        base_rate = snap_rate(1.0 / float(np.median(np.diff(times))))
    return {"content": content, "gaps": gaps, "base_times": times, "base_rate": base_rate, "seamless": seamless,
            "dups": m["dups"], "cadence": m["cadence"], "silhouette": sil, "frames": m["frames"]}


def extract(path: Path, info: dict, wanted: list[int], out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    want = {f: n for n, f in enumerate(wanted)}
    w, h = db.even(info["w"]), db.even(info["h"])
    for i, f in enumerate(db.frames(path, w, h, gray=False)):
        n = want.get(i)
        if n is not None:
            cv2.imwrite(str(out / f"{n + 1:08d}.png"), f)


def same(a: Path, b: Path) -> float:
    return float(np.abs(cv2.imread(str(a)).astype(int) - cv2.imread(str(b)).astype(int)).mean())


def interpolate(path: Path, factor: int, out_dir: Path | None, keep: bool, compare: bool) -> Path:
    info = db.probe(path)
    p = plan(path, info)
    n_content = len(p["content"])
    needs_base = bool((p["gaps"] > 1).any())
    print(f"{path.name.strip()}: {p['frames']} frames, {p['dups']} repeats, {n_content} real"
          f"{', cadence ' + str(p['cadence']) if p['cadence'] else ''} -> base {float(p['base_rate']):.3f} fps,"
          f" output {float(p['base_rate'] * factor):.3f} fps{', seamless loop' if p['seamless'] else ''}")
    tmp = Path(tempfile.mkdtemp(prefix="reforce_rife_", dir=db.CACHE))
    try:
        content_dir = tmp / "content"
        extract(path, info, p["content"], content_dir)
        if p["seamless"]:  # the first frame again after the last (see plan)
            shutil.copyfile(content_dir / "00000001.png", content_dir / f"{n_content + 1:08d}.png")
            n_content += 1
        # 2. base timeline: real frames, plus the midpoint of each 2-gap (RIFE at x2: output 2k+1 = midpoint k,k+1)
        if needs_base:
            if (p["gaps"] > 2).any():
                raise RuntimeError("gaps longer than 2 frames are not supported")
            half = tmp / "half"
            rife(content_dir, half, 2)
            # the mapping: output 2k is real frame k (RIFE re-renders it at timestep 0: near, not exact)
            for k in (1, n_content // 3, n_content // 2, n_content - 2):
                d = {j: same(half / f"{2 * k + 1:08d}.png", content_dir / f"{j + 1:08d}.png") for j in (k - 1, k, k + 1)}
                if d[k] > min(d.values()) + 0.5 or d[k] > 4.0:  # (near-still frames: neighbors tie)
                    raise RuntimeError(f"unexpected RIFE frame mapping at real frame {k}: {d}")
            base_dir = tmp / "base"
            base_dir.mkdir()
            b = 0
            for k in range(n_content):
                b += 1
                shutil.copyfile(content_dir / f"{k + 1:08d}.png", base_dir / f"{b:08d}.png")  # the real frame
                if k < n_content - 1 and p["gaps"][k] == 2:
                    b += 1
                    shutil.copyfile(half / f"{2 * k + 2:08d}.png", base_dir / f"{b:08d}.png")
            n_base = b
        else:
            base_dir, n_base = content_dir, n_content
        # 3. x factor on the even base timeline
        if factor > 1:
            fine = tmp / "fine"
            n_fine = rife(base_dir, fine, factor)
        else:
            fine, n_fine = base_dir, n_base
        # timing: base frame b belongs at base_times[b] (locally even; slips between stretches kept), fine frame j
        # at base position j / factor. Output frame i (time i / rate) shows the fine frame nearest that time.
        rate = p["base_rate"] * factor
        total = int(round(info["duration"] * float(rate)))
        bt = p["base_times"][:n_base]
        x = np.interp(np.arange(total) / float(rate), bt, np.arange(len(bt)))  # holds the ends
        src = np.clip(np.rint(x * factor).astype(int), 0, n_fine - 1)
        seq = tmp / "seq"
        seq.mkdir()
        for j, k in enumerate(src):
            (seq / f"{j + 1:08d}.png").symlink_to(fine / f"{k + 1:08d}.png")
        out_dir = out_dir or path.parent
        out_dir.mkdir(parents=True, exist_ok=True)
        out = out_dir / f"{path.stem}.rife{factor}x.mov"
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-framerate", f"{rate.numerator}/{rate.denominator}",
                        "-i", str(seq / "%08d.png"), *HEVC, str(out)], check=True)
        print(f"  wrote {out}  ({float(rate):.3f} fps)")
        if compare:
            cmp = db.CACHE / f"{path.stem.strip()}.compare.mp4"
            side_by_side(path, out, cmp)
            print(f"  compare {cmp}")
        return out
    finally:
        if keep:
            print(f"  kept {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)


def side_by_side(orig: Path, new: Path, out: Path, speed: float = 0.5, seconds: float = 30.0) -> None:
    """Original | interpolated at half speed, labeled, 60 fps."""
    f = (f"[0:v]setpts=PTS/{speed},fps=60,scale=640:-2,drawtext=text='original (half speed)':x=10:y=10:"
         f"fontcolor=white:fontsize=20:box=1:boxcolor=black@0.5[a];"
         f"[1:v]setpts=PTS/{speed},fps=60,scale=640:-2,drawtext=text='restored + RIFE (half speed)':x=10:y=10:"
         f"fontcolor=white:fontsize=20:box=1:boxcolor=black@0.5[b];[a][b]hstack[v]")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-t", f"{seconds * speed}", "-i", str(orig),
                    "-t", f"{seconds * speed}", "-i", str(new), "-filter_complex", f, "-map", "[v]",
                    "-c:v", "h264_videotoolbox", "-b:v", "12M", "-pix_fmt", "yuv420p", str(out)], check=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--factor", type=int, default=4)
    ap.add_argument("--out", type=Path, help="output folder (default: beside the original)")
    ap.add_argument("--compare", action="store_true")
    ap.add_argument("--keep-temp", action="store_true")
    ap.add_argument("--install", action="store_true", help="download rife-ncnn-vulkan into tools/dance_sync/bin")
    a = ap.parse_args()
    if a.install:
        install()
        if not a.paths:
            return 0
    db.CACHE.mkdir(parents=True, exist_ok=True)
    failed = 0
    for m in db.collect(a.paths):
        if db.RIFE_NAME.search(m.name):
            continue  # already an interpolated file
        try:
            interpolate(m, a.factor, a.out.expanduser() if a.out else None, a.keep_temp, a.compare)
        except (RuntimeError, subprocess.CalledProcessError) as e:
            print(f"  FAILED {m.name.strip()}: {e}", file=sys.stderr)
            failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

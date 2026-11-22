#!/usr/bin/env python3
"""Renders the story page's dance-video examples from Peter's clips in ~/reforce/movies (personal media, not in the
repo), with borders flashing on the beats of each clip's saved grid (.reforce.txt; red on the downbeat) and a click
track on the same beats:

    luma.mp4    two halves at half speed: " dance_luma_2" as it came | its RIFE-smoothed copy (4 bars)
    beeple.mp4  "- beeple_harvest_128bpm", the whole seamless loop at normal speed, 640x360 (it repeats in the page)

    python3 -I tools/story/clips.py OUT_DIR
"""
import pathlib
import re
import subprocess
import sys

MOVIES = pathlib.Path.home() / "reforce" / "movies"


def grid(stem):
    opts = (MOVIES / f"{stem}.reforce.txt").read_text()
    bpm = float(re.search(r"BPM\s*=\s*([\d.]+)", opts).group(1))
    start = float(re.search(r"BeatStart\s*=\s*([\d.]+)", opts).group(1))
    return 60.0 / bpm, start


def flash(P, phase, thick):
    """Border on every beat (white), red on the downbeat; output time t, first downbeat at `phase` seconds."""
    bar = 4 * P
    on = f"mod(t+{bar * 8 - phase:.5f},{P:.5f})"
    down = f"mod(t+{bar * 8 - phase:.5f},{bar:.5f})"
    return (f"drawbox=x=0:y=0:w=iw:h=ih:t={thick}:color=white:enable='lt({on},0.14)*gte({down},0.14)',"
            f"drawbox=x=0:y=0:w=iw:h=ih:t={thick}:color=0xff3b4f:enable='lt({down},0.14)'")


def click(P, phase, dur):
    bar = 4 * P
    on = f"mod(t+{bar * 8 - phase:.5f},{P:.5f})"
    down = f"mod(t+{bar * 8 - phase:.5f},{bar:.5f})"
    return (f"aevalsrc='if(lt({down},{P:.5f}),sin(2*PI*1600*t),sin(2*PI*1000*t))*exp(-{on}*45)*0.6'"
            f":s=44100:d={dur:.4f}")


def encode(args, out):
    subprocess.run(["ffmpeg", "-v", "error", "-y", *args, "-c:v", "libx264", "-preset", "slow", "-crf", "27",
                    "-c:a", "aac", "-b:a", "64k", "-movflags", "+faststart", str(out)], check=True)
    print(f"{out}: {out.stat().st_size / 1e6:.1f} MB")


def luma(out):
    stem, first_bar, bars, slow = " dance_luma_2", 21, 4, 2.0    # file names start with a space
    beat, beat_start = grid(stem)
    start, src_len = beat_start + first_bar * 4 * beat, bars * 4 * beat
    P, dur = beat * slow, src_len * slow
    side = f"setpts={slow}*(PTS-STARTPTS),fps=50,scale=640:360,{flash(P, 0.0, 10)}"
    encode(["-ss", f"{start:.4f}", "-t", f"{src_len:.4f}", "-i", str(MOVIES / f"{stem}.mov"),
            "-ss", f"{start:.4f}", "-t", f"{src_len:.4f}", "-i", str(MOVIES / f"{stem}.rife4x.mov"),
            "-f", "lavfi", "-i", click(P, 0.0, dur),
            "-filter_complex", f"[0:v]{side}[a];[1:v]{side}[b];[a][b]hstack=inputs=2,format=yuv420p[v]",
            "-map", "[v]", "-map", "2:a", "-t", f"{dur:.4f}"], out)


def beeple(out):
    stem = "- beeple_harvest_128bpm"
    beat, beat_start = grid(stem)
    src = MOVIES / f"{stem}.mov"
    dur = float(subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0",
                                str(src)], capture_output=True, text=True, check=True).stdout)
    encode(["-i", str(src), "-f", "lavfi", "-i", click(beat, beat_start, dur),
            "-filter_complex", f"[0:v]scale=640:360,{flash(beat, beat_start, 8)},format=yuv420p[v]",
            "-map", "[v]", "-map", "1:a", "-t", f"{dur:.4f}"], out)


if __name__ == "__main__":
    outdir = pathlib.Path(sys.argv[1])
    outdir.mkdir(parents=True, exist_ok=True)
    luma(outdir / "luma.mp4")
    beeple(outdir / "beeple.mp4")

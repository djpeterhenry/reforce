#!/usr/bin/env python3
"""Renders the story page's 3D solid examples: reforce draws each solid offline over our own small scenes
(tools/story/scenes: a flow, a wave and a color scheme we wrote; no G-Force content), and ffmpeg makes a short
looping video of a whole number of beats. reforce listens to a 120 BPM kick test signal of our own while it renders,
so its beat clock locks and the solids move on real beats.

    python3 -I tools/story/solid_clips.py OUT_DIR [REFORCE_BINARY]
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
FPS = 60
SKIP = 8.0                      # seconds after the start: faded in, and the beat clock has locked onto the kicks
KICKS = ROOT / "probes/audio/kick120.wav"   # our own test signal: a kick every 0.5 s (120 BPM) from about 1 s to 17 s
CLIPS = {                       # file -> (solid, beats per video at 120 BPM, wave)
    "dodecahedron.mp4": ("Dodecahedron Beat", 16, "story wave"),
    # the sphere has no edges to light, so it shows only through the wave on its surface: a brighter wave
    "sphere.mp4": ("Sphere Bounce", 16, "story wave bright"),
}

outdir = pathlib.Path(sys.argv[1])
app = sys.argv[2] if len(sys.argv) > 2 else str(ROOT / "build/reforce.app/Contents/MacOS/reforce")
outdir.mkdir(parents=True, exist_ok=True)
for file, (name, beats, wave) in CLIPS.items():
    first, count = round(SKIP * FPS), round(beats * 0.5 * FPS)
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([app, "--background", "--no-settings", "--audio-file", str(KICKS), "--no-link", "--deterministic",
                        "--scenes", str(ROOT / "tools/story/scenes"), "--content", str(ROOT / "content"),
                        "--flowfield", "story flow", "--waveshape", wave, "--colormap", "story color",
                        "--slideshow", "off", "--frame-size", "960x540", "--render-frames", str(first + count),
                        "--out", tmp, "--save-frames", ",".join(str(f) for f in range(first, first + count)),
                        "--command", f"0.2=switch:solid:{name}"], check=True, capture_output=True)
        out = outdir / file
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-framerate", str(FPS), "-start_number", str(first),
                        "-i", f"{tmp}/frame_%05d.png", "-frames:v", str(count), "-vf", "scale=640:360,format=yuv420p",
                        "-c:v", "libx264", "-preset", "slow", "-crf", "24", "-movflags", "+faststart", str(out)],
                       check=True)
        print(f"{out}: {name}, {count} frames, {out.stat().st_size / 1e6:.1f} MB")

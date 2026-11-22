"""Builds the probe-only scenes root and renders each probe timeline with reforce's offline mode."""
import glob
import os
import shutil
import subprocess

import timeline

# Run directories in time order (probes/captures/<dir>), timeline and G-Force frame rate during the run.
RUNS = [
    ("run1", "20261111-125107", 60),
    ("run2", "20261111-125819", 60),
    ("run3", "20261111-131648", 30),  # Graphics.TargetFrameRate = 30
    ("run4", "20261111-132031", 60),
]
WIDTH, HEIGHT = 1980, 1260  # G-Force's TakeScreenshot() size during the runs
# Optional extra run (used when its captures exist): run 6 switches G-Force to full screen between its two shots
# (vis.SetFullscreen), so each shot is rendered at its own size.
RUN6 = ("run6", "20261111-140539", 60, {1: (1980, 1260), 2: (3456, 2234)})

_LAYOUT = {  # probes/<dir> -> package path under Packages/
    "WaveShape": "G-Force.WaveShape.package/WaveShapes",
    "Particle": "G-Force.WaveShape.package/Particles",
    "FlowField": "G-Force.FlowField.package",
    "FlowField Union": "G-Force.FlowField Union.package",
    "ColorMap": "G-Force.ColorMap.package",
}


def build_scenes(repo, work):
    """A scenes root holding ONLY our probe configs, in G-Force's package layout."""
    root = os.path.join(work, "scenes")
    if os.path.isdir(root):
        shutil.rmtree(root)
    for src, dst in _LAYOUT.items():
        d = os.path.join(root, "Packages", dst)
        os.makedirs(d)
        for f in sorted(glob.glob(os.path.join(repo, "probes", src, "*.txt"))):
            shutil.copy(f, d)
    return root




def render_run(repo, reforce, scenes, work, run, fps, log, out_dir=None, size=(WIDTH, HEIGHT), only=None, audio=None,
               extra_sets=()):
    """audio: a WAV analyzed deterministically from frame 0 (reforce --audio-file), else silence (--no-audio).
    extra_sets: additional --set Key=value prefs (e.g. the audio prefs G-Force ran with)."""
    events = timeline.parse(os.path.join(repo, "probes", "timelines", run + ".py"))
    start, commands, shots, frames = timeline.reforce_args(events, fps)
    if only is not None:
        shots = {n: f for n, f in shots.items() if n in only}
        frames = max(shots.values()) + 1
    out = out_dir or os.path.join(work, "frames", run)
    if os.path.isdir(out):
        shutil.rmtree(out)
    args = [reforce, "--no-settings", "--no-ui", "--deterministic", "--fps", "%g" % fps]
    args += ["--audio-file", audio] if audio else ["--no-audio"]
    args += ["--render-size", "%dx%d" % size, "--render-frames", str(frames), "--scenes", scenes,
            "--slideshow", "off",
            # Settings during the probe runs (docs/probe-results.md); scripted morphs carry their own duration.
            "--set", "ColorMap.MapFunction=i",
            "--set", "Graphics.TargetFrameRate=%g" % fps,
            "--set", "Particle.Transition.Duration.Deviation=0",
            # Compare against G-Force's own line/dot rasterization (probe #32), not reforce's smooth default.
            "--set", "reforce.ThinLines=gforce",
            "--out", out, "--save-frames", ",".join(str(f) for f in shots.values())]
    for kv in extra_sets:
        args += ["--set", kv]
    for layer in ("flowfield", "waveshape", "colormap"):
        if layer in start:
            args += ["--" + layer, start[layer]]
    for c in commands:
        args += ["--command", c]
    with open(log, "w") as f:
        f.write(" ".join(repr(a) for a in args) + "\n")
        f.flush()
        rc = subprocess.run(args, stdout=f, stderr=subprocess.STDOUT).returncode
    if rc != 0:
        raise RuntimeError("reforce failed for %s (exit %d); see %s" % (run, rc, log))
    return {n: os.path.join(out, "frame_%05d.png" % fr) for n, fr in shots.items()}, shots, commands

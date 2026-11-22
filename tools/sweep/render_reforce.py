#!/usr/bin/env python3
"""Phase 5B.4 corpus sweep, reforce side: replay each sweep timeline in reforce's deterministic offline mode.

    python3 -I tools/sweep/render_reforce.py [--reforce PATH] [--fps 60] [--only sweep_ff_01,...]
                                             [--out DIR] [--set KEY=VALUE ...] [--seed N]

Same scenes as G-Force sees during the run (build/sweep/scenes: the corpus from scenes/ plus the probe and
derived configs in G-Force's install layout, so configs resolve by the same names), same size as G-Force's
TakeScreenshot() (1980x1260), same script times, built-in settings (--no-settings) + ColorMap.MapFunction = i
(as probes/run.sh sets in G-Force), silence. Frames go to build/sweep/reforce/<timeline>/ with index.json
mapping shot number -> PNG. --set/--seed exist for validation (perturbed runs must fail the compare).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sweeplib as S  # noqa: E402

WIDTH, HEIGHT = 1980, 1260
LAYER = {"ff": "flowfield", "cm": "colormap", "ws": "waveshape"}
_PROBE_DIRS = {  # probes/<dir> -> package path, as probes/install.sh installs them into G-Force
    "WaveShape": "G-Force.WaveShape.package/WaveShapes",
    "FlowField": "G-Force.FlowField.package",
    "ColorMap": "G-Force.ColorMap.package",
}


def build_scenes(root=os.path.join(S.WORK, "scenes"), derived=S.DERIVED):
    """The scenes root G-Force sees during a sweep run: the corpus packages (copied from scenes/; local only)
    plus 'zz reforce probes/' (our probe configs) and 'zz reforce probes/zz sweep/' (the derived copies), so
    configs resolve by the same names the timelines give G-Force."""
    if os.path.isdir(root):
        shutil.rmtree(root)
    for pkg in ("G-Force.FlowField.package", "G-Force.ColorMap.package", "G-Force.WaveShape.package",
                "G-Force.FlowField Union.package"):
        shutil.copytree(os.path.join(S.SCENES, "Packages", pkg), os.path.join(root, "Packages", pkg))
    for src, dst in _PROBE_DIRS.items():
        d = os.path.join(root, "Packages", dst, "zz reforce probes")
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(os.path.join(S.REPO, "probes", src)):
            if f.endswith(".txt"):
                shutil.copy(os.path.join(S.REPO, "probes", src, f), d)
        der = os.path.join(derived, "Packages", dst, S.DERIVED_SUBDIR)
        if os.path.isdir(der):
            shutil.copytree(der, os.path.join(d, S.DERIVED_SUBDIR))
    return root


def render(reforce, tl, out, scenes, fps=60.0, extra_sets=(), seed=None, log=None):
    hdr, setup, slots = S.parse_sweep(os.path.join(S.TIMELINE_DIR, tl + ".py"))
    commands, shots = [], {}
    for sl in slots:
        if "t_clear" in sl:
            commands.append("%g=morph:flowfield:0:%s" % (sl["t_clear"], setup["flowfield"]))
        if not os.path.isfile(os.path.join(S.REPO, sl["source"])):
            raise RuntimeError("%s slot %d: missing %s (run make_timelines.py)" % (tl, sl["index"], sl["source"]))
        commands.append("%g=morph:%s:0:%s" % (sl["t_switch"], LAYER[sl["fn"]], sl["name"]))
        shots[sl["shot"]] = int(round(sl["t_shot"] * fps))
    frames = max(shots.values()) + 1
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    args = [reforce, "--no-settings", "--no-ui", "--deterministic", "--fps", "%g" % fps, "--no-audio",
            "--render-size", "%dx%d" % (WIDTH, HEIGHT), "--render-frames", str(frames), "--scenes", scenes,
            "--slideshow", "off", "--set", "ColorMap.MapFunction=i", "--out", out,
            "--save-frames", ",".join(str(f) for f in sorted(shots.values()))]
    if seed is not None:
        args += ["--seed", str(seed)]
    for kv in extra_sets:
        args += ["--set", kv]
    for layer, name in setup.items():
        args += ["--" + layer, name]
    for c in commands:
        args += ["--command", c]
    log = log or os.path.join(out, "reforce.log")
    with open(log, "w") as f:
        f.write(" ".join(repr(a) for a in args) + "\n")
        f.flush()
        rc = subprocess.run(args, stdout=f, stderr=subprocess.STDOUT).returncode
    if rc != 0:
        raise RuntimeError("reforce failed for %s (exit %d); see %s" % (tl, rc, log))
    index = {str(n): "frame_%05d.png" % fr for n, fr in shots.items()}
    missing = [p for p in index.values() if not os.path.isfile(os.path.join(out, p))]
    if missing:
        raise RuntimeError("%s: %d frame(s) not written, e.g. %s" % (tl, len(missing), missing[0]))
    json.dump({"timeline": tl, "fps": fps, "sets": list(extra_sets), "seed": seed, "shots": index},
              open(os.path.join(out, "index.json"), "w"), indent=1)
    return index


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reforce", default=S.DEFAULT_REFORCE)
    ap.add_argument("--fps", type=float, default=60.0)
    ap.add_argument("--only", default="", help="comma-separated timeline names")
    ap.add_argument("--out", default=os.path.join(S.WORK, "reforce"))
    ap.add_argument("--set", action="append", default=[], help="extra reforce --set (validation)")
    ap.add_argument("--seed", type=int, default=None)
    a = ap.parse_args()
    tls = [t for t in a.only.split(",") if t] or S.sweep_timelines()
    if not tls:
        sys.exit("no probes/timelines/sweep_*.py: run tools/sweep/make_timelines.py")
    scenes = build_scenes()
    for tl in tls:
        idx = render(a.reforce, tl, os.path.join(a.out, tl), scenes, a.fps, a.set, a.seed)
        print("%s: %d frames -> %s" % (tl, len(idx), os.path.relpath(os.path.join(a.out, tl), S.REPO)))


if __name__ == "__main__":
    main()

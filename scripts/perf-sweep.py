#!/usr/bin/env python3.11
"""Performance sweep over every Object (solids and organics), offline, one at a time, then all of them in a row
with crossfades. Peter 2026-11-23: "a performance sweep that stayed complete and was ready to go".

Complete by construction: the objects come from content/Categories/Object/*.txt (every group's Include.List), so a
new object added to a group is swept without touching this script; a scene listed there that no longer starts is
reported as a failure.

Each run is an offline, deterministic render (no window, --background: a running reforce is never disturbed) of
--seconds at 60 fps at --size, the object switched on half a second in (after the unmeasured warm-up, so its start
is counted) over the default visuals; reforce reports CPU and GPU milliseconds per frame (average, 99th percentile, worst). A baseline run without an object shows what the visuals
alone cost. Verdicts per run:
  OVER  a 99th percentile or worst frame (CPU or GPU) over the frame budget (--budget-ms, 16.7 = 60 fps)
  WARN  a 99th percentile over --warn-ms (default: half the budget), or the worst frame over 1.5 x it
  ok    otherwise
Exit status 1 when anything is OVER or an object failed to start.

  scripts/perf-sweep.py                      # 1080p (reforce's render size), 20 s per object, plus the crossfades
  scripts/perf-sweep.py --size 3840x2160     # a stress run at 4K
  scripts/perf-sweep.py --size 1920x1080 --seconds 8 --only Rain
  scripts/perf-sweep.py --no-crossfade --app /Applications/reforce.app/Contents/MacOS/reforce

Offline numbers are the GPU's own time per frame (Metal command buffer start to end) and the render thread's CPU
time; they leave out presenting to the screen, Syphon and OBS. Best run on a quiet machine (no other GPU work).
"""

import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent

SUMMARY = re.compile(r"(\d+) frames at (\d+)x(\d+): cpu ([\d.]+) ms avg \(max ([\d.]+);.*?\), "
                     r"gpu ([\d.]+) ms avg \(max ([\d.]+)\), max (\d+) segments")
CPU_P99 = re.compile(r"cpu p99 ([\d.]+) ms")
GPU_P99 = re.compile(r"gpu p99 ([\d.]+) ms")
STARTED = re.compile(r'(?:Solid|Organic) "([^"]+)" starts')


def objects_from_groups(content: pathlib.Path) -> list[tuple[str, str]]:
    """(group, scene) for every scene in every Object group, in group order."""
    out = []
    for cat in sorted((content / "Categories" / "Object").glob("*.txt")):
        text = cat.read_text()
        title = re.search(r'CategoryTitle\s*=\s*"([^"]*)"', text)
        group = title.group(1) if title else cat.stem
        m = re.search(r'Include\.List\s*=\s*"(.*?)"', text, re.S)
        if not m:
            continue
        for line in m.group(1).splitlines():
            name = line.strip()
            if not name:
                continue
            # (A name in parentheses is the scene's real name, e.g. "(Beat Clock)": kept as it is.)
            out.append((group, name))
    return out


def run(app: str, content: pathlib.Path, size: str, frames: int, commands: list[str], tmp: pathlib.Path,
        timeout: float) -> tuple[dict, str]:
    out_dir = tempfile.mkdtemp(dir=tmp)
    args = [app, "--background", "--no-log-file", "--deterministic", "--fps", "60", "--no-audio", "--no-settings",
            "--no-ui", "--no-link", "--allow-multiple", "--slideshow", "off", "--content", str(content),
            "--render-size", size, "--render-frames", str(frames), "--out", out_dir]
    for c in commands:
        args += ["--command", c]
    t0 = time.monotonic()
    try:
        p = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        log = p.stdout + p.stderr
    except subprocess.TimeoutExpired as e:
        log = (e.stdout or "") + (e.stderr or "") if isinstance(e.stdout, str) else ""
        log += f"\n[perf-sweep] timed out after {timeout:.0f} s"
    shutil.rmtree(out_dir, ignore_errors=True)
    r: dict = {"wall_s": round(time.monotonic() - t0, 1), "started": STARTED.findall(log)}
    m = SUMMARY.search(log)
    if m:
        r.update(cpu_avg=float(m.group(4)), cpu_max=float(m.group(5)), gpu_avg=float(m.group(6)),
                 gpu_max=float(m.group(7)), segments=int(m.group(8)))
    for key, rx in (("cpu_p99", CPU_P99), ("gpu_p99", GPU_P99)):
        mm = rx.search(log)
        if mm:
            r[key] = float(mm.group(1))
    return r, log


def verdict(r: dict, budget: float, warn: float) -> str:
    if "gpu_avg" not in r:
        return "FAIL"
    p99 = max(r.get("cpu_p99", 0.0), r.get("gpu_p99", 0.0))
    worst = max(r["cpu_max"], r["gpu_max"])
    if p99 > budget or worst > budget:
        return "OVER"
    if p99 > warn or worst > 1.5 * warn:
        return "WARN"
    return "ok"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", default=str(ROOT / "build/reforce.app/Contents/MacOS/reforce"))
    ap.add_argument("--content", default=str(ROOT / "content"))
    ap.add_argument("--size", default="1920x1080",
                    help="render size: reforce renders at this fixed size (its default, what runs live) and scales "
                         "to the screen; 3840x2160 is a stress run")
    ap.add_argument("--seconds", type=float, default=20.0,
                    help="seconds per object (at 60 fps; 20 covers an 8-bar cycle at the 120 BPM fallback)")
    ap.add_argument("--only", default="", help="only objects whose name contains this (case-insensitive)")
    ap.add_argument("--no-crossfade", action="store_true", help="skip the run through all objects in a row")
    ap.add_argument("--crossfade-every", type=float, default=8.0, help="seconds per object in the crossfade run")
    ap.add_argument("--budget-ms", type=float, default=1000.0 / 60.0)
    ap.add_argument("--warn-ms", type=float, default=0.0, help="default: half the budget")
    ap.add_argument("--out", default=str(ROOT / "build/perf-sweep"), help="report folder (JSON and log)")
    a = ap.parse_args()
    warn = a.warn_ms if a.warn_ms > 0 else a.budget_ms / 2
    content = pathlib.Path(a.content)
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="perf-sweep-"))
    frames = max(60, int(a.seconds * 60))
    objs = [o for o in objects_from_groups(content) if a.only.lower() in o[1].lower()]
    if not objs:
        print("no objects found", file=sys.stderr)
        return 1

    results = []
    logs = []
    print(f"reforce perf sweep: {len(objs)} objects at {a.size}, {a.seconds:g} s each; budget {a.budget_ms:.1f} ms, "
          f"warn {warn:.1f} ms")
    hdr = f"{'':4} {'group':10} {'object':26} {'gpu avg':>8} {'p99':>7} {'max':>7}   {'cpu avg':>8} {'p99':>7} {'max':>7}"
    print(hdr)

    def show(group: str, name: str, r: dict, v: str) -> None:
        if "gpu_avg" in r:
            print(f"{v:4} {group[:10]:10} {name[:26]:26} {r['gpu_avg']:8.2f} {r.get('gpu_p99', 0):7.2f} "
                  f"{r['gpu_max']:7.2f}   {r['cpu_avg']:8.2f} {r.get('cpu_p99', 0):7.2f} {r['cpu_max']:7.2f}",
                  flush=True)
        else:
            print(f"{v:4} {group[:10]:10} {name[:26]:26} (no report; see the log)", flush=True)

    timeout = 120 + frames / 10
    r, log = run(a.app, content, a.size, frames, [], tmp, timeout)
    v = verdict(r, a.budget_ms, warn)
    show("-", "(visuals only)", r, v)
    results.append({"group": "-", "object": "(visuals only)", "verdict": v, **r})
    logs.append(("(visuals only)", log))
    for group, name in objs:
        # Switched on after the warm-up frames (not measured), so what an object costs to start is counted too.
        r, log = run(a.app, content, a.size, frames, [f"0.5=switch:object:{name}"], tmp, timeout)
        v = verdict(r, a.budget_ms, warn)
        if v != "FAIL" and name not in r["started"]:
            v = "FAIL"  # rendered, but the object never started (renamed, unknown Kind / Mesh, refused)
        show(group, name, r, v)
        results.append({"group": group, "object": name, "verdict": v, **r})
        logs.append((name, log))

    if not a.no_crossfade and len(objs) > 1:
        cmds = [f"{0.5 + i * a.crossfade_every:g}=switch:object:{name}" for i, (_, name) in enumerate(objs)]
        cf_frames = int((len(objs) * a.crossfade_every + 4) * 60)
        r, log = run(a.app, content, a.size, cf_frames, cmds, tmp, 300 + cf_frames / 10)
        v = verdict(r, a.budget_ms, warn)
        show("all", f"crossfades ({len(objs)} objects)", r, v)
        results.append({"group": "all", "object": "crossfades", "verdict": v, **r})
        logs.append(("crossfades", log))

    shutil.rmtree(tmp, ignore_errors=True)
    stamp = time.strftime("%Y-%m-%d_%H%M%S")
    (out / f"perf-sweep_{stamp}.json").write_text(json.dumps(
        {"size": a.size, "seconds": a.seconds, "budget_ms": a.budget_ms, "warn_ms": warn, "results": results},
        indent=1))
    with open(out / f"perf-sweep_{stamp}.log", "w") as f:
        for name, log in logs:
            f.write(f"===== {name}\n{log}\n")
    bad = [x for x in results if x["verdict"] in ("OVER", "FAIL")]
    warns = [x for x in results if x["verdict"] == "WARN"]
    print(f"\n{len(results)} runs: {len(bad)} over budget or failed, {len(warns)} warnings. "
          f"Report: {out}/perf-sweep_{stamp}.json (+ .log)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

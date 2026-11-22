#!/usr/bin/env python3
"""G-Force vs reforce resource comparison (PLAN 5G). No sudo needed.

  python3 -I tools/perfcompare/perfcompare.py gforce  --seconds 120
  python3 -I tools/perfcompare/perfcompare.py reforce --seconds 120 [-- extra reforce args]

One app at a time, machine otherwise idle. Both get the same audio: a loop of
probes/audio/{kick120,noise_bands,sweep}.wav played to the Loopback device
"reforce probe" (silent), which each app reads as its input. Each app runs its
normal slideshow with the user's own settings, windowed, in front.

Measured over the window after --warmup seconds:
  cpu%      process CPU time (user+system, all threads) / wall time; 100% = one core
  rss       resident memory (MB), mean and max; threads (max)
  gpu%      whole-GPU "Device Utilization %" from IOAccelerator (ioreg), mean; the
            idle baseline (nothing running) is measured first and reported too
  fps       reforce: from its --perf-report; G-Force: not measurable here
            (its frame rate was measured earlier at ~55.6 fps, PLAN)

G-Force safety (memory: gforce-must-stay-unaffected): prefs are backed up before
and restored after (then diffed); only Audio.InputSource/AutoDetect and
UI.ShowOnStartup are changed for the run; nothing is installed; the window is
never resized or full-screened; G-Force and its Toolbar are quit afterwards.
Results are appended as JSON lines to build/perfcompare/results.jsonl.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "perfcompare"
PLAYER = ROOT / "build" / "reforce-audio-play"
DEVICE = "reforce probe"
AUDIO = [ROOT / "probes" / "audio" / f for f in ("kick120.wav", "noise_bands.wav", "sweep.wav")]
PREFS = Path.home() / "Library" / "Preferences" / "SoundSpectrum"
GF_PREFS = PREFS / "G-Force" / "Preferences (Standalone).txt"
GF_APP = "/Applications/G-Force/G-Force Standalone.app"
GF_PROCS = ["/Applications/G-Force/G-Force Standalone.app/Contents/MacOS/",
            "/Applications/G-Force/G-Force Toolbar.app/Contents/MacOS/"]
RF_BIN = "/Applications/reforce.app/Contents/MacOS/reforce"


def sh(*cmd: str) -> str:
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def gpu_util() -> float | None:
    m = re.search(r'"Device Utilization %"=(\d+)', sh("ioreg", "-r", "-c", "IOAccelerator", "-d", "1"))
    return float(m.group(1)) if m else None


def cputime_s(text: str) -> float:
    # ps cputime: [[dd-]hh:]mm:ss.cc
    days = 0
    if "-" in text:
        d, text = text.split("-", 1)
        days = int(d)
    parts = [float(p) for p in text.split(":")]
    secs = 0.0
    for p in parts:
        secs = secs * 60 + p
    return days * 86400 + secs


def pids_matching(pattern: str) -> list[int]:
    # Patterns are executable paths ("X.app/Contents/MacOS/"), so shells whose
    # command line merely mentions the app never match.
    out = sh("pgrep", "-f", pattern)
    pids = [int(x) for x in out.split() if int(x) != os.getpid()]
    # Keep only processes whose executable (not argument text) is under the pattern.
    return [p for p in pids if sh("ps", "-o", "comm=", "-p", str(p)).strip().startswith(pattern)]


def proc_sample(pids: list[int]) -> dict[int, tuple[float, float, int]]:
    """pid -> (cpu seconds, rss MB, threads)."""
    res = {}
    if not pids:
        return res
    out = sh("ps", "-o", "pid=,cputime=,rss=", "-p", ",".join(map(str, pids)))
    for line in out.splitlines():
        f = line.split()
        if len(f) < 3:
            continue
        pid = int(f[0])
        threads = max(0, len(sh("ps", "-M", "-p", str(pid)).splitlines()) - 1)
        res[pid] = (cputime_s(f[1]), int(f[2]) / 1024.0, threads)
    return res


class AudioLoop:
    def __init__(self) -> None:
        self.stop = threading.Event()
        self.proc: subprocess.Popen | None = None
        self.t = threading.Thread(target=self.run, daemon=True)

    def run(self) -> None:
        while not self.stop.is_set():
            for f in AUDIO:
                if self.stop.is_set():
                    return
                self.proc = subprocess.Popen([str(PLAYER), "--device", DEVICE, str(f)],
                                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self.proc.wait()

    def __enter__(self):
        self.t.start()
        return self

    def __exit__(self, *a):
        self.stop.set()
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
        self.t.join(timeout=5)


def measure(pattern_list: list[str], warmup: float, seconds: float) -> dict:
    time.sleep(warmup)
    pids = sorted({p for pat in pattern_list for p in pids_matching(pat)})
    if not pids:
        raise SystemExit(f"no process matching {pattern_list}")
    start = proc_sample(pids)
    t0 = time.monotonic()
    rss, thr, gpu, cpu_inst = [], [], [], []
    last, tl = start, t0
    while time.monotonic() - t0 < seconds:
        time.sleep(1.0)
        s = proc_sample(pids)
        now = time.monotonic()
        rss.append(sum(v[1] for v in s.values()))
        thr.append(sum(v[2] for v in s.values()))
        cpu_inst.append(100 * sum(s[p][0] - last.get(p, s[p])[0] for p in s) / (now - tl))
        last, tl = s, now
        g = gpu_util()
        if g is not None:
            gpu.append(g)
    end = proc_sample(pids)
    wall = time.monotonic() - t0
    per = {}
    names = {p: sh("ps", "-o", "comm=", "-p", str(p)).strip().rsplit("/", 1)[-1] for p in pids}
    for p in end:
        if p in start:
            per[names[p]] = round(100 * (end[p][0] - start[p][0]) / wall, 1)
    return {
        "cpu_pct": round(sum(per.values()), 1),
        "cpu_by_process": per,
        "cpu_pct_p95": round(sorted(cpu_inst)[int(0.95 * (len(cpu_inst) - 1))], 1) if cpu_inst else None,
        "rss_mb_mean": round(statistics.mean(rss), 1) if rss else None,
        "rss_mb_max": round(max(rss), 1) if rss else None,
        "threads_max": max(thr) if thr else None,
        "gpu_util_mean": round(statistics.mean(gpu), 1) if gpu else None,
        "gpu_util_p95": round(sorted(gpu)[int(0.95 * (len(gpu) - 1))], 1) if gpu else None,
        "wall_s": round(wall, 1),
    }


def idle_gpu(seconds: float = 10) -> float | None:
    vals = []
    for _ in range(int(seconds)):
        g = gpu_util()
        if g is not None:
            vals.append(g)
        time.sleep(1)
    return round(statistics.mean(vals), 1) if vals else None


def set_pref(path: Path, key: str, value: str) -> None:
    lines = [l for l in path.read_text(errors="surrogateescape").splitlines() if not l.startswith(key + " = ")]
    lines.append(f"{key} = {value}")
    path.write_text("\n".join(lines) + "\n", errors="surrogateescape")


def quit_gforce() -> None:
    for pat in GF_PROCS:
        subprocess.run(["pkill", "-TERM", "-f", pat])
    for _ in range(20):
        if not any(pids_matching(p) for p in GF_PROCS):
            return
        time.sleep(0.5)


def run_gforce(a) -> dict:
    if any(pids_matching(p) for p in GF_PROCS):
        raise SystemExit("G-Force is running; quit it first")
    bak = Path(tempfile.mkdtemp(prefix="gf-prefs-")) / "SoundSpectrum"
    shutil.copytree(PREFS, bak, symlinks=True)
    try:
        set_pref(GF_PREFS, "Audio.InputSource", f'"{DEVICE}"')
        set_pref(GF_PREFS, "Audio.AutoDetect.Enabled", "0")
        set_pref(GF_PREFS, "UI.ShowOnStartup", "0")
        with AudioLoop():
            subprocess.run(["open", GF_APP])
            time.sleep(3)
            subprocess.run(["osascript", "-e", 'tell application "G-Force Standalone" to activate'])
            r = measure(GF_PROCS, a.warmup, a.seconds)
    finally:
        quit_gforce()
        shutil.rmtree(PREFS)
        shutil.copytree(bak, PREFS, symlinks=True)
        d = subprocess.run(["diff", "-r", str(bak), str(PREFS)], capture_output=True, text=True)
        print("G-Force prefs restored:", "identical" if d.returncode == 0 else "DIFFERENT\n" + d.stdout)
    r["fps"] = None
    return r


def run_reforce(a, extra: list[str]) -> dict:
    if pids_matching("/Applications/reforce.app/Contents/MacOS/reforce"):
        raise SystemExit("reforce is running; quit it first")
    total = a.warmup + a.seconds + 5
    cmd = [RF_BIN, "--audio-device", DEVICE, "--render-size", a.render_size,
           "--perf-report", str(total - 3), *extra]
    with AudioLoop():
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        time.sleep(2)
        subprocess.run(["osascript", "-e", 'tell application "reforce" to activate'])
        r = measure(["/Applications/reforce.app/Contents/MacOS/reforce"], a.warmup - 2, a.seconds)
        report, _ = p.communicate(timeout=total + 30)
    r["perf_report"] = report.strip().splitlines()
    m = re.search(r"^frames (\d+)", report, re.M)
    r["fps"] = round(int(m.group(1)) / (total - 3 - 3), 1) if m else None  # frames after the 3 s warm-up
    return r


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("app", choices=["gforce", "reforce"])
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--warmup", type=float, default=20)
    ap.add_argument("--render-size", default="1980x1316",
                    help="reforce render size; default = G-Force's 990x658 pt window on Retina")
    ap.add_argument("--label", default="")
    ap.add_argument("extra", nargs="*", help="extra reforce args (after --)")
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    base = idle_gpu()
    r = run_gforce(a) if a.app == "gforce" else run_reforce(a, a.extra)
    r.update(app=a.app, label=a.label or a.app, extra=a.extra, idle_gpu_util=base,
             when=time.strftime("%Y-%m-%d %H:%M:%S"))
    with open(OUT / "results.jsonl", "a") as f:
        f.write(json.dumps(r) + "\n")
    print(json.dumps(r, indent=1))


if __name__ == "__main__":
    main()

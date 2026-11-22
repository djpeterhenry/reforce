#!/usr/bin/env python3
"""Phase 5C: compare G-Force vs reforce audio time series, score them and write docs/audio-parity.md.

    # compare already-decoded series (audio_measure.py output)
    python3 -I tools/parity/audio_compare.py [--work build/audio-parity] [--out docs/audio-parity.md] [--strict]
    # everything: decode the G-Force captures (probes/captures/, local only), render + decode reforce, compare
    python3 -I tools/parity/audio_compare.py --all --reforce build/reforce.app/Contents/MacOS/reforce [--strict]

Inputs: <work>/{gforce,reforce}/<run>/series.json from audio_measure.py. Runs that have both sides are compared.
With --all and no G-Force captures present, exits 77 (ctest SKIP).

Checks (each PASS / FAIL against a tolerance; KNOWN = a documented gap, reported but not counted against the score
and never failing --strict):
  * steady  per WAV segment (schedule from probes/audio/<file>.json) and signal: the mean over segment start + 0.3 s
            .. end, on audio time (each side aligned on its own detected sync click), |G - r| <= tol(signal).
            mag(.5) is compared as its RMS (the probe samples one point of the waveform at a random phase).
  * dynamics rise 10->90 % after silence and fall 90->10 % into silence of LEVEL / BASS / ENERGY: |G - r| <= max(abs,
            rel * G).
  * series  per run and signal: RMS of G(t) - r(t) over the whole run (r interpolated at G's frame times).
  * spectrum per snapshot shot (fft(s) curves): RMS over the 255 bins.
Score = passed / (passed + failed), in %. Saturated samples (at a clamp end of the probe's affine map) are counted.
"""
import argparse
import concurrent.futures
import glob
import json
import math
import os
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import audio_layout as L  # noqa: E402

REPO = L.REPO
KEYS = [s[0] for s in L.SIGNALS if s[0] != "time"]
ENVELOPE_KEYS = ["LEVEL", "BASS", "ENERGY"]

# G-Force capture directories (probes/captures/<dir>, local only, never committed) per audio run.
CAPTURES = {
    "run7": "20261111-170514", "run8": "20261111-170617", "run9": "20261111-170704", "run10": "20261111-170742",
    "run11": "20261111-170826", "run12": "20261111-170907", "run13": "20261111-170945", "run14": "20261111-171027",
    # follow-up 2026-11-11 evening: tones at 125 Hz..4 kHz + 20 s tone + falling sweep; two/three-tone chords
    "run15": "20261111-203921", "run16": "20261111-204133", "run23": "20261111-205240",
}

# Tolerances in scene units. The strip probe resolves ~1/61 of a band's range per pixel (LEVEL 0.016, BASS/fft
# 0.033, mag 0.033); noise signals fluctuate frame to frame, so steady means use ~3-6 px.
TOL_STEADY = {"LEVEL": 0.06, "BASS": 0.25, "ENERGY": 0.15, "fft.05": 0.18, "fft.2": 0.18, "fft.5": 0.18,
              "fft.8": 0.18, "mag.5": 0.05}
# Whole-run series also absorb G-Force's ~90 ms audio-buffer holds and its 0..0.1 s onset jitter (kicks), hence
# looser than the steady tolerances. mag(.5) is not compared as a series: it is one random-phase sample per frame.
TOL_SERIES = {"LEVEL": 0.10, "BASS": 0.40, "ENERGY": 0.12, "fft.05": 0.20, "fft.2": 0.20, "fft.5": 0.20,
              "fft.8": 0.20}
TOL_DYN_ABS, TOL_DYN_REL = 0.12, 0.6
TOL_SNAPSHOT = 0.18

# Documented gaps (docs/audio-probe-results.md section 7, "Not reproduced"); numbers refer to that list. Entries:
# (run, check kind, signal, substring of the check's "what" column, reason). "*" matches anything.
KNOWN = [
    ("run7", "steady", "fft.*", "*", "gap 1: tone fft() bump drifts over seconds"),
    ("run7", "series", "fft.*", "*", "gap 1: tone fft() bump drifts over seconds"),
    ("run7", "spectrum", "*", "*", "gap 1: tone fft() bump drifts over seconds"),
    ("run7", "steady", "mag.5", "*", "gap 3: G-Force mag() pre-filtering (tones read 1.06-1.19x)"),
    ("run9", "steady", "mag.5", "*", "gap 3: G-Force mag() attenuates noise (0.1-0.8x)"),
    ("run10", "steady", "mag.5", "*", "gap 3: G-Force mag() attenuates noise (0.1-0.8x)"),
    ("run11", "steady", "mag.5", "*", "gap 3: G-Force mag() attenuates noise (0.1-0.8x)"),
    ("run9", "steady", "*", "high", "gap 4: 4-12 kHz band: G-Force LEVEL 0.03, out-of-band fft(.8) grows"),
    ("run9", "steady", "ENERGY", "low", "gap 4: G-Force's weak low band grows over 2.5 s, so ENERGY lags"),
    ("run9", "steady", "LEVEL", "low", "gap 4: G-Force's weak low band grows over 2.5 s"),
    ("run14", "spectrum", "*", "*", "gap 4: out-of-band leakage / weak-band growth in G-Force"),
    ("run11", "steady", "LEVEL", "pink", "gap 5: pink bursts read higher and creep up in reforce"),
    ("run11", "steady", "BASS", "pink", "gap 5: pink bursts read higher and creep up in reforce"),
    ("run11", "steady", "fft.05", "pink", "gap 5: pink bursts read higher and creep up in reforce"),
    ("run11", "dynamics", "ENERGY", "*", "gap 5: ENERGY follows the higher pink LEVEL"),
    ("run10", "steady", "ENERGY", "kick 4", "gap 4: G-Force's kick LEVEL still growing (weak in-range content)"),
    ("run8", "*", "*", "*", "gap 2: sweep: G-Force fft()/LEVEL run ahead, then go dark above ~0.8 kHz"),
    ("run13", "*", "*", "*", "gap 2: sweep: G-Force fft()/LEVEL run ahead, then go dark above ~0.8 kHz"),
    # Gap 1/2 mechanism (follow-up runs 15/16/23): G-Force's fft() frequency axis adapts to the content (its upper
    # edge contracts toward the highest tone within ~2 s, its lower edge has a 240 Hz floor and rises slowly), so
    # every fft()-derived value of a steady tone depends on the tone's history. reforce keeps a fixed axis.
    ("run15", "*", "fft.*", "*", "gap 1: adaptive fft() axis (tones settle near the top of the axis)"),
    ("run15", "*", "LEVEL", "*", "gap 1: adaptive fft() axis: LEVEL = mean of the moving curve"),
    ("run15", "*", "BASS", "*", "gap 1: adaptive fft() axis: BASS = 4 x LEVEL"),
    ("run15", "*", "ENERGY", "*", "gap 1: adaptive fft() axis: ENERGY follows LEVEL"),
    ("run15", "spectrum", "*", "*", "gap 1: adaptive fft() axis"),
    ("run16", "spectrum", "*", "*", "gap 1: adaptive fft() axis"),
    ("run23", "spectrum", "*", "*", "gap 1: adaptive fft() axis"),
]


def known(run, kind, sig, what="*"):
    for r, k, sg, w, reason in KNOWN:
        if r not in ("*", run) or k not in ("*", kind):
            continue
        if not (sg == "*" or sg == sig or (sg.endswith("*") and sig.startswith(sg[:-1]))):
            continue
        if w != "*" and (what == "*" or w not in what):
            continue
        return reason
    return None


def load(work, side):
    out = {}
    for p in sorted(glob.glob(os.path.join(work, side, "run*", "series.json"))):
        with open(p) as f:
            d = json.load(f)
        out[d["run"]] = d
    return out


def window(series, key, t0, t1):
    ts = series.get("audio_t") or []
    return [(t, v, s) for t, v, s in zip(ts, series[key], series[key + "_sat"]) if t0 <= t < t1 and v is not None]


def seg_stats(series, key, seg):
    w = window(series, key, seg["t0"] + 0.3, seg["t1"])
    if not w:
        return None
    vals = [v for _, v, _ in w]
    if key == "mag.5":
        m = math.sqrt(sum(v * v for v in vals) / len(vals))
    else:
        m = sum(vals) / len(vals)
    return {"mean": m, "max": max(vals), "n": len(vals), "sat": sum(1 for *_, s in w if s)}


def crossing_time(series, key, t0, t1, frac_from, frac_to, lo, hi):
    """Time from the frac_from crossing to the frac_to crossing of the lo->hi (or hi->lo) transition."""
    w = window(series, key, t0, t1)
    if not w or abs(hi - lo) < 1e-6:
        return None
    a = b = None
    for t, v, _ in w:
        f = (v - lo) / (hi - lo)
        if a is None and f >= frac_from:
            a = t
        if a is not None and f >= frac_to:
            b = t
            break
    return None if a is None or b is None else b - a


def dynamics(series, key, seg, prev_silent, next_seg):
    st = seg_stats(series, key, seg)
    lo, hi = [(x[2], x[3]) for x in L.SIGNALS if x[0] == key][0]
    if not st or st["mean"] < 0.05 * (hi - lo):  # too small to time on the probe's scale
        return {}
    out = {}
    if prev_silent:
        out["rise_10_90"] = crossing_time(series, key, seg["t0"] - 0.1, seg["t0"] + 1.0, 0.1, 0.9, 0.0, st["mean"])
    if next_seg is not None and next_seg.get("kind") == "silence":
        out["fall_90_10"] = crossing_time(series, key, seg["t1"] - 0.05, seg["t1"] + 3.0, 0.1, 0.9, st["mean"], 0.0)
    return out


def interp(ts, vs, t):
    """Linear interpolation of (ts, vs) at t (None where the series has a gap > 0.1 s or no value)."""
    lo, hi = 0, len(ts) - 1
    if hi < 1 or t < ts[0] or t > ts[-1]:
        return None
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if ts[mid] <= t:
            lo = mid
        else:
            hi = mid
    if vs[lo] is None or vs[hi] is None or ts[hi] - ts[lo] > 0.1:
        return None
    if ts[hi] == ts[lo]:
        return vs[lo]
    u = (t - ts[lo]) / (ts[hi] - ts[lo])
    return vs[lo] + u * (vs[hi] - vs[lo])


def series_rms(g, r, key, t_min=0.3):
    gs, rs = g["series"], r["series"]
    if "audio_t" not in gs or "audio_t" not in rs:
        return None
    err, n = 0.0, 0
    for t, v in zip(gs["audio_t"], gs[key]):
        if v is None or t < t_min:
            continue
        rv = interp(rs["audio_t"], rs[key], t)
        if rv is None:
            continue
        err += (v - rv) ** 2
        n += 1
    return math.sqrt(err / n) if n > 20 else None


def fmt(v, nd=3):
    if v is None:
        return "–"
    if isinstance(v, float) and (math.isnan(v) or math.isinf(v)):
        return "–"
    return ("%%.%df" % nd) % v


def run_summary(d):
    fps = [s["fps"] for s in d["shots"] if s.get("fps")]
    rails = [abs(v) for s in d["shots"] for r in s.get("rails", {}).values() for v in (r["lo_err_px"], r["hi_err_px"])]
    dec = [s["age_minus_intensity_age"]["median"] for s in d["shots"] if s.get("age_minus_intensity_age")]
    return {"shots": len(d["shots"]), "frames": len(d["series"]["t"]),
            "fps": (min(fps), max(fps)) if fps else None, "rail_err_px": max(rails) if rails else None,
            "age_offset": (min(dec), max(dec)) if dec else None, "onset": d.get("click_onset_t"),
            "onset_bias": d.get("onset_detector_bias_s")}


class Score:
    def __init__(self):
        self.rows = []  # (run, kind, what, signal, G, r, err, tol, verdict, note)

    def add(self, run, kind, what, sig, g, r, err, tol):
        if g is None or r is None or err is None:
            return None
        note = known(run, kind, sig, what)
        ok = err <= tol
        verdict = "PASS" if ok else ("KNOWN" if note else "FAIL")
        self.rows.append((run, kind, what, sig, g, r, err, tol, verdict, note or ""))
        return verdict

    def counts(self, run=None, kind=None):
        rs = [x for x in self.rows if (run is None or x[0] == run) and (kind is None or x[1] == kind)]
        p = sum(1 for x in rs if x[8] == "PASS")
        f = sum(1 for x in rs if x[8] == "FAIL")
        k = sum(1 for x in rs if x[8] == "KNOWN")
        return p, f, k

    def pct(self, run=None, kind=None):
        p, f, _ = self.counts(run, kind)
        return 100.0 * p / (p + f) if p + f else None


def segments_for(meta):
    segs = [s for s in meta["segments"] if s["kind"] != "silence" or s.get("label") == "silence"]
    if len(segs) > 12:  # kick: every 4th hit (each hit is its own segment)
        segs = segs[::4]
    return segs


def seg_name(seg):
    if seg["kind"] == "tone" and "peak_dbfs" in seg:
        if seg.get("freq_hz", 1000.0) != 1000.0 or seg["peak_dbfs"] == -12:  # run 15/16 frequency steps
            return "tone %g Hz %+g dBFS" % (seg["freq_hz"], seg["peak_dbfs"])
        return "tone %+g dBFS" % seg["peak_dbfs"]
    if seg["kind"] == "tone_long":
        return "tone %g Hz 20 s" % seg["freq_hz"]
    if seg["kind"] == "tones":
        return "tones %s Hz" % "+".join("%g" % f for f in seg["freqs_hz"])
    if seg["kind"] == "kick":
        return "kick %d" % seg["index"]
    return seg.get("band") or seg.get("label") or seg["kind"] + (" %d" % seg["index"] if "index" in seg else "")


def compare_strip(run, g, r, meta, sc):
    md = ["### Steady state per segment", "",
          "| segment | t (s) | signal | G-Force | reforce | Δ | tol | | G sat | r sat |",
          "|---|---|---|---|---|---|---|---|---|---|"]
    for seg in segments_for(meta):
        for k in KEYS:
            sg, sr = seg_stats(g["series"], k, seg), seg_stats(r["series"], k, seg)
            if not sg or not sr:
                continue
            err = abs(sg["mean"] - sr["mean"])
            v = sc.add(run, "steady", seg_name(seg), k, sg["mean"], sr["mean"], err, TOL_STEADY[k])
            md.append("| %s | %.1f–%.1f | %s%s | %s | %s | %s | %s | %s | %d | %d |" % (
                seg_name(seg), seg["t0"], seg["t1"], k, " (rms)" if k == "mag.5" else "", fmt(sg["mean"]),
                fmt(sr["mean"]), fmt(err), fmt(TOL_STEADY[k], 2), v, sg["sat"], sr["sat"]))
    dyn = ["### Attack / release", "",
           "| segment | signal | G rise 10–90 (s) | r rise | G fall 90–10 (s) | r fall | |",
           "|---|---|---|---|---|---|---|"]
    segs = meta["segments"]
    any_dyn = False
    for i, seg in enumerate(segs):
        if seg["kind"] in ("silence", "kick"):
            continue
        prev_silent = i == 0 or segs[i - 1]["kind"] == "silence"
        nxt = segs[i + 1] if i + 1 < len(segs) else {"kind": "silence"}
        for k in ENVELOPE_KEYS:
            dg = dynamics(g["series"], k, seg, prev_silent, nxt)
            dr = dynamics(r["series"], k, seg, prev_silent, nxt)
            if not dg:
                continue
            verdicts = []
            for what in ("rise_10_90", "fall_90_10"):
                a, b = dg.get(what), dr.get(what)
                if a is None or b is None:
                    continue
                verdicts.append(sc.add(run, "dynamics", "%s %s" % (seg_name(seg), what), k, a, b, abs(a - b),
                                       max(TOL_DYN_ABS, TOL_DYN_REL * a)))
            dyn.append("| %s @%.1f | %s | %s | %s | %s | %s | %s |" % (
                seg_name(seg), seg["t0"], k, fmt(dg.get("rise_10_90")), fmt(dr.get("rise_10_90")),
                fmt(dg.get("fall_90_10")), fmt(dr.get("fall_90_10")), " ".join(x for x in verdicts if x)))
            any_dyn = True
    if any_dyn:
        md += [""] + dyn
    ser = ["### Whole-run time series (RMS of G − r)", "", "| signal | RMS | tol | |", "|---|---|---|---|"]
    for k in [x for x in KEYS if x in TOL_SERIES]:
        e = series_rms(g, r, k)
        if e is None:
            continue
        v = sc.add(run, "series", "whole run", k, 0.0, 0.0, e, TOL_SERIES[k])
        ser.append("| %s | %s | %s | %s |" % (k, fmt(e), fmt(TOL_SERIES[k], 2), v))
    return md + [""] + ser + [""]


def compare_snapshots(run, g, r, sc):
    bg = {s["label"]: s for s in g["shots"]}
    br = {s["label"]: s for s in r["shots"]}
    md = ["### fft(s) snapshots", "", "| shot | G peak s / value | r peak s / value | G mean | r mean | RMS | |",
          "|---|---|---|---|---|---|---|"]
    n = 0
    for lab in [x for x in bg if x in br]:
        cg = bg[lab].get("snapshot", {}).get("fft")
        cr = br[lab].get("snapshot", {}).get("fft")
        if not cg or not cr:
            continue
        pg = [(v, s) for s, v, _ in cg if v is not None]
        pr = [(v, s) for s, v, _ in cr if v is not None]
        if not pg or not pr or (max(pg)[0] < 0.02 and max(pr)[0] < 0.02):
            continue  # both dark (silence)
        diffs = [(a[1] - b[1]) ** 2 for a, b in zip(cg, cr) if a[1] is not None and b[1] is not None]
        rms = math.sqrt(sum(diffs) / len(diffs)) if diffs else None
        mg = sum(v for v, _ in pg) / len(pg)
        mr = sum(v for v, _ in pr) / len(pr)
        v = sc.add(run, "spectrum", lab, "fft(s)", mg, mr, rms, TOL_SNAPSHOT)
        md.append("| %s | %.2f / %.2f | %.2f / %.2f | %s | %s | %s | %s |" % (
            lab, max(pg)[1], max(pg)[0], max(pr)[1], max(pr)[0], fmt(mg), fmt(mr), fmt(rms), v))
        n += 1
    return md + [""] if n else []


def write_report(gf, ref, out):
    runs = sorted(set(ref) & set(gf), key=lambda x: int(x[3:]))
    sc = Score()
    body = []
    for run in runs:
        stem, kind = L.RUNS[run]
        meta = L.wav_meta(stem)
        body += ["", "## %s: %s (%s)" % (run, meta["file"], kind), "", meta["description"], ""]
        note = known(run, "*", "*") if (run, "*") in {(x[0], x[1]) for x in KNOWN} else None
        if note:
            body += ["*Known gap:* %s (see docs/audio-probe-results.md)." % note, ""]
        if kind == "strip":
            body += compare_strip(run, gf[run], ref[run], meta, sc)
        body += compare_snapshots(run, gf[run], ref[run], sc)
    p, f, k = sc.counts()
    md = ["# Audio parity (Phase 5C): G-Force vs reforce", "",
          "Generated by `tools/parity/audio_compare.py` from `tools/parity/audio_measure.py` output (numbers only; "
          "no images). Probes: strip charts and spectrum snapshots (`tools/parity/audio_layout.py`), test signals "
          "from `tools/audio_signals/gen_signals.py`, G-Force 5.9.7 captures of 2026-11-11 (Peter's prefs, "
          "`Audio.FFT.Smooth = 2`; reforce renders with the same). Time axis = audio file time, each side aligned "
          "on its own detected sync click; G-Force ran at ~52–54 fps, reforce at 60. Values are in scene units "
          "(what `LEVEL`, `BASS`, `ENERGY`, `fft()`, `mag()` return); \"sat\" counts samples at a clamp end of the "
          "probe's affine map (true value outside the plotted range). Findings and the fitted model: "
          "`docs/audio-probe-results.md`.", "",
          "**Score: %s %%** of the checks outside documented gaps (%d pass, %d fail). Including the %d known-gap "
          "checks: %s %% (%d of %d)." % (fmt(sc.pct(), 1), p, f, k, fmt(100.0 * p / max(1, p + f + k), 1), p, p + f + k),
          "",
          "| run | file | steady | dynamics | series | spectrum | fail | known |", "|---|---|---|---|---|---|---|---|"]
    for run in runs:
        meta = L.wav_meta(L.RUNS[run][0])
        cells = []
        for kind in ("steady", "dynamics", "series", "spectrum"):
            pp, ff, _ = sc.counts(run, kind)
            cells.append("%d/%d" % (pp, pp + ff) if pp + ff else "–")
        _, ff, kk = sc.counts(run)
        md.append("| %s | %s | %s | %d | %d |" % (run, meta["file"], " | ".join(cells), ff, kk))
    fails = [x for x in sc.rows if x[8] == "FAIL"]
    if fails:
        md += ["", "### Failing checks", "", "| run | check | what | signal | G | r | err | tol |",
               "|---|---|---|---|---|---|---|---|"]
        for x in fails:
            md.append("| %s | %s | %s | %s | %s | %s | %s | %s |" % (x[0], x[1], x[2], x[3], fmt(x[4]), fmt(x[5]),
                                                                     fmt(x[6]), fmt(x[7], 2)))
    md += ["", "## Runs", "", "| run | file | side | shots | frames | fps (fit) | rail err (px) | age − intensity age | "
           "click onset (s) |", "|---|---|---|---|---|---|---|---|---|"]
    for run in runs:
        meta = L.wav_meta(L.RUNS[run][0])
        for side, d in (("G-Force", gf[run]), ("reforce", ref[run])):
            s = run_summary(d)
            md.append("| %s | %s | %s | %d | %d | %s | %s | %s | %s |" % (
                run, meta["file"], side, s["shots"], s["frames"], "%.2f–%.2f" % s["fps"] if s["fps"] else "–",
                fmt(s["rail_err_px"], 2), "%.1f…%.1f" % s["age_offset"] if s["age_offset"] else "–",
                fmt(s["onset"])))
    with open(out, "w") as f:
        f.write("\n".join(md + body) + "\n")
    return sc, runs


def run_all(a):
    """Decode every G-Force capture present, render + decode reforce, return True if any G-Force data."""
    have = {run: os.path.join(REPO, "probes", "captures", d) for run, d in CAPTURES.items()
            if os.path.isdir(os.path.join(REPO, "probes", "captures", d))}
    if not have:
        return False
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "audio_measure.py")
    runs = sorted(have, key=lambda r: int(r[3:]))
    # Shared inputs once, before the runs go parallel.
    import audio_measure
    import render
    audio_measure.ensure_wavs()
    os.makedirs(a.work, exist_ok=True)
    scenes = render.build_scenes(REPO, a.work)

    def one(run):
        # The runs are independent (own work subdirectories): decode G-Force, then render + decode reforce.
        subprocess.run([sys.executable, "-I", tool, "gforce", "--run", run, "--captures", have[run], "--work", a.work],
                       check=True, stdout=subprocess.DEVNULL)
        return subprocess.run([sys.executable, "-I", tool, "reforce", "--runs", run, "--reforce", a.reforce,
                               "--work", a.work, "--scenes", scenes], check=True, stdout=subprocess.PIPE, text=True).stdout

    # Longest runs first so they do not start last; at most half the cores (each reforce render uses the GPU too).
    workers = max(1, min(len(runs), (os.cpu_count() or 2) // 2))
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        order = sorted(runs, key=lambda r: -len(os.listdir(have[r])))  # shots per run ~ its time
        outs = dict(zip(order, pool.map(one, order)))
    for run in runs:
        sys.stdout.write(outs[run])
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--work", default=os.path.join(REPO, "build", "audio-parity"))
    ap.add_argument("--out", default=os.path.join(REPO, "docs", "audio-parity.md"))
    ap.add_argument("--all", action="store_true", help="decode captures and render reforce first")
    ap.add_argument("--reforce", default=os.path.join(REPO, "build", "reforce.app", "Contents", "MacOS", "reforce"))
    ap.add_argument("--strict", action="store_true", help="exit 1 when a non-known check fails")
    a = ap.parse_args()
    if a.all:
        if not os.path.exists(a.reforce):
            sys.exit("reforce binary not found: %s" % a.reforce)
        if not run_all(a):
            print("SKIP: no G-Force audio captures under probes/captures/")
            return 77
    ref, gf = load(a.work, "reforce"), load(a.work, "gforce")
    if not gf or not ref:
        print("SKIP: need both sides in %s (G-Force: %d runs, reforce: %d runs)" % (a.work, len(gf), len(ref)))
        return 77 if a.all or a.strict else 0
    sc, runs = write_report(gf, ref, a.out)
    p, f, k = sc.counts()
    print("wrote %s: %d runs, score %s %% (%d pass, %d fail, %d known; all checks %s %%)" % (
        os.path.relpath(a.out, REPO), len(runs), fmt(sc.pct(), 1), p, f, k, fmt(100.0 * p / max(1, p + f + k), 1)))
    for x in sc.rows:
        if x[8] == "FAIL":
            print("  FAIL %s %s %s %s: G %s r %s err %s > %s" % (x[0], x[1], x[2], x[3], fmt(x[4]), fmt(x[5]),
                                                               fmt(x[6]), fmt(x[7], 2)))
    return 1 if a.strict and f else 0


if __name__ == "__main__":
    sys.exit(main())

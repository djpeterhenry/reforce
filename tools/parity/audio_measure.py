#!/usr/bin/env python3
"""Phase 5C audio probes: decode strip-chart / spectrum screenshots into time series (G-Force and reforce).

    # reforce side: render every audio timeline with the same WAV, deterministically, then decode
    python3 -I tools/parity/audio_measure.py reforce [--runs run7,run8] [--reforce PATH] [--work DIR]
    # G-Force side: decode a capture directory made by probes/run.sh with PROBE_TIMELINE=runN
    python3 -I tools/parity/audio_measure.py gforce --run run7 --captures probes/captures/<stamp> [--work DIR]
    # one image (debug)
    python3 -I tools/parity/audio_measure.py decode IMAGE.png [--layout strip|spectrum]

Output (per side and run) in <work>/<side>/<run>/: series.json (all shots + the stitched time series),
series.csv (stitched: t, audio_t, one column per signal), snapshots.csv (fft/mag curves per shot).
Default work dir: build/audio-parity. Stdlib only (PNG decoding via macOS `sips`, see images.py).

How a strip shot is read (geometry in audio_layout.py): for every column left of the dot head, in every band,
the run of lit pixels that is not one of the two rails gives y (intensity-weighted centroid) -> u -> value.
Each band's y -> u map is calibrated from its two rails as measured in that image (G-Force draws dots ~0.5-1.5 px
low; uncalibrated, silence read slightly negative). The column's frame age is head_px - column (1 px scroll per frame); its decayed intensity gives a second age
estimate (255 - peak). The time band (wrap(t/8)) gives `t` per column; a linear fit t(age) per shot gives the
frame rate and a precise time per column. Shots are stitched on t (fresher shot wins in overlaps).
audio_t = t - click_onset + click_start_in_file, where click_onset is detected from the decoded series
(first column where |mag(.5)| or LEVEL or any fft band leaves its pre-click baseline). Both sides use the
same detector; for reforce the true onset is known (delay padding) and its detector bias is reported.
"""
import argparse
import csv
import glob
import json
import os
import subprocess
import sys
import wave

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import audio_layout as L  # noqa: E402
import images  # noqa: E402
import measure as M  # noqa: E402

REPO = L.REPO
THR = 12          # pixel threshold for "lit"
RAIL_TOL = 1.6    # px: a run centred this close to a rail's expected row is the rail
MAX_AGE = 250


# ---- single image -----------------------------------------------------------------------------------------------
def head_px(img):
    return (L.STRIP_X + 1) * 0.5 * img.w - 0.5


def _fy_to_py(img, y):
    return (1 - y) * 0.5 * img.h - 0.5


def decode_strip(img, max_age=MAX_AGE, calibrate=True):
    """-> {"head_px", "rails": {key: (lo_y, hi_y)}, "rows": [ {age, col, peak, <key>: value or None, <key>_sat} ]}.
    Rows are ordered by age (0 = newest)."""
    head = head_px(img)
    bands = [L.band(k) for k in range(len(L.SIGNALS))]
    rows, rail_acc = [], {b["key"]: [[], []] for b in bands}
    c_max = min(img.w - 1, int(head))
    for c in range(c_max, max(-1, int(head - max_age) - 1), -1):
        age = head - c
        row = {"col": c, "age": round(age, 2), "peak": 0}
        any_val = False
        for b in bands:
            p0, p1 = int(_fy_to_py(img, b["yhi"])), int(_fy_to_py(img, b["ylo"])) + 1
            p0, p1 = max(p0, 0), min(p1, img.h)
            col = img.col(c, 0, p0, p1)
            rl, rh = _fy_to_py(img, b["rail_lo"]), _fy_to_py(img, b["rail_hi"])
            best = None
            for first, last, cy in M.runs(col, THR, p0):
                if abs(cy - rl) < RAIL_TOL:
                    rail_acc[b["key"]][0].append(cy)
                    continue
                if abs(cy - rh) < RAIL_TOL:
                    rail_acc[b["key"]][1].append(cy)
                    continue
                pk = max(col[first - p0:last - p0 + 1])
                if best is None or pk > best[1]:
                    best = (cy, pk)
            if best is None:
                row[b["key"]] = None
                continue
            row[b["key"]] = img.fy(best[0])  # frame y for now; mapped to a value below (rail calibration)
            row["peak"] = max(row["peak"], best[1])
            any_val = True
        if any_val:
            rows.append(row)
    rails = {}
    for k, (lo, hi) in rail_acc.items():
        if lo and hi:
            b = L.band([s[0] for s in L.SIGNALS].index(k))
            ylo, yhi = img.fy(M.median(lo)), img.fy(M.median(hi))
            rails[k] = {"lo_y": round(ylo, 5), "hi_y": round(yhi, 5),
                        "lo_err_px": round((ylo - b["rail_lo"]) * img.h / 2, 2),
                        "hi_err_px": round((yhi - b["rail_hi"]) * img.h / 2, 2)}
    # y -> u -> value. With both rails measured (and plausible), map y affinely through them: this removes the
    # renderer's sub-pixel dot offset (G-Force draws dots ~0.5-1 px low), which would otherwise read as a bias.
    for b in bands:
        r = rails.get(b["key"]) if calibrate else None
        if r and abs(r["lo_err_px"]) < 3 and abs(r["hi_err_px"]) < 3:
            y_of = lambda u, r=r: r["lo_y"] + (u - L.RAIL_LO) / (L.RAIL_HI - L.RAIL_LO) * (r["hi_y"] - r["lo_y"])
        else:
            y_of = lambda u, b=b: b["y0"] + u * b["h"]
        y_u0, y_u1 = y_of(0.0), y_of(1.0)
        for row in rows:
            y = row.get(b["key"])
            if y is None:
                continue
            u = (y - y_u0) / (y_u1 - y_u0)
            row[b["key"]] = b["lo"] + u * (b["hi"] - b["lo"])
            # at a clamp end: the top always; the bottom only for signed signals (0 is a real value for the rest)
            row[b["key"] + "_sat"] = u > 0.99 or (b["lo"] < 0 and u < 0.01)
    return {"head_px": head, "rails": rails, "rows": rows}


def decode_snapshot(img, layout, bins=255):
    """-> {curve: [ (s, value or None, saturated) ]} for the fft(s) and mag(s) dot curves."""
    out = {}
    for curve, (y0, h, lo, hi) in L.SNAPSHOTS[layout].items():
        p0 = max(0, int(_fy_to_py(img, y0 + h + 0.02)))
        p1 = min(img.h, int(_fy_to_py(img, y0 - 0.02)) + 1)
        vals = []
        for i in range(bins):
            s = i / (bins - 1)
            xc = (L.SNAP_X0 + L.SNAP_XW * s + 1) * 0.5 * img.w - 0.5
            sw = sy = 0.0
            for c in range(int(round(xc)) - 1, int(round(xc)) + 2):
                if 0 <= c < img.w:
                    for j, v in enumerate(img.col(c, 0, p0, p1)):
                        if v > THR:
                            sw += v
                            sy += v * (p0 + j)
            if sw == 0:
                vals.append((round(s, 5), None, False))
                continue
            u = (img.fy(sy / sw) - y0) / h
            vals.append((round(s, 5), lo + u * (hi - lo), u > 0.99 or (lo < 0 and u < 0.01)))
        out[curve] = vals
    return out


# ---- one run ------------------------------------------------------------------------------------------------------
def _fit_line(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if sxx else 0.0
    return my - b * mx, b


def shot_time_axis(rows):
    """Unwrap the time band leftwards and fit t = a + b * age. -> (a, b, rms residual, n) or None."""
    pts = [(r["age"], r["time"] * L.TIME_PERIOD) for r in rows if r.get("time") is not None and not r.get("time_sat")]
    if len(pts) < 10:
        return None
    pts.sort()
    un, prev, k = [], None, 0
    for age, t in pts:
        if prev is not None and t - prev > L.TIME_PERIOD / 2:
            k -= 1
        prev = t
        un.append((age, t + k * L.TIME_PERIOD))
    a, b = _fit_line([p[0] for p in un], [p[1] for p in un])
    # one robust pass: drop > 3 px outliers (one time-band pixel = TIME_PERIOD / band px seconds)
    tol = 3 * L.TIME_PERIOD / (L.USE_H * 630)
    keep = [p for p in un if abs(p[1] - (a + b * p[0])) < tol]
    if len(keep) >= 10:
        a, b = _fit_line([p[0] for p in keep], [p[1] for p in keep])
        un = keep
    rms = (sum((p[1] - (a + b * p[0])) ** 2 for p in un) / len(un)) ** 0.5
    return a, b, rms, len(un)


SERIES_KEYS = [s[0] for s in L.SIGNALS if s[0] != "time"]
# Audio prefs G-Force ran with during the Phase 5C captures (Peter's prefs: docs/gforce-defaults.md). reforce renders
# the probes with the same values so the comparison is like for like.
GFORCE_AUDIO_PREFS = ("Audio.FFT.Smooth=2",)
# The strip/spectrum decoders and their tolerances were calibrated on reforce's sub-pixel dot readout; the audio
# analyzer is what is compared here, so the replays keep the smooth line rasterization (render.py's default for the
# probe parity is G-Force's pixel-snapped one, probe #32, which quantizes the readout to whole rows).
AUDIO_LINES = ("reforce.ThinLines=smooth",)
# G-Force sees the audio ~0.23 s after the timeline's nominal audio start (player start-up + device + capture
# latency; measured from the sync click in every strip run: shot audio time = T - delay - 0.23 +- 0.03 s). The
# reforce render starts its audio this much later too, so the snapshot shots see the same audio moments. Time
# series are aligned on each side's own click anyway.
GFORCE_INPUT_LATENCY = 0.23


def detect_onset(series, t_from=None):
    """First t where any signal leaves its baseline (median of the first 0.3 s of data) by a margin."""
    ts = series["t"]
    if not ts:
        return None
    if t_from is None:
        # start at the first frame that has signal values (a shot can show older, value-less columns, e.g. the
        # previous WaveShape's last frames before the strip started)
        t_from = next((t for i, t in enumerate(ts) if series["LEVEL"][i] is not None), ts[0])
    t0 = t_from
    margins = {"LEVEL": 0.01, "mag.5": 0.02, "BASS": 0.05, "ENERGY": 0.05,
               "fft.05": 0.05, "fft.2": 0.05, "fft.5": 0.05, "fft.8": 0.05}
    base = {}
    for k in margins:
        v = [x for t, x in zip(ts, series[k]) if x is not None and t0 <= t < t0 + 0.3]
        base[k] = M.median(v) if v else 0.0
    for i, t in enumerate(ts):
        if t < t0 + 0.3:
            continue
        for k, m in margins.items():
            x = series[k][i]
            if x is not None and abs(x - base[k]) > m:
                return t
    return None


def _sustained(series, onset):
    """True when LEVEL stays above 0.03 for most of onset + 0.3 .. onset + 1.2 s (content, not the sync click)."""
    v = [x for t, x in zip(series["t"], series["LEVEL"]) if x is not None and onset + 0.3 <= t < onset + 1.2]
    return len(v) >= 10 and sum(1 for x in v if x > 0.03) > 0.7 * len(v)


def analyze_run(shots, wav_meta, true_onset=None):
    """shots: [(label, timeline_T, image)] -> result dict (see module doc)."""
    out_shots, all_rows = [], []
    t_abs_ref = None
    for label, T, img, layout in shots:
        rec = {"label": label, "T": T, "layout": layout}
        if layout == "strip":
            d = decode_strip(img)
            fit = shot_time_axis(d["rows"])
            rec["rails"] = d["rails"]
            rec["n_columns"] = len(d["rows"])
            # decay check: column age minus intensity age (255 - peak); constant ~0.6 when the scroll is exact
            # and the decay is 1/255 per frame. Its spread grows if either is not.
            diffs = [r["age"] - (255 - r["peak"]) for r in d["rows"] if 30 <= r["peak"] <= 250]
            if diffs:
                rec["age_minus_intensity_age"] = {"median": round(M.median(diffs), 2),
                                                  "min": round(min(diffs), 2), "max": round(max(diffs), 2)}
            if fit:
                a, b, rms, n = fit
                # absolute G-Force/reforce clock: pick the 8 s wrap so shots follow the timeline spacing
                if t_abs_ref is None:
                    t_abs_ref = (a, T)
                else:
                    pred = t_abs_ref[0] + (T - t_abs_ref[1])
                    a += round((pred - a) / L.TIME_PERIOD) * L.TIME_PERIOD
                rec.update({"t_head": round(a, 4), "fps": round(-1.0 / b, 3) if b else None,
                            "time_fit_rms_s": round(rms, 4), "time_fit_n": n})
                for r in d["rows"]:
                    r2 = dict(r)
                    r2["t"] = a + b * r["age"]
                    r2["shot"] = label
                    all_rows.append(r2)
            rec["snapshot"] = decode_snapshot(img, "strip")
        else:
            rec["snapshot"] = decode_snapshot(img, "spectrum")
        out_shots.append(rec)
    # stitch: bucket by frame time (fresher shot = smaller age wins)
    all_rows.sort(key=lambda r: (r["t"], r["age"]))
    stitched = []
    for r in all_rows:
        if stitched and abs(r["t"] - stitched[-1]["t"]) < 0.5 / 60:
            if r["age"] < stitched[-1]["age"]:
                stitched[-1] = r
            continue
        stitched.append(r)
    series = {"t": [round(r["t"], 4) for r in stitched], "age": [r["age"] for r in stitched],
              "peak": [r["peak"] for r in stitched], "shot": [r["shot"] for r in stitched]}
    for k in SERIES_KEYS:
        series[k] = [None if r.get(k) is None else round(r[k], 5) for r in stitched]
        series[k + "_sat"] = [bool(r.get(k + "_sat")) for r in stitched]
    onset = detect_onset(series)
    click_file = wav_meta["sync"]["t"]
    # The click is 50 ms followed by 1.45 s of silence. If the detected onset is followed by sustained signal, the
    # click was missed (run 15: G-Force's input came up ~0.45 s late, after the click) and the onset is the
    # content start instead.
    if onset is not None and "content_start" in wav_meta and _sustained(series, onset):
        res_note = "click not seen: onset = content start"
        click_file = wav_meta["content_start"]
    else:
        res_note = None
    res = {"shots": out_shots, "series": series, "click_onset_t": onset, "click_file_t": click_file,
           "wav": wav_meta["file"]}
    if true_onset is not None:
        res["true_click_t"] = true_onset
        res["onset_detector_bias_s"] = None if onset is None else round(onset - true_onset, 4)
    if res_note:
        res["onset_note"] = res_note
    if onset is not None:
        series["audio_t"] = [round(t - onset + click_file, 4) for t in series["t"]]
    return res


def write_outputs(res, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "series.json"), "w") as f:
        json.dump(res, f, indent=0)
    s = res["series"]
    if s["t"]:
        cols = ["t"] + (["audio_t"] if "audio_t" in s else []) + ["age", "peak"] + SERIES_KEYS
        with open(os.path.join(out_dir, "series.csv"), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(cols)
            for i in range(len(s["t"])):
                w.writerow(["" if s[c][i] is None else s[c][i] for c in cols])
    with open(os.path.join(out_dir, "snapshots.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["shot", "T", "curve", "s", "value", "saturated"])
        for sh in res["shots"]:
            for curve, vals in sh.get("snapshot", {}).items():
                for sv, v, sat in vals:
                    w.writerow([sh["label"], sh["T"], curve, sv, "" if v is None else round(v, 5), int(sat)])


# ---- sides --------------------------------------------------------------------------------------------------------
def timeline_shots(run):
    import timeline
    events = timeline.parse(os.path.join(REPO, "probes", "timelines", run + ".py"))
    return [(e.label, e.t) for e in events if e.kind == "shot"]


def ensure_wavs():
    if not all(os.path.exists(os.path.join(REPO, "probes", "audio", st + ".wav")) for st, _ in L.RUNS.values()):
        subprocess.run([sys.executable, "-I", os.path.join(REPO, "tools", "audio_signals", "gen_signals.py")],
                       check=True)


def padded_wav(stem, delay, total_seconds, work):
    """The probe WAV with `delay` s of silence in front (reforce starts its audio at frame 0) and silence to
    total_seconds."""
    src = os.path.join(REPO, "probes", "audio", stem + ".wav")
    dst = os.path.join(work, "audio", "%s+%gs.wav" % (stem, delay))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with wave.open(src, "rb") as r:
        sr, ch, sw = r.getframerate(), r.getnchannels(), r.getsampwidth()
        data = r.readframes(r.getnframes())
    pre = b"\0" * (int(round(delay * sr)) * ch * sw)
    post_n = max(0, int(total_seconds * sr) - len(pre) // (ch * sw) - len(data) // (ch * sw))
    tmp = "%s.%d.tmp" % (dst, os.getpid())  # runs in parallel may share a padded WAV
    with wave.open(tmp, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(sw)
        w.setframerate(sr)
        w.writeframes(pre + data + b"\0" * (post_n * ch * sw))
    os.replace(tmp, dst)
    return dst


def run_reforce(a):
    import render
    os.makedirs(a.work, exist_ok=True)
    if a.scenes:
        scenes = a.scenes  # built by the caller (audio_compare runs several of us in parallel)
    else:
        ensure_wavs()
        scenes = render.build_scenes(REPO, a.work)
    for run in a.runs:
        stem, kind = L.RUNS[run]
        hdr = L.parse_header(os.path.join(REPO, "probes", "timelines", run + ".py"))
        delay = hdr[1] + GFORCE_INPUT_LATENCY
        shots = timeline_shots(run)
        wav = padded_wav(stem, delay, shots[-1][1] + 2.0, a.work)
        frames_dir = os.path.join(a.work, "reforce", run, "frames")
        log = os.path.join(a.work, "reforce", run, "render.log")
        os.makedirs(os.path.dirname(log), exist_ok=True)
        got, frame_of, _ = render.render_run(REPO, a.reforce, scenes, a.work, run, a.fps, log, out_dir=frames_dir,
                                             audio=wav, extra_sets=GFORCE_AUDIO_PREFS + AUDIO_LINES)
        cache = os.path.join(a.work, "cache", "reforce", run)
        images.to_bmp_all([got[i] for i in range(1, len(shots) + 1)], cache)
        lst = []
        for i, (label, T) in enumerate(shots, 1):
            lst.append((label, frame_of[i] / a.fps, images.load(got[i], cache), kind))
        meta = L.wav_meta(stem)
        # reforce's t = frame / fps; the click starts delay + sync.t into the padded file.
        res = analyze_run(lst, meta, true_onset=delay + meta["sync"]["t"])
        res.update({"side": "reforce", "run": run, "fps_nominal": a.fps, "audio_delay": delay})
        write_outputs(res, os.path.join(a.work, "reforce", run))
        summarize(res)


def run_gforce(a):
    stem, kind = L.RUNS[a.run]
    shots = timeline_shots(a.run)
    pngs = sorted(glob.glob(os.path.join(a.captures, "*.png")))
    if len(pngs) != len(shots):
        print("warning: %d screenshots for %d scripted shots; pairing in order" % (len(pngs), len(shots)))
    cache = os.path.join(a.work, "cache", "gforce", a.run)
    images.to_bmp_all(pngs[:len(shots)], cache)
    lst = [(label, T, images.load(p, cache), kind) for (label, T), p in zip(shots, pngs)]
    res = analyze_run(lst, L.wav_meta(stem))
    res.update({"side": "gforce", "run": a.run, "captures": os.path.relpath(a.captures, REPO)})
    write_outputs(res, os.path.join(a.work, "gforce", a.run))
    summarize(res)


def summarize(res):
    s = res["series"]
    fps = [sh.get("fps") for sh in res["shots"] if sh.get("fps")]
    rails = [abs(v) for sh in res["shots"] for r in sh.get("rails", {}).values()
             for v in (r["lo_err_px"], r["hi_err_px"])]
    print("%s %s: %d shots, %d stitched frames%s, fps %s, rail err max %.2f px, click onset %s%s" % (
        res["side"], res["run"], len(res["shots"]), len(s["t"]),
        " (t %.2f..%.2f)" % (s["t"][0], s["t"][-1]) if s["t"] else "",
        "%.2f..%.2f" % (min(fps), max(fps)) if fps else "-", max(rails) if rails else float("nan"),
        "%.3f" % res["click_onset_t"] if res["click_onset_t"] is not None else "-",
        " (true %.3f, bias %+.3f s)" % (res["true_click_t"], res["onset_detector_bias_s"])
        if res.get("onset_detector_bias_s") is not None else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("reforce")
    r.add_argument("--runs", default=",".join(L.RUNS))
    r.add_argument("--reforce", default=os.path.join(REPO, "build", "reforce.app", "Contents", "MacOS", "reforce"))
    r.add_argument("--fps", type=float, default=60.0)
    r.add_argument("--scenes", help="a probe scenes root already built (default: build one in --work)")
    g = sub.add_parser("gforce")
    g.add_argument("--run", required=True, choices=list(L.RUNS))
    g.add_argument("--captures", required=True)
    d = sub.add_parser("decode")
    d.add_argument("image")
    d.add_argument("--layout", default="strip", choices=["strip", "spectrum"])
    for p in (r, g, d):
        p.add_argument("--work", default=os.path.join(REPO, "build", "audio-parity"))
    a = ap.parse_args()
    if a.cmd == "reforce":
        a.runs = [x for x in a.runs.split(",") if x]
        if not os.path.exists(a.reforce):
            sys.exit("reforce binary not found: %s" % a.reforce)
        run_reforce(a)
    elif a.cmd == "gforce":
        run_gforce(a)
    else:
        img = images.load(a.image, os.path.join(a.work, "cache", "decode"))
        if a.layout == "strip":
            dd = decode_strip(img)
            print(json.dumps({"rails": dd["rails"], "fit": shot_time_axis(dd["rows"]), "newest": dd["rows"][:3]},
                             indent=1))
        print(json.dumps({k: v[::32] for k, v in decode_snapshot(img, a.layout).items()}, indent=1))


if __name__ == "__main__":
    main()

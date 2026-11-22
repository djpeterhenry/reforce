#!/usr/bin/env python3
"""Phase 5B.4 corpus sweep: compare G-Force's sweep screenshots with reforce's replay.

    python3 -I tools/sweep/compare.py [--captures-map build/sweep/captures.json] [--captures TL=DIR ...]
                                      [--reforce-dir build/sweep/reforce] [--reference-dir DIR]
                                      [--out docs/sweep.md] [--gallery build/sweep/gallery] [--calibrate]

Inputs: probes/timelines/sweep_*.py (slot list), G-Force captures per timeline (captures.json is written by
tools/sweep/run_gforce_sweeps.sh; --captures overrides), reforce frames (render_reforce.py).
--reference-dir DIR uses another reforce render (same index.json layout) in place of G-Force: that is how the
kit is validated without G-Force (validate.py). --calibrate prints the metrics on the existing G-Force probe
captures vs their parity-suite reforce frames (known-good pairs) and exits.

Outputs: docs/sweep.md (numbers only: per-config metrics, class, verdict, worst offenders) and a local-only
side-by-side gallery (build/sweep/gallery/index.html with downsampled PNGs; never committed).

FlowField metrics (gray ColorMap, MapFunction i: pixel = intensity, pattern at 102):
  both screenshots are reduced to 330x210 (6x6 area average via sips; hides line antialiasing and sub-pixel
  differences), then
  mad    mean |G-Force - reforce| in intensity levels (0..255; the pattern's own level is 102)
  ssim   mean structural similarity (8x8 windows, L = 102)
  edge   correlation of Sobel gradient magnitudes (structure of lines and trails, independent of level)
  exact classes (det, derand) pass with mad <= MAD_MAX and ssim >= SSIM_MIN.
  loose classes (noise, rnd-load, time) compare at 66x42 (30x30 average): mad_c (coarse) and the intensity
  histogram distance hist (L1 of 26 bins, 0..2); pass with mad_c <= LOOSE_MAD_MAX and hist <= LOOSE_HIST_MAX.
ColorMap metrics: the palette read at the 256 dot centers of the three ramp rows (median of the rows):
  mean / p95 / max |diff| over 256 levels x RGB, in 8-bit units. Exact classes (det, derand, png, ref) pass with
  mean <= CM_MEAN_MAX and p95 <= CM_P95_MAX; 'time' maps (t-dependent; t is config-local, so both sides are at
  t = 0.7 s, up to +-2 frames of shot jitter) pass loosely when the average palette color differs by <= CM_TIME_MAX.
A '<!-- notes:begin -->' ... '<!-- notes:end -->' block in the output (per-config notes) is kept across runs.
"""
import argparse
import glob
import html
import json
import math
import os
import re
import struct
import subprocess
import sys
import zlib

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "parity"))
import images  # noqa: E402  (tools/parity/images.py: BMP reader + frame coordinates)
import sweeplib as S  # noqa: E402

# Thresholds (calibration in docs/sweep.md "Thresholds"; see validate.py).
MAD_MAX, SSIM_MIN = 3.0, 0.85
LOOSE_MAD_MAX, LOOSE_HIST_MAX = 6.0, 0.35
CM_MEAN_MAX, CM_P95_MAX, CM_TIME_MAX = 2.0, 6.0, 40.0
EXACT = ("det", "derand", "png", "ref")
DS = (330, 210)      # FlowField comparison size (6x6 of 1980x1260)
COARSE = (66, 42)    # loose comparison size (30x30)
L_FF = 102.0         # pattern intensity (Pen .4)
CACHE = os.path.join(S.WORK, "cache")


# ------------------------------------------------------------------------------------------- image access
def _bmp(png, size, tag):
    """PNG -> BMP via sips (optionally resampled to size), cached. -> images.Image."""
    key = re.sub(r"[^A-Za-z0-9]+", "_", os.path.relpath(os.path.abspath(png), S.REPO)) + "_" + tag + ".bmp"
    out = os.path.join(CACHE, key)
    if not os.path.exists(out) or os.path.getmtime(out) < os.path.getmtime(png):
        os.makedirs(CACHE, exist_ok=True)
        args = ["sips", "-s", "format", "bmp"]
        if size:
            args += ["--resampleHeightWidth", str(size[1]), str(size[0])]
        subprocess.run(args + [png, "--out", out], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return images.Image(out)


def gray(png, size):
    im = _bmp(png, size, "%dx%d" % size)
    if (im.w, im.h) != size:
        raise RuntimeError("%s: resample gave %dx%d" % (png, im.w, im.h))
    px = []
    for y in range(im.h):
        px.extend(im.row(y, 1))  # G channel (gray map: R = G = B)
    return px


def native_size(png):
    im = _bmp(png, None, "full")
    return im.w, im.h


# ---------------------------------------------------------------------------------------------- metrics
def _integral(v, w, h):
    s = [0.0] * ((w + 1) * (h + 1))
    for y in range(h):
        row = 0.0
        o, p = (y + 1) * (w + 1), y * (w + 1)
        for x in range(w):
            row += v[y * w + x]
            s[o + x + 1] = s[p + x + 1] + row
    return s


def ssim(a, b, w, h, win=8, L=L_FF):
    c1, c2 = (0.01 * L) ** 2, (0.03 * L) ** 2
    ia, ib = _integral(a, w, h), _integral(b, w, h)
    iaa, ibb = _integral([x * x for x in a], w, h), _integral([x * x for x in b], w, h)
    iab = _integral([x * y for x, y in zip(a, b)], w, h)
    n, tot, cnt = float(win * win), 0.0, 0
    W = w + 1

    def box(s, x, y):
        return s[(y + win) * W + x + win] - s[y * W + x + win] - s[(y + win) * W + x] + s[y * W + x]

    for y in range(0, h - win + 1, win // 2):
        for x in range(0, w - win + 1, win // 2):
            ma, mb = box(ia, x, y) / n, box(ib, x, y) / n
            va, vb = box(iaa, x, y) / n - ma * ma, box(ibb, x, y) / n - mb * mb
            cov = box(iab, x, y) / n - ma * mb
            tot += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2))
            cnt += 1
    return tot / cnt


def sobel(v, w, h):
    g = [0.0] * (w * h)
    for y in range(1, h - 1):
        r0, r1, r2 = (y - 1) * w, y * w, (y + 1) * w
        for x in range(1, w - 1):
            gx = (v[r0 + x + 1] + 2 * v[r1 + x + 1] + v[r2 + x + 1]) - (v[r0 + x - 1] + 2 * v[r1 + x - 1] + v[r2 + x - 1])
            gy = (v[r2 + x - 1] + 2 * v[r2 + x] + v[r2 + x + 1]) - (v[r0 + x - 1] + 2 * v[r0 + x] + v[r0 + x + 1])
            g[r1 + x] = math.hypot(gx, gy)
    return g


def corr(a, b):
    n = len(a)
    ma, mb = sum(a) / n, sum(b) / n
    sab = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    saa = sum((x - ma) ** 2 for x in a)
    sbb = sum((y - mb) ** 2 for y in b)
    if saa < 1e-9 and sbb < 1e-9:
        return 1.0
    if saa < 1e-9 or sbb < 1e-9:
        return 0.0
    return sab / math.sqrt(saa * sbb)


def hist_dist(a, b, bins=26, top=260.0):
    ha, hb = [0] * bins, [0] * bins
    for x in a:
        ha[min(bins - 1, int(x * bins / top))] += 1
    for x in b:
        hb[min(bins - 1, int(x * bins / top))] += 1
    return sum(abs(p - q) for p, q in zip(ha, hb)) / float(len(a))


def ff_metrics(g_png, r_png):
    w, h = DS
    a, b = gray(g_png, DS), gray(r_png, DS)
    m = {"mad": sum(abs(x - y) for x, y in zip(a, b)) / len(a), "ssim": ssim(a, b, w, h),
         "edge": corr(sobel(a, w, h), sobel(b, w, h)), "mean_g": sum(a) / len(a), "mean_r": sum(b) / len(b)}
    ca, cb = gray(g_png, COARSE), gray(r_png, COARSE)
    m["mad_c"] = sum(abs(x - y) for x, y in zip(ca, cb)) / len(ca)
    m["hist"] = hist_dist(a, b)
    return m, (a, b)


def ff_verdict(cls, m):
    if cls in EXACT:
        return "pass" if m["mad"] <= MAD_MAX and m["ssim"] >= SSIM_MIN else "FAIL"
    return "pass~" if m["mad_c"] <= LOOSE_MAD_MAX and m["hist"] <= LOOSE_HIST_MAX else "FAIL~"


def palette(png):
    """256 x (r, g, b) read at the 'zz reforce sweep ramp' dot centers (median of the three rows)."""
    im = _bmp(png, None, "full")
    rows = [im.py(y) for y in (0.5, 0.0, -0.5)]
    out = []
    for k in range(256):
        x = im.px(-0.96 + 1.92 * k / 255.0)
        rgb = []
        for ch in range(3):
            vals = sorted(im.v(x, y, ch) for y in rows)
            rgb.append(vals[1])
        out.append(tuple(rgb))
    return out


def cm_metrics(g_png, r_png):
    pg, pr = palette(g_png), palette(r_png)
    d = sorted(abs(a - b) for p, q in zip(pg, pr) for a, b in zip(p, q))
    avg = lambda p, c: sum(x[c] for x in p) / 256.0  # noqa: E731
    return {"mean": sum(d) / len(d), "p95": d[int(0.95 * (len(d) - 1))], "max": d[-1],
            "avgcol": max(abs(avg(pg, c) - avg(pr, c)) for c in range(3))}, (pg, pr)


def cm_verdict(cls, m):
    if cls == "time":
        return "pass~" if m["avgcol"] <= CM_TIME_MAX else "FAIL~"
    return "pass" if m["mean"] <= CM_MEAN_MAX and m["p95"] <= CM_P95_MAX else "FAIL"


# ------------------------------------------------------------------------------------------ PNG writing
def write_png(path, w, h, rows_rgb):
    raw = b"".join(b"\x00" + bytes(r) for r in rows_rgb)

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(data)


def gray_png(path, v, w, h, gain=2.5):
    rows = []
    for y in range(h):
        r = bytearray()
        for x in range(w):
            q = max(0, min(255, int(v[y * w + x] * gain)))
            r += bytes((q, q, q))
        rows.append(r)
    write_png(path, w, h, rows)


def diff_png(path, a, b, w, h, gain=5.0):
    rows = []
    for y in range(h):
        r = bytearray()
        for x in range(w):
            d = (a[y * w + x] - b[y * w + x]) * gain
            r += bytes((max(0, min(255, int(d))), 0, max(0, min(255, int(-d)))))  # red: G-Force brighter
        rows.append(r)
    write_png(path, w, h, rows)


def palette_png(path, pg, pr):
    rows = []
    for band in (pg, pr, [tuple(min(255, 4 * abs(a - b)) for a, b in zip(p, q)) for p, q in zip(pg, pr)]):
        line = bytearray()
        for p in band:
            line += bytes(p) * 2
        rows += [line] * 16
    write_png(path, 512, len(rows), rows)


# ------------------------------------------------------------------------------------------------ sides
def gforce_shots(cap_dir):
    files = sorted(glob.glob(os.path.join(cap_dir, "G-Force Screen Shot *.png")))
    return {int(re.search(r"(\d+)\.png$", f).group(1)): f for f in files}


def reforce_shots(d):
    idx = json.load(open(os.path.join(d, "index.json")))
    return {int(k): os.path.join(d, v) for k, v in idx["shots"].items()}


# -------------------------------------------------------------------------------------------- comparing
def compare_all(sources, reforce_dir, gallery=None, shuffle=False):
    """sources: {timeline: ('gforce', dir) | ('reforce', dir)}. -> [result dict]."""
    results = []
    if gallery:
        os.makedirs(os.path.join(gallery, "img"), exist_ok=True)
    for tl in S.sweep_timelines():
        if tl not in sources:
            continue
        hdr, _, slots = S.parse_sweep(os.path.join(S.TIMELINE_DIR, tl + ".py"))
        kind = hdr["kind"]
        side, d = sources[tl]
        ref = gforce_shots(d) if side == "gforce" else reforce_shots(d)
        mine = reforce_shots(os.path.join(reforce_dir, tl))
        note = ""
        if len(ref) != len(slots):
            note = "%s has %d shots for %d slots: shot numbers may be misaligned" % (d, len(ref), len(slots))
            print("WARNING %s: %s" % (tl, note))
        for sl in slots:
            n = sl["shot"]
            g = ref.get(n % len(slots) + 1 if shuffle else n)
            r = mine.get(n)
            row = {"timeline": tl, "kind": kind, "slot": n, "name": sl["name"], "cls": sl["cls"],
                   "source": sl["source"], "g": g, "r": r}
            if not g or not r:
                row.update(verdict="missing", m={})
                results.append(row)
                continue
            if kind == "FlowField":
                if native_size(g) != (1980, 1260):
                    row.update(verdict="size %dx%d" % native_size(g), m={})
                    results.append(row)
                    continue
                m, (a, b) = ff_metrics(g, r)
                row.update(m=m, verdict=ff_verdict(sl["cls"], m))
                if gallery:
                    base = os.path.join(gallery, "img", "%s_%03d" % (tl, n))
                    gray_png(base + "_g.png", a, DS[0], DS[1])
                    gray_png(base + "_r.png", b, DS[0], DS[1])
                    diff_png(base + "_d.png", a, b, DS[0], DS[1])
            else:
                m, (pg, pr) = cm_metrics(g, r)
                row.update(m=m, verdict=cm_verdict(sl["cls"], m))
                if gallery:
                    palette_png(os.path.join(gallery, "img", "%s_%03d_p.png" % (tl, n)), pg, pr)
            results.append(row)
    return results


def score(row):
    m = row.get("m") or {}
    if row["kind"] == "FlowField":
        return m.get("mad", 1e9) if row["cls"] in EXACT else m.get("mad_c", 1e9)
    return m.get("mean", 1e9) if row["cls"] != "time" else m.get("avgcol", 1e9)


def summary(results):
    out = {}
    for r in results:
        k = out.setdefault((r["kind"], r["cls"]), {"n": 0, "pass": 0, "fail": 0, "other": 0})
        k["n"] += 1
        if r["verdict"].startswith("pass"):
            k["pass"] += 1
        elif r["verdict"].startswith("FAIL"):
            k["fail"] += 1
        else:
            k["other"] += 1
    return out


def fmt(v, p=2):
    return "-" if v is None else ("%.*f" % (p, v))


def write_md(results, path, sources, title_note=""):
    L = ["# Corpus sweep (Phase 5B.4)", "",
         "Generated by `tools/sweep/compare.py` (numbers only; G-Force screenshots and the gallery stay local).",
         "Design and timing: `tools/sweep/make_timelines.py`; classes: `tools/sweep/sweeplib.py`; metrics and "
         "thresholds: `tools/sweep/compare.py`.", ""]
    if title_note:
        L += [title_note, ""]
    L += ["Sources: " + ", ".join("%s = %s `%s`" % (tl, s, os.path.relpath(d, S.REPO)) for tl, (s, d) in
                                  sorted(sources.items())), ""]
    L += ["## Summary", "", "| type | class | configs | pass | fail | other |", "|---|---|---:|---:|---:|---:|"]
    for (kind, cls), c in sorted(summary(results).items()):
        L.append("| %s | %s | %d | %d | %d | %d |" % (kind, cls, c["n"], c["pass"], c["fail"], c["other"]))
    L += ["", "Thresholds: FlowField exact (det, derand): mad <= %.1f and ssim >= %.2f; loose (noise, rnd-load, time): "
          "mad_c <= %.1f and hist <= %.2f. ColorMap exact (det, derand, png, ref): mean <= %.1f and p95 <= %.1f; "
          "time: avg color <= %.0f. `pass~`/`FAIL~` = loose verdict." % (MAD_MAX, SSIM_MIN, LOOSE_MAD_MAX,
                                                                          LOOSE_HIST_MAX, CM_MEAN_MAX, CM_P95_MAX,
                                                                          CM_TIME_MAX), ""]
    for kind in ("FlowField", "ColorMap"):
        rows = [r for r in results if r["kind"] == kind]
        if not rows:
            continue
        bad = sorted([r for r in rows if not r["verdict"].startswith("pass")], key=lambda r: -score(r))
        L += ["## %s: worst offenders" % kind, ""]
        if not bad:
            L += ["None: every %s passes." % kind, ""]
        else:
            L += ["%d of %d not passing, worst first:" % (len(bad), len(rows)), ""]
            L += [", ".join("`%s` (%s, %s)" % (r["name"], r["cls"], fmt(score(r))) for r in bad[:30]), ""]
        L += ["## %s: all configs" % kind, ""]
        if kind == "FlowField":
            L += ["| # | config | class | mad | ssim | edge | mad_c | hist | verdict |",
                  "|---|---|---|---:|---:|---:|---:|---:|---|"]
            for r in rows:
                m = r["m"]
                L.append("| %s.%d | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                    r["timeline"][-2:], r["slot"], r["name"], r["cls"], fmt(m.get("mad")), fmt(m.get("ssim"), 3),
                    fmt(m.get("edge"), 3), fmt(m.get("mad_c")), fmt(m.get("hist"), 3), r["verdict"]))
        else:
            L += ["| # | config | class | mean | p95 | max | avg color | verdict |", "|---|---|---|---:|---:|---:|---:|---|"]
            for r in rows:
                m = r["m"]
                L.append("| %d | %s | %s | %s | %s | %s | %s | %s |" % (
                    r["slot"], r["name"], r["cls"], fmt(m.get("mean")), fmt(m.get("p95"), 0), fmt(m.get("max"), 0),
                    fmt(m.get("avgcol"), 1), r["verdict"]))
        L.append("")
    # keep the hand-written notes and the validation section written by validate.py
    old = open(path).read() if os.path.isfile(path) else ""
    for b, e in (("<!-- notes:begin -->", "<!-- notes:end -->"), ("<!-- validation:begin -->", "<!-- validation:end -->")):
        if b in old and e in old:
            L += [old[old.index(b):old.index(e) + len(e)], ""]
    open(path, "w").write("\n".join(L))


def write_gallery(results, gallery):
    rows = sorted(results, key=lambda r: (r["kind"], r["verdict"].startswith("pass"), -score(r)))
    H = ["<!doctype html><meta charset=utf-8><title>reforce sweep gallery (local)</title>",
         "<style>body{font:13px system-ui;background:#111;color:#ddd}td{vertical-align:top;padding:4px}"
         "img{display:block}.FAIL{color:#f66}.pass{color:#6c6}</style>",
         "<h1>reforce corpus sweep (local only, never commit)</h1>"
         "<p>FlowFields: G-Force | reforce | diff (red: G-Force brighter, blue: reforce brighter, x5). Images are "
         "330x210 averages, gain 2.5. ColorMaps: G-Force / reforce / |diff| x4 palettes.</p><table>"]
    for r in rows:
        m = r.get("m") or {}
        stats = " ".join("%s=%s" % (k, fmt(v, 3)) for k, v in sorted(m.items()))
        cls = "pass" if r["verdict"].startswith("pass") else "FAIL"
        cell = "<td><b>%s</b><br>%s.%d %s<br><span class=%s>%s</span><br><small>%s<br>%s</small></td>" % (
            html.escape(r["name"]), r["timeline"], r["slot"], r["cls"], cls, r["verdict"], stats,
            html.escape(r["source"]))
        base = "img/%s_%03d" % (r["timeline"], r["slot"])
        if r["kind"] == "FlowField":
            imgs = "".join("<td><img src='%s_%s.png'></td>" % (base, s) for s in "grd")
        else:
            imgs = "<td><img src='%s_p.png'></td>" % base
        H.append("<tr>%s%s</tr>" % (cell, imgs))
    H.append("</table>")
    open(os.path.join(gallery, "index.html"), "w").write("\n".join(H))


# ----------------------------------------------------------------------------------------- calibration
def calibrate():
    """Metrics on known-good pairs: G-Force probe captures (runs 1-4) vs the parity suite's reforce frames."""
    import render as PR  # tools/parity
    import timeline as PT
    print("FlowField metrics on parity-suite pairs (G-Force capture vs reforce frame; known good):")
    for run, stamp, fps in PR.RUNS:
        cap = os.path.join(S.REPO, "probes", "captures", stamp)
        frames = os.path.join(S.REPO, "build", "parity", "frames", run)
        if not os.path.isdir(cap) or not os.path.isdir(frames):
            print("  %s: missing captures or build/parity frames (run tools/parity/parity.py)" % run)
            continue
        ev = PT.parse(os.path.join(S.REPO, "probes", "timelines", run + ".py"))
        _, _, shots, _ = PT.reforce_args(ev, fps)
        labels = [e.label for e in ev if e.kind == "shot"]
        g = gforce_shots(cap)
        for n, fr in sorted(shots.items()):
            r = os.path.join(frames, "frame_%05d.png" % fr)
            if n not in g or not os.path.isfile(r):
                continue
            m, _ = ff_metrics(g[n], r)
            print("  %s #%02d %-24s mad %5.2f ssim %.3f edge %.3f mad_c %5.2f hist %.3f  (mean G %.1f R %.1f)" % (
                run, n, labels[n - 1], m["mad"], m["ssim"], m["edge"], m["mad_c"], m["hist"], m["mean_g"], m["mean_r"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--captures-map", default=os.path.join(S.WORK, "captures.json"))
    ap.add_argument("--captures", action="append", default=[], help="TIMELINE=DIR (G-Force screenshots)")
    ap.add_argument("--reference-dir", default=None, help="reforce render used in place of G-Force (validation)")
    ap.add_argument("--reforce-dir", default=os.path.join(S.WORK, "reforce"))
    ap.add_argument("--out", default=os.path.join(S.REPO, "docs", "sweep.md"))
    ap.add_argument("--gallery", default=os.path.join(S.WORK, "gallery"))
    ap.add_argument("--no-gallery", action="store_true")
    ap.add_argument("--shuffle", action="store_true", help="compare slot n against reference slot n+1 (validation)")
    ap.add_argument("--json", default=None, help="also write the raw results here")
    ap.add_argument("--calibrate", action="store_true")
    a = ap.parse_args()
    if a.calibrate:
        calibrate()
        return 0
    sources = {}
    if a.reference_dir:
        for tl in S.sweep_timelines():
            if os.path.isfile(os.path.join(a.reference_dir, tl, "index.json")):
                sources[tl] = ("reforce", os.path.join(a.reference_dir, tl))
    else:
        if os.path.isfile(a.captures_map):
            for tl, d in json.load(open(a.captures_map)).items():
                sources[tl] = ("gforce", d if os.path.isabs(d) else os.path.join(S.REPO, d))
        for kv in a.captures:
            tl, d = kv.split("=", 1)
            sources[tl] = ("gforce", os.path.abspath(d))
    if not sources:
        print("no G-Force captures (run tools/sweep/run_gforce_sweeps.sh, or pass --captures TL=DIR)")
        return 77
    gallery = None if a.no_gallery else a.gallery
    results = compare_all(sources, a.reforce_dir, gallery, a.shuffle)
    for r in results:
        if r["cls"] == "ref" and not r["verdict"].startswith("pass"):
            print("WARNING: the gray reference ColorMap (slot %d) does not read back as identity: the ramp geometry "
                  "or Pen->intensity mapping differs, so ColorMap verdicts are not meaningful (see the gallery)."
                  % r["slot"])
    note = "" if not a.reference_dir else "VALIDATION RUN: the reference is reforce itself (%s), not G-Force." % (
        os.path.relpath(a.reference_dir, S.REPO))
    write_md(results, a.out, sources, note)
    if gallery:
        write_gallery(results, gallery)
    if a.json:
        json.dump(results, open(a.json, "w"), indent=1)
    for (kind, cls), c in sorted(summary(results).items()):
        print("%-9s %-9s %3d configs: %3d pass, %3d fail, %d other" % (kind, cls, c["n"], c["pass"], c["fail"],
                                                                        c["other"]))
    print("wrote %s%s" % (os.path.relpath(a.out, S.REPO), "" if not gallery else
                         " and %s/index.html" % os.path.relpath(gallery, S.REPO)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

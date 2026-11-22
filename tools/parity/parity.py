#!/usr/bin/env python3
"""reforce <-> G-Force parity suite (milestone M7a).

Replays each black-box probe timeline (probes/timelines/runN.py) in reforce's deterministic offline mode,
using a scenes root that holds only our probe configs, then runs the SAME measurement for every probe
finding (docs/probe-results.md) on the G-Force screenshot and on the matching reforce frame, and writes
docs/parity.md (numbers only; never images).

    python3 -I tools/parity/parity.py [--captures DIR] [--reforce PATH] [--work DIR] [--out FILE]
                                      [--no-render] [--strict] [--skip-code N] [--set KEY=VALUE ...]

Exit codes: 0 = report written (all pass, or failures without --strict), 1 = failures with --strict,
--skip-code (default 77) = no G-Force captures available (nothing to compare), 2 = usage / reforce error.
Stdlib only; needs macOS `sips` for PNG decoding.
"""
import argparse
import math
import os
import sys

sys.dont_write_bytecode = True  # keep the source tree clean
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import images  # noqa: E402
import linewidth  # noqa: E402
import measure as M  # noqa: E402
import parity5b  # noqa: E402
import render  # noqa: E402
import timeline  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
JITTER_FRAMES = 2  # G-Force's TakeScreenshot() lands within +-1-2 frames of the scripted time

# Measurements that fail for an understood (or bounded) reason that is not a reforce bug we can fix from the
# probe data. They are reported as "gap"; --strict ignores them, so ctest catches only new regressions.
KNOWN_GAPS = {
    (10, "mid-morph: stripe angle"):
        "At mid-morph G-Force's trails look like reforce's ~0.13 s (1.3 %) earlier in the 10 s FlowField morph "
        "(reforce reproduces G-Force's angle with the morph started 0.13-0.15 s later). Both end states match, "
        "and the dark-center width agrees. Consistent with G-Force starting a FlowField morph only after its "
        "threaded field build, or with FlowField.Transition.Duration.Deviation (0.02) applying to scripted "
        "morphs too; one shot cannot tell. reforce keeps the exact scripted timing",
    (19, "run4 age 15.00 s"):
        "The fade-out tail is 3 levels brighter in G-Force: its fade-out (and only the fade-out; the fade-in "
        "matches at 60 fps) starts ~0.07 s (4 frames) after spawn + 6 + PDur. Run 4's three fade-out points all "
        "fit a 0.07 s later start (198/93/19 vs G-Force 197/93/20); reforce keeps the probe #19 model",
}


KNOWN_GAPS.update(parity5b.KNOWN_GAPS_5B)  # 5B probes (runs 17-20): findings #30-#34


def known_gap(num, item):
    for (n, prefix), why in KNOWN_GAPS.items():
        if n == num and item.startswith(prefix):
            return why
    return None


# ----------------------------------------------------------------------------------------------------------
# G-Force model pieces used only to size tolerances (sensitivity to +-JITTER_FRAMES of shot timing).
def fn_pow(e):
    return lambda p: 1 - (1 - min(max(p, 0.0), 1.0)) ** e


FN_WS, FN_FF, FN_CM, FN_P = fn_pow(1.6), fn_pow(1.35), fn_pow(1.5), fn_pow(1.5)  # G-Force default curves


def dilation(u):
    return 1 + 15.5 * u ** 4.16  # probe #12


def particle_env(age, fade=6.0, hold=4.0):
    if age < 0:
        return 0.0
    if age < fade:
        return FN_P(age / fade)
    if age < fade + hold:
        return 1.0
    return max(0.0, 1 - FN_P((age - fade - hold) / fade))


def jitter_tol(f, t, fps, base):
    """base + half the change of f over +-JITTER_FRAMES around t."""
    d = JITTER_FRAMES / fps
    return base + 0.5 * abs(f(t + d) - f(t - d))


# ----------------------------------------------------------------------------------------------------------
class Row:
    """One measurement. Frame-counted rows (judge_rate) carry a second reforce value rendered at G-Force's
    effective frame rate (`re`); they pass when G-Force lies within tol of the interval between the two
    reforce values (G-Force's true rate during a segment lies between its measured effective rate and the
    target)."""

    def __init__(self, item, g, r, tol, unit="", re=None, judge_rate=False, info=False, note=""):
        self.item, self.g, self.r, self.tol, self.unit = item, g, r, tol, unit
        self.re = re
        self.judge_rate = judge_rate and re is not None
        self.info = info  # informational: not scored
        self.note = note

    @property
    def delta(self):
        """Signed distance from G-Force to reforce (to the nearer end of the interval for judge_rate rows)."""
        if self.g is None or self.r is None or (isinstance(self.r, float) and math.isnan(self.r)):
            return float("nan")
        if not self.judge_rate:
            return self.r - self.g
        lo, hi = min(self.r, self.re), max(self.r, self.re)
        if lo <= self.g <= hi:
            return 0.0
        return lo - self.g if self.g < lo else hi - self.g

    @property
    def judged(self):
        return self.g + self.delta if not math.isnan(self.delta) else self.r

    @property
    def ok(self):
        d = self.delta
        return not math.isnan(d) and abs(d) <= self.tol + 1e-9


class Finding:
    def __init__(self, num, title, method, tolerance):
        self.num, self.title, self.method, self.tolerance = num, title, method, tolerance
        self.rows = []

    def add(self, *a, **k):
        row = Row(*a, **k)
        row.gap = known_gap(self.num, row.item) if not row.ok else None
        self.rows.append(row)


class Context:
    def __init__(self, captures, work, frames, rate_frames):
        self.captures, self.work = captures, work
        self.frames, self.rate_frames = frames, rate_frames  # {run: {shot: png}}
        self.cache = os.path.join(work, "cache")
        self.fps = {r: f for r, _, f in render.RUNS}
        self.effective = {}
        self._img = {}
        self.frames5, self.shots5 = {}, {}  # 5B runs (parity5b): {run: {shot: png}}, {run: [(label, T)]}
        self.events5 = {}  # 5B runs: {run: [timeline.Event]} (script call times)

    def _load(self, key, png, sub):
        if key not in self._img:
            self._img[key] = images.load(png, os.path.join(self.cache, sub))
        return self._img[key]

    def g(self, run, n):
        d = dict((r, dd) for r, dd, _ in render.RUNS + [render.RUN6[:3]])[run]
        return self._load(("g", run, n), os.path.join(self.captures, d, "G-Force Screen Shot %04d.png" % n),
                          "gforce-" + run)

    def r(self, run, n):
        return self._load(("r", run, n), self.frames[run][n], "reforce-" + run)

    def re(self, run, n):
        if run not in self.rate_frames:
            return None
        return self._load(("re", run, n), self.rate_frames[run][n], "reforce-rate-" + run)

    def g5(self, run, n):
        d = parity5b.RUNS_5B[run][0]
        return self._load(("g", run, n), os.path.join(self.captures, d, "G-Force Screen Shot %04d.png" % n),
                          "gforce-" + run)

    def r5(self, run, n):
        return self._load(("r", run, n), self.frames5[run][n], "reforce-" + run)

    def each(self, run, n, fn):
        """fn applied to G-Force, reforce, reforce@effective rate (None when not rendered)."""
        re = self.re(run, n)
        return fn(self.g(run, n)), fn(self.r(run, n)), (fn(re) if re is not None else None)


# ----------------------------------------------------------------------------------------------------------
# Effective G-Force frame rate (from the B0 frame counter of run 1).
B_PX_PER_FRAME = 0.002 * 990  # X0 = wrap(B0 * .001) * 2 - 1 at 1980 px


def b_head_edge(img):
    """Right edge (px) of the B-persistence trail on row y = -.5."""
    rr = M.runs(img.row(img.py(-0.5)), 40)
    return rr[0][1] if rr and rr[0][0] == 0 else float("nan")


def effective_rates(captures, work):
    ctx = Context(captures, work, {}, {})
    e0, e1 = b_head_edge(ctx.g("run1", 8)), b_head_edge(ctx.g("run1", 9))
    fps60 = (e1 - e0) / B_PX_PER_FRAME / 1.8
    overhead = 1.0 / fps60 - 1.0 / 60
    return {60: fps60, 30: 1.0 / (1.0 / 30 + overhead), "overhead_ms": overhead * 1000}


# ----------------------------------------------------------------------------------------------------------
def ramp_bar_x(k):
    return k / 16.0 - 0.97  # zz reforce ramp: bar k (Pen k/31)


def bar_value(img, k, ch=0):
    """Median of the 9x9 px core of ramp bar k's first dot (step i = 2k at y = .4 * 2k/63 - .2): the dot is
    redrawn every frame (overwrite), so its core is exactly the current Pen whatever trails lie around it."""
    x, y = ramp_bar_x(k), 0.4 * (2 * k) / 63 - 0.2
    cx, cy = img.px(x), img.py(y)
    return M.median([v for yy in range(cy - 4, cy + 5) for v in img.row(yy, ch, cx - 4, cx + 5)])


def bar_rgb(img, k):
    return tuple(bar_value(img, k, ch) for ch in range(3))


def checks(ctx, rates):
    F = []

    # #1, #2 operator precedence (run 1 shot 6)
    f = Finding(1, "Unary minus vs ^ (`-1^2`)", "y of the dot drawn at x = -.5 (Y0 = -1^2 * .5): intensity-weighted "
                "centroid of pixels > 200 (the fresh dots; the frozen zoom-out rings are dimmer) in the box above or "
                "below y = 0, whichever holds the dot", "+-0.01 (6 px)")

    def dot_y(im, x, gap):
        hits = [c for c in (M.centroid(im, x - 0.1, gap, x + 0.1, 0.95, thr=200),
                            M.centroid(im, x - 0.1, -0.85, x + 0.1, -gap, thr=200)) if c]
        return hits[0][1] if len(hits) == 1 else float("nan")

    g, r, _ = ctx.each("run1", 6, lambda im: dot_y(im, -0.5, 0.1))
    f.add("dot y", g, r, 0.01)
    F.append(f)
    f = Finding(2, "^ associativity (`2^3^2`)", "y of the dot at x = +.5 (Y1 = 2^3^2 / 1000)", "+-0.01")
    g, r, _ = ctx.each("run1", 6, lambda im: dot_y(im, 0.5, 0.03))
    f.add("dot y", g, r, 0.01)
    F.append(f)

    # #3 s spacing
    f = Finding(3, "`s` spacing (inclusive i/(N-1))",
                "run 2 shot 3 (s3: X0 = s - .5, void field): centers of the dots on row y = 0. Run 1 shot 7 "
                "(X0 = 2s - 1): number of dots drawn on row y = 0 (x = -1, 0, +1)", "+-0.005 (5 px); count exact")
    g, r, _ = ctx.each("run2", 3, lambda im: M.row_runs(im, 0, 100))
    for i in range(3):
        f.add("dot %d x" % (i + 1), g[i] if i < len(g) else None, r[i] if i < len(r) else None, 0.005)
    g, r, _ = ctx.each("run1", 7, lambda im: len(M.row_runs(im, 0, 100)))
    f.add("dots drawn for x = 2s-1, Stps=3", g, r, 0, note="G-Force draws no dot at x = +1 (the right edge)")
    F.append(f)

    # #4 B persistence (run 1 shots 8, 9)
    f = Finding(4, "B var starts at 0 and persists",
                "B0 = B0+1 drives X0 = wrap(B0*.001)*2-1: B0 = frames since the switch, from the trail's right "
                "edge on row y = -.5 (minus the dot radius, measured as half the trail thickness)",
                "+-%d frames (shot jitter)" % JITTER_FRAMES)

    def b_frames(im):
        e = b_head_edge(im)
        col = im.col(int(e) - 6, 0, im.py(-0.5) - 30, im.py(-0.5) + 31)
        radius = sum(col) / 255.0 / 2
        return ((e - radius + 0.5) / 990.0) * 500

    g8, r8, re8 = ctx.each("run1", 8, b_frames)
    g9, r9, re9 = ctx.each("run1", 9, b_frames)
    f.add("B0 0.2 s after the switch", g8, r8, JITTER_FRAMES, "frames", re=re8, judge_rate=True)
    f.add("B0 2.0 s after the switch", g9, r9, JITTER_FRAMES, "frames", re=re9, judge_rate=True)
    f.add("frames between the two shots (1.8 s)", g9 - g8, r9 - r8, 2 * JITTER_FRAMES, "frames",
          re=(re9 - re8) if re9 is not None else None, info=True,
          note="defines G-Force's effective frame rate (%.1f fps at TargetFrameRate 60)" % rates[60])
    F.append(f)

    # #5 C per-step vector (run 1 shot 8)
    f = Finding(5, "C vars: per-step vector persisting across frames",
                "C0 = C0+.1, Y1 = C0 at X1 = s-.5 (Stps=5): rungs (dot centers) along the five columns, y in "
                "(.05, .95]", "spacing +-0.005; columns identical within 0.005; rung count exact")

    def ladder(im):
        cols = [M.col_runs(im, x, 40, 0.05, 0.95) for x in (-0.5, -0.25, 0, 0.25, 0.5)]
        sp = [b - a for c in cols for a, b in zip(c[1:], c[:-1])]
        n = min(len(c) for c in cols)
        diff = max(abs(c[i] - cols[0][i]) for c in cols for i in range(n)) if n else float("nan")
        return M.median(sp), diff, len(cols[2])

    g, r, _ = ctx.each("run1", 8, ladder)
    f.add("rung spacing", g[0], r[0], 0.005)
    f.add("max difference between columns", g[1], r[1], 0.005)
    f.add("rungs in (.05, .95] at 0.2 s", g[2], r[2], 0)
    F.append(f)

    # #6 decay
    f = Finding(6, "Decay: linear, 1/255 per frame",
                "Per frame (frame-rate independent): slope of the B-persistence trail (levels/px x 1.98 px/frame, "
                "run 1 shot 9) and the intensity step between consecutive C-ladder rungs (one rung per frame, shot 8). "
                "Per second: median of the flash bar (row y = 0, x in [-.8, .8]) at the shot times; these count "
                "G-Force frames, so they are judged against the range between reforce at the target and at G-Force's effective rate",
                "per frame +-0.1 level; bar values +-%d levels (+-%d frames of jitter, 1 level/frame, + rounding)"
                % (JITTER_FRAMES + 1, JITTER_FRAMES))
    g, r, _ = ctx.each("run1", 9, lambda im: -M.trail_slope(im, -0.5, int(b_head_edge(im))) * B_PX_PER_FRAME)
    f.add("trail decay per frame (60 fps run)", g, r, 0.1, "levels")

    def rung_step(im):
        c = M.col_runs(im, 0, 20, 0.05, 0.95)
        peaks = [M.box_max(im, -0.01, y - 0.005, 0.01, y + 0.005) for y in c]
        d = [a - b for a, b in zip(peaks[:-1], peaks[1:])]  # top rung is the newest
        return M.median(d)

    g, r, _ = ctx.each("run1", 8, rung_step)
    f.add("ladder step per frame", g, r, 0.1, "levels")
    bar = lambda im: M.box_median(im, -0.8, -0.01, 0.8, 0.01, step=3)
    for n, label in ((1, "+0.25 s"), (2, "+1.0 s"), (3, "+2.0 s"), (4, "+4.0 s")):
        g, r, re = ctx.each("run1", n, bar)
        f.add("60 fps, bar at %s" % label, g, r, JITTER_FRAMES + 1, "levels", re=re, judge_rate=True)
    for n, label in ((1, "+1.0 s"), (2, "+2.0 s"), (3, "+4.0 s")):
        g, r, re = ctx.each("run3", n, bar)
        f.add("30 fps, bar at %s" % label, g, r, JITTER_FRAMES + 1, "levels", re=re, judge_rate=True)
    F.append(f)

    # #7 Pen ramp, #8 clamp
    f = Finding(7, "Pen -> intensity (linear, 255 x Pen)", "run 1 shot 11: median of the 9x9 px core of each ramp bar k's first dot (Pen = k/31)",
                "+-2 levels")
    gv = [bar_value(ctx.g("run1", 11), k) for k in range(32)]
    rv = [bar_value(ctx.r("run1", 11), k) for k in range(32)]
    for k in (9, 16, 24, 30):
        f.add("bar %d (Pen %.3f)" % (k, k / 31), gv[k], rv[k], 2, "levels")
    f.add("max |delta| over the 32 bars", 0, max(abs(a - b) for a, b in zip(gv, rv)), 2, "levels")
    F.append(f)
    f = Finding(8, "ColorMap channels clamp to [0, 1]", "run 1 shot 12 (R = 2i, G = -1 + i): bar dot cores per channel",
                "+-2 levels")
    for k in (8, 24):
        g, r, _ = ctx.each("run1", 12, lambda im: bar_value(im, k, 0))
        f.add("R at bar %d (2 x %.3f)" % (k, k / 31), g, r, 2, "levels")
    g, r, _ = ctx.each("run1", 12, lambda im: max(bar_value(im, k, 1) for k in range(32)))
    f.add("max G over all bars", g, r, 2, "levels")
    F.append(f)

    # #9 ColorMap morph
    f = Finding(9, "ColorMap morph = RGB lerp", "run 1 shots 13-15 (red -> green over 10 s): RGB of ramp bar 31",
                "+-3 levels (+-2 frames move w by < .005)")
    for n, label in ((13, "25%"), (14, "50%"), (15, "done")):
        g, r, _ = ctx.each("run1", n, lambda im: bar_rgb(im, 31))
        f.add("%s R" % label, g[0], r[0], 3, "levels")
        f.add("%s G" % label, g[1], r[1], 3, "levels")
    F.append(f)

    # #10 FlowField morph
    f = Finding(10, "FlowField morph = linear source lerp",
                "run 1 shots 16-18 (+.3 rad -> -.3 rad rotation over 10 s, two vertical lines): dominant stripe "
                "angle of the trails left (x in [-.95, -.6]) and right ([.6, .95]) of the lines (structure tensor), and "
                "the half-width of the dark center on row y = 0", "angle +-2 deg; dark center +-0.01")
    for n, label in ((16, "rot +"), (17, "mid-morph"), (18, "rot -")):
        g, r, re = ctx.each("run1", n, lambda im: M.stripe_angle(im, -0.95, -0.6, -0.5, 0.5))
        f.add("%s: stripe angle left" % label, g, r, 2, "deg", re=re)
        g, r, re = ctx.each("run1", n, lambda im: M.stripe_angle(im, 0.6, 0.95, -0.5, 0.5))
        f.add("%s: stripe angle right" % label, g, r, 2, "deg", re=re)
        g, r, _ = ctx.each("run1", n, M.dark_center)
        f.add("%s: dark center half-width" % label, g, r, 0.01)
    F.append(f)

    # #11 WaveShape morph (matched) and #12 (unmatched)
    f = Finding(11, "WaveShape morph: matched paths lerp per index",
                "run 2 (void field): x of every curve crossing on row y = .3 (circle -> two lines, shots 7-10; two "
                "lines -> circle, shots 11-15). The unmatched line (path 1) is included where it is on screen",
                "+-0.01 + the change over +-%d frames (dilation model for the unmatched line)" % JITTER_FRAMES)
    for n in range(7, 16):
        t_shot = {7: 0.5, 8: 1, 9: 2, 10: 5, 11: 0.5, 12: 1, 13: 2, 14: 5, 15: 8}[n]
        incoming = n <= 10
        g, r, _ = ctx.each("run2", n, lambda im: M.row_runs(im, 0.3, 100))
        for i in range(max(len(g), len(r))):
            gi = g[i] if i < len(g) else None
            ri = r[i] if i < len(r) else None
            ref = gi if gi is not None else ri
            unmatched = ref is not None and ref > 0.45 and ((incoming and n == 10) or (not incoming and i == len(g) - 1))
            if unmatched:
                u = (lambda tt: 1 - FN_WS(tt / 10)) if incoming else (lambda tt: FN_WS(tt / 10))
                tol = jitter_tol(lambda tt: 0.5 * dilation(u(tt)), t_shot, 60, 0.01)
            else:
                tol = 0.01 + 0.5 * abs(FN_WS((t_shot + 2 / 60) / 10) - FN_WS((t_shot - 2 / 60) / 10))
            f.add("shot %d (%s %.0f%%) crossing %d%s" % (n, "1->2" if incoming else "2->1", t_shot * 10, i + 1,
                                                        " (unmatched)" if unmatched else ""), gi, ri, tol)
    F.append(f)

    f = Finding(12, "WaveShape morph: unmatched paths dilate by 1 + 15.5 u^4.16",
                "run 3 (30 fps, void field): two dots -> one dot; the outgoing dot (.2, .3): f - 1 from its centroid "
                "x / .2 and y / .3", "+-0.005 + the change over +-%d frames at 30 fps" % JITTER_FRAMES)
    for n in range(5, 11):
        t_shot = (n - 3) * 0.5
        g, r, _ = ctx.each("run3", n, lambda im: M.centroid(im, -0.3, -0.85, 1.0, 1.0, thr=60))
        tol = jitter_tol(lambda tt: dilation(FN_WS(tt / 10)), t_shot, 30, 0.005)
        f.add("shot %d (+%.1f s) from x" % (n, t_shot), g[0] / 0.2 - 1, r[0] / 0.2 - 1, tol)
        f.add("shot %d (+%.1f s) from y" % (n, t_shot), g[1] / 0.3 - 1, r[1] / 0.3 - 1, tol)
    F.append(f)

    # #13 out of frame
    f = Finding(13, "Out-of-frame source is black", "run 1 shot 5 (zoom-out field, circle r = .5): mean value "
                "outside radius .6, and the number of rings on row y = 0 for x in [0, .6] (> 64). Also the frame "
                "order of a simultaneous cut: shot 6 (identity + precedence at 0:14) keeps the frozen rings; the "
                "peak where the outermost (r = .5) one would be", "mean +-2 levels; rings +-1; ring peak +-6 levels")
    g, r, _ = ctx.each("run1", 5, lambda im: M.mean_outside_radius(im, 0.6))
    f.add("mean outside r = .6", g, r, 2, "levels")
    g, r, _ = ctx.each("run1", 5, lambda im: len(M.row_runs(im, 0, 64, 0.0, 0.6)))
    f.add("rings on the +x axis", g, r, 1)
    g, r, _ = ctx.each("run1", 6, lambda im: M.box_max(im, -0.52, -0.01, -0.48, 0.01))
    f.add("r = .5 ring left frozen after the FlowField + WaveShape cut (run 1 shot 6, at (-.5, 0))", g, r, 6, "levels",
          note="frame order: the old field moves the old WaveShape's last frame once more")
    F.append(f)

    # #14 rnd per row / per pixel
    f = Finding(14, "rnd: C var once per row, D var per pixel",
                "run 2 shot 2 (vertical jitter field, hlines): mean std-dev along rows within +-.08 of each line, "
                "left half (C var) and right half (D var)", "left +-3 levels; right +-30%")
    ys = (-0.6, -0.3, 0, 0.3, 0.6)
    g, r, _ = ctx.each("run2", 2, lambda im: M.row_nonuniformity(im, -0.95, -0.05, ys))
    f.add("left (C var): row non-uniformity", g, r, 3, "levels")
    g, r, _ = ctx.each("run2", 2, lambda im: M.row_nonuniformity(im, 0.05, 0.95, ys))
    f.add("right (D var): row non-uniformity", g, r, 0.3 * g, "levels")
    F.append(f)

    # #15 overwrite blending
    f = Finding(15, "Lines overwrite in path order; Pen 0 erases",
                "run 2 shot 4: value at the Pen .8 x Pen .3 crossing (-.3, 0), the Pen .8 line elsewhere, and the "
                "width of the gap the Pen-0 line (LWdt 6) cuts at x = .3", "values +-3 levels; gap +-1.5 px")
    g, r, _ = ctx.each("run2", 4, lambda im: M.box_median(im, -0.302, -0.002, -0.298, 0.002))
    f.add("crossing value", g, r, 3, "levels")
    g, r, _ = ctx.each("run2", 4, lambda im: M.box_median(im, -0.7, -0.002, -0.6, 0.002))
    f.add("Pen .8 line", g, r, 3, "levels")

    def gap(im):
        a, b = im.px(0.2), im.px(0.4)
        row = im.row(im.py(0), 0, a, b)
        return sum(1 - v / 204.0 for v in row)  # dark px (partial pixels count fractionally)

    g, r, _ = ctx.each("run2", 4, gap)
    f.add("Pen-0 gap width", g, r, 1.5, "px")
    F.append(f)

    # #16 line width
    f = Finding(16, "Line width per LWdt", "run 2 shot 1 (void field): width of each hline from the column integral "
                "(sum / 255, median over x = -.8..8); dot diameters from the area (run 2 shot 3, LWdt 10); the same "
                "hlines at full screen (run 6, when its captures exist)", "+-0.5 px (lines; +0.5 % at full screen), "
                "+-0.75 px (dots)")
    for i, y in enumerate((-0.6, -0.3, 0, 0.3, 0.6)):
        lw = 1 + 2 * i  # hlines: path i at y = -.6 + .3i, LWdt = 1 + 2 PATH_ID
        g, r, _ = ctx.each("run2", 1, lambda im: M.median([M.area_width(im, x / 10, y) for x in range(-8, 9)]))
        f.add("LWdt %d" % lw, g, r, 0.5, "px")
    g, r, _ = ctx.each("run2", 3, lambda im: M.dot_diameter(im, 0, 0))
    f.add("dot LWdt 10 (diameter)", g, r, 0.75, "px")
    if "run6" in ctx.frames:
        for i, y in enumerate((-0.6, -0.3, 0, 0.3, 0.6)):
            g, r, _ = ctx.each("run6", 2, lambda im: M.median([M.area_width(im, x / 10, y, 60) for x in range(-8, 9)]))
            f.add("full screen 3456x2234 (run 6 shot 2), LWdt %d" % (1 + 2 * i), g, r, 0.5 + 0.005 * g, "px")
    F.append(f)

    # #17 time t
    f = Finding(17, "`t` is real seconds", "run 2 shots 5, 6 (2 s apart): x of the clock dot (X0 = wrap(t*.1)*2-1)",
                "+-0.01 (2 frames = .007)")
    g5, r5, _ = ctx.each("run2", 5, lambda im: M.centroid(im, -1, -0.05, 1, 0.05, thr=60)[0])
    g6, r6, _ = ctx.each("run2", 6, lambda im: M.centroid(im, -1, -0.05, 1, 0.05, thr=60)[0])
    f.add("dot dx over 2 s", g6 - g5, r6 - r5, 0.01)
    F.append(f)

    # #18 Union
    f = Finding(18, "FlowField Union: weight = w x ALFA during the transition",
                "identity -> zoom in (source = 1.03p) over 6 s on a held grid: grid scale S = right end of the y = 0 "
                "line (x > 0; threshold 12), left grid line x = -.8 peak (static half), and whether anything of the "
                "grid is left after the end. Zoom accumulates per frame, so S is judged against the target-to-effective-rate range",
                "S: +-(0.01 + the change over +-%d frames); peaks +-6 levels" % JITTER_FRAMES)

    def s_tol(t_shot, S):
        return 0.01 + S * 0.03 * FN_FF(t_shot / 6) * JITTER_FRAMES

    for run, n, t_shot, label in (("run3", 11, 1.5, "half (x>0), 25%"), ("run3", 12, 3.0, "half (x>0), 50%"),
                                  ("run4", 8, 1.5, "no union, 25%"), ("run4", 9, 3.0, "no union, 50%"),
                                  ("run4", 10, 1.5, "ALFA .5, 25%"), ("run4", 11, 3.0, "ALFA .5, 50%")):
        g, r, re = ctx.each(run, n, lambda im: M.right_extent_row(im, 0))
        f.add("%s: S" % label, g, r, s_tol(t_shot, g), re=re, judge_rate=True)
    for run, n, label in (("run3", 11, "half, 25%"), ("run3", 12, "half, 50%"), ("run3", 13, "half, end + 3 s"),
                          ("run4", 12, "ALFA .5, end + 0.2 s")):
        g, r, re = ctx.each(run, n, lambda im: M.box_max(im, -0.83, 0.25, -0.77, 0.35))
        f.add("%s: left grid line peak" % label, g, r, 6, "levels", re=re)
    F.append(f)

    # #19 particles
    f = Finding(19, "Particle life: 6 s fade-in, PDur, 6 s fade-out",
                "peak value of the particle dot at (0, .5): run 3 (30 fps) shots 14-20, run 4 (60 fps) shots 1-7",
                "+-(2 + the envelope change over +-%d frames)" % JITTER_FRAMES)
    for run, n, age in [("run3", 13 + i, a) for i, a in enumerate((0.25, 0.75, 1.5, 2.5, 3.25, 3.75, 4.5), 1)] + \
                       [("run4", i, a) for i, a in enumerate((3, 5, 7, 9, 11, 13, 15), 1)]:
        g, r, _ = ctx.each(run, n, lambda im: M.box_max(im, -0.04, 0.44, 0.04, 0.56))
        tol = jitter_tol(lambda a: 255 * particle_env(a), age, ctx.fps[run], 2)
        f.add("%s age %.2f s" % (run, age), g, r, tol, "levels")
    F.append(f)
    return F


# ----------------------------------------------------------------------------------------------------------
def fmt(v, unit=""):
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "—"
    if isinstance(v, int):
        return str(v)
    a = abs(v)
    if unit == "px":
        return "%.2f" % v
    if unit in ("levels", "frames", "deg"):
        return "%.1f" % v if a < 1000 else "%.0f" % v
    return "%.3f" % v if a < 10 else "%.2f" % v


def report(F, rates, runs, out):
    scored = [r for f in F for r in f.rows if not r.info]
    npass = sum(1 for r in scored if r.ok)
    fpass = sum(1 for f in F if all(r.ok for r in f.rows if not r.info))
    worst = sorted(((abs(r.delta) / r.tol if r.tol else (0 if r.ok else float("inf")), f.num, r) for f in F
                    for r in f.rows if not r.info and not math.isnan(r.delta)), key=lambda t: -t[0])
    L = []
    w = L.append
    w("# reforce vs G-Force parity")
    w("")
    w("Generated by `python3 -I tools/parity/parity.py` (milestone M7a). Each probe timeline "
      "(`probes/timelines/run1-4.py`, plus run 6 for line width and the 5B runs 17-20 when captured) is replayed in reforce's deterministic offline mode with a scenes root that "
      "holds only our probe configs; then the same measurement runs on the G-Force screenshot "
      "(`probes/captures/`, local only) and on the matching reforce frame. Numbers only; no images are stored.")
    w("")
    gaps = [(f.num, r) for f in F for r in f.rows if not r.info and not r.ok and r.gap]
    unexpected = [(f.num, r) for f in F for r in f.rows if not r.info and not r.ok and not r.gap]
    w("**Score: %d / %d measurements pass; %d / %d findings fully pass. Failures: %d known gap(s), %d "
      "unexpected.**" % (npass, len(scored), fpass, len(F), len(gaps), len(unexpected)))
    w("")
    w("Tolerances: every value G-Force produces from a moving quantity is sampled with +-%d frames of screenshot "
      "jitter, so each tolerance is a fixed measurement allowance plus the change of the quantity over +-%d frames "
      "(from the probe model) where it moves; static quantities get the measurement allowance only." %
      (JITTER_FRAMES, JITTER_FRAMES))
    w("")
    w("## How the runs map")
    w("")
    w("| Timeline | G-Force capture dir | reforce run | shots |")
    w("|---|---|---|---|")
    for run, d, fps in render.RUNS:
        w("| %s | %s | `--deterministic --fps %d --render-size 1980x1260`, %d commands | %d |" %
          (run, d, fps, len(runs[run]["commands"]), len(runs[run]["shots"])))
    if "run24" in runs:
        w("| run24 (line width vs frame size, #38) | %s | one render of shot 1 per G-Force frame size | %d sizes |"
          % (", ".join(linewidth.DIRS), runs["run24"]["sizes"]))
    if "run6" in runs:
        w("| run6 (line width only) | %s | one render per shot: 1980x1260 (windowed) and 3456x2234 (after "
          "`SetFullscreen`) | 2 |" % render.RUN6[1])
    for run in sorted(r for r in runs if runs[r].get("5b")):
        extra = parity5b.RUNS_5B[run][1]
        w("| %s (5B, #30-#34) | %s | `--deterministic --fps 60 --render-size 1980x1260`, %d commands%s | %d |" %
          (run, parity5b.RUNS_5B[run][0], len(runs[run]["commands"]),
           "".join(", `--set %s`" % kv for kv in extra), len(runs[run]["shots"])))
    w("")
    w("- Settings: `--no-settings` (built-in = G-Force 5.9.7 defaults, incl. the transition curves) plus "
      "`ColorMap.MapFunction=i`, `--slideshow off`, `Particle.Transition.Duration.Deviation=0`, "
      "`reforce.ThinLines=gforce` (G-Force's pixel-snapped line/dot rasterization, finding #32; reforce's default "
      "is `smooth`).")
    w("- The 0:00 configs are the starting configs. Later `ws/cm/ff(name, morph)` become "
      "`--command T=morph:LAYER:SECS:NAME` (0 = cut); `ff(name, union, morph)` adds `T=switch:union:UNION` first; "
      "`vis.StartParticle(name, 4)` becomes `T=start:particle:NAME` (the config's PDur is 4).")
    w("- Shot at script time T = reforce frame round(T x fps). G-Force's screenshots land within +-1-2 frames of "
      "T; tolerances below include +-%d frames of that jitter where the measured value moves with time." %
      JITTER_FRAMES)
    w("- **G-Force's effective frame rate.** The B0 frame counter in run 1 advances %.1f frames per second at "
      "`TargetFrameRate = 60` (%.2f ms per frame more than 1/60 s). With the same overhead, the 30 fps run "
      "would run at %.1f fps. reforce simulates exactly at the target rate. Findings that accumulate per frame "
      "(decay per second, B0 count, Union zoom scale) are also rendered at the effective rate (`--fps %.2f` / "
      "`%.2f`, column \"reforce @eff\") and pass when G-Force lies within the tolerance of the range between the "
      "two reforce values (marked \"range\"; delta = distance to the nearer end). G-Force's true rate in a "
      "segment lies between the two: its frame pacing varies with load." %
      (rates[60], rates["overhead_ms"], rates[30], rates[60], rates[30]))
    w("")
    w("## Summary")
    w("")
    w("| # | Finding | pass | measurements |")
    w("|---|---|---|---|")
    for f in F:
        s = [r for r in f.rows if not r.info]
        k = sum(1 for r in s if r.ok)
        w("| %d | %s | %s | %d / %d |" % (f.num, f.title, "PASS" if k == len(s) else "**FAIL**", k, len(s)))
    w("")
    w("Largest deltas relative to their tolerance:")
    w("")
    for ratio, num, r in worst[:6]:
        w("- #%d %s: G-Force %s, reforce %s (delta %s, tolerance %s)" %
          (num, r.item, fmt(r.g, r.unit), fmt(r.judged, r.unit), fmt(r.delta, r.unit), fmt(r.tol, r.unit)))
    w("")
    w("## reforce changes made in M7a (from this suite)")
    w("")
    w("- **Line width** (#16): area-based widths (column integral / 255, which counts antialiased edge pixels "
      "fractionally) are 2.836 px per LWdt at 1980x1260 and 5.165 px per LWdt at 3456x2234 (run 6, full "
      "screen), exactly proportional to LWdt at each size. Counting lit pixels gives 3 and ~5 px (the probe "
      "table's 3,9,15,21,27 and 6,14,26,38,46) because each edge adds a partial pixel; neither H/420 (3.0 / "
      "5.32) nor round(H/420) (3 / 5) matches the areas. reforce now uses LWdt x (H - 68) / 420 (2.838 / 5.157 "
      "px per LWdt; was H / 420). Two sizes cannot tell whether G-Force scales with H, W or both.")
    w("- **Line width vs frame size** (#38, F6): run 24 resized G-Force's window to 17 sizes (plus full screen): "
      "px per LWdt = 0.733 + 8.67e-4 min(W, H) + 5.10e-4 max(W, H) on G-Force's frame (max error 0.0005), and "
      "G-Force renders frames above ~5.2 MP at half size with doubled pixels, so 3456x2234 full screen is "
      "2 x s(1728, 1117) = 5.165. The (H - 68) / 420 line went through 1980x1260 and that half-size frame. "
      "reforce uses this law (`reforce.LineScale = gforce`, default; `frame` applies it to the render size).")
    w("- **Dots off the pixel grid** (#3): G-Force draws the x = -1 dot of `X0 = 2s-1` (half visible) but no "
      "dot at x = +1. reforce now skips unconnected dots whose center is outside [0, W) x [0, H).")
    w("- **Frame order** (#1/#13, run 1 shot 6): with a FlowField and a WaveShape cut in the same script call, "
      "G-Force flows the old WaveShape's last frame once more with the old field (the frozen zoom-out rings have "
      "no ring at r = .5) and does not move the new WaveShape's first frame. G-Force runs script commands "
      "between a frame's flow pass and its draw; reforce now applies FlowField/Union switches one step after "
      "they are issued (ctest `reforce_frame_order_render`).")
    w("- New command `morph:LAYER:SECS:NAME` (explicit morph duration, 0 = cut) so scripted "
      "`StartConfig(type, name, secs)` / `SetFlowField(name, union, secs)` replay exactly.")
    w("")
    w("## Remaining gaps")
    w("")
    if not gaps and not unexpected:
        w("None.")
    seen = set()
    for num, r in gaps:
        same = [x for n2, x in gaps if x.gap == r.gap]
        if r.gap in seen:
            continue
        seen.add(r.gap)
        vals = "; ".join("%s: G-Force %s, reforce %s" % (x.item, fmt(x.g, x.unit), fmt(x.judged, x.unit)) for x in same)
        w("- #%d %s. %s." % (num, vals, r.gap))
    for num, r in unexpected:
        w("- #%d %s: G-Force %s, reforce %s — **unexplained**." % (num, r.item, fmt(r.g, r.unit),
                                                                   fmt(r.judged, r.unit)))
    w("- G-Force's effective frame rate (above) is not emulated: reforce runs its simulation at exactly "
      "`Graphics.TargetFrameRate`, so trails and B-var counters advance ~8 % faster per second than in G-Force "
      "on this machine (decay 60 vs ~55 levels/s at 60 fps).")
    w("- Not covered here: sprites (run 5 is not replayed), the Union end-of-transition jump, line width at sizes other than 1260 and 2234 px tall.")
    w("")
    w("## Findings")
    for f in F:
        w("")
        w("### %d. %s" % (f.num, f.title))
        w("")
        w("Measurement: %s. Tolerance: %s." % (f.method, f.tolerance))
        w("")
        has_eff = any(r.re is not None for r in f.rows)
        if has_eff:
            w("| item | G-Force | reforce | reforce @eff | delta | tol | result |")
            w("|---|---|---|---|---|---|---|")
        else:
            w("| item | G-Force | reforce | delta | tol | result |")
            w("|---|---|---|---|---|---|")
        for r in f.rows:
            res = "info" if r.info else ("pass" if r.ok else ("**FAIL** (known gap)" if r.gap else "**FAIL**"))
            item = r.item + (" (%s)" % r.unit if r.unit else "")
            if r.note:
                item += " — " + r.note
            cells = [item, fmt(r.g, r.unit), fmt(r.r, r.unit)]
            if has_eff:
                cells.append(fmt(r.re, r.unit) + (" (range)" if r.judge_rate else ""))
            cells += [fmt(r.delta, r.unit), fmt(r.tol, r.unit), res]
            w("| " + " | ".join(cells) + " |")
    with open(out, "w") as fh:
        fh.write("\n".join(L) + "\n")
    return npass, len(scored), fpass, worst


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--captures", default=os.path.join(REPO, "probes", "captures"))
    ap.add_argument("--reforce", default=os.path.join(REPO, "build", "reforce.app", "Contents", "MacOS", "reforce"))
    ap.add_argument("--work", default=os.path.join(REPO, "build", "parity"))
    ap.add_argument("--out", default=os.path.join(REPO, "docs", "parity.md"))
    ap.add_argument("--no-render", action="store_true", help="reuse the frames from the last run")
    ap.add_argument("--strict", action="store_true", help="exit 1 if any measurement fails")
    ap.add_argument("--skip-code", type=int, default=77)
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE",
                    help="extra reforce --set for every replay (e.g. reforce.FlowPrecision=float32)")
    a = ap.parse_args()

    missing = [d for _, d, _ in render.RUNS if not os.path.isdir(os.path.join(a.captures, d))]
    if missing:
        print("parity: no G-Force captures (%s missing under %s); skipping" % (", ".join(missing), a.captures))
        return a.skip_code
    if not os.path.exists(a.reforce):
        print("parity: reforce not found at %s" % a.reforce)
        return 2
    os.makedirs(a.work, exist_ok=True)
    rates = effective_rates(a.captures, a.work)
    scenes = render.build_scenes(REPO, a.work)
    frames, rate_frames, runs = {}, {}, {}
    for run, _, fps in render.RUNS:
        events = timeline.parse(os.path.join(REPO, "probes", "timelines", run + ".py"))
        _, commands, shots, _ = timeline.reforce_args(events, fps)
        runs[run] = {"commands": commands, "shots": shots}
        frames[run] = render_or_reuse(a, scenes, run, fps, run)
        if run in ("run1", "run3", "run4"):  # runs with frame-counted findings
            frames_eff = render_or_reuse(a, scenes, run, rates[fps], run + "-eff")
            rate_frames[run] = frames_eff
    run6, d6, fps6, sizes6 = render.RUN6
    if os.path.isdir(os.path.join(a.captures, d6)):
        frames[run6] = {}
        for n, size in sizes6.items():
            frames[run6].update(render_or_reuse(a, scenes, run6, fps6, "%s-%d" % (run6, n), size=size, only={n}))
        runs[run6] = {}
    lw_items = linewidth.prepare(a, scenes, render_or_reuse)
    if lw_items:
        runs["run24"] = {"sizes": len(lw_items)}
    ctx = Context(a.captures, a.work, frames, rate_frames)
    for run in sorted(parity5b.captures_present(a.captures)):
        events = timeline.parse(os.path.join(REPO, "probes", "timelines", run + ".py"))
        _, commands, shots, _ = timeline.reforce_args(events, 60)
        runs[run] = {"commands": commands, "shots": shots, "5b": True}
        ctx.shots5[run] = [(e.label, e.t) for e in events if e.kind == "shot"]
        ctx.events5[run] = events
        ctx.frames5[run] = render_or_reuse(a, scenes, run, 60, run, extra_sets=parity5b.RUNS_5B[run][1])
    F = checks(ctx, rates) + parity5b.checks_5b(ctx, Finding) + linewidth.checks(lw_items, ctx, Finding)
    npass, n, fpass, worst = report(F, rates, runs, a.out)
    print("parity: %d / %d measurements pass, %d / %d findings; report: %s" % (npass, n, fpass, len(F), a.out))
    for f in F:
        for r in f.rows:
            if not r.info and not r.ok:
                print("  FAIL%s #%d %s: G-Force %s reforce %s (tol %s)" % (
                    " (known gap)" if r.gap else "", f.num, r.item, fmt(r.g, r.unit), fmt(r.judged, r.unit),
                    fmt(r.tol, r.unit)))
    unexpected = [r for f in F for r in f.rows if not r.info and not r.ok and not r.gap]
    return 1 if a.strict and unexpected else 0


def render_or_reuse(a, scenes, run, fps, tag, size=(render.WIDTH, render.HEIGHT), only=None, extra_sets=()):
    out_dir = os.path.join(a.work, "frames", tag)
    events = timeline.parse(os.path.join(REPO, "probes", "timelines", run + ".py"))
    _, _, shots, _ = timeline.reforce_args(events, fps)
    if only is not None:
        shots = {n: f for n, f in shots.items() if n in only}
    paths = {n: os.path.join(out_dir, "frame_%05d.png" % fr) for n, fr in shots.items()}
    if a.no_render and all(os.path.exists(p) for p in paths.values()):
        return paths
    got, _, _ = render.render_run(REPO, a.reforce, scenes, a.work, run, fps, os.path.join(a.work, tag + ".log"),
                                  out_dir=out_dir, size=size, only=only, extra_sets=tuple(extra_sets) + tuple(a.set))
    return got


if __name__ == "__main__":
    sys.exit(main())

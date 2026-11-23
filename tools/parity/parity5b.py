"""Phase 5B parity findings #30-#34 (runs 17-20, captured 2026-11-11 20:38-20:45), used by parity.py.

Each run is optional: findings are added only when its G-Force capture directory exists.
"""
import glob
import math
import os

import measure as M
import probe5b as P
import strip5b as S

# run -> (capture dir, extra reforce --set prefs)
RUNS_5B = {
    "run17": ("20261111-204332", ()),
    "run18": ("20261111-204436", ()),
    "run19": ("20261111-203846", ()),
    "run20": ("20261111-204522", ("ColorMap.MapFunction=i^2.1",)),
}
D_MORPH = 8 / 1260.0  # 'zz reforce morph b' moves the morph region down by this much (frame units) per frame


# ---- runs 17/18: time since the clock config started --------------------------------------------------------------
def since_call(fit, T, t_call):
    """Time band reading at the head column, as seconds since the clock config was started at script time t_call.

    G-Force's WaveShape `t` is config-local (0 at its StartConfig), reforce's is the global clock: take whichever
    reading of the 8 s wrap is closer to T - t_call. The B0 band is NOT used for this: G-Force's strip scrolls only
    ~0.95 px per frame (1/16-px weights, probe #34), so one column there holds ~1.055 frames (B0 rises 55.6/s, the
    trail 52.7 columns/s) and "B0 frames x seconds per column" stretched every time since the start by ~5.5 %. That
    made G-Force's morphs look 0.11-0.17 s later than reforce's (#30) and its particle fade-outs 0.3-1 s later
    (#31); read from the time band both match."""
    since = T - t_call
    th = fit["t_head"]
    local = th + round((since - th) / S.TIME[2]) * S.TIME[2]
    glob_ = th + round((T - th) / S.TIME[2]) * S.TIME[2] - t_call
    return local if abs(local - since) <= abs(glob_ - since) else glob_


# ---- run 17: per-frame morph shift samples --------------------------------------------------------------------
MIN_AGE = 5  # the LWdt-3 morph dot is ~8.5 px wide: the 4 freshest columns are still overdrawn by the newest dots


def morph_samples(img, T, t_call, max_age=50):
    """-> [(t since the morph call, per-frame shift / D)] from the freshest columns of one shot."""
    fit = S.clock_fit(S.decode_clock(img), T - t_call)
    if not fit or fit["b0_head"] is None or fit["b0_head"] > 700 or not fit["fps"] or not 40 < fit["fps"] < 70:
        return []
    t_head = since_call(fit, T, t_call)
    tr = {int(round(r["age"])): r for r in S.morph_track(img, max_age + 2) if r["peak"] >= 30}
    out = []
    for a in sorted(tr):
        if a + 1 in tr and MIN_AGE <= a < max_age and a < fit["b0_head"] - 1:
            out.append((t_head + fit["dt_per_age"] * a, (tr[a]["y"] - tr[a + 1]["y"]) / D_MORPH))
    return out


def crossing(samples, level, rising, h=0.2):
    """First t where the samples' running mean (+-h s) crosses `level`."""
    pts = sorted(samples)
    if not pts:
        return float("nan")
    prev = None
    t = pts[0][0]
    while t <= pts[-1][0]:
        v = [s for tt, s in pts if abs(tt - t) <= h]
        if len(v) >= 4:
            m = sum(v) / len(v)
            if prev is not None and ((rising and prev[1] < level <= m) or (not rising and prev[1] > level >= m)):
                return prev[0] + (level - prev[1]) / (m - prev[1]) * (t - prev[0])
            prev = (t, m)
        t += 0.05
    return float("nan")


# ---- run 18: particle envelopes from the freshly drawn dot -------------------------------------------------------
def particle_fresh(img, T, t_call):
    """-> (t since the clock start, [fresh value of particle 0, 1, 2]) or None."""
    fit = S.clock_fit(S.decode_clock(img), T - t_call)
    if not fit or fit["b0_head"] is None or not fit["fps"] or not 40 < fit["fps"] < 70:
        return None
    vals = [max(r["peak"] for r in S.particle_track(img, y) if r["age"] < 1.5) for y in S.PARTICLE_Y]
    return since_call(fit, T, t_call), vals


def fall_crossing(pts, level):
    """Time where a falling sequence of (t, v) first goes below `level` (linear interpolation)."""
    pts = sorted(pts)
    hold = next((i for i, (_, v) in enumerate(pts) if v >= 250), None)
    if hold is None:
        return float("nan")
    pts = pts[hold:]  # the fade-out only (after the hold at full pen)
    for (t0, v0), (t1, v1) in zip(pts, pts[1:]):
        if v0 >= 250 and v1 >= 250:
            continue
        if v0 >= level > v1:
            return t0 + (v0 - level) / (v0 - v1) * (t1 - t0)
    return float("nan")


# ---- run 17/18: decay per frame of a 1-px/frame translation ----------------------------------------------------------
def strip_decay(img):
    """Levels lost per column (= per frame) along the time-band trail, ages 25..95 (median of 10-column slopes)."""
    rows = [r for r in S.decode_clock(img) if r["t"] is not None and 25 <= r["age"] <= 95]
    pk = {int(round(r["age"])): r["peak"] for r in rows}
    sl = [(pk[a] - pk[a + 10]) / 10.0 for a in pk if a + 10 in pk and pk[a + 10] > 20]
    return M.median(sl) if sl else float("nan")


def checks_5b(ctx, Finding):
    """ctx: parity.Context with ctx.g5(run, n) / ctx.r5(run, n) and ctx.shots5[run] = [(label, T)]."""
    F = []
    if "run17" in ctx.shots5:
        f = Finding(30, "FlowField morph timing (frame by frame)",
                    "run 17: three 10 s morphs a -> b -> a -> b ('zz reforce morph a/b', the b region moves down 4 px per "
                    "frame), each started with a fresh clock WaveShape. From the freshest 50 columns of every shot: the "
                    "per-column shift (in units of 4 px) vs time since the call, read from the clock's time band (G-Force's "
                    "WaveShape t is config-local, reforce's global; not from B0 / columns, see since_call); columns "
                    "younger than 5 are skipped (still under the 8.5 px wide newest dots); reported: the time where it "
                    "crosses half of its full value (a -> b rising, b -> a falling; full value = the steady shift after "
                    "morph 0)",
                    "+-0.08 s (~4 G-Force frames)")
        per = {}
        for side, get in (("g", ctx.g5), ("r", ctx.r5)):
            smp = {0: [], 1: [], 2: []}
            calls = [e.t for e in ctx.events5["run17"] if e.kind == "switch" and e.layer == "flowfield" and e.morph > 0]
            for n, (label, T) in enumerate(ctx.shots5["run17"], 1):
                mi = int(label.split("morph")[1][0])
                smp[mi] += morph_samples(get("run17", n), T, calls[mi])
            full = M.median([s for t, s in smp[0] if 10.2 <= t <= 11.0]) or 1.0
            per[side] = {mi: crossing(smp[mi], 0.5 * full, mi != 1) for mi in smp}
            per[side]["full"] = full
        f.add("steady shift of field b (x 4 px per frame)", per["g"]["full"], per["r"]["full"], 0.03, info=True,
              note="per strip column, not per frame: G-Force's strip scrolls ~0.95 px per frame (#34), so one column "
                   "holds ~1.055 frames (B0 rises 55.6/s vs 52.7 columns/s); per frame it moves the full 4 px")
        for mi, lab in ((0, "a -> b (1st)"), (1, "b -> a"), (2, "a -> b (2nd)")):
            f.add("morph %s: half-way time" % lab, per["g"][mi], per["r"][mi], 0.08, "s")
        F.append(f)
    if "run18" in ctx.shots5:
        f = Finding(31, "Particle fade-out (dense)",
                    "run 18: three particles (PDur 2) started at 0, +1, +2 s with a fresh clock (particle 0 at the clock "
                    "start); value of the freshly drawn dot in every shot (every 0.5 s), timed by the clock's time band "
                    "(see #30). Reported per particle: when "
                    "the fade-out passes 224, 128 and 32 (time since the clock start) and the 224 -> 32 duration",
                    "crossings +-0.15 s (script jitter of the later StartParticle calls); duration +-0.15 s")
        seq = {"g": {0: [], 1: [], 2: []}, "r": {0: [], 1: [], 2: []}}
        for side, get in (("g", ctx.g5), ("r", ctx.r5)):
            clock = next(e.t for e in ctx.events5["run18"] if e.kind == "switch" and e.layer == "waveshape" and e.t > 0)
            for n, (label, T) in enumerate(ctx.shots5["run18"], 1):
                res = particle_fresh(get("run18", n), T, clock)
                if res:
                    for k in range(3):
                        seq[side][k].append((res[0], res[1][k]))
        for k in range(3):
            c = {side: {lv: fall_crossing(seq[side][k], lv) for lv in (224, 128, 32)} for side in ("g", "r")}
            for lv in (224, 128, 32):
                f.add("particle %d: fade-out passes %d" % (k, lv), c["g"][lv], c["r"][lv], 0.15, "s")
            f.add("particle %d: fade-out 224 -> 32 duration" % k, c["g"][32] - c["g"][224],
                  c["r"][32] - c["r"][224], 0.15, "s")
        F.append(f)
    if "run19" in ctx.shots5:
        f = Finding(32, "Sub-pixel lines and dots (LWdt .05-.5)",
                    "run 19 (void field): per line k (LWdt .05 + .05 k = 0.14-1.4 px at 1260 px): area width (column "
                    "integral / 255) and peak over 40 columns of the horizontal and the slanted line; per dot row: "
                    "integrated intensity (px^2) and peak",
                    "area +-0.05 px; peaks +-12 levels")
        gl, rl = ctx.g5("run19", 1), ctx.r5("run19", 1)
        gd, rd = ctx.g5("run19", 3), ctx.r5("run19", 3)
        for k in range(10):
            g, r = P.line_profile(gl, k), P.line_profile(rl, k)
            f.add("line %d (LWdt %.2f): horizontal area width" % (k, P.LWDT[k]), g["area_px"], r["area_px"], 0.05, "px")
            f.add("line %d (LWdt %.2f): horizontal peak" % (k, P.LWDT[k]), g["peak"], r["peak"], 12, "levels")
            gs, rs = P.line_profile(gl, k, True), P.line_profile(rl, k, True)
            f.add("line %d (LWdt %.2f): slanted area width" % (k, P.LWDT[k]), gs["area_px"], rs["area_px"], 0.05, "px")
        for k in range(10):
            g, r = P.dot_profile(gd, k), P.dot_profile(rd, k)
            f.add("dot row %d (LWdt %.2f): area" % (k, P.LWDT[k]), g["area_px2"], r["area_px2"], 0.05, "px2")
            f.add("dot row %d (LWdt %.2f): peak" % (k, P.LWDT[k]), g["peak"], r["peak"], 12, "levels")
        F.append(f)
    if "run20" in ctx.shots5:
        f = Finding(33, "Default ColorMap.MapFunction i^2.1: LUT domain",
                    "run 20: 256-dot Pen ramp (Pen = k/255) with MapFunction \"i^2.1\" through the gray ColorMap and "
                    "through 'zz reforce clamp' (R = 2i, G = -1 + i): max |G - r| over the 256 entries per channel",
                    "+-2 levels")
        for n, cm, ch, lab in ((1, "gray", 0, "gray"), (3, "clamp", 0, "clamp R = 2 f(i)"),
                               (3, "clamp", 1, "clamp G = -1 + f(i)")):
            g, r = P.ramp(ctx.g5("run20", n), ch=ch), P.ramp(ctx.r5("run20", n), ch=ch)
            f.add("%s: max |G - r| over the ramp" % lab, 0, max(abs(a - b) for a, b in zip(g, r)), 2, "levels")
        g = P.ramp(ctx.g5("run20", 1))
        f.add("gray: entry 128 (255 (128/255)^2.1 = 60.0)", g[128], P.ramp(ctx.r5("run20", 1))[128], 2, "levels")
        F.append(f)
    if "run17" in ctx.shots5 or "run18" in ctx.shots5:
        f = Finding(34, "Decay under a 1-px-per-frame translation",
                    "runs 17/18 (field scrolls left exactly 1 px per frame, srcX = x + 2/1980): levels lost per frame "
                    "along the clock trail (ages 25-95), median over the shots",
                    "+-0.2 level")
        for run in ("run17", "run18"):
            if run not in ctx.shots5:
                continue
            gv = [strip_decay(ctx.g5(run, n)) for n in range(1, len(ctx.shots5[run]) + 1, 3)]
            rv = [strip_decay(ctx.r5(run, n)) for n in range(1, len(ctx.shots5[run]) + 1, 3)]
            f.add("%s: decay per frame" % run, M.median([v for v in gv if not math.isnan(v)]),
                  M.median([v for v in rv if not math.isnan(v)]), 0.2, "levels")
        F.append(f)
    return F


KNOWN_GAPS_5B = {
    (34, "run"):
        "G-Force loses 2 levels per frame on a pure 1-px translation (reforce 1): its source coordinate lands 1/1980 px "
        "short (pixel = (x + 1) / 2 x (W - 1), corner-aligned: probe finding #26), so the 1/16-px bilinear weights take 1/16 of the "
        "older neighbor and the truncation drops a level every frame",
}


def captures_present(captures):
    return {run: d for run, (d, _) in RUNS_5B.items() if os.path.isdir(os.path.join(captures, d))
            and glob.glob(os.path.join(captures, d, "*.png"))}

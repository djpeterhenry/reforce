"""Phase 5B strip probes (runs 17 and 18): decode a screenshot into per-frame samples.

Probe geometry (probes/WaveShape/zz reforce morph strip*.txt, FlowField zz reforce morph a/b): the whole frame scrolls
left by exactly 1 px per frame at 1980 px width, so column c holds what was drawn (head - c) frames ago, head = the
x = .99 dot column. Per frame the clock WaveShape draws
  * a time band  y = .72 + .2 u,  u = wrap(t / 8)  with rails at u = -.1 / 1.1,
  * a B0 band    y = .45 + .18 u, u = clamp(B0 / 1024) (B0 = frames since the clock config started), same rails,
  * a morph dot at y = .25 (LWdt 3) in the region y <= .3 that 'zz reforce morph b' moves DOWN by 4 px x w per frame.
Particles 'zz reforce pstrip 0/1/2' draw one Pen-1 dot at y = .2 / -.15 / -.5 (LWdt 3).

decode_clock(img)  -> per column: age, t (time band), b0 (B0 band)
morph_track(img)   -> per column: age, y of the morph dot trail (frame units)
particle_track(img, y) -> per column: age, peak pixel value of the particle dot at row y
"""
import measure as M

WIDTH = 1980
X_DOT = 0.99
THR = 12
TIME = (0.72, 0.2, 8.0)       # y0, h, period
B0 = (0.45, 0.18, 1024.0)     # y0, h, scale
RAIL_LO, RAIL_HI = -0.1, 1.1
MORPH_REGION = (-0.9, 0.3)    # y range of the morph region (frame units; G-Force's watermark is below -0.95)
PARTICLE_Y = (0.2, -0.15, -0.5)


def head_px(img):
    return (X_DOT + 1) * 0.5 * img.w - 0.5


def _py(img, y):
    return (1 - y) * 0.5 * img.h - 0.5


def _band_values(img, c, y0, h):
    """Signal centroid (u) in one band column, calibrated by the two rails seen in the same column
    (or the nominal map). -> (u, peak) or None."""
    ylo, yhi = y0 + (RAIL_LO - 0.08) * h, y0 + (RAIL_HI + 0.08) * h
    p0, p1 = int(_py(img, yhi)), int(_py(img, ylo)) + 1
    col = img.col(c, 0, p0, p1)
    rl, rh = _py(img, y0 + RAIL_LO * h), _py(img, y0 + RAIL_HI * h)
    best = None
    for first, last, cy in M.runs(col, THR, p0):
        if abs(cy - rl) < 1.6 or abs(cy - rh) < 1.6:
            continue
        pk = max(col[first - p0:last - p0 + 1])
        if best is None or pk > best[1]:
            best = (cy, pk)
    if best is None:
        return None
    y = 1 - (best[0] + 0.5) / img.h * 2
    return (y - y0) / h, best[1]


def decode_clock(img, max_age=250):
    head = head_px(img)
    out = []
    for c in range(min(img.w - 1, int(head)), max(-1, int(head - max_age) - 1), -1):
        age = head - c
        tv = _band_values(img, c, TIME[0], TIME[1])
        bv = _band_values(img, c, B0[0], B0[1])
        out.append({"col": c, "age": age,
                    "t": None if tv is None else tv[0] * TIME[2],
                    "b0": None if bv is None else bv[0] * B0[2],
                    "peak": max(tv[1] if tv else 0, bv[1] if bv else 0)})
    return out


def fit_line(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if sxx else 0.0
    return my - b * mx, b


def clock_fit(rows, t_hint):
    """Unwrap the 8 s time band and fit t = a + b * age over the columns of the current clock config
    (those with B0 < B0 at the head + age, i.e. after its start). Returns dict(t_head, fps, b0_head, start_age)."""
    pts = [(r["age"], r["b0"]) for r in rows if r["b0"] is not None and 0.02 * B0[2] < r["b0"] < 0.98 * B0[2]]
    # B0 decreases by exactly 1 per column of age within one config: b0_head = mean(b0 + age) from the freshest
    # columns only (older columns may still show the previous config's counter), around their median. A mean over
    # 27 columns (3 rows of the band): G-Force snaps dots to whole pixel rows (probe #32), so one row of the B0 band
    # holds ~9 frames and b0 + age is a sawtooth; its median would be off by up to +-4.5 frames per shot.
    cand = sorted(b + a for a, b in sorted(pts)[:27])
    b0_head = None
    if cand:
        med = M.median(cand)
        near = [v for v in cand if abs(v - med) <= 6]
        b0_head = sum(near) / len(near)
    tp = []
    for r in rows:
        if r["t"] is None:
            continue
        if b0_head is not None and r["age"] > b0_head + 0.5:
            continue  # before the clock config started (previous config's columns)
        tp.append((r["age"], r["t"]))
    if len(tp) < 10:
        return None
    tp.sort()
    un, prev, k = [], None, 0
    for age, t in tp:
        if prev is not None and t - prev > TIME[2] / 2:
            k -= 1
        prev = t
        un.append((age, t + k * TIME[2]))
    a, b = fit_line([p[0] for p in un], [p[1] for p in un])
    keep = [p for p in un if abs(p[1] - (a + b * p[0])) < 0.1]
    if len(keep) >= 10:
        a, b = fit_line([p[0] for p in keep], [p[1] for p in keep])
    # pick the 8 s wrap nearest the hint
    a += round((t_hint - a) / TIME[2]) * TIME[2]
    return {"t_head": a, "dt_per_age": b, "fps": -1.0 / b if b else None, "b0_head": b0_head}


def morph_track(img, max_age=250):
    """Centroid y (frame units) of the morph-dot trail per column (brightest run in the morph region)."""
    head = head_px(img)
    p0, p1 = int(_py(img, MORPH_REGION[1])), int(_py(img, MORPH_REGION[0])) + 1
    out = []
    for c in range(min(img.w - 1, int(head)), max(-1, int(head - max_age) - 1), -1):
        col = img.col(c, 0, p0, p1)
        best = None
        for first, last, cy in M.runs(col, THR, p0):
            pk = max(col[first - p0:last - p0 + 1])
            w = sum(col[first - p0:last - p0 + 1])
            if best is None or w > best[2]:
                best = (cy, pk, w)
        if best:
            out.append({"col": c, "age": head - c, "y": 1 - (best[0] + 0.5) / img.h * 2, "peak": best[1],
                        "mass": best[2]})
    return out


def particle_track(img, y, max_age=250, half=8):
    """Peak pixel value within +-half px of row y per column (0 when nothing is lit)."""
    head = head_px(img)
    pc = int(round(_py(img, y)))
    out = []
    for c in range(min(img.w - 1, int(head)), max(-1, int(head - max_age) - 1), -1):
        col = img.col(c, 0, pc - half, pc + half + 1)
        out.append({"col": c, "age": head - c, "peak": max(col)})
    return out

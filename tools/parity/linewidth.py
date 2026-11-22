"""F6 / probe #38: WaveShape line width vs frame size (probe run 24, docs/probe-results.md #38).

G-Force draws 'zz reforce widths' (horizontal lines LWdt 2/5/10 at y = -.6/-.1/.5, vertical lines LWdt 5/10 at
x = .5/.8) on the void field while tools/parity/resize_gforce.sh resizes its window between shots, then goes full
screen. Every G-Force shot is replayed in reforce at the screenshot's own size (run24 shot 1, one render per size)
and the area width (integral across the line / 255, median along it) is compared.
"""
import os
import struct

import measure as M

# Capture dirs of run 24 (probes/captures/<dir>): default SIZES, and RESIZE_SIZES = larger / smaller / portrait
# frames ("1728 1076;1500 1000;1400 950;300 228;1728 500;400 1000;1728 800;700 1028").
DIRS = ["20261111-232255", "20261111-233553"]
# Same timeline without resizing, with LineWidth prefs: Scale 2 + Offset 3 (shots 1 = 1980x1260, 10 = full screen).
PREFS_DIR, PREFS_SETS = "20261111-233150", ("WaveShape.LineWidth.Scale=2", "WaveShape.LineWidth.Offset=3")
H_LINES = ((-0.6, 2), (-0.1, 5), (0.5, 10))  # (y, LWdt), x -.95 .. +.2
V_LINES = ((0.5, 5), (0.8, 10))              # (x, LWdt), y -.95 .. +.95


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    return struct.unpack(">II", head[16:24])


def shots(captures):
    """[(dir, n, (w, h), png)] of the G-Force shots, first occurrence of each frame size only."""
    out, seen = [], set()
    for d in DIRS:
        dd = os.path.join(captures, d)
        if not os.path.isdir(dd):
            continue
        for n in range(1, 100):
            p = os.path.join(dd, "G-Force Screen Shot %04d.png" % n)
            if not os.path.exists(p):
                break
            size = png_size(p)
            if size not in seen:
                seen.add(size)
                out.append((d, n, size, p))
    return out


def prepare(a, scenes, render_or_reuse):
    """Renders reforce at every G-Force shot size. Returns [(dir, n, size, gforce png, reforce png)]."""
    items = []
    for d, n, size, p in shots(a.captures):
        got = render_or_reuse(a, scenes, "run24", 60, "run24-%dx%d" % size, size=size, only={1})
        items.append((d, n, size, p, got[1], ()))
    for n in (1, 10):
        p = os.path.join(a.captures, PREFS_DIR, "G-Force Screen Shot %04d.png" % n)
        if os.path.exists(p):
            size = png_size(p)
            got = render_or_reuse(a, scenes, "run24", 60, "run24-prefs-%dx%d" % size, size=size, only={1},
                                  extra_sets=PREFS_SETS)
            items.append((PREFS_DIR, n, size, p, got[1], PREFS_SETS))
    return items


def h_width(img, y):
    half = 70
    cy = img.py(y)
    return M.median([sum(img.col(img.px(-0.8 + 0.02 * i), 0, max(0, cy - half), min(img.h, cy + half + 1))) / 255.0
                     for i in range(41)])


def v_width(img, x):
    half = 70
    cx = img.px(x)
    ys = [v for v in (-0.9 + 0.03 * i for i in range(61)) if all(abs(v - l) > 0.2 for l, _ in H_LINES)]
    return M.median([sum(img.row(img.py(y), 0, max(0, cx - half), min(img.w, cx + half + 1))) / 255.0 for y in ys])


def checks(items, ctx, Finding):
    if not items:
        return []
    f = Finding(38, "Line width vs frame size",
                "run 24: G-Force window resized between shots (tools/parity/resize_gforce.sh), then full screen; "
                "each shot replayed at its own size. Area width (integral across the line / 255, median along it) "
                "of the LWdt 2 and 10 horizontal and the LWdt 10 vertical line. G-Force: LWdt x s(W, H), "
                "s = 0.733 + 8.67e-4 min(W, H) + 5.10e-4 max(W, H) on its frame, which is half size (pixels "
                "doubled) above ~5.2 MP; LineWidth.Offset is added in those frame pixels", "+-(0.25 px + 0.5 %)")
    for d, n, (w, h), gp, rp, sets in items:
        g = ctx._load(("g", "run24", d, n), gp, "gforce-run24-" + d)
        r = ctx._load(("r", "run24", w, h, sets), rp, "reforce-run24-%dx%d%s" % (w, h, "-prefs" if sets else ""))
        tag = "%dx%d%s%s" % (w, h, " (half-size frame)" if w * h > 5.0 * 1024 * 1024 else "",
                             ", LineWidth.Scale 2 + Offset 3" if sets else "")
        for y, lw in (H_LINES[0], H_LINES[2]):
            gv, rv = h_width(g, y), h_width(r, y)
            f.add("%s: LWdt %d horizontal" % (tag, lw), gv, rv, 0.25 + 0.005 * gv, "px")
        x, lw = V_LINES[1]
        gv, rv = v_width(g, x), v_width(r, x)
        f.add("%s: LWdt %d vertical" % (tag, lw), gv, rv, 0.25 + 0.005 * gv, "px")
    return [f]

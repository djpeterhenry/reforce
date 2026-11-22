"""Phase 5B probe measurements (runs 19 and 20), applied identically to G-Force captures and reforce frames.

run19 (probes/timelines/run19.py): 'zz reforce thin lines' / 'zz reforce thin dots' on the void field (one frame per
shot). Per line k (LWdt = .05 + .05 k): area width (column integral / 255, px), peak value, over the middle of the line.
run20: 'zz reforce sweep ramp' (dot k: Pen = k/255) through ColorMap.MapFunction = "i^2.1" with the gray and the
'zz reforce clamp' ColorMaps: the 256-entry output per channel.
"""
import measure as M

LINE_Y = [0.85 - 0.18 * k for k in range(10)]
LWDT = [0.05 + 0.05 * k for k in range(10)]


def _py(img, y):
    return (1 - y) * 0.5 * img.h - 0.5


def line_profile(img, k, slanted=False, cols=40):
    """Area width (px) and peak value of line k, averaged over `cols` columns in the middle of the line."""
    if slanted:
        xs = [0.3 + 0.4 * i / (cols - 1) for i in range(cols)]
        yc = [LINE_Y[k] - 0.04 + 0.08 * (x - 0.05) / 0.9 for x in xs]
    else:
        xs = [-0.7 + 0.4 * i / (cols - 1) for i in range(cols)]
        yc = [LINE_Y[k]] * cols
    areas, peaks, centers = [], [], []
    for x, y in zip(xs, yc):
        c = img.px(x)
        p = int(round(_py(img, y)))
        col = img.col(c, 0, p - 8, p + 9)
        areas.append(sum(col) / 255.0)
        peaks.append(max(col))
        if sum(col):
            centers.append(sum(v * i for i, v in enumerate(col)) / sum(col) + p - 8)
    return {"area_px": sum(areas) / len(areas), "peak": sum(peaks) / len(peaks),
            "peak_min": min(peaks), "peak_max": max(peaks)}


def dot_profile(img, k):
    """Mean integrated intensity (/255, px^2) and peak of the 12 dots of row k."""
    sums, peaks = [], []
    for i in range(12):
        x = -0.9 + 1.8 * i / 11 + 0.0007 * k
        c, p = img.px(x), int(round(_py(img, LINE_Y[k])))
        tot, pk = 0, 0
        for cc in range(c - 6, c + 7):
            col = img.col(cc, 0, p - 6, p + 7)
            tot += sum(col)
            pk = max(pk, max(col))
        sums.append(tot / 255.0)
        peaks.append(pk)
    return {"area_px2": sum(sums) / len(sums), "peak": sum(peaks) / len(peaks)}


def ramp(img, row_y=0.0, ch=0):
    """256 dot values (median of the 3x3 core of dot k at x = -.96 + 1.92 k/255) on the given row."""
    out = []
    p = int(round(_py(img, row_y)))
    for k in range(256):
        c = img.px(-0.96 + 1.92 * k / 255)
        vals = [img.v(cc, pp, ch) for cc in (c - 1, c, c + 1) for pp in (p - 1, p, p + 1)]
        out.append(M.median(vals))
    return out

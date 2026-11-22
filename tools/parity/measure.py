"""Measurements applied identically to a G-Force capture and to the matching reforce frame.

Every function takes an images.Image and returns numbers in frame units (x, y in [-1, 1]), pixels or
8-bit channel values, as documented on the function.
"""
import math


def median(v):
    s = sorted(v)
    n = len(s)
    if not n:
        return float("nan")
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def runs(values, thr, offset=0):
    """Runs of values > thr -> [(first, last, weighted center)] in index units (+offset)."""
    out, i, n = [], 0, len(values)
    while i < n:
        if values[i] > thr:
            j = i
            sw = sx = 0.0
            while j < n and values[j] > thr:
                sw += values[j]
                sx += values[j] * j
                j += 1
            out.append((i + offset, j - 1 + offset, sx / sw + offset))
            i = j
        else:
            i += 1
    return out


def centroid(img, x0, y0, x1, y1, thr=40, ch=0):
    """Intensity-weighted centroid of pixels > thr in the frame-coordinate box -> (x, y, peak) or None."""
    px0, px1 = sorted((img.px(x0), img.px(x1)))
    py0, py1 = sorted((img.py(y0), img.py(y1)))
    px0, py0 = max(px0, 0), max(py0, 0)
    px1, py1 = min(px1, img.w - 1), min(py1, img.h - 1)
    sw = sx = sy = 0.0
    peak = 0
    for y in range(py0, py1 + 1):
        r = img.row(y, ch, px0, px1 + 1)
        for i, v in enumerate(r):
            if v > thr:
                sw += v
                sx += v * (px0 + i)
                sy += v * y
                peak = max(peak, v)
    if sw == 0:
        return None
    return img.fx(sx / sw), img.fy(sy / sw), peak


def box_max(img, x0, y0, x1, y1, ch=0):
    px0, px1 = sorted((img.px(x0), img.px(x1)))
    py0, py1 = sorted((img.py(y0), img.py(y1)))
    return max(max(img.row(y, ch, max(px0, 0), min(px1, img.w - 1) + 1)) for y in range(max(py0, 0), min(py1, img.h - 1) + 1))


def box_median(img, x0, y0, x1, y1, ch=0, step=1):
    px0, px1 = sorted((img.px(x0), img.px(x1)))
    py0, py1 = sorted((img.py(y0), img.py(y1)))
    vals = []
    for y in range(py0, py1 + 1, step):
        vals += img.row(y, ch, px0, px1 + 1)[::step]
    return median(vals)


def box_mean_rgb(img, x0, y0, x1, y1, thr=8):
    """Mean RGB of the pixels whose max channel > thr (the drawn ones) in the box."""
    px0, px1 = sorted((img.px(x0), img.px(x1)))
    py0, py1 = sorted((img.py(y0), img.py(y1)))
    s = [0.0, 0.0, 0.0]
    n = 0
    for y in range(py0, py1 + 1):
        for x in range(px0, px1 + 1):
            c = img.rgb(x, y)
            if max(c) > thr:
                s[0] += c[0]; s[1] += c[1]; s[2] += c[2]
                n += 1
    return tuple(v / n for v in s) if n else (0.0, 0.0, 0.0)


def top_extent(img, x0, x1, thr=20):
    """Topmost row (frame y) whose max over [x0, x1] exceeds thr."""
    a, b = sorted((img.px(x0), img.px(x1)))
    for y in range(img.h):
        if max(img.row(y, 0, a, b + 1)) > thr:
            return img.fy(y)
    return float("nan")


def right_extent_row(img, y, thr=12, band=4):
    """Rightmost x (frame units) with value > thr on rows within +-band px of frame y (right half only)."""
    best = -1
    for yy in range(img.py(y) - band, img.py(y) + band + 1):
        row = img.row(yy)
        for x in range(img.w - 1, img.w // 2, -1):
            if row[x] > thr:
                best = max(best, x)
                break
    return img.fx(best) if best >= 0 else 0.0


def row_runs(img, y, thr=100, x0=-1.0, x1=1.0):
    """Centers (frame x) of the runs > thr along the row at frame y."""
    a, b = img.px(x0), min(img.px(x1), img.w - 1)
    return [img.fx(c) for _, _, c in runs(img.row(img.py(y), 0, a, b + 1), thr, a)]


def col_runs(img, x, thr=40, y0=-1.0, y1=1.0):
    """Centers (frame y) of the runs > thr along the column at frame x, top to bottom."""
    a, b = img.py(y1), img.py(y0)
    return [img.fy(c) for _, _, c in runs(img.col(img.px(x), 0, a, b + 1), thr, a)]


def area_width(img, x, y, half=30):
    """Line width in px from the column integral (sum / 255) across a horizontal line at (x, y)."""
    c = img.col(img.px(x), 0, img.py(y) - half, img.py(y) + half + 1)
    return sum(c) / 255.0


def dot_diameter(img, x, y, half=30):
    s = 0.0
    cx, cy = img.px(x), img.py(y)
    for yy in range(cy - half, cy + half + 1):
        s += sum(img.row(yy, 0, cx - half, cx + half + 1)) / 255.0
    return 2 * math.sqrt(s / math.pi)


def stripe_angle(img, x0, x1, y0, y1):
    """Dominant stripe orientation in the box, degrees from vertical (structure tensor of the gradients)."""
    a, b = img.px(x0), img.px(x1)
    c, d = img.py(y1), img.py(y0)
    jxx = jyy = jxy = 0.0
    for y in range(c + 1, d - 1, 2):
        r0, r1, r2 = img.row(y - 1, 0, a, b), img.row(y, 0, a, b), img.row(y + 1, 0, a, b)
        for i in range(1, len(r1) - 1, 2):
            gx = r1[i + 1] - r1[i - 1]
            gy = r2[i] - r0[i]
            jxx += gx * gx
            jyy += gy * gy
            jxy += gx * gy
    return math.degrees(0.5 * math.atan2(2 * jxy, jxx - jyy))


def dark_center(img, thr=128):
    """Half-width (frame x) of the dark region around the center of row y=0 (mean of both sides)."""
    row = img.row(img.py(0))
    c = img.w // 2
    left = next((i for i in range(c, -1, -1) if row[i] > thr), 0)
    right = next((i for i in range(c, img.w) if row[i] > thr), img.w - 1)
    return 0.5 * (img.fx(right) - img.fx(left))


def mean_outside_radius(img, radius, step=3):
    """Mean value of the pixels with x^2 + y^2 > radius^2 (frame units), sampled every `step` px."""
    s = n = 0
    for py in range(0, img.h, step):
        fy = img.fy(py)
        row = img.row(py)
        for px in range(0, img.w, step):
            fx = img.fx(px)
            if fx * fx + fy * fy > radius * radius and not (fx > 0.75 and fy < -0.9):  # skip G-Force's logo corner
                s += row[px]
                n += 1
    return s / n if n else float("nan")


def row_nonuniformity(img, x0, x1, ys, half=0.08, step=2):
    """Mean over rows near each y in ys of the std-dev across the row segment [x0, x1]: 0 when every row is
    uniform (rows shift as units), large when pixels vary independently along a row."""
    a, b = img.px(x0), img.px(x1)
    vals = []
    for y in ys:
        for py in range(img.py(y + half), img.py(y - half) + 1, step):
            r = img.row(py, 0, a, b + 1)
            m = sum(r) / len(r)
            vals.append(math.sqrt(sum((v - m) ** 2 for v in r) / len(r)))
    return sum(vals) / len(vals)


def trail_slope(img, y, head_px, length=160, skip=30):
    """Least-squares slope (levels per px) of the intensity along row y behind a moving head (to the left)."""
    row = img.row(img.py(y))
    xs = list(range(max(head_px - skip - length, 0), head_px - skip))
    if len(xs) < 10:
        return float("nan")
    vs = [row[x] for x in xs]
    mx, mv = sum(xs) / len(xs), sum(vs) / len(vs)
    return sum((x - mx) * (v - mv) for x, v in zip(xs, vs)) / sum((x - mx) ** 2 for x in xs)

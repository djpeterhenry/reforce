#!/usr/bin/env python3
"""Unit test for the Phase 5C strip-chart / snapshot decoder on synthetic images (no G-Force, no reforce).

    python3 -I tools/parity/test_audio_measure.py

Draws a strip-chart screenshot as G-Force would (1980x1260, 1 px/frame scroll, 1/255-per-frame decay,
antialiased ~2.8 px dots, overwrite in age order) for known signals plus a snapshot curve, writes it as a BMP
and checks that audio_measure decodes the values, the frame rate, the rails and the curves.
"""
import math
import os
import struct
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import audio_layout as L  # noqa: E402
import audio_measure as A  # noqa: E402
import images  # noqa: E402

W, H = L.WIDTH, L.HEIGHT
DOT_R = 1.42  # px (LWdt 1 ~ 2.84 px wide)


class Canvas:
    def __init__(self):
        self.v = bytearray(W * H)

    def dot(self, x_px, y_px, val):
        for py in range(int(y_px - 3), int(y_px + 4)):
            for px in range(int(x_px - 3), int(x_px + 4)):
                if not (0 <= px < W and 0 <= py < H):
                    continue
                # coverage of a disc of radius DOT_R, 4x4 supersampled
                cov = sum(1 for i in range(4) for j in range(4)
                          if (px - 0.375 + 0.25 * i - x_px) ** 2 + (py - 0.375 + 0.25 * j - y_px) ** 2 <= DOT_R ** 2) / 16
                if cov > 0:
                    k = py * W + px
                    self.v[k] = int(round(val * cov + self.v[k] * (1 - cov)))  # overwrite with AA edge

    def write_bmp(self, path):
        stride = (W * 3 + 3) // 4 * 4
        out = bytearray(b"BM" + struct.pack("<IHHI", 54 + stride * H, 0, 0, 54))
        out += struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, 0, stride * H, 2835, 2835, 0, 0)
        pad = b"\0" * (stride - W * 3)
        for y in range(H - 1, -1, -1):
            row = self.v[y * W:(y + 1) * W]
            out += bytes(b for v in row for b in (v, v, v)) + pad
        with open(path, "wb") as f:
            f.write(out)


def px_x(x):
    return (x + 1) * 0.5 * W - 0.5


def px_y(y):
    return (1 - y) * 0.5 * H - 0.5


def signal(key, t):
    """Known test signals (in each band's units)."""
    return {"LEVEL": 0.5 + 0.4 * math.sin(2 * math.pi * 0.7 * t),
            "BASS": 1.0 + 0.9 * math.sin(2 * math.pi * 0.3 * t + 1),
            "ENERGY": 0.1 + 0.05 * t,
            "fft.05": 2.5,                          # saturates (above hi)
            "fft.2": 0.75,
            "fft.5": 1.6 * abs((t % 2.0) - 1.0),   # triangle
            "fft.8": 0.02,
            "mag.5": -0.8 + 0.1 * t}[key]


FPS, T_SHOT = 55.6, 13.37


def draw_strip(cv):
    head = px_x(L.STRIP_X)
    for age in range(240, -1, -1):  # oldest first; newer dots overwrite
        t = T_SHOT - age / FPS
        val = 255 - age
        for k in range(len(L.SIGNALS)):
            b = L.band(k)
            v = (t / L.TIME_PERIOD) % 1.0 if b["key"] == "time" else signal(b["key"], t)
            u = min(1.0, max(0.0, (v - b["lo"]) / (b["hi"] - b["lo"])))
            for uu in (L.RAIL_LO, L.RAIL_HI, u):
                cv.dot(head - age, px_y(b["y0"] + b["h"] * uu), val)


def draw_snapshot(cv, layout, fn, bins=255):
    for curve, (y0, h, lo, hi) in L.SNAPSHOTS[layout].items():
        for i in range(bins):
            s = i / (bins - 1)
            u = min(1.0, max(0.0, (fn(curve, s) - lo) / (hi - lo)))
            cv.dot(px_x(L.SNAP_X0 + L.SNAP_XW * s), px_y(y0 + h * u), 255)


def snap_fn(curve, s):
    return 1.5 * math.exp(-((s - 0.3) / 0.05) ** 2) if curve == "fft" else 0.6 * math.sin(2 * math.pi * 2 * s)


class DecoderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cv = Canvas()
        draw_strip(cv)
        draw_snapshot(cv, "strip", snap_fn)
        p = os.path.join(cls.tmp, "strip.bmp")
        cv.write_bmp(p)
        cls.img = images.Image(p)
        cv2 = Canvas()
        draw_snapshot(cv2, "spectrum", snap_fn)
        p2 = os.path.join(cls.tmp, "spectrum.bmp")
        cv2.write_bmp(p2)
        cls.img2 = images.Image(p2)

    def test_strip_values(self):
        d = A.decode_strip(self.img)
        fit = A.shot_time_axis(d["rows"])
        self.assertIsNotNone(fit)
        a, b, rms, n = fit
        self.assertAlmostEqual(-1 / b, FPS, delta=FPS * 0.01)          # frame rate within 1 %
        self.assertGreater(n, 200)
        a += round((T_SHOT - a) / L.TIME_PERIOD) * L.TIME_PERIOD  # undo the 8 s wrap of the time band
        self.assertAlmostEqual(a, T_SHOT, delta=0.03)                # head time
        worst = {}
        for r in d["rows"]:
            if r["peak"] < 40:
                continue
            t = a + b * r["age"]
            for k in A.SERIES_KEYS:
                if r.get(k) is None:
                    continue
                lo, hi = [(s[2], s[3]) for s in L.SIGNALS if s[0] == k][0]
                want = min(hi, max(lo, signal(k, t)))
                worst[k] = max(worst.get(k, 0), abs(r[k] - want) / (hi - lo))
        for k in A.SERIES_KEYS:
            self.assertIn(k, worst, k)
            # 1.5 % of the band range: sub-pixel centroid + time-fit error on fast signals
            self.assertLess(worst[k], 0.015, "%s worst %.4f" % (k, worst[k]))
        print("\n  worst |err|/range: " + " ".join("%s %.4f" % kv for kv in sorted(worst.items())), file=sys.stderr)
        self.assertTrue(all(r["fft.05_sat"] for r in d["rows"] if r.get("fft.05") is not None))
        self.assertFalse(any(r["fft.2_sat"] for r in d["rows"] if r.get("fft.2") is not None))

    def test_rails(self):
        d = A.decode_strip(self.img)
        self.assertEqual(len(d["rails"]), len(L.SIGNALS))
        for r in d["rails"].values():
            self.assertLess(abs(r["lo_err_px"]), 0.3)
            self.assertLess(abs(r["hi_err_px"]), 0.3)

    def check_snapshot(self, img, layout):
        sn = A.decode_snapshot(img, layout)
        for curve in ("fft", "mag"):
            y0, h, lo, hi = L.SNAPSHOTS[layout][curve]
            for s, v, sat in sn[curve]:
                self.assertIsNotNone(v)
                self.assertLess(abs(v - snap_fn(curve, s)) / (hi - lo), 0.01, "%s s=%.3f" % (curve, s))

    def test_snapshot_strip(self):
        self.check_snapshot(self.img, "strip")

    def test_snapshot_spectrum(self):
        self.check_snapshot(self.img2, "spectrum")

    def test_run_stitch_and_onset(self):
        # Two overlapping shots of the same synthetic run: stitching must give monotonic, unique times.
        res = A.analyze_run([("a", T_SHOT - 2.0, self.img, "strip"), ("b", T_SHOT, self.img, "strip")],
                            {"sync": {"t": 0.5}, "file": "x.wav"})
        ts = res["series"]["t"]
        self.assertTrue(all(t2 > t1 for t1, t2 in zip(ts, ts[1:])))


if __name__ == "__main__":
    unittest.main(verbosity=2)

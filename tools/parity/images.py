"""Pixel access for the parity suite (stdlib only).

PNGs are converted once to uncompressed BMP with macOS `sips` (cached next to the run output), then read
straight from the bytes. Coordinates: pixels (0,0) top-left; frame coordinates x, y in [-1, 1], +y up
(G-Force's WaveShape space without aspect correction), x = -1 at the left edge, y = +1 at the top edge.
"""
import os
import struct
import subprocess


def to_bmp(png, cache_dir):
    os.makedirs(cache_dir, exist_ok=True)
    out = os.path.join(cache_dir, os.path.splitext(os.path.basename(png))[0] + ".bmp")
    if not os.path.exists(out) or os.path.getmtime(out) < os.path.getmtime(png):
        subprocess.run(["sips", "-s", "format", "bmp", png, "--out", out], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return out


def to_bmp_all(pngs, cache_dir):
    """Converts the stale ones of `pngs` with one `sips` call (sips costs ~80 ms per launch; runs have ~200 shots)."""
    os.makedirs(cache_dir, exist_ok=True)
    out = lambda p: os.path.join(cache_dir, os.path.splitext(os.path.basename(p))[0] + ".bmp")
    stale = [p for p in pngs if not os.path.exists(out(p)) or os.path.getmtime(out(p)) < os.path.getmtime(p)]
    if len({os.path.basename(p) for p in stale}) != len(stale):
        return  # same name twice: to_bmp() converts them one by one
    for i in range(0, len(stale), 200):
        subprocess.run(["sips", "-s", "format", "bmp"] + stale[i:i + 200] + ["--out", cache_dir], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class Image:
    def __init__(self, path):
        b = open(path, "rb").read()
        if b[:2] != b"BM":
            raise ValueError("%s: not a BMP" % path)
        off = struct.unpack_from("<I", b, 10)[0]
        w, h = struct.unpack_from("<ii", b, 18)
        bpp = struct.unpack_from("<H", b, 28)[0]
        if bpp not in (24, 32):
            raise ValueError("%s: %d bpp" % (path, bpp))
        self.w, self.h, self.bpp = w, abs(h), bpp // 8
        self.stride = ((w * bpp // 8) + 3) // 4 * 4
        self.flip = h > 0
        self.off = off
        self.b = b
        self.path = path

    def _i(self, x, y):
        row = (self.h - 1 - y) if self.flip else y
        return self.off + row * self.stride + x * self.bpp

    def rgb(self, x, y):
        i = self._i(x, y)
        return (self.b[i + 2], self.b[i + 1], self.b[i])

    def v(self, x, y, ch=0):
        """Channel value (0=R, 1=G, 2=B)."""
        return self.b[self._i(x, y) + (2 - ch)]

    def row(self, y, ch=0, x0=0, x1=None):
        x1 = self.w if x1 is None else x1
        i = self._i(x0, y) + (2 - ch)
        return list(self.b[i:i + (x1 - x0) * self.bpp:self.bpp])

    def col(self, x, ch=0, y0=0, y1=None):
        y1 = self.h if y1 is None else y1
        if y1 <= y0:
            return []
        # One strided slice (rows are `stride` bytes apart; bottom-up BMPs run backwards).
        i0 = self._i(x, y0) + (2 - ch)
        i1 = self._i(x, y1 - 1) + (2 - ch)
        if i1 >= i0:
            return list(self.b[i0:i1 + 1:self.stride])
        return list(self.b[i1:i0 + 1:self.stride])[::-1]

    # frame coordinates <-> pixels
    def px(self, x):
        return int(round((x + 1) * 0.5 * self.w - 0.5))

    def py(self, y):
        return int(round((1 - y) * 0.5 * self.h - 0.5))

    def fx(self, px):
        return (px + 0.5) / self.w * 2 - 1

    def fy(self, py):
        return 1 - (py + 0.5) / self.h * 2


def load(png, cache_dir):
    return Image(to_bmp(png, cache_dir))

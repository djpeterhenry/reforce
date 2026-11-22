"""Probe run 32 sprite set: the keying threshold during fades and at fractional pens (stdlib only).

Writes build/probe-sprites32/. 660x420 gray images, 11 columns of 60 px, native size (Flag 0 / 1024) in the frame centre:
  K1 fade ramp     10 12 14 16 18 20 23 26 30 35 255   (the 255 column gives the envelope in each shot)
  K2 fine ramp     26 28 29 30 31 32 33 34 35 36 255   (x .3 = 7.8 ... 10.8 levels)
  K3 dark image     0  4  8 10 12 14 16 18 20 20 20   (no bright pixel at all: auto-levelled?)
Sprites:
  zz k fade p      K1 Pen 1                 fade-in / fade-out sweep (feedback draw)
  zz k fade e      K1 ThresholdCopy EPen 1  fade-in sweep (display draw)
  zz k p3          K2 Pen .3
  zz k e3          K2 ThresholdCopy EPen .3
  zz k pe3         K2 Pen 1, EPen .3        (display keyed on EPen)
  zz k dark        K3 Pen 1

Usage: python3 -I tools/parity/make_sprite32.py [outdir]
"""
import os
import struct
import sys
import zlib

RAMPS = {
    "K1": [10, 12, 14, 16, 18, 20, 23, 26, 30, 35, 255],
    "K2": [26, 28, 29, 30, 31, 32, 33, 34, 35, 36, 255],
    "K3": [0, 4, 8, 10, 12, 14, 16, 18, 20, 20, 20],
}
SPRITES = {
    "zz k fade p": ("K1", 'Pen="1"\nFlag="0"\n'),
    "zz k fade e": ("K1", 'EPen="1"\nFlag="1024"\n'),
    "zz k p3": ("K2", 'Pen=".3"\nFlag="0"\n'),
    "zz k e3": ("K2", 'EPen=".3"\nFlag="1024"\n'),
    "zz k pe3": ("K2", 'Pen="1"\nEPen=".3"\nFlag="0"\n'),
    "zz k dark": ("K3", 'Pen="1"\nFlag="0"\n'),
}


def write_png(path, w, h, row):
    raw = b"".join(b"\x00" + row for _ in range(h))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "../../build/probe-sprites32")
    os.makedirs(out, exist_ok=True)
    for name, (img, params) in SPRITES.items():
        lums = RAMPS[img]
        write_png(os.path.join(out, name + ".png"), 660, 420, bytes(lums[x // 60] for x in range(660)))
        with open(os.path.join(out, name + ".txt"), "w") as f:
            f.write("// reforce probe run 32 params\nX=\"0\"\nY=\"0\"\n" + params + "Vers=100\n")
    print(os.path.abspath(out))


if __name__ == "__main__":
    main()

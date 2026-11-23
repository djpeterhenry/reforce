"""Probe run 27 sprite set: sprite X/Y with each fit mode, and Pen/EPen combinations (stdlib only).

Writes build/probe-sprites27/. Images (grayscale):
  A  600x382 (the 1980x1260 frame's aspect): black, white band u in [.725, .875], 50% gray band u in [.125, .275]
     (same as run 26)
  W  800x400 (2:1, wider than the frame): the same bands plus a center stripe of 200 at u in [.49, .51], so a
     cover fit crops its sides and leaves horizontal slack, a contain fit letterboxes it vertically
Sprites (each its own image copy + params .txt, EPen-only unless noted):
  zz x a5     A, Flag 7 (cover), X = .5            does X move a cover sprite with no slack?
  zz x w0     W, Flag 7, X = 0                     baseline crop
  zz x w1     W, Flag 7, X = .1                    X inside the slack: moves? by how much?
  zz x w2     W, Flag 7, X = .2
  zz x w10    W, Flag 7, X = 1                     beyond the slack: clamped?
  zz x n5     A, Flag 0 (native size), X = .5, Y = .3
  zz x c      W, Flag 15 (contain: MaintainAspect | DontCrop + fit), X = .2, Y = .3
  zz pe 1-0   A, Flag 7, Pen = 1, EPen = 0         display with an explicit EPen 0?
  zz pe 1-25  A, Flag 7, Pen = 1, EPen = .25       display = EPen x lum?
  zz pe 5-1   A, Flag 7, Pen = .5, EPen = 1        EPen above Pen
  zz pe 0-1   A, Flag 7, Pen = 0, EPen = 1         Pen 0: erases the feedback under non-black pixels?
  zz pe 1-x   A, Flag 7, Pen = 1 (no EPen)         Pen fade-in on a moving field

Usage: python3 -I tools/parity/make_sprite27.py [outdir]
"""
import os
import struct
import sys
import zlib


def band(u, center=False):
    if center and .49 <= u < .51:
        return 200
    if .725 <= u < .875:
        return 255
    if .125 <= u < .275:
        return 128
    return 0


def write_png(path, w, h, row):
    raw = b"".join(b"\x00" + row for _ in range(h))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


IMAGES = {
    "A": (600, 382, False),
    "W": (800, 400, True),
}

SPRITES = {
    "zz x a5": ("A", 'X=".5"\nY="0"\nEPen="1"\nFlag="7"\n'),
    "zz x w0": ("W", 'X="0"\nY="0"\nEPen="1"\nFlag="7"\n'),
    "zz x w1": ("W", 'X=".1"\nY="0"\nEPen="1"\nFlag="7"\n'),
    "zz x w2": ("W", 'X=".2"\nY="0"\nEPen="1"\nFlag="7"\n'),
    "zz x w10": ("W", 'X="1"\nY="0"\nEPen="1"\nFlag="7"\n'),
    "zz x n5": ("A", 'X=".5"\nY=".3"\nEPen="1"\nFlag="0"\n'),
    "zz x c": ("W", 'X=".2"\nY=".3"\nEPen="1"\nFlag="15"\n'),
    "zz pe 1-0": ("A", 'X="0"\nY="0"\nPen="1"\nEPen="0"\nFlag="7"\n'),
    "zz pe 1-25": ("A", 'X="0"\nY="0"\nPen="1"\nEPen=".25"\nFlag="7"\n'),
    "zz pe 5-1": ("A", 'X="0"\nY="0"\nPen=".5"\nEPen="1"\nFlag="7"\n'),
    "zz pe 0-1": ("A", 'X="0"\nY="0"\nPen="0"\nEPen="1"\nFlag="7"\n'),
    "zz pe 1-x": ("A", 'X="0"\nY="0"\nPen="1"\nFlag="7"\n'),
}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "../../build/probe-sprites27")
    os.makedirs(out, exist_ok=True)
    for name, (img, params) in SPRITES.items():
        w, h, center = IMAGES[img]
        row = bytes(band((i + .5) / w, center) for i in range(w))
        write_png(os.path.join(out, name + ".png"), w, h, row)
        with open(os.path.join(out, name + ".txt"), "w") as f:
            f.write("// reforce probe run 27 params\n" + params + "Vers=100\n")
    print(os.path.abspath(out))


if __name__ == "__main__":
    main()

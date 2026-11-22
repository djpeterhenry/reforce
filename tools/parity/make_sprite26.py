"""Probe run 26 sprite set (sprite layering vs the FlowField; stdlib only).

Writes build/probe-sprites26/: one grayscale image (frame aspect 1980x1260, so the default cover fit fills the frame
exactly) under four names, three with a sibling params .txt:

  zz reforce sq        no .txt: G-Force's default sprite params (EPen=1, cover)
  zz reforce sq pen    X=.2, Pen=1 (no EPen)
  zz reforce sq both   X=.2, Pen=1, EPen=.5
  zz reforce sq epen   X=.2, EPen=1 (no Pen)

X=.2 shifts the image right by .2 (frame units), so a shot shows whether G-Force read the .txt at all.
Image: black, a white band at u in [.725, .875] (frame x in [.45, .75]) and a 50% gray band at u in [.125, .275]
(x in [-.75, -.45]); both avoid the probe grid's vertical lines (x = 0, +-.4, +-.8).

Usage: python3 -I tools/parity/make_sprite26.py [outdir]
"""
import os
import struct
import sys
import zlib

W, H = 600, 382


def pixel(u):
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


PARAMS = {
    "zz reforce sq": None,
    "zz reforce sq pen": 'X=".2"\nY="0"\nPen="1"\nFlag="7"\n',
    "zz reforce sq both": 'X=".2"\nY="0"\nPen="1"\nEPen=".5"\nFlag="7"\n',
    "zz reforce sq epen": 'X=".2"\nY="0"\nEPen="1"\nFlag="7"\n',
}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "../../build/probe-sprites26")
    os.makedirs(out, exist_ok=True)
    row = bytes(pixel((i + .5) / W) for i in range(W))
    for name, params in PARAMS.items():
        write_png(os.path.join(out, name + ".png"), W, H, row)
        if params:
            with open(os.path.join(out, name + ".txt"), "w") as f:
                f.write("// reforce probe run 26 params\n" + params + "Vers=100\n")
    print(os.path.abspath(out))


if __name__ == "__main__":
    main()

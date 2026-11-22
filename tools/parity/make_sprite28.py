"""Probe run 28 sprite set: blend flags, alpha, keying threshold, EPen scaling, colour weights, fades (stdlib only).

Writes build/probe-sprites28/. Images:
  G  660x420 RGBA (native size, Flag bits without fit, so 1:1 in the frame centre, cells 60x70 px):
     11 columns of luminance  0 1 4 16 64 120 127 128 136 192 255
      6 rows of alpha        255 192 128 127 64 0   (straight, not premultiplied)
  C  660x420 RGB: 6 columns red green blue yellow cyan magenta (255), rows: full, half (128)
  B  600x382 gray flat 96: full-frame Pen 1 background, restamped every frame (F = 96 under the tests)
  A  600x382 the run 26/27 bands (white u .725-.875, gray 128 u .125-.275) for the fades
Sprites (.txt beside each):
  zz bg            B  Flag 7 Pen 1
  zz f <m> p       G  Flag <bits> Pen 1             for m in copy thr add sel am ab inv
  zz f <m> e       G  Flag <bits> EPen 1
  zz f add p5      G  AdditiveBlend Pen .5
  zz f ab p5       G  AlphaBlend Pen .5
  zz f copy e5     G  Flag 0 EPen .5                EPen-only scaling
  zz f copy pe     G  Flag 0 Pen 1 EPen .5          Pen + EPen with alpha
  zz f col p       C  Flag 0 Pen 1                  luminance weights
  zz fd p          A  Flag 7 Pen .5                 fade reference
  zz fd pe         A  Flag 7 Pen .5 EPen 1          EPen-with-Pen envelope
  zz fd e          A  Flag 7 EPen 1                 EPen-only envelope (fade-out check)

Run 29 (same folder, order flipped: test sprite first, background 0.05 s later):
  T  660x420 gray: 11 columns of low luminance 0 4 6 8 10 12 14 16 20 24 32 (keying threshold)
  zz f <m> e5      G  Flag <bits> EPen .5           for m in thr add am ab
  zz t copy p      T  Flag 0 Pen 1
  zz t thr e       T  ThresholdCopy EPen 1
  zz t copy pe     T  Flag 0 Pen 1 EPen 1
  zz bg2           B  as zz bg (a second name, so a stale sprite instance cannot be reused)
Run 30 (keying for Pen < 1 and Pen 0; the envelope in each blend mode):
  zz t copy p5     T  Flag 0 Pen .5
  zz t copy p0     T  Flag 0 Pen 0
Run 31 (does Pen 0 with EPen 1 erase the feedback? run 27's "erase" was over an already-black buffer):
  zz f pe 0-1      G  Flag 0 Pen 0 EPen 1

Usage: python3 -I tools/parity/make_sprite28.py [outdir]
"""
import os
import struct
import sys
import zlib

LUMS = [0, 1, 4, 16, 64, 120, 127, 128, 136, 192, 255]
ALPHAS = [255, 192, 128, 127, 64, 0]
COLOURS = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255)]
FLAGS = {"copy": 0, "thr": 1024, "add": 2048, "sel": 4096, "am": 8192, "ab": 16384, "inv": 16}


def write_png(path, w, h, ctype, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def img_g():
    rows = []
    for y in range(420):
        a = ALPHAS[y // 70]
        rows.append(b"".join(bytes((v, v, v, a)) for v in (LUMS[x // 60] for x in range(660))))
    return 660, 420, 6, rows


def img_c():
    rows = []
    for y in range(420):
        k = 1.0 if y < 210 else 128 / 255
        rows.append(b"".join(bytes(int(round(c * k)) for c in COLOURS[x // 110]) for x in range(660)))
    return 660, 420, 2, rows


LOWS = [0, 4, 6, 8, 10, 12, 14, 16, 20, 24, 32]


def img_t():
    return 660, 420, 0, [bytes(LOWS[x // 60] for x in range(660))] * 420


def img_b():
    return 600, 382, 0, [bytes([96]) * 600] * 382


def img_a():
    def band(u):
        return 255 if .725 <= u < .875 else 128 if .125 <= u < .275 else 0
    return 600, 382, 0, [bytes(band((i + .5) / 600) for i in range(600))] * 382


def sprites():
    s = {"zz bg": (img_b, 'Pen="1"\nFlag="7"\n')}
    for m, bits in FLAGS.items():
        s["zz f %s p" % m] = (img_g, 'Pen="1"\nFlag="%d"\n' % bits)
        s["zz f %s e" % m] = (img_g, 'EPen="1"\nFlag="%d"\n' % bits)
    s["zz f add p5"] = (img_g, 'Pen=".5"\nFlag="2048"\n')
    s["zz f ab p5"] = (img_g, 'Pen=".5"\nFlag="16384"\n')
    s["zz f copy e5"] = (img_g, 'EPen=".5"\nFlag="0"\n')
    s["zz f copy pe"] = (img_g, 'Pen="1"\nEPen=".5"\nFlag="0"\n')
    s["zz f col p"] = (img_c, 'Pen="1"\nFlag="0"\n')
    s["zz fd p"] = (img_a, 'Pen=".5"\nFlag="7"\n')
    s["zz fd pe"] = (img_a, 'Pen=".5"\nEPen="1"\nFlag="7"\n')
    s["zz fd e"] = (img_a, 'EPen="1"\nFlag="7"\n')
    for m in ("thr", "add", "am", "ab"):
        s["zz f %s e5" % m] = (img_g, 'EPen=".5"\nFlag="%d"\n' % FLAGS[m])
    s["zz t copy p"] = (img_t, 'Pen="1"\nFlag="0"\n')
    s["zz t thr e"] = (img_t, 'EPen="1"\nFlag="1024"\n')
    s["zz t copy pe"] = (img_t, 'Pen="1"\nEPen="1"\nFlag="0"\n')
    s["zz t copy p5"] = (img_t, 'Pen=".5"\nFlag="0"\n')
    s["zz t copy p0"] = (img_t, 'Pen="0"\nFlag="0"\n')
    s["zz f pe 0-1"] = (img_g, 'Pen="0"\nEPen="1"\nFlag="0"\n')
    s["zz bg2"] = (img_b, 'Pen="1"\nFlag="7"\n')
    return s


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "../../build/probe-sprites28")
    os.makedirs(out, exist_ok=True)
    for name, (img, params) in sprites().items():
        w, h, ctype, rows = img()
        write_png(os.path.join(out, name + ".png"), w, h, ctype, rows)
        with open(os.path.join(out, name + ".txt"), "w") as f:
            f.write("// reforce probe run 28 params\nX=\"0\"\nY=\"0\"\n" + params + "Vers=100\n")
    print(os.path.abspath(out))


if __name__ == "__main__":
    main()

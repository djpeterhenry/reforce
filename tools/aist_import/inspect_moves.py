"""Reads a moves file and reports per move: biggest per-sample jump (which channel), lowest hips, and more."""
import struct, sys
import numpy as np
from collections import Counter

def read(path):
    b = open(path, 'rb').read(); o = 8
    def u(fmt):
        nonlocal o; v = struct.unpack_from(fmt, b, o); o += struct.calcsize(fmt); return v
    def s8():
        nonlocal o; n = b[o]; o += 1; v = b[o:o+n].decode(); o += n; return v
    assert b[:8] == b'RFMOVES2', 'not a moves file (v2)'
    C, = u('<I'); names = [s8() for _ in range(C)]
    bits = list(b[o:o+C]); o += C
    dt = np.dtype([(f'c{c}', '<u2' if bits[c] == 16 else 'u1') for c in range(C)])
    N, = u('<I'); moves = []
    for _ in range(N):
        mid = s8(); label = s8(); g = b[o:o+2].decode(); o += 2
        kind, bpm, beats, spb, turns = u('<BHHHh')
        mm = np.array(u('<%df' % (2*C))).reshape(C, 2)
        rec = np.frombuffer(b, dt, beats*spb, o); o += beats*spb*dt.itemsize
        q = np.stack([rec[f'c{c}'].astype(float) / ((1 << bits[c]) - 1) for c in range(C)], 1)
        v = mm[:, 0] + q * (mm[:, 1] - mm[:, 0])
        moves.append(dict(id=mid, label=label, genre=g, kind=kind, bpm=bpm, beats=beats, spb=spb, turns=turns, v=v))
    return names, moves

if __name__ == '__main__':
    names, moves = read(sys.argv[1]); ch = {n: i for i, n in enumerate(names)}
    jumps = []
    for m in moves:
        v = m['v']; d = np.abs(np.diff(np.vstack([v, v[:1]]), axis=0))
        d[-1, ch['bodyYaw']] = abs(((v[0, ch['bodyYaw']] + 2*np.pi*m['turns'] - v[-1, ch['bodyYaw']]) + np.pi) % (2*np.pi) - np.pi)
        k = np.unravel_index(np.argmax(d), d.shape)
        jumps.append((d.max(), names[k[1]], k[0], m['id']))
    jumps.sort(reverse=True)
    for j in jumps[:15]: print('%.2f %-10s sample %3d %s' % j)
    print(Counter(j[1] for j in jumps[:100]))
    hips = sorted((m['v'][:, ch['hipY']].min(), m['id']) for m in moves)
    print('lowest hips', [(round(h, 2), i) for h, i in hips[:8]])
    print('moves with hips below -0.35 leg:', Counter(m['genre'] for m in moves if m['v'][:, ch['hipY']].min() < -0.35))

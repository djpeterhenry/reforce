#!/usr/bin/env python3.11
"""Measurement (2026-11-27, why dancers sometimes spin super-rapidly): per move, the body heading's angular speed (bodyYaw, and the pelvis as drawn), at 124 BPM; stretches over
1.5 turns/s; compared with the raw AIST++ pelvis heading for the same frames.
    tools/aist_import/run heading_speed.py [out.json]
"""
import sys, pickle, re, json
import numpy as np
from scipy.spatial.transform import Rotation
from scipy.ndimage import gaussian_filter1d
import struct




def read_moves(path):
    b = open(path, 'rb').read()
    assert b[:8] == b'RFMOVES2'
    o = 8
    C, = struct.unpack_from('<I', b, o); o += 4
    names = []
    for _ in range(C):
        l = b[o]; o += 1; names.append(b[o:o + l].decode()); o += l
    bits = list(b[o:o + C]); o += C
    n, = struct.unpack_from('<I', b, o); o += 4
    moves = []
    for _ in range(n):
        l = b[o]; o += 1; mid = b[o:o + l].decode(); o += l
        l = b[o]; o += 1; label = b[o:o + l].decode('utf-8'); o += l
        genre = b[o:o + 2].decode(); o += 2
        kind, bpm, beats, spb, turns = struct.unpack_from('<BHHHh', b, o); o += struct.calcsize('<BHHHh')
        mm = np.frombuffer(b, dtype='<f4', count=2 * C, offset=o).reshape(C, 2).astype(np.float64); o += 8 * C
        N = beats * spb
        dt = np.dtype([(f'c{c}', '<u2' if bits[c] == 16 else 'u1') for c in range(C)])
        rec = np.frombuffer(b, dtype=dt, count=N, offset=o); o += dt.itemsize * N
        s = np.zeros((N, C))
        for c in range(C):
            top = (1 << bits[c]) - 1
            s[:, c] = mm[c, 0] + rec[f'c{c}'].astype(np.float64) / top * (mm[c, 1] - mm[c, 0])
        moves.append(dict(id=mid, label=label, genre=genre, kind=kind, bpm=bpm, beats=beats, spb=spb, turns=turns, s=s))
    return names, moves


BPM = 124.0
SPS = BPM / 60.0  # beats per second
FAST = 1.5 * 2 * np.pi  # rad/s
import os
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIST = os.path.expanduser('~/reforce/aist/data/keypoints3d')
LSHO, RSHO, LHIP, RHIP = 5, 6, 11, 12


class _Safe(pickle.Unpickler):
    def find_class(self, m, n):
        if m in ('numpy.core.multiarray', 'numpy._core.multiarray', 'numpy', 'numpy.core.numeric',
                 'numpy._core.numeric') and n in ('_reconstruct', 'ndarray', 'dtype', 'scalar', '_frombuffer'):
            import importlib
            return getattr(importlib.import_module(m), n)
        raise pickle.UnpicklingError(f'blocked {m}.{n}')


_raw = {}
def raw_heading(stem):
    if stem not in _raw:
        with open(f'{AIST}/{stem}.pkl', 'rb') as f:
            k = np.asarray(_Safe(f).load()['keypoints3d_optim'], dtype=np.float64)
        def head(a, b):
            left = k[:, a] - k[:, b]
            fwd = np.cross(left, [0, 1, 0])
            return np.unwrap(np.arctan2(fwd[:, 0], fwd[:, 2])), np.hypot(fwd[:, 0], fwd[:, 2]) / (np.linalg.norm(left, axis=1) + 1e-9)
        hp, hq = head(LHIP, RHIP)
        sp, sq = head(LSHO, RSHO)
        _raw[stem] = (hp, hq, sp, sq)
    return _raw[stem]


def ry(a):
    c, s = np.cos(a), np.sin(a)
    R = np.zeros(a.shape + (3, 3))
    R[..., 0, 0] = c; R[..., 0, 2] = s; R[..., 1, 1] = 1; R[..., 2, 0] = -s; R[..., 2, 2] = c
    return R


def rz(a):
    c, s = np.cos(a), np.sin(a)
    R = np.zeros(a.shape + (3, 3))
    R[..., 0, 0] = c; R[..., 0, 1] = -s; R[..., 1, 0] = s; R[..., 1, 1] = c; R[..., 2, 2] = 1
    return R


def rx(a):
    c, s = np.cos(a), np.sin(a)
    R = np.zeros(a.shape + (3, 3))
    R[..., 0, 0] = 1; R[..., 1, 1] = c; R[..., 1, 2] = -s; R[..., 2, 1] = s; R[..., 2, 2] = c
    return R


def stretches(speed, thr):
    """(start, end) index pairs where |speed| > thr."""
    over = np.abs(speed) > thr
    out, i = [], 0
    while i < len(over):
        if over[i]:
            j = i
            while j < len(over) and over[j]: j += 1
            out.append((i, j)); i = j
        else:
            i += 1
    return out


def analyze(names, m, floor):
    CH = {n: i for i, n in enumerate(names)}
    s = m['s']; N = len(s); spb = m['spb']
    # Two loops, the whole turns added (as clipPose does).
    yaw = np.concatenate([s[:, CH['bodyYaw']], s[:, CH['bodyYaw']] + 2 * np.pi * m['turns'], s[:1, CH['bodyYaw']] + 4 * np.pi * m['turns']])
    vb = np.diff(yaw) * spb * SPS  # rad/s, per 16th-of-a-beat step
    vb = vb[:N]  # one loop (step i: sample i -> i+1, the last one across the seam)
    # Pelvis as drawn: Ry(bodyYaw) [Rq] Ry(hipYaw) Rz(roll) Rx(pitch) e_z; heading where it is horizontal enough.
    by = s[:, CH['bodyYaw']]
    R = ry(by)
    if floor:
        q = s[:, [CH['hipQx'], CH['hipQy'], CH['hipQz'], CH['hipQw']]]
        q = q / np.linalg.norm(q, axis=1, keepdims=True)
        R = R @ Rotation.from_quat(q).as_matrix()
    R = R @ ry(s[:, CH['hipYaw']]) @ rz(s[:, CH['hipRoll']]) @ rx(s[:, CH['hipPitch']])
    z = R[:, :, 2]
    yv = R[:, :, 1]
    h = z + (yv if floor else 0)  # floor: facing plus the spine's lie (as the importer's heading)
    hh = np.hypot(h[:, 0], h[:, 2])
    ph = np.arctan2(h[:, 0], h[:, 2])
    dp = np.remainder(np.diff(np.concatenate([ph, ph[:1]])) + np.pi, 2 * np.pi) - np.pi
    ok = (hh > 0.4) & (np.roll(hh, -1) > 0.4)
    vp = np.where(ok, dp * spb * SPS, 0.0)
    # Chest as drawn (standing): pelvis yaw + chestYaw (roughly)
    r = dict(label=m['label'], id=m['id'], genre=m['genre'], bpm=m['bpm'], beats=m['beats'], turns=m['turns'],
             floor=floor, bmax=float(np.abs(vb).max()), b99=float(np.percentile(np.abs(vb), 99)),
             pmax=float(np.abs(vp).max()), p99=float(np.percentile(np.abs(vp), 99)),
             big16b=int((np.abs(vb) / (spb * SPS) > 2).sum()), big16p=int((np.abs(vp) / (spb * SPS) > 2).sum()),
             net=float((yaw[N] - yaw[0])), maxper_beat_b=float(np.abs(vb).max() / SPS))
    # Stretches of the body heading over 1.5 turns/s.
    st = []
    for a, b in stretches(vb, FAST):
        ang = float(np.sum(vb[a:b]) / (spb * SPS))
        st.append(dict(kind='body', b0=a / spb, b1=b / spb, peak=float(np.abs(vb[a:b]).max()), angle=ang,
                       seam=bool(b >= N - 2 * spb or a == 0)))
    for a, b in stretches(vp, FAST * 1.0):
        ang = float(np.sum(vp[a:b]) / (spb * SPS))
        st.append(dict(kind='pelvis', b0=a / spb, b1=b / spb, peak=float(np.abs(vp[a:b]).max()), angle=ang,
                       seam=bool(b >= N - 2 * spb or a == 0)))
    r['stretches'] = st
    # Raw AIST++: the pelvis heading per frame (no smoothing), same beats, at 124 BPM.
    mm = re.match(r'aist(?:floor)?:(.+)@(\d+)$', m['id'])
    if mm:
        stem, b0 = mm[1], int(mm[2])
        hp, hq, sp, sq = raw_heading(stem)
        P = 60.0 * 60.0 / m['bpm']
        f0, f1 = int(round(b0 * P)), int(round((b0 + m['beats']) * P))
        f1 = min(f1, len(hp) - 1)
        seg = hp[f0:f1 + 1]
        q = hq[f0:f1 + 1]
        v = np.diff(seg) * 60.0 * (BPM / m['bpm'])  # rad/s at 124 BPM
        vs = np.diff(gaussian_filter1d(seg, 2)) * 60.0 * (BPM / m['bpm'])
        r['rawmax'] = float(np.abs(v).max()); r['raw99'] = float(np.percentile(np.abs(v), 99))
        r['rawsmax'] = float(np.abs(vs).max())
        r['rawflips'] = int((np.abs(np.diff(seg)) > 1.5).sum())
        r['rawnet'] = float(seg[-1] - seg[0])
        r['rawlowq'] = float((q < 0.3).mean())
        # Raw at each stretch: the max speed of the raw pelvis heading (smoothed 2 frames) within +-0.5 beat
        for x in st:
            a = int((b0 + x['b0'] - 0.5) * P); b = int((b0 + x['b1'] + 0.5) * P)
            a = max(f0, a); b = min(f1, b)
            if b > a + 1:
                w = np.diff(gaussian_filter1d(hp, 2)[a:b + 1]) * 60.0 * (BPM / m['bpm'])
                x['raw_peak'] = float(np.abs(w).max())
                x['raw_angle'] = float(hp[b] - hp[a])
                x['raw_flip'] = int((np.abs(np.diff(hp[a:b + 1])) > 1.0).sum())
                ws = np.diff(gaussian_filter1d(sp, 2)[a:b + 1]) * 60.0 * (BPM / m['bpm'])
                x['raw_sho_peak'] = float(np.abs(ws).max())
                x['raw_sho_angle'] = float(sp[b] - sp[a])
    return r


def main():
    out = []
    for f, floor in (('aist.moves', False), ('aist_floor.moves', True)):
        names, moves = read_moves(f'{REPO}/content/Moves/{f}')
        for m in moves:
            out.append(analyze(names, m, floor))
    if len(sys.argv) > 1: json.dump(out, open(sys.argv[1], 'w'), indent=1)
    report(out)


def report(d):
    import collections
    TURN = 2 * np.pi
    print('moves', len(d))
    for floor in (False, True):
        sub = [r for r in d if r['floor'] == floor]
        print(f"\n== {'floor' if floor else 'standing'} moves: {len(sub)}")
        by = collections.defaultdict(list)
        for r in sub: by[r['genre']].append(r)
        print(f"{'genre':5s} {'n':>3s} {'body max t/s':>12s} {'body p99 med':>12s} {'pelvis max':>10s} {'raw max(sm)':>11s} {'moves w/ body>1.5t/s':>20s} {'pelvis>1.5':>10s} {'whole turns!=0':>14s}")
        for g, rs in sorted(by.items()):
            bm = max(r['bmax'] for r in rs) / TURN
            b99 = np.median([r['b99'] for r in rs]) / TURN
            pm = max(r['pmax'] for r in rs) / TURN
            rm = max(r.get('rawsmax', 0) for r in rs) / TURN
            nb = sum(any(x['kind'] == 'body' for x in r['stretches']) for r in rs)
            npl = sum(any(x['kind'] == 'pelvis' for x in r['stretches']) for r in rs)
            nt = sum(r['turns'] != 0 for r in rs)
            print(f"{g:5s} {len(rs):3d} {bm:12.2f} {b99:12.3f} {pm:10.2f} {rm:11.2f} {nb:20d} {npl:10d} {nt:14d}")
        print('\nworst 25 by body heading max (turns/s at 124 BPM):')
        for r in sorted(sub, key=lambda r: -r['bmax'])[:25]:
            st = [x for x in r['stretches'] if x['kind'] == 'body']
            desc = '; '.join(f"b{x['b0']:.2f}-{x['b1']:.2f} pk {x['peak']/TURN:.2f}t/s ang {x['angle']:.2f}rad raw pk {x.get('raw_peak',0)/TURN:.2f} ang {x.get('raw_angle',0):.2f} flips {x.get('raw_flip',0)} sho pk {x.get('raw_sho_peak',0)/TURN:.2f} ang {x.get('raw_sho_angle',0):.2f}{' SEAM' if x['seam'] else ''}" for x in st[:4])
            print(f"  {r['bmax']/TURN:5.2f} p99 {r['b99']/TURN:4.2f} turns {r['turns']:+d} net {r['net']:+.2f} rawnet {r.get('rawnet',0):+.2f} rawflips {r.get('rawflips',0)} lowq {r.get('rawlowq',0):.2f} | {r['label']} [{r['id']}] bpm {r['bpm']}\n      {desc}")
        print('\nworst 15 by pelvis max:')
        for r in sorted(sub, key=lambda r: -r['pmax'])[:15]:
            st = [x for x in r['stretches'] if x['kind'] == 'pelvis']
            desc = '; '.join(f"b{x['b0']:.2f}-{x['b1']:.2f} pk {x['peak']/TURN:.2f} ang {x['angle']:.2f} raw pk {x.get('raw_peak',0)/TURN:.2f} flips {x.get('raw_flip',0)}{' SEAM' if x['seam'] else ''}" for x in st[:3])
            print(f"  {r['pmax']/TURN:5.2f} | {r['label']} body {r['bmax']/TURN:.2f} | {desc}")
        # 16th-of-a-beat jumps
        print('moves with a 2 rad jump in a 16th (body / pelvis):', sum(r['big16b'] > 0 for r in sub), sum(r['big16p'] > 0 for r in sub))
        # distribution of all body stretches by angle
        allst = [x for r in sub for x in r['stretches'] if x['kind'] == 'body']
        print('body stretches >1.5 t/s:', len(allst), 'in', len(set(r['id'] for r in sub for x in r['stretches'] if x['kind']=='body')), 'moves; seam:', sum(x['seam'] for x in allst))
        if allst:
            print('  raw (smoothed 2 frames) peak at those, turns/s: median', np.median([x.get('raw_peak',0) for x in allst])/TURN,
                  ' with raw flips (>1 rad a frame):', sum(x.get('raw_flip',0)>0 for x in allst))



if __name__ == '__main__':
    main()

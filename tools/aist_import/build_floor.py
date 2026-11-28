"""Break floor work from AIST++ (tools/aist_import, docs/dancers-design.md "Break floor work").

    tools/aist_import/run build_floor.py ~/reforce/aist/data content/Moves/aist_floor.moves

What build_moves leaves out of break (hips most of a leg down, lying, upside down) as its own file, so aist.moves stays
as it is. Same format as build_moves (RFMOVES2) with five more channels (retarget FLOOR_CHANNELS: the pelvis's turn
as a quaternion, the upper body's clearance above the floor, each hand's, knee's and the head's own), and the move
kind's bits 4-5 say the tier (Peter, 2026-11-27: "footwork" and "power" read as the same thing):
  1 floor   low work on the hands, knees and hips (six-step, CCs, get-downs, kick-outs)
  3 flips   goes upside down (kick-ups, handstands, rolls over the shoulder)
(2, freezes, held positions off the feet: none in AIST++, so no longer looked for.) A move's tier is the most daring
thing in it. Moves:
  - the floor basics (choreographies whose 16 beats don't pass build_moves.usable: break 7-10), every performance at
    120-130 BPM, at least KEEP_BASIC each;
  - every break routine (sFM) build_moves left out, whole (16 bars), else its better clean half (8 bars), else its
    clean 4-bar stretches with floor work in them (played as basic moves).
The tracking is worse on the floor (the 2D detectors behind AIST++ are trained on people standing): a move is kept
only where the source's bone lengths hold (median error under BONE_ERR, at most BAD_SHARE of frames off by 30 %) and
glitches cover at most REPAIR_SHARE of it (bridged as build_moves does). Flips moves are found but left out (Peter,
2026-11-27: not good enough yet). A move on the floor (the upper body within a quarter leg of it, or upside down) less
than MIN_FLOOR of the time is mostly toprock with a drop in it: kept as a standing break move (tier 0, "break routine
3 · dancer 4"; Peter, 2026-11-27: more break variety), the rest are floor moves.
"""
import sys, collections
from pathlib import Path
import numpy as np
from aistlib import *
from retarget import FLOOR_CHANNELS, CH, retarget, calibrate_head, frames_from, nrm
import build_moves as bm

GENRE = 'BR'
KEEP_BASIC = 2
REPAIR_SHARE = 0.16
BONE_ERR = 0.15
BAD_SHARE = 0.10
SEAM = 4.0  # beats a loop's end blends into its start (build_moves: 2): getting up from the floor takes longer
MIN_FLOOR = 0.25
TIERS = {0: 'standing', 1: 'floor', 3: 'flips'}
L = np.linalg.norm
BONES = [(LHIP, LKNE), (RHIP, RKNE), (LKNE, LANK), (RKNE, RANK), (LSHO, LELB), (RSHO, RELB), (LELB, LWRI), (RELB, RWRI)]


def bone_error(k):
    """Per frame: the worst bone's length off its median, relative (tracking quality)."""
    return np.max([np.abs(L(k[:, a] - k[:, c], axis=1) / np.median(L(k[:, a] - k[:, c], axis=1)) - 1)
                   for a, c in BONES], 0)


def pelvis_axes(s):
    """World-up components of the pelvis's own up and forward axes per sample (up: 1 upright, 0 lying, -1 upside
    down; forward: -1 face down, 1 face up)."""
    x, y, z, w = (s[:, CH['hipQ' + c]] for c in 'xyzw')
    return 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)


def tier(s):
    """1 floor, 3 flips, 0 none (no floor work in it). Per beat: on the floor when the hips are most of a leg down,
    the pelvis lies past 60 degrees or the upper body touches the floor. Flips: upside down (the pelvis past 120
    degrees) or rolling over (face down to face up) for 2 beats or more; else floor."""
    up, face = pelvis_axes(s)
    n = len(s) // bm.SPB
    flips = floor = 0
    for b in range(n):
        f = slice(b * bm.SPB, (b + 1) * bm.SPB)
        lying = (up[f] < 0.5).mean() > 0.5
        if not ((s[f, CH['hipY']] < -0.45).mean() > 0.5 or lying or (s[f, CH['clear']] < 0.06).mean() > 0.5):
            continue
        floor += 1
        if (up[f] < -0.5).mean() > 0.5 or (lying and np.ptp(face[f]) > 1.4):
            flips += 1
    return 3 if flips >= 2 else 1 if floor else 0


def floor_share(s):
    """How much of a move is on the floor: the upper body within a quarter leg of it, or upside down."""
    up, _ = pelvis_axes(s)
    return float(((s[:, CH['clear']] < 0.25) | (up < -0.5)).mean())


def main():
    root, out = sys.argv[1], Path(sys.argv[2])
    allc = [(p, i) for p, i in clips(root) if i['genre'] == GENRE]
    # The same neutral head pitch as build_moves (its sample of every genre).
    calibrate_head([load_keypoints(p) for p, _ in list(clips(root))[::25]])
    basic = collections.defaultdict(list)
    moves, why = [], collections.Counter()
    for p, info in allc:
        k = load_keypoints(p)
        P = FPS * 60 / info['bpm']
        stand, _ = retarget(k, P)
        chan, body = retarget(k, P, floor=True)
        err = bone_error(k)
        total = int(round(len(k) / P))

        def standing_ok(b0, beats):  # build_moves takes it: not ours
            if (b0 + beats) * P > len(k) + 0.1 * P: return False
            loop = bm.make_loop(stand, P, b0, beats)
            if loop is None: return False
            s = bm.repair(loop[0])
            return s is not None and bm.usable(s)

        def cand(b0, beats):
            why_ = why if beats == total else collections.Counter()
            if (b0 + beats) * P > len(k) + 0.1 * P: return None
            fr = slice(int(b0 * P), int((b0 + beats) * P))
            if np.median(err[fr]) > BONE_ERR or (err[fr] > 0.3).mean() > BAD_SHARE:
                why_['tracking'] += 1
                return None
            loop = bm.make_loop(chan, P, b0, beats, SEAM)
            if loop is None:
                why_['no loop'] += 1
                return None
            s, turns, e, motion = loop
            s = bm.repair(s, REPAIR_SHARE) if np.all(np.isfinite(s)) else None
            if s is None:
                why_['glitches'] += 1
                return None
            t = tier(s)
            if t == 0:
                why_['no floor work'] += 1
                return None
            return dict(samples=s, turns=turns, err=e, motion=motion, b0=b0, beats=beats, info=info, stem=p.stem,
                        tier=t, score=bm.score(e, motion, float(np.median(err[fr])), info['bpm']))

        if info['situation'] == 'BM':
            if standing_ok(0, total): continue
            c = cand(0, total)
            if c: basic[info['ch']].append(c)
            continue
        if standing_ok(0, total): continue
        c = cand(0, total)
        if c:
            moves.append(c)
            continue
        halves = [h for b0 in (0, total // 2) if not standing_ok(b0, total // 2) for h in [cand(b0, total // 2)] if h]
        if halves:
            moves.append(min(halves, key=lambda h: h['score']))
            continue
        # Clean 4-bar stretches with floor work in them, on any bar line: the most daring first, then the best
        # scores, none overlapping.
        quarters = sorted([q for b0 in range(0, total - 15, 4) for q in [cand(b0, 16)] if q],
                          key=lambda q: (-q['tier'], q['score']))
        for q in quarters:
            if all(q['b0'] + 16 <= m['b0'] or m['b0'] + 16 <= q['b0'] for m in moves if m['stem'] == p.stem):
                moves.append(q)
    for ch, cs in sorted(basic.items()):
        cs = sorted(cs, key=lambda c: c['score'])
        near = [c for c in cs if bm.TEMPO[0] <= c['info']['bpm'] <= bm.TEMPO[1]]
        keep = near + [c for c in cs if all(c is not x for x in near)][:max(0, KEEP_BASIC - len(near))]
        for c in keep:
            i = c['info']
            same = sorted([x for x in keep if x['info']['dancer'] == i['dancer']], key=lambda x: x['info']['bpm'])
            take = next(n for n, x in enumerate(same) if x is c) + 1
            c['label'] = (f"break {TIERS[c['tier']]} basic {ch} · dancer {i['dancer']}" +
                          (f" · take {take}" if len(same) > 1 else ''))
            c['kind'] = c['tier'] << 4
    routines = [m for m in moves]
    for c in routines:
        i = c['info']
        part = '' if c['beats'] >= 64 else f" · bars {c['b0'] // 4 + 1}-{(c['b0'] + c['beats']) // 4}"
        c['label'] = f"break {TIERS[c['tier']]} routine {i['ch']} · dancer {i['dancer']}{part}"
        c['kind'] = (c['tier'] << 4) | (1 if c['beats'] >= 32 else 0)
    allm = sorted([c for cs in basic.values() for c in cs if 'kind' in c], key=lambda c: (c['tier'], c['label']))
    allm += sorted(routines, key=lambda c: (c['tier'], c['stem'], c['b0']))
    # Flips left out (Peter, 2026-11-27, docs/dancers-design.md "Break floor work"): the inversions track poorly and
    # our big head and short arms can't do them justice yet.
    for m in allm:
        if m['tier'] == 3: print(f"  flips, left out: {m['label']}")
    allm = [m for m in allm if m['tier'] != 3]
    # Mostly standing (on the floor under MIN_FLOOR of the time): standing break with a drop or some floor work in it,
    # kept for variety (Peter, 2026-11-27) as standing break moves: tier 0, plain labels ("break routine 3 · dancer 4").
    for m in allm:
        m['share'] = floor_share(m['samples'])
        if m['share'] < MIN_FLOOR:
            m['tier'] = 0
            m['label'] = m['label'].replace('break floor ', 'break ')
            m['kind'] &= 1
            print(f"  mostly standing, kept as standing break: {m['label']} ({m['share'] * 100:.0f} % on the floor)")
    allm = [m for m in allm if m['tier'] != 0] + [m for m in allm if m['tier'] == 0]
    bm.write(out, allm, FLOOR_CHANNELS, 'aistfloor')  # (ids apart from aist.moves': a routine may be in both)
    by = collections.Counter((TIERS[m['tier']], 'routine' if m['kind'] & 1 else 'basic', m['beats'] // 4) for m in allm)
    for k, v in sorted(by.items()): print(f'{k[0]:6s} {k[1]:7s} {k[2]:2d} bars: {v}')
    print('left out:', dict(why))
    print(f'{len(allm)} moves, closure error median {np.median([m["err"] for m in allm]):.2f}; '
          f'{out.stat().st_size / 1e3:.0f} KB')
    for m in allm: print(f"  {m['label']} ({m['info']['bpm']} BPM, err {m['err']:.2f}, {m['share'] * 100:.0f} % on the floor)")


if __name__ == '__main__':
    main()

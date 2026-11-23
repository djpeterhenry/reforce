"""Build reforce dance moves from AIST++ (tools/aist_import, docs/dancers-design.md "Mocap moves").

    tools/aist_import/run build_moves.py ~/reforce/aist/data content/Moves/aist.moves

AIST++ clips are cut to the music: a basic clip (sBM) is exactly 16 beats, an advanced one (sFM) 64, from frame 0 on
the beat, at the music's tempo (80-130 BPM in steps of 10, house 110-135 in steps of 5, from the music ID). So the
beat grid is known without the music.

A move is a whole clip, sampled SPB times a beat and made to loop: its last SEAM beats blend into its start, and its
travel and turning are evened out over the whole loop (a whole turn of the body counts as none). A basic move is its 16 beats
(4 bars: one step, repeated); a routine its 64 (16 bars, a phrase). A routine that can't be used whole (floor work
or a tracking glitch somewhere) gives its better clean half (8 bars) instead.

Per genre the importer keeps, of each of the 10 basic choreographies, every usable performance recorded at 120-130
BPM (different dancers do the same step differently: variety, now that the dancers use only these), at least
KEEP_BASIC (the best of the rest make up the number), and every usable routine. "Best": recorded near the tempo Peter plays (120-130 BPM; further off costs TEMPO_COST per BPM, so a
step recorded at 80 isn't sped up by half), it loops closely, the tracking is steady, and it moves.

File format (little-endian), read by engine/DanceClips:
  "RFMOVES2", u32 channel count C, C channel names (u8 length + bytes), C bytes: bits per channel (8 or 16), u32 move
  count, then per move: u8 length + id, u8 length + label, 2 bytes genre code, u8 kind (0 basic, 1 routine), u16
  source BPM, u16 beats, u16 samples per beat, i16 whole body turns over the loop, f32 x 2C (min, max per channel),
  then beats * spb samples, each C values (channel fastest): u8 (8 bits) or u16 (16), quantized over min..max.
  8 bits is plenty for a channel within one move (a step of 1/255 of its range: about 2 mm on a character or half a
  degree, smoothed by the interpolation); the body's heading has 16 (spins cover several turns).
"""
import struct, sys, collections
from pathlib import Path
import numpy as np
from scipy.ndimage import gaussian_filter1d
from aistlib import *
from retarget import CHANNELS, CH, ANGLES, retarget, calibrate_head

SPB = 16          # samples per beat
KEEP_BASIC = 2    # performances kept per basic choreography
TRAVEL_RADIUS = 0.75  # leg lengths: how far a move may take the body from the dancer's spot
SEAM = 2.0         # beats over which a loop's end blends into its start (at most an 8th of the loop)
TEMPO = (120, 130)  # Peter's usual tempo range (BPM)
TEMPO_COST = 0.03   # score per BPM outside it (80 BPM: 1.2, about a typical loop-closure error)
TRAVEL = 1.0      # how much of the source's travel across the floor to keep (1: as the feet, so planted feet stay put)


def sample(chan, frames):
    """Channels (N, C) at fractional frames (linear)."""
    i0 = np.clip(np.floor(frames).astype(int), 0, len(chan) - 2)
    t = np.clip(frames - i0, 0, 1)[:, None]
    return chan[i0] * (1 - t) + chan[i0 + 1] * t


# How much each channel counts in the loop-closure score (rough scale of a noticeable difference).
SCALE = np.ones(len(CHANNELS))
for n in CHANNELS:
    if n in ANGLES: SCALE[CH[n]] = 0.15
    if n.startswith(('hand', 'foot')) and n[-1] in 'uvwyz': SCALE[CH[n]] = 0.1
    if n.startswith(('elbow', 'knee')): SCALE[CH[n]] = 0.5
    if n in ('bodyX', 'bodyZ', 'hipX', 'hipY', 'hipZ'): SCALE[CH[n]] = 0.05


def make_loop(chan, P, b0, beats):
    """`beats` beats from beat b0 as a loop: (samples (beats*SPB, C), body turns, closure error, motion)."""
    n = beats * SPB
    fr = (b0 + np.arange(n + 1) / SPB) * P
    s = sample(chan, fr)
    # The body: relative to its mean heading and spot over the loop.
    yaw = s[:, CH['bodyYaw']]
    turns = int(np.round((yaw[-1] - yaw[0]) / (2 * np.pi)))
    mean_yaw = float(np.mean(yaw[:-1]))
    d = s[:, [CH['bodyX'], CH['bodyZ']]] - s[:-1, [CH['bodyX'], CH['bodyZ']]].mean(0)
    c, si = np.cos(mean_yaw), np.sin(mean_yaw)
    # local = Ry(mean)^T world: x' = c x - s z, z' = s x + c z
    s[:, CH['bodyX']] = TRAVEL * (c * d[:, 0] - si * d[:, 1])
    s[:, CH['bodyZ']] = TRAVEL * (si * d[:, 0] + c * d[:, 1])
    s[:, CH['bodyYaw']] = yaw - mean_yaw
    # Close the loop: the end minus the start (less whole turns). Travel and turning (drift) come out as a ramp over
    # the whole loop; everything else is left as recorded and only the last SEAM beats blend into the start.
    res = s[-1] - s[0]
    res[CH['bodyYaw']] -= 2 * np.pi * turns
    for name in ANGLES:  # joint angles close the short way round; a head ending turned the other way: no loop
        if name != 'bodyYaw':
            res[CH[name]] = wrap_angle(res[CH[name]])
            if abs(res[CH[name]]) > np.pi / 2: return None
    t = np.arange(n + 1) / n
    seam = min(SEAM, beats / 8) / beats  # as a share of the loop
    x = np.clip((t - (1 - seam)) / seam, 0, 1)
    blend = x * x * (3 - 2 * x)
    drift = np.zeros(len(CHANNELS), bool)
    drift[[CH['bodyX'], CH['bodyZ'], CH['bodyYaw']]] = True
    s = s - np.where(drift[None, :], t[:, None], blend[:, None]) * res[None, :]
    # Travel kept near the dancer's spot (TRAVEL_RADIUS leg lengths, about a body width): dancers on a crowded stage
    # only avoid each other's spots. What goes further is eased away over a beat or two (the feet glide a little),
    # then held to 1.3 x the radius at most.
    xz = s[:-1, [CH['bodyX'], CH['bodyZ']]]
    xz = xz - xz.mean(0)
    dist = np.linalg.norm(xz, axis=1, keepdims=True)
    excess = xz * np.clip(1 - TRAVEL_RADIUS / np.maximum(dist, 1e-9), 0, None)
    xz = xz - gaussian_filter1d(excess, SPB, axis=0, mode='wrap')
    dist = np.linalg.norm(xz, axis=1, keepdims=True)
    xz = xz * np.minimum(1, 1.3 * TRAVEL_RADIUS / np.maximum(dist, 1e-9))
    s[:-1, CH['bodyX']], s[:-1, CH['bodyZ']] = xz[:, 0], xz[:, 1]
    s[-1, [CH['bodyX'], CH['bodyZ']]] = xz[0]
    # Hands at most a little past an arm from their shoulder (the blend can push a straight arm out).
    for side in 'LR':
        idx = [CH['hand' + side + c] for c in 'uvw']
        ln = np.linalg.norm(s[:, idx], axis=1, keepdims=True)
        s[:, idx] *= np.minimum(1, 1.15 / np.maximum(ln, 1e-9))
    err = float(np.sqrt(np.mean((res / SCALE) ** 2)))
    motion = float(np.mean(np.abs(np.diff(s[:-1], axis=0)) / SCALE) * SPB)
    return s[:-1], turns, err, motion


POS = [CH[n] for n in CHANNELS if n.startswith(('hand', 'foot')) and n[-1] in 'uvwyz'] + \
      [CH[n] for n in ('hipX', 'hipY', 'hipZ')]
ANG = [CH[n] for n in ANGLES if n != 'bodyYaw']


def to_lengths(s):
    """Feet's across coordinates back to leg lengths (typical stance half-width), for measuring jumps."""
    r = s.copy()
    for n in ('footLu', 'footRu'):
        u = r[:, CH[n]]
        r[:, CH[n]] = np.sign(u) * np.where(np.abs(u) <= 1, np.abs(u) * 0.17, 0.17 + np.abs(u) - 1)
    return r


def jumps(s):
    """Per sample step: True where a hand, foot or hip jumps 0.6 limb lengths or an angle 0.8 rad (a tracking
    glitch: faster than anyone moves in a 16th of a beat)."""
    r = to_lengths(s)
    d = np.abs(np.diff(r, axis=0))
    a = np.abs(wrap_angle(np.diff(s[:, ANG], axis=0)))
    return (d[:, POS].max(1) > 0.6) | (a.max(1) > 0.8)


def wrap_angle(a):
    return (a + np.pi) % (2 * np.pi) - np.pi


def repair(s, max_share=0.04):
    """Short tracking glitches bridged: the samples around each jump (a quarter beat either side) replaced by a
    straight line between the good samples on either side. None when glitches cover more than `max_share`."""
    bad = np.zeros(len(s), bool)
    for i in np.nonzero(jumps(s))[0]:
        bad[max(0, i - SPB // 4): i + SPB // 4 + 2] = True
    if not bad.any(): return s
    if bad.mean() > max_share or bad[0] or bad[-1]: return None
    out = s.copy()
    good = np.nonzero(~bad)[0]
    for c in range(s.shape[1]):
        v = s[:, c] if CHANNELS[c] not in ANGLES or CHANNELS[c] == 'bodyYaw' else np.unwrap(s[:, c])
        out[:, c] = np.interp(np.arange(len(s)), good, v[good])
        if CHANNELS[c] in ANGLES and CHANNELS[c] != 'bodyYaw': out[:, c] = wrap_angle(out[:, c])
    return out


def usable(s):
    """Standing dance our characters can do: no floor work (hips most of a leg down: freezes, drops, splits),
    nothing upside down or lying down, and no tracking glitches (a hand, foot or angle jumping between samples)."""
    if s[:, CH['hipY']].min() < -0.45: return False
    if np.abs(s[:, [CH['chestPitch'], CH['chestRoll'], CH['hipPitch'], CH['hipRoll']]]).max() > 1.2: return False
    return not jumps(s).any()


def tempo_cost(bpm):
    return TEMPO_COST * max(0, TEMPO[0] - bpm, bpm - TEMPO[1])


def score(err, motion, wobble, bpm):
    return err + 30 * wobble - 0.15 * min(motion, 6.0) + tempo_cost(bpm)


def main():
    root, out = sys.argv[1], Path(sys.argv[2])
    only = set(sys.argv[3].split(',')) if len(sys.argv) > 3 else None
    allc = [(p, i) for p, i in clips(root) if not only or i['genre'] in only]
    calibrate_head([load_keypoints(p) for p, _ in allc[::25]])
    basic = collections.defaultdict(list)   # (genre, ch) -> candidates
    adv = []
    for n, (p, info) in enumerate(allc):
        k = load_keypoints(p)
        P = FPS * 60 / info['bpm']
        chan, body = retarget(k, P)
        total = int(round(len(k) / P))

        def cand(b0, beats):
            if (b0 + beats) * P > len(k) + 0.1 * P: return None  # (some clips end a frame or two short: held)
            loop = make_loop(chan, P, b0, beats)
            if loop is None: return None
            s, turns, err, motion = loop
            s = repair(s) if np.all(np.isfinite(s)) else None
            if s is None or not usable(s): return None
            return dict(samples=s, turns=turns, err=err, motion=motion, b0=b0, beats=beats, info=info, stem=p.stem,
                        score=score(err, motion, body.wobble, info['bpm']), wobble=body.wobble)
        if info['situation'] == 'BM':
            c = cand(0, total)
            if c: basic[(info['genre'], info['ch'])].append(c)
        else:
            c = cand(0, total)
            if not c:  # the better clean half
                halves = [h for h in (cand(0, total // 2), cand(total // 2, total // 2)) if h]
                c = min(halves, key=lambda h: h['score']) if halves else None
            if c: adv.append(c)
        if n % 100 == 0: print(f'{n}/{len(allc)}', file=sys.stderr)
    moves = []
    for (g, ch), cs in sorted(basic.items()):
        cs = sorted(cs, key=lambda c: c['score'])
        near = [c for c in cs if TEMPO[0] <= c['info']['bpm'] <= TEMPO[1]]
        keep = near + [c for c in cs if all(c is not x for x in near)][:max(0, KEEP_BASIC - len(near))]
        for c in sorted(keep, key=lambda c: (c['info']['dancer'], c['info']['bpm'])):
            i = c['info']
            same = [x for x in sorted(keep, key=lambda x: x['info']['bpm']) if x['info']['dancer'] == i['dancer']]
            take = next(n for n, x in enumerate(same) if x is c) + 1
            c['label'] = f"{GENRES[g]} basic {ch} · dancer {i['dancer']}" + (f" · take {take}" if len(same) > 1 else '')
            c['kind'] = 0
            moves.append(c)
    for c in sorted(adv, key=lambda c: (c['info']['genre'], c['stem'], c['b0'])):
        i = c['info']
        part = '' if c['beats'] >= 64 else f" · bars {c['b0'] // 4 + 1}-{(c['b0'] + c['beats']) // 4}"
        c['label'] = f"{GENRES[i['genre']]} routine {i['ch']} · dancer {i['dancer']}{part}"
        c['kind'] = 1
        moves.append(c)
    write(out, moves)
    by = collections.Counter((GENRES[m['info']['genre']], m['kind'], m['beats'] // 4) for m in moves)
    for k, v in sorted(by.items()): print(f'{k[0]:16s} {"basic" if k[1] == 0 else "routine"} {k[2]:2d} bars: {v}')
    bpms = collections.Counter(m['info']['bpm'] for m in moves)
    print('recorded at:', ', '.join(f'{b} BPM x{n}' for b, n in sorted(bpms.items())))
    errs = np.array([m['err'] for m in moves])
    print(f'{len(moves)} moves, closure error median {np.median(errs):.2f}, 90% {np.percentile(errs, 90):.2f}; '
          f'{out.stat().st_size / 1e6:.1f} MB')


def write(path, moves):
    def s8(x):
        b = x.encode('utf-8')[:255]
        return struct.pack('<B', len(b)) + b
    buf = bytearray(b'RFMOVES2')
    buf += struct.pack('<I', len(CHANNELS))
    for n in CHANNELS: buf += s8(n)
    bits = [16 if n == 'bodyYaw' else 8 for n in CHANNELS]
    buf += bytes(bits)
    buf += struct.pack('<I', len(moves))
    for m in moves:
        i, s = m['info'], m['samples'].astype(np.float64)
        buf += s8(f"aist:{m['stem']}@{m['b0']}") + s8(m['label']) + i['genre'].encode()
        buf += struct.pack('<BHHHh', m['kind'], i['bpm'], m['beats'], SPB, m['turns'])
        lo, hi = s.min(0), s.max(0)
        hi = np.where(hi - lo < 1e-6, lo + 1e-6, hi)
        for a, b in zip(lo, hi): buf += struct.pack('<ff', a, b)
        rows = []
        for c, b in enumerate(bits):
            top = (1 << b) - 1
            q = np.round((s[:, c] - lo[c]) / (hi[c] - lo[c]) * top).clip(0, top)
            rows.append(q.astype('<u2' if b == 16 else 'u1'))
        rec = np.zeros(len(s), dtype=[(f'c{c}', r.dtype) for c, r in enumerate(rows)])
        for c, r in enumerate(rows): rec[f'c{c}'] = r
        buf += rec.tobytes()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(buf))


if __name__ == '__main__':
    main()

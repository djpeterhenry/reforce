"""The web sample of mocap moves for the /dance page's preview (tools/dancer_web): per dance style, 3 basic moves and
1 routine, picked from reforce's moves file (Peter, 2026-11-21: "3 basics and 1 routine is good").

    tools/aist_import/run web_moves.py content/Moves/aist.moves build-web/dancer/moves.bin

Picked to show the style well: recorded at 120-130 BPM where there are such, the most movement, basic moves from
different choreographies (and, for hip-hop, from both of its genres), the routine a whole 16 bars where
there is one. Same file format as the full one (build_moves.write).
"""
import re, sys
from pathlib import Path
import numpy as np
import inspect_moves
from build_moves import write
from retarget import CHANNELS

# AIST genre -> reforce dance style (engine/DanceClips kGenres).
STYLE = {'HO': 'house', 'PO': 'pop', 'LO': 'lock', 'BR': 'break', 'WA': 'disco', 'KR': 'stomp', 'MH': 'hiphop',
         'LH': 'hiphop', 'JS': 'jazz', 'JB': 'ballet'}
ORDER = ['house', 'pop', 'lock', 'break', 'disco', 'stomp', 'hiphop', 'jazz', 'ballet']
BASICS, ROUTINES = 3, 1


def motion(m, ch):
    """Average speed of the hands and feet (per sample), the bigger the more there is to see."""
    idx = [ch[n] for n in CHANNELS if n.startswith(('hand', 'foot')) and n[-1] in 'uvwyz']
    return float(np.mean(np.abs(np.diff(m['v'][:, idx], axis=0))))


def in_tempo(m):
    return 120 <= m['bpm'] <= 130


def pick_basics(cands, n):
    """n basic moves: in tempo first, most movement, each from a different choreography, alternating genres."""
    cands = sorted(cands, key=lambda m: (not in_tempo(m), -m['motion']))
    genres = sorted({m['genre'] for m in cands})
    out, used = [], set()
    while len(out) < n:
        added = False
        for g in genres:
            for m in cands:
                key = (m['genre'], m['ch'])
                if m['genre'] == g and key not in used:
                    out.append(m)
                    used.add(key)
                    added = True
                    break
            if len(out) == n: break
        if not added: break
    return out


def pick_routines(cands, n):
    """n routines: whole 16 bars first, in tempo, most movement."""
    return sorted(cands, key=lambda m: (m['beats'] < 64, not in_tempo(m), -m['motion']))[:n]


def main():
    src, out = sys.argv[1], Path(sys.argv[2])
    names, moves = inspect_moves.read(src)
    ch = {n: i for i, n in enumerate(names)}
    assert names == CHANNELS, 'the moves file has other channels than retarget.CHANNELS'
    for m in moves:
        m['motion'] = motion(m, ch)
        m['ch'] = int(re.search(r'_ch(\d+)@', m['id']).group(1))
    picked = []
    for style in ORDER:
        own = [m for m in moves if STYLE[m['genre']] == style]
        basics = pick_basics([m for m in own if m['kind'] == 0], BASICS)
        routines = pick_routines([m for m in own if m['kind'] == 1], ROUTINES)
        picked += basics + routines
        print(f'{style:7s}', ' | '.join(f"{m['label']} ({m['bpm']})" for m in basics + routines))
    # In build_moves' form for its writer.
    rows = []
    for m in picked:
        stem, b0 = m['id'][len('aist:'):].rsplit('@', 1)
        rows.append(dict(info=dict(genre=m['genre'], bpm=m['bpm']), stem=stem, b0=int(b0), label=m['label'],
                         kind=m['kind'], beats=m['beats'], samples=m['v'], turns=m['turns']))
    write(out, rows)
    print(f'{len(rows)} moves, {out.stat().st_size / 1e3:.0f} KB -> {out}')


if __name__ == '__main__':
    main()

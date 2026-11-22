"""AIST++ loading and beat helpers (tools/aist_import)."""
import pickle, re
from pathlib import Path
import numpy as np

# AIST genre codes -> the names reforce uses (its dance styles since 2026-11-21: waack is disco, krump is stomp, middle
# hip-hop is party hip-hop; party and LA hip-hop are the hip-hop style, street jazz is jazz, ballet jazz ballet).
GENRES = {'BR': 'break', 'PO': 'pop', 'LO': 'lock', 'MH': 'party hip-hop', 'LH': 'LA hip-hop', 'HO': 'house',
          'WA': 'disco', 'KR': 'stomp', 'JS': 'street jazz', 'JB': 'ballet jazz'}
FPS = 60.0
# COCO-17 joints
NOSE, LEYE, REYE, LEAR, REAR, LSHO, RSHO, LELB, RELB, LWRI, RWRI, LHIP, RHIP, LKNE, RKNE, LANK, RANK = range(17)


class _Safe(pickle.Unpickler):
    """Only numpy arrays: the pickles are downloaded data, never code."""
    def find_class(self, m, n):
        if m in ('numpy.core.multiarray', 'numpy._core.multiarray', 'numpy', 'numpy.core.numeric',
                 'numpy._core.numeric') and n in ('_reconstruct', 'ndarray', 'dtype', 'scalar', '_frombuffer'):
            import importlib
            return getattr(importlib.import_module(m), n)
        raise pickle.UnpicklingError(f'blocked {m}.{n}')


def load_keypoints(path):
    with open(path, 'rb') as f:
        d = _Safe(f).load()
    return np.asarray(d['keypoints3d_optim'], dtype=np.float64)


NAME = re.compile(r'g(?P<g>[A-Z]{2})_s(?P<s>[A-Z]{2})_c\w+?_d(?P<d>\d+)_m(?P<mg>[A-Z]{2})(?P<m>\d)_ch(?P<ch>\d+)')


def parse_name(stem):
    m = NAME.match(stem)
    if not m:
        return None
    g, mi = m['g'], int(m['m'])
    bpm = (110 + 5 * mi) if m['mg'] == 'HO' else (80 + 10 * mi)
    return dict(genre=g, situation=m['s'], dancer=int(m['d']), music=f"m{m['mg']}{mi}", ch=int(m['ch']), bpm=bpm)


def clips(root):
    for p in sorted(Path(root, 'keypoints3d').glob('*.pkl')):
        info = parse_name(p.stem)
        if info:
            yield p, info

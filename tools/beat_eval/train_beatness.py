#!/usr/bin/env python3.11
"""Fit the on-beat probability of an onset ("beatness") from its band features, on Peter's library.

Input: NAME.onsets.csv from `beat_eval.py --dump-onsets` (tuning-split songs only): per onset its phase in the hand
grid (0 = on a beat), kick-ness, strength and 9 band features of the onset's hop and of the next hop
(BeatTracker::lastOnsetBands / lastOnsetBandsNext). Label: on the beat =
phase within 0.1 beat of a whole beat. Logistic regression (Newton / IRLS, numpy), checked on 20 % of the songs held
out by name. Prints the C++ initialisers for BeatConfig::beatnessW / beatnessWNext / beatnessBias.

  python3.11 -I tools/beat_eval/train_beatness.py build/beat-eval/train1500.onsets.csv
"""

import hashlib
import sys

import numpy as np

np.seterr(all="ignore")  # Accelerate's matmul raises spurious warnings


def load(path):
    """Columns after the id (which may contain commas) come from the header: phase, kickness, strength, t, kick,
    then band features (b*, n*, and h* when present)."""
    ids, rows = [], []
    with open(path, encoding="utf-8") as f:
        cols = f.readline().rstrip("\n").split(",")[1:]
        for line in f:
            parts = line.rstrip("\n").rsplit(",", len(cols))
            if len(parts) != len(cols) + 1:
                continue
            ids.append(parts[0])
            rows.append([float(x) for x in parts[1:]])
    a = np.array(rows)
    c = {n: i for i, n in enumerate(cols)}
    feats = [i for n, i in c.items() if n[0] in "bnh" and n[1:].isdigit()]
    return np.array(ids), a[:, c["phase"]], a[:, c["kickness"]], a[:, c["strength"]], a[:, c["kick"]], a[:, feats]


def fit(X, y, l2=1e-3, iters=25):
    Xb = np.hstack([X, np.ones((len(X), 1))])
    w = np.zeros(Xb.shape[1])
    for _ in range(iters):
        p = 1 / (1 + np.exp(-Xb @ w))
        g = Xb.T @ (p - y) + l2 * w
        H = (Xb * (p * (1 - p))[:, None]).T @ Xb + l2 * np.eye(len(w))
        w -= np.linalg.solve(H, g)
    return w


def auc(score, y):
    order = np.argsort(score)
    ranks = np.empty(len(score))
    ranks[order] = np.arange(1, len(score) + 1)
    pos = y == 1
    return (ranks[pos].sum() - pos.sum() * (pos.sum() + 1) / 2) / (pos.sum() * (~pos).sum())


def main():
    ids, phase, kick, strength, kickOn, bands = load(sys.argv[1])
    if "--kick-only" in sys.argv:  # train on onsets while a kick plays (not breakdowns, intros, outros)
        keep = kickOn > 0.5
        print("kick sections only: %d of %d onsets" % (keep.sum(), len(keep)))
        ids, phase, kick, strength, kickOn, bands = ids[keep], phase[keep], kick[keep], strength[keep], kickOn[keep], bands[keep]
    d = np.minimum(phase, 1 - phase)
    y = (d < 0.1).astype(float)
    hold = np.array([hashlib.sha1(i.encode()).digest()[1] % 5 == 0 for i in ids])
    print("%d onsets, %.1f%% on the beat; %d songs" % (len(y), 100 * y.mean(), len(set(ids))))
    feats = {
        "this hop": bands[:, :9],
        "this + next hop": bands[:, :18],
    }
    if bands.shape[1] >= 27:
        feats["+ half a beat before"] = bands[:, :27]
    for name, X in feats.items():
        w = fit(X[~hold], y[~hold])
        Xh = np.hstack([X[hold], np.ones((hold.sum(), 1))])
        a = auc(Xh @ w, y[hold])
        print("%-12s held-out AUC %.3f" % (name, a))

    print("kick-ness alone: held-out AUC %.3f" % auc(kick[hold], y[hold]))
    w = fit(bands, y)  # final: all songs
    print("\n    std::array<double, 9> beatnessW{%s};" % ", ".join("%.4f" % v for v in w[:9]))
    print("    std::array<double, 9> beatnessWNext{%s};" % ", ".join("%.4f" % v for v in w[9:18]))
    if bands.shape[1] >= 27:
        print("    std::array<double, 9> beatnessWHalf{%s};" % ", ".join("%.4f" % v for v in w[18:27]))
    print("    double beatnessBias = %.4f;" % w[-1])


if __name__ == "__main__":
    main()

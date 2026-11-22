#!/usr/bin/env python3.11
"""Fit the beat fold's score ("which half is the beat", BeatConfig::foldW) on Peter's library.

Input: NAME.folds.bin + NAME.tsv from `beat_eval.py --dump-folds` (tuning-split songs): every second while TEMPO is
right, the tracker's fold (16 bins per beat x 18 features: band flux, band level) and where the grid's beat sits on
the fold's circle. Model: a linear score of the normalised profile rotated so that a candidate beat is at bin 0;
softmax over the 16 rotations, the true one is the target (conditional logit, Adam). Checked on 20 % of the songs
held out by name. Prints the C++ initialiser for BeatConfig::foldW.

  python3.11 -I tools/beat_eval/train_fold.py build/beat-eval/fold1k
"""

import hashlib
import sys

import numpy as np

np.seterr(all="ignore")
B, F, NB = 16, 18, 9


def load(prefix):
    dt = np.dtype([("i", "<i4"), ("t", "<f4"), ("kick", "<f4"), ("theta", "<f4"), ("ratio", "<f4"), ("p", "<f4", (B, F))])
    rec = np.fromfile(prefix + ".folds.bin", dtype=dt)
    ids = [l.split("\t")[0] for l in open(prefix + ".tsv", encoding="utf-8")]
    return rec, np.array(ids)


def normalise(p):
    """As BeatTracker::scoreFold: per feature minus its mean over the bins; each group divided by its RMS."""
    p = p - p.mean(axis=1, keepdims=True)
    for g in (slice(0, NB), slice(NB, F)):
        rms = np.sqrt((p[:, :, g] ** 2).mean(axis=(1, 2), keepdims=True))
        p[:, :, g] /= rms + 1e-6
    return p


def rotations(p, theta):
    """(N, B, B, F): rotation k has the candidate beat theta + k / B at bin 0 (linear interpolation)."""
    n = len(p)
    pos = theta * B - 0.5
    j = pos[:, None, None] + np.arange(B)[None, :, None] + np.arange(B)[None, None, :]  # (N, k, b)
    j0 = np.floor(j).astype(int)
    fr = (j - j0)[..., None]
    rows = np.arange(n)[:, None, None]
    return (1 - fr) * p[rows, j0 % B] + fr * p[rows, (j0 + 1) % B]


def scores(X, W):
    return np.einsum("nkbf,bf->nk", X, W)


def train(p, theta, l2=1e-3, epochs=6, batch=2048, lr=0.02, seed=1):
    rng = np.random.default_rng(seed)
    W = np.zeros((B, F))
    m, v = np.zeros_like(W), np.zeros_like(W)
    step = 0
    for ep in range(epochs):
        order = rng.permutation(len(p))
        for s in range(0, len(p), batch):
            idx = order[s:s + batch]
            X = rotations(p[idx], theta[idx])
            z = scores(X, W)
            z -= z.max(axis=1, keepdims=True)
            q = np.exp(z)
            q /= q.sum(axis=1, keepdims=True)
            q[:, 0] -= 1.0  # d loss / d score
            g = np.einsum("nk,nkbf->bf", q, X) / len(idx) + l2 * W
            step += 1
            m = 0.9 * m + 0.1 * g
            v = 0.999 * v + 0.001 * g * g
            W -= lr / (1 + ep) * (m / (1 - 0.9 ** step)) / (np.sqrt(v / (1 - 0.999 ** step)) + 1e-8)
    return W


def evaluate(p, theta, W, label):
    hits = half = 0
    confs, rights = [], []
    for s in range(0, len(p), 4096):
        X = rotations(p[s:s + 4096], theta[s:s + 4096])
        z = scores(X, W)
        best = z.argmax(axis=1)
        d = np.minimum(best, B - best)  # bins from the truth
        hits += (d <= 1).sum()
        right = (d < B // 4) | ((d == B // 4) & (np.arange(len(d)) % 2 == 0))
        half += right.sum()
        e = np.exp(z - z.max(axis=1, keepdims=True))
        kk = np.arange(B)
        dist = np.minimum((kk[None, :] - best[:, None]) % B, (best[:, None] - kk[None, :]) % B)
        same = (e * np.where(dist < B // 4, 1.0, np.where(dist == B // 4, 0.5, 0.0))).sum(1) / e.sum(1)
        confs.append(2 * same - 1)
        rights.append(right)
    c, r = np.concatenate(confs), np.concatenate(rights)
    line = "%-28s beat within 1/16: %.1f%%  right half: %.1f%%" % (label, 100 * hits / len(p), 100 * half / len(p))
    for th in (0.5, 0.8):
        k = c >= th
        if k.any():
            line += "  | conf>=%.1f: %.0f%% of the time, right half %.1f%%" % (th, 100 * k.mean(), 100 * r[k].mean())
    print(line)


def main():
    rec, ids = load(sys.argv[1])
    ok = (rec["p"][:, :, :].reshape(len(rec), -1) != 0).any(1) & (np.abs(rec["ratio"] - 1) < 0.02)
    if "--all" not in sys.argv:
        ok &= rec["kick"] > 0.5
    rec = rec[ok]
    names = ids[rec["i"]]
    hold = np.array([hashlib.sha1(n.encode()).digest()[1] % 5 == 0 for n in names])
    p = normalise(rec["p"].astype(np.float64))
    theta = rec["theta"].astype(np.float64)
    print("%d folds from %d songs (%d held out)" % (len(p), len(set(names)), hold.sum()))
    # Baselines: the strongest kick-band (30-120 Hz) flux, and the strongest overall flux, at the beat.
    Wk = np.zeros((B, F)); Wk[0, 1] = Wk[0, 2] = 1
    evaluate(p[hold], theta[hold], Wk, "baseline: kick flux peak")
    Wa = np.zeros((B, F)); Wa[0, :NB] = 1
    evaluate(p[hold], theta[hold], Wa, "baseline: all flux peak")
    W = train(p[~hold], theta[~hold])
    evaluate(p[hold], theta[hold], W, "learnt (held out)")
    evaluate(p[~hold][:50000], theta[~hold][:50000], W, "learnt (training)")
    W = train(p, theta)
    print("\n    std::array<double, kFoldBins * kFoldFeat> foldW{%s};" % ", ".join("%.4f" % x for x in W.ravel()))


if __name__ == "__main__":
    main()

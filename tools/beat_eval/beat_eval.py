#!/usr/bin/env python3
"""Score reforce's audio tempo tracker and beat clock against Peter's hand-made beat grids.

The grids are the Ableton warp markers in his DJ library (djtools: Songs/database.json, one record per song with
the clip's warp markers and its audio file). The library is only read: database.json is loaded, the audio files are
opened read-only by reforce-beat-eval. Everything this writes goes to --out (default build/beat-eval).

  python3 -I tools/beat_eval/beat_eval.py --sample 200                    # baseline on 200 tuning songs
  python3 -I tools/beat_eval/beat_eval.py --sample 200 --split test       # held-out songs
  python3 -I tools/beat_eval/beat_eval.py --sample 200 --set beat.switchSeconds=12
  python3 -I tools/beat_eval/beat_eval.py --recent 2000 --split all --mixes 400   # simulated DJ mixes

Untrusted grids (never-warped clips, acapellas, marker tempo != record BPM) are skipped unless --all-grids.

Split: a song is "test" when the first byte of sha1(id) % 5 == 0 (~20 %), else "train"; tune on train only.
"""

import argparse
import hashlib
import json
import math
import os
import random
import re
import subprocess
import sys

# The music library: a djtools Songs folder (database.json). No default: --songs DIR or REFORCE_SONGS.
DEFAULT_SONGS = os.path.expanduser(os.environ.get("REFORCE_SONGS", ""))
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def split_of(song_id):
    return "test" if hashlib.sha1(song_id.encode()).digest()[0] % 5 == 0 else "train"


def untrusted(song_id, rec, marks):
    """Why a grid can't be scored against (or ''): the clip was never warped (name says unwarped / no warp, or the
    record has no BPM: Ableton's default markers), it is an acapella (no beat), or the markers' tempo disagrees with
    the record's BPM (not an octave apart). 2026-11-16: 3.5 % of the library, a third of the held-out tempo misses."""
    if re.search(r"unwarp|no warp|a ?capp?ella", song_id, re.I):
        return "name"
    bpm = rec.get("bpm") or 0
    if not bpm:
        return "no bpm"
    (s0, b0), (s1, b1) = marks[0], marks[1]
    ratio = 60.0 * (b1 - b0) / (s1 - s0) / bpm
    return "disagree" if abs(ratio / 2 ** round(math.log2(ratio)) - 1) >= 0.01 else ""


def load_songs(songs_dir, split, tags, bpm_lo, bpm_hi, all_grids=False):
    with open(os.path.join(songs_dir, "database.json"), "r", encoding="utf-8") as f:
        db = json.load(f)
    out = []
    for song_id, rec in sorted(db.items()):
        clip = rec.get("clip")
        if not clip or len(clip.get("warp_markers", [])) < 2:
            continue
        if split != "all" and split_of(song_id) != split:
            continue
        if tags and not set(tags) & set(rec.get("tags", [])):
            continue
        bpm = rec.get("bpm") or 0
        if bpm and not (bpm_lo <= bpm <= bpm_hi):
            continue
        path = os.path.normpath(os.path.join(songs_dir, clip["sample"]))
        if not os.path.isfile(path):
            continue
        marks = sorted((m["sec_time"], m["beat_time"]) for m in clip["warp_markers"])
        if not all_grids and untrusted(song_id, rec, marks):
            continue
        out.append((song_id, path, marks, max(rec.get("ts_list") or [0.0])))
    return out


def weighted(results, key):
    tot = sum(r["seconds"] for r in results) or 1.0
    a, b = key.split(".")
    return sum(r[a][b] * r["seconds"] for r in results) / tot


def song_fields(song, mp3_offset_ms):
    song_id, path, marks, _ = song
    shift = mp3_offset_ms / 1000.0 if path.lower().endswith(".mp3") else 0.0
    return "%s\t%s\t%s" % (song_id.replace("\t", " "), path, ",".join("%.6f:%.6f" % (s - shift, b) for s, b in marks))


def marker_bpm(song):
    (s0, b0), (s1, b1) = song[2][0], song[2][1]
    return 60.0 * (b1 - b0) / (s1 - s0)


def run_mixes(a, songs, out_dir):
    rng = random.Random(a.seed)
    pairs = []
    for i in range(a.mixes * 20):
        if len(pairs) >= a.mixes:
            break
        A, B = rng.choice(songs), rng.choice(songs)
        r = marker_bpm(A) / marker_bpm(B)
        r /= 2 ** round(math.log2(r))
        if A is not B and abs(r - 1) < 0.06 and abs(math.log2(marker_bpm(A) / marker_bpm(B))) < 0.5:
            pairs.append((A, B, "nudge:%.2f:%.1f" % (rng.choice([-1, 1]) * rng.uniform(1, 3), rng.uniform(3, 10))
                          if a.nudge else str(len(pairs) % 2)))
    if a.nudge and a.nudge_at != "30":  # where the glide starts (s after the blend): 30 = the old fixed place
        rng2 = random.Random(a.seed + 1)  # (its own stream: the pairs stay the same)
        pairs = [(A, B, r + ":%.1f" % (rng2.uniform(10, 80) if a.nudge_at == "random" else float(a.nudge_at)))
                 for A, B, r in pairs]
    manifest = os.path.join(out_dir, a.name + ".mix.tsv")
    with open(manifest, "w", encoding="utf-8") as f:
        for A, B, ride in pairs:
            f.write("%s\t%s\t%s\n" % (song_fields(A, a.mp3_offset_ms), song_fields(B, a.mp3_offset_ms), ride))
    cmd = [a.binary, "--mix", "--blend-beats", str(a.blend_beats)] + (["--oracle-tempo"] if a.oracle_tempo else [])
    if a.jobs:
        cmd += ["--jobs", str(a.jobs)]
    for s in a.set:
        cmd += ["--set", s]
    cmd.append(manifest)
    print("%d mixes, %s" % (len(pairs), " ".join(a.set) or "defaults"), file=sys.stderr)
    res = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    with open(os.path.join(out_dir, a.name + ".mix.jsonl"), "w", encoding="utf-8") as f:
        f.write(res)
    results = [json.loads(l) for l in res.splitlines() if l.strip()]
    errors = [r for r in results if "error" in r]
    results = [r for r in results if "error" not in r]
    print("%d mixes scored, %d skipped (%s)" % (len(results), len(errors), ", ".join(sorted(set(e["error"] for e in errors)))))
    names = [("a", "A alone (last minute)"), ("blend1", "blend, 1st half (B fading in, A's bass)"),
             ("blend2", "blend, 2nd half (B's bass, A fading)"), ("b30", "B, first 30 s after"),
             ("b90", "B, 30-90 s after"), ("ride", "B: tempo nudge (glide + 40 s)" if a.nudge else "B while the pitch rides to its tempo + 30 s")]
    for key, label in names:
        seg = [r["segs"][key] for r in results]
        n = sum(s["n"] for s in seg)
        if n:
            print("  %-46s TEMPO right %5.1f%%  on the beat %5.1f%%  off-beat %4.1f%%  locked %5.1f%%" % (
                label, 100 * sum(s["ok"] for s in seg) / n, 100 * sum(s["on"] for s in seg) / n,
                100 * sum(s["off"] for s in seg) / n, 100 * sum(s["locked"] for s in seg) / n))
    st = sorted(r["settle_s"] for r in results if not r["ride"])
    if a.nudge:
        print("on the beat for good after the nudge's glide (-1 = throughout): median %.1f s, 75%% %.1f s, 90%% %.1f s"
              % tuple(st[min(len(st) - 1, int(f * len(st)))] for f in (0.5, 0.75, 0.9)))
        st = []
    if st:
        q = lambda f: st[min(len(st) - 1, int(f * len(st)))]
        print("on the beat for good after the blend (no ride; -1 = throughout): median %.1f s, 75%% %.1f s, 90%% %.1f s" % (
            q(0.5), q(0.75), q(0.9)))
    if a.worst:
        print("worst mixes (B's first 30 s on the beat):")
        for r in sorted(results, key=lambda r: r["segs"]["b30"]["on"] / max(1, r["segs"]["b30"]["n"]))[:a.worst]:
            s = r["segs"]["b30"]
            print("  %5.1f%%  %6.1f -> %6.1f BPM  %s  ->  %s" % (100 * s["on"] / max(1, s["n"]), r["tempo_a"], r["tempo_b"],
                                                          r["a"][:45], r["b"][:45]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--songs", default=DEFAULT_SONGS, help="djtools Songs folder (database.json); default $REFORCE_SONGS")
    ap.add_argument("--sample", type=int, default=200, help="random songs (0 = all)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--recent", type=int, default=0,
                    help="the N most recently active songs (latest play timestamp, djtools ts_list) instead of --sample")
    ap.add_argument("--split", choices=["train", "test", "all"], default="train")
    ap.add_argument("--tag", action="append", default=[], help="only songs with this tag (repeatable)")
    ap.add_argument("--bpm", default="0-1000", help="record bpm range, e.g. 115-135")
    ap.add_argument("--max-seconds", type=float, default=600)
    ap.add_argument("--mp3-offset-ms", type=float, default=25.0,
                    help="MP3 grids sit this much later than the decoded audio (encoder padding: Ableton vs Apple)")
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--set", action="append", default=[], help="parameter override NAME=VALUE (see beat_eval.cpp)")
    ap.add_argument("--binary", default=os.path.join(ROOT, "build", "reforce-beat-eval"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "beat-eval"))
    ap.add_argument("--name", default="run", help="output file prefix")
    ap.add_argument("--dump-onsets", action="store_true", help="also write NAME.onsets.csv (features, for training)")
    ap.add_argument("--dump-folds", action="store_true", help="also write NAME.folds.bin (beat folds, train_fold.py)")
    ap.add_argument("--all-grids", action="store_true",
                    help="also score untrusted grids (unwarped clips, acapellas, marker / record BPM mismatch)")
    ap.add_argument("--mixes", type=int, default=0,
                    help="instead: N simulated DJ mixes of the chosen songs (pairs within 6 %% tempo, 16-bar blend with a "
                         "bass swap; every other one rides the pitch back to B's tempo afterwards)")
    ap.add_argument("--blend-beats", type=float, default=64)
    ap.add_argument("--oracle-tempo", action="store_true",
                    help="with --mixes: B's true tempo replaces TEMPO (the ceiling for better tempo tracking)")
    ap.add_argument("--nudge-at", default="random",
                    help="with --nudge: where the glide starts, seconds after the blend, or 'random' (10-80 s; "
                         "30 = the first version, which put most nudges at a section change about 32 bars into B)")
    ap.add_argument("--nudge", action="store_true",
                    help="mixes: instead of rides, 30 s after the blend the tempo moves +-1-3 BPM over 3-10 s and holds "
                         "(Peter's quick master tempo changes)")
    ap.add_argument("--worst", type=int, default=15, help="list this many worst songs")
    a = ap.parse_args()
    if not a.songs:
        sys.exit("Which music library? Pass --songs DIR (a djtools Songs folder) or set REFORCE_SONGS.")

    songs_dir = os.path.realpath(a.songs)
    out_dir = os.path.realpath(a.out)
    if out_dir == songs_dir or out_dir.startswith(songs_dir + os.sep) or out_dir.startswith(
            os.path.dirname(songs_dir) + os.sep):
        sys.exit("--out must be outside the music library")
    os.makedirs(out_dir, exist_ok=True)

    lo, hi = (float(x) for x in a.bpm.split("-"))
    songs = load_songs(songs_dir, a.split, a.tag, lo, hi, a.all_grids)
    if a.recent:
        songs = sorted(songs, key=lambda s: -s[3])[:a.recent]
    elif a.sample and a.sample < len(songs):
        songs = random.Random(a.seed).sample(songs, a.sample)
    if not songs:
        sys.exit("no songs")
    if a.mixes:
        return run_mixes(a, songs, out_dir)
    manifest = os.path.join(out_dir, a.name + ".tsv")
    with open(manifest, "w", encoding="utf-8") as f:
        for song_id, path, marks, _ in songs:
            shift = a.mp3_offset_ms / 1000.0 if path.lower().endswith(".mp3") else 0.0
            f.write("%s\t%s\t%s\n" % (song_id.replace("\t", " "), path,
                                       ",".join("%.6f:%.6f" % (s - shift, b) for s, b in marks)))
    cmd = [a.binary, "--max-seconds", str(a.max_seconds)]
    if a.jobs:
        cmd += ["--jobs", str(a.jobs)]
    for s in a.set:
        cmd += ["--set", s]
    if a.dump_onsets:
        cmd += ["--dump-onsets", os.path.join(out_dir, a.name + ".onsets.csv")]
    if a.dump_folds:
        cmd += ["--dump-folds", os.path.join(out_dir, a.name + ".folds.bin")]
    cmd.append(manifest)
    print("%d songs (%s), %s" % (len(songs), a.split, " ".join(a.set) or "defaults"), file=sys.stderr)
    res = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    results = [json.loads(l) for l in res.splitlines() if l.strip()]
    errors = [r for r in results if "error" in r]
    results = [r for r in results if "error" not in r and r["seconds"] > 30]
    with open(os.path.join(out_dir, a.name + ".jsonl"), "w", encoding="utf-8") as f:
        f.write(res)

    hours = sum(r["seconds"] for r in results) / 3600
    print("%d songs scored (%.1f h), %d unreadable" % (len(results), hours, len(errors)))
    print("TEMPO (time-weighted): ok %.1f%%  0 %.1f%%  x1/2 %.1f%%  x2 %.1f%%  x2/3 %.1f%%  x3/2 %.1f%%  other %.1f%%" % tuple(
        100 * weighted(results, "tempo." + k)
        for k in ["ok", "zero", "half", "double", "two_thirds", "three_halves", "other"]))
    print("tempo within 0.3%%: TEMPO %.1f%%, clock %.1f%%" % (100 * weighted(results, "tempo.fine"),
                                                           100 * weighted(results, "tempo.clock_fine")))
    print("clock: locked %.1f%%  on the beat %.1f%%  off-beat %.1f%%  aligned (song's own offset removed) %.1f%%" % tuple(
        100 * weighted(results, "clock." + k) for k in ["locked", "onbeat", "offbeat", "aligned"]))
    ph = [r["clock"]["phase_ms"] for r in results if r["clock"]["onbeat"] > 0.2]
    if ph:
        ph.sort()
        print("clock phase vs the grid while on the beat (+ = clock early): median %.1f ms, IQR %.1f..%.1f ms" % (
            ph[len(ph) // 2], ph[len(ph) // 4], ph[3 * len(ph) // 4]))
    print("off-beat rescue slides: %.2f per minute" % (sum(r["clock"].get("rescues", 0) for r in results) / (60 * hours)))
    fl = sorted(r["clock"]["first_lock"] for r in results if r["clock"]["first_lock"] >= 0)
    if fl:
        print("first right lock: median %.1f s" % fl[len(fl) // 2])
    per = sorted(r["clock"]["onbeat"] for r in results)
    sp = [r["split"] for r in results]
    k, nk = sum(s["kick"] for s in sp) or 1, sum(s["nokick"] for s in sp) or 1
    print("with a kick (%.0f%% of the time): TEMPO right %.1f%%, on the beat %.1f%%;  without (breakdowns, intros, "
          "outros, %.0f%%): TEMPO right %.1f%%, on the beat %.1f%%" % (
              100 * k / (k + nk), 100 * sum(s["ok_kick"] for s in sp) / k, 100 * sum(s["on_kick"] for s in sp) / k,
              100 * nk / (k + nk), 100 * sum(s["ok_nokick"] for s in sp) / nk, 100 * sum(s["on_nokick"] for s in sp) / nk))
    tests = [r for r in results if split_of(r["id"]) == "test"]
    if a.split == "all" and tests:
        print("  of these, held-out songs (%d): TEMPO right %.1f%%, on the beat %.1f%%" % (
            len(tests), 100 * weighted(tests, "tempo.ok"), 100 * weighted(tests, "clock.onbeat")))
    print("songs on the beat >= 80%% of the time: %.0f%%;  < 30%%: %.0f%%" % (
        100 * sum(x >= 0.8 for x in per) / len(per), 100 * sum(x < 0.3 for x in per) / len(per)))
    if a.worst:
        print("worst (on the beat, tempo ok, main tempo error):")
        for r in sorted(results, key=lambda r: r["clock"]["onbeat"])[:a.worst]:
            t = r["tempo"]
            err = max(["zero", "half", "double", "two_thirds", "three_halves", "other"], key=lambda k: t[k])
            print("  %5.1f%%  %5.1f%%  %-12s %6.1f BPM  %s" % (100 * r["clock"]["onbeat"], 100 * t["ok"], err,
                                                            r["bpm"], r["id"]))


if __name__ == "__main__":
    main()

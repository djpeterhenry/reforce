#!/usr/bin/env python3
"""Phase 5B.4 corpus sweep: validate the kit without G-Force (reforce vs reforce).

    python3 -I tools/sweep/validate.py [--reforce PATH] [--only same,fps55,...] [--no-docs]

Renders variants of the reforce replay into build/sweep/validate/<variant>/ and compares each with the
baseline build/sweep/reforce/ (render_reforce.py) using compare.py's metrics and thresholds, as if the
variant were G-Force:
  same      identical re-render                                -> everything must pass (determinism)
  fps55     55.6 fps instead of 60 (G-Force's real rate here),   -> must pass: the steady-state design makes
            same script times (fewer frames per slot)             the shots independent of frame rate/timing
  noaa      WaveShape.AntiAlias = 0                             -> should pass (AA differences tolerated)
  seed2     --seed 2                                            -> exact classes pass; per-pixel/row random
                                                                   fields differ (loose verdict)
  kperturb  derived configs with different K for each rnd(x)   -> 'derand' FlowFields/ColorMaps must FAIL
            (a parameter-evaluation bug)
  shuffle   slot n compared with slot n+1 of 'same'            -> (nearly) everything must FAIL
Writes build/sweep/validate/summary.json and the validation section of docs/sweep.md (kept by compare.py).
"""
import argparse
import json
import os
import shutil
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import compare as C  # noqa: E402
import render_reforce as R  # noqa: E402
import sweeplib as S  # noqa: E402

VAL = os.path.join(S.WORK, "validate")
BEGIN, END = "<!-- validation:begin -->", "<!-- validation:end -->"
EXPECT = {"same": "pass", "fps55": "pass", "noaa": "pass", "seed2": "pass (exact classes)",
          "kperturb": "FAIL (derand)", "shuffle": "FAIL"}


def render_variant(reforce, v, base_scenes):
    out = os.path.join(VAL, v)
    fps, sets, seed, scenes = 60.0, [], None, base_scenes
    if v == "fps55":
        fps = 55.6
    elif v == "noaa":
        sets = ["WaveShape.AntiAlias=0"]
    elif v == "seed2":
        seed = 2
    elif v == "kperturb":
        der = os.path.join(VAL, "derived-kperturb")
        if os.path.isdir(der):
            shutil.rmtree(der)
        for kind in ("FlowField", "ColorMap"):
            for name, path in S.corpus(kind):
                if os.path.isfile(S.derived_path(kind, name)):
                    S.write_derived(kind, name, path, root=der, salt="perturbed:")
        scenes = R.build_scenes(os.path.join(VAL, "scenes-kperturb"), der)
    for tl in S.sweep_timelines():
        R.render(reforce, tl, os.path.join(out, tl), scenes, fps, sets, seed)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reforce", default=S.DEFAULT_REFORCE)
    ap.add_argument("--only", default="")
    ap.add_argument("--no-docs", action="store_true")
    a = ap.parse_args()
    variants = [v for v in a.only.split(",") if v] or list(EXPECT)
    base = os.path.join(S.WORK, "reforce")
    scenes = R.build_scenes()
    if not all(os.path.isfile(os.path.join(base, tl, "index.json")) for tl in S.sweep_timelines()):
        for tl in S.sweep_timelines():
            R.render(a.reforce, tl, os.path.join(base, tl), scenes)
    summ_path = os.path.join(VAL, "summary.json")
    summ = json.load(open(summ_path)) if os.path.isfile(summ_path) else {}
    for v in variants:
        if v == "shuffle":
            ref = os.path.join(VAL, "same")
            if not os.path.isdir(ref):
                render_variant(a.reforce, "same", scenes)
        else:
            ref = render_variant(a.reforce, v, scenes)
        sources = {tl: ("reforce", os.path.join(ref, tl)) for tl in S.sweep_timelines()}
        res = C.compare_all(sources, base, gallery=None, shuffle=(v == "shuffle"))
        summ[v] = {"%s/%s" % k: c for k, c in C.summary(res).items()}
        worst = sorted((r for r in res if r["m"]), key=lambda r: -C.score(r))[:3]
        summ[v]["_worst"] = ["%s %s %.2f" % (r["name"], r["cls"], C.score(r)) for r in worst]
        print("%-8s (expect %s)" % (v, EXPECT.get(v, "?")))
        for k, c in sorted(C.summary(res).items()):
            print("    %-9s %-9s %3d: %3d pass %3d fail" % (k[0], k[1], c["n"], c["pass"], c["fail"]))
    os.makedirs(VAL, exist_ok=True)
    json.dump(summ, open(summ_path, "w"), indent=1)
    if not a.no_docs:
        write_docs(summ)


def section(summ):
    keys = sorted({k for v in summ.values() for k in v if not k.startswith("_")})
    L = [BEGIN, "## Validation (reforce vs reforce, no G-Force)", "",
         "`tools/sweep/validate.py` compares variants of the reforce replay with the baseline replay using the same "
         "metrics and thresholds (pass/total per type/class):", "",
         "| variant | expected | " + " | ".join(keys) + " | worst scores |",
         "|---|---|" + "---:|" * len(keys) + "---|"]
    for v in [x for x in EXPECT if x in summ]:
        cells = []
        for k in keys:
            c = summ[v].get(k)
            cells.append("%d/%d" % (c["pass"], c["n"]) if c else "-")
        L.append("| %s | %s | %s | %s |" % (v, EXPECT[v], " | ".join(cells), "; ".join(summ[v].get("_worst", []))))
    L += ["", END]
    return "\n".join(L)


def write_docs(summ):
    path = os.path.join(S.REPO, "docs", "sweep.md")
    text = open(path).read() if os.path.isfile(path) else (
        "# Corpus sweep (Phase 5B.4)\n\nNo G-Force results yet: run `tools/sweep/run_gforce_sweeps.sh`, then "
        "`python3 -I tools/sweep/compare.py`.\n")
    sec = section(summ)
    if BEGIN in text and END in text:
        text = text[:text.index(BEGIN)] + sec + text[text.index(END) + len(END):]
    else:
        text = text.rstrip("\n") + "\n\n" + sec + "\n"
    open(path, "w").write(text)
    print("updated docs/sweep.md (validation section)")


if __name__ == "__main__":
    main()

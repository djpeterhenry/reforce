#!/usr/bin/env bash
# Phase 5B.4: run every corpus-sweep timeline in G-Force, one probes/run.sh --auto run each (screen unlocked,
# G-Force not running, Accessibility permission for this terminal). ~20 min in total.
#
#   tools/sweep/run_gforce_sweeps.sh [sweep_ff_01 sweep_cm_01 ...]     (default: all probes/timelines/sweep_*.py)
#   DRY_RUN=1 tools/sweep/run_gforce_sweeps.sh   checks + staging + cleanup only (does not start G-Force)
#
# Before: checks that the G-Force prefs equal the reference backup and that no probe files are installed.
# Installs the derived 'zz sweep' configs (build/sweep/derived, made by make_timelines.py; SoundSpectrum content,
# local only) into probes/<Type>/zz sweep/ for the duration, so run.sh/install.sh copy them into G-Force's
# 'zz reforce probes' folder and remove them after each run.
# Records timeline -> capture dir in build/sweep/captures.json (read by compare.py).
# After: removes probes/<Type>/zz sweep/, verifies prefs == backup, G-Force probe folders empty, Key 5.py empty.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
PREFS="$HOME/Library/Preferences/SoundSpectrum"
REF_BACKUP="${REF_BACKUP:-$HOME/Library/Preferences/SoundSpectrum.reforce-backup-20261111-123844}"
GF="${GFORCE_DIR:-/Library/Application Support/SoundSpectrum/G-Force}"
MAP="$REPO/build/sweep/captures.json"
DERIVED="$REPO/build/sweep/derived/Packages"
STAGE_FF="$REPO/probes/FlowField/zz sweep"
STAGE_CM="$REPO/probes/ColorMap/zz sweep"
PROBE_DIRS=("$GF/Packages/G-Force.FlowField.package/zz reforce probes"
            "$GF/Packages/G-Force.WaveShape.package/WaveShapes/zz reforce probes"
            "$GF/Packages/G-Force.ColorMap.package/zz reforce probes"
            "$GF/Packages/G-Force.WaveShape.package/Particles/zz reforce probes"
            "$GF/Packages/G-Force.FlowField Union.package/zz reforce probes")

check_clean() {  # $1 = before|after
  local bad=0
  if ! diff -rq "$REF_BACKUP" "$PREFS" > /dev/null; then
    echo "!! $1: G-Force prefs differ from $REF_BACKUP:"; diff -rq "$REF_BACKUP" "$PREFS" || true; bad=1
  fi
  for d in "${PROBE_DIRS[@]}"; do
    if [[ -d "$d" && -n "$(ls -A "$d")" ]]; then echo "!! $1: probe files left in $d"; bad=1; fi
  done
  if [[ -s "$GF/Scripts/Key 5.py" ]]; then echo "!! $1: $GF/Scripts/Key 5.py is not empty"; bad=1; fi
  if [[ -e "$STAGE_FF" || -e "$STAGE_CM" ]]; then echo "!! $1: staged derived configs left under probes/"; bad=1; fi
  if pgrep -f "G-Force Standalone.app" > /dev/null; then echo "!! $1: G-Force is running"; bad=1; fi
  return $bad
}
unstage() { rm -rf "$STAGE_FF" "$STAGE_CM"; }

TLS=("$@")
if [[ ${#TLS[@]} -eq 0 ]]; then
  while IFS= read -r f; do TLS+=("$(basename "$f" .py)"); done < <(ls "$REPO"/probes/timelines/sweep_*.py | sort)
fi
[[ ${#TLS[@]} -gt 0 ]] || { echo "No sweep timelines: python3 -I tools/sweep/make_timelines.py"; exit 1; }
[[ -d "$DERIVED" ]] || { echo "No $DERIVED: python3 -I tools/sweep/make_timelines.py"; exit 1; }
[[ -d "$REF_BACKUP" ]] || { echo "No reference prefs backup $REF_BACKUP (set REF_BACKUP)"; exit 1; }
check_clean before || { echo "Not clean before the sweep: fix the above first (nothing was run)."; exit 1; }

trap 'unstage; echo; echo "== cleanup check"; check_clean after && echo "Clean: prefs == backup, no probe files left." || echo "!! NOT CLEAN (see above)"' EXIT
unstage
[[ -d "$DERIVED/G-Force.FlowField.package/zz sweep" ]] && cp -R "$DERIVED/G-Force.FlowField.package/zz sweep" "$STAGE_FF"
[[ -d "$DERIVED/G-Force.ColorMap.package/zz sweep" ]] && cp -R "$DERIVED/G-Force.ColorMap.package/zz sweep" "$STAGE_CM"

mkdir -p "$(dirname "$MAP")"
[[ -f "$MAP" ]] || echo "{}" > "$MAP"
for tl in "${TLS[@]}"; do
  f="$REPO/probes/timelines/$tl.py"
  secs="$(sed -n 's/^# reforce-sweep:.* seconds=\([0-9]*\).*/\1/p' "$f" | head -1)"
  [[ -n "$secs" ]] || { echo "$f: no '# reforce-sweep: ... seconds=N' header"; exit 1; }
  slots="$(sed -n 's/^# reforce-sweep:.* slots=\([0-9]*\).*/\1/p' "$f" | head -1)"
  echo "== $tl: $slots shots, PROBE_SECONDS=$secs ($(date +%H:%M:%S))"
  if [[ -n "${DRY_RUN:-}" ]]; then
    echo "   (dry run) would run: PROBE_TIMELINE=$tl PROBE_SECONDS=$secs probes/run.sh --auto"
    echo "   staged: $(ls "$STAGE_FF" 2>/dev/null | wc -l | tr -d ' ') FlowField + $(ls "$STAGE_CM" 2>/dev/null | wc -l | tr -d ' ') ColorMap derived configs"
    continue
  fi
  before="$(ls -d "$REPO"/probes/captures/*/ 2>/dev/null | sort || true)"
  PROBE_TIMELINE="$tl" PROBE_SECONDS="$secs" "$REPO/probes/run.sh" --auto
  after="$(ls -d "$REPO"/probes/captures/*/ 2>/dev/null | sort || true)"
  cap="$(comm -13 <(echo "$before") <(echo "$after") | tail -1)"
  cap="${cap%/}"
  [[ -n "$cap" ]] || { echo "!! $tl: no new capture dir"; exit 1; }
  n="$(ls "$cap" | grep -c '^G-Force Screen Shot .*\.png$' || true)"
  echo "== $tl -> ${cap#$REPO/} ($n of $slots screenshots)"
  [[ "$n" == "$slots" ]] || echo "!! $tl: expected $slots screenshots, got $n (compare.py will warn)"
  python3 -I - "$MAP" "$tl" "${cap#$REPO/}" <<'EOF'
import json, sys
p, tl, d = sys.argv[1:]
m = json.load(open(p)); m[tl] = d; json.dump(m, open(p, "w"), indent=1, sort_keys=True)
EOF
  # run.sh restored the prefs from its own backup; make sure nothing drifted before the next run
  if ! diff -rq "$REF_BACKUP" "$PREFS" > /dev/null; then echo "!! prefs differ after $tl; stopping"; exit 1; fi
done
echo "Done. Captures: $MAP. Next: python3 -I tools/sweep/compare.py"

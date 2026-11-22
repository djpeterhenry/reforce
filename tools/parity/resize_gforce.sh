#!/usr/bin/env bash
# F6 helper for probe run24 (line width vs frame size): resizes the G-Force Standalone window between shots.
# Start it just before 'PROBE_TIMELINE=run24 probes/run.sh --auto'. It waits for the new capture folder, and each time
# shot k lands there it moves/resizes the window to SIZES[k] (window frame in points, title bar included) via System
# Events (Accessibility), then logs the window's AX position/size to <capture>/resize.log. No prefs are touched (run.sh
# restores them anyway). G-Force reopens at the window rect it had when full screen was last entered, and that memory
# is NOT in the prefs run.sh restores, so after the last windowed shot this script puts the window back to the rect it
# had at shot 1 before run24 enters full screen (if it ever went wrong: probes/timelines/run24_restore.py).
# RESIZE_SIZES="w h;w h;..." overrides SIZES[1..] (window frame in points).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
CAP="$HERE/probes/captures"
SIZES=("" "990 458" "990 858" "990 1058" "600 658" "1600 658" "500 958" "400 328" "1300 900")
[[ -n "${RESIZE_SIZES:-}" ]] && { IFS=';' read -r -a extra <<< "$RESIZE_SIZES"; SIZES=("" "${extra[@]}"); }
START=$(date +%s)
before="$(ls "$CAP" 2>/dev/null | sort | tail -1)"
OUT=""
while [[ -z "$OUT" ]]; do
  sleep 0.5; now="$(ls "$CAP" | sort | tail -1)"
  [[ "$now" != "$before" ]] && OUT="$CAP/$now"
  (( $(date +%s) - START > 60 )) && { echo "no new capture folder"; exit 1; }
done
LOG="$OUT/resize.log"
ax() { osascript -e 'tell application "System Events" to tell process "G-Force Standalone"' \
  -e "$1" -e 'end tell' 2>&1; }
winfo() { ax 'set {x, y} to position of window 1
set {w, h} to size of window 1
return "pos " & x & "," & y & " size " & w & "x" & h' | tr '\n' ' '; }
seen=0; orig=""
while (( seen < ${#SIZES[@]} )); do
  sleep 0.2
  n=$(ls "$OUT" | grep -c -i '\.png$' || true)
  if (( n > seen )); then
    seen=$n
    echo "shot $n landed; window before: $(winfo)" >> "$LOG"
    [[ -z "$orig" ]] && orig="$(ax 'set {x, y} to position of window 1
set {w, h} to size of window 1
return (x as text) & "," & (y as text) & "," & (w as text) & "," & (h as text)' | tr -d ' ')"
    sz="${SIZES[$n]:-}"
    if [[ -n "$sz" ]]; then
      ax "set position of window 1 to {20, 40}
set size of window 1 to {${sz% *}, ${sz#* }}" >> "$LOG"
      sleep 2
      echo "  set ${sz}: now $(winfo)" >> "$LOG"
    fi
  fi
  (( $(date +%s) - START > 150 )) && break
done
if [[ -n "$orig" ]]; then
  IFS=',' read -r ox oy ow oh <<< "$orig"
  ax "set position of window 1 to {$ox, $oy}
set size of window 1 to {$ow, $oh}" >> "$LOG"
  echo "restored window rect $orig: now $(winfo)" >> "$LOG"
fi
echo "done (seen $seen shots)" >> "$LOG"

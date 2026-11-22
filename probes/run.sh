#!/usr/bin/env bash
# Run the black-box probe timeline in G-Force and collect its screenshots into probes/captures/ (git-ignored).
# Backs up G-Force prefs first and restores them afterwards; installs the probe configs only for the
# duration of the run (install.sh --remove on exit), so normal G-Force use is unaffected.
# The timeline starts with Shift+R then 5 in G-Force ("run script on next key" -> Scripts/Key 5.py).
# (Plain number keys load user presets.)
# With Accessibility permission for this terminal, pass --auto to send the key press automatically.
#
# Audio (Phase 5C): PROBE_AUDIO=<wav> plays that file to the Loopback device "reforce probe" (silent: no
# monitors) PROBE_AUDIO_DELAY seconds (default 2) after the Shift+R,5 keypress, and sets G-Force's
# Audio.InputSource to "reforce probe" for the run (prefs are restored afterwards). Audio timelines declare
# their file in a "# reforce-audio: <file>.wav delay=D seconds=S" header, which supplies PROBE_AUDIO (from
# probes/audio/), PROBE_AUDIO_DELAY and PROBE_SECONDS when those are unset. Needs --auto (the delay is
# relative to the scripted keypress), the WAVs (python3 -I tools/audio_signals/gen_signals.py) and the player
# (cmake --build build --target reforce-audio-play; or PROBE_PLAYER=<path>). The player only ever plays to
# the exact device "reforce probe"; the run aborts if that device is missing.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PREFS="$HOME/Library/Preferences/SoundSpectrum"
P="$PREFS/G-Force/Preferences (Standalone).txt"
TL="$HERE/timelines/${PROBE_TIMELINE:-run2}.py"
AUDIO_DEVICE="reforce probe"
HDR="$(grep -m1 '^# reforce-audio:' "$TL" 2>/dev/null || true)"
hdr() { sed -n "s/.* $1=\([0-9.]*\).*/\1/p" <<< "$HDR"; }
if [[ -n "$HDR" ]]; then
  [[ -n "${PROBE_AUDIO:-}" ]] || PROBE_AUDIO="$HERE/audio/$(awk '{print $3}' <<< "$HDR")"
  [[ -n "${PROBE_AUDIO_DELAY:-}" ]] || PROBE_AUDIO_DELAY="$(hdr delay)"
  [[ -n "${PROBE_SECONDS:-}" ]] || PROBE_SECONDS="$(hdr seconds)"
fi
PLAYER_PID=""
if [[ -n "${PROBE_AUDIO:-}" ]]; then
  PLAYER="${PROBE_PLAYER:-$HERE/../build/reforce-audio-play}"
  [[ "${1:-}" == "--auto" ]] || { echo "PROBE_AUDIO needs --auto (audio starts at a fixed delay after the keypress)"; exit 1; }
  [[ -f "$PROBE_AUDIO" ]] || { echo "Missing $PROBE_AUDIO: run python3 -I tools/audio_signals/gen_signals.py"; exit 1; }
  [[ -x "$PLAYER" ]] || { echo "Missing player $PLAYER: cmake --build build --target reforce-audio-play"; exit 1; }
  "$PLAYER" --device "$AUDIO_DEVICE" --check || { echo "No output device named '$AUDIO_DEVICE' (Loopback): not running"; exit 1; }
  PROBE_PREFS="${PROBE_PREFS:+$PROBE_PREFS;}Audio.InputSource=\"$AUDIO_DEVICE\";Audio.AutoDetect.Enabled=0"
fi
OUT="$HERE/captures/$(date +%Y%m%d-%H%M%S)"
BAK="$(mktemp -d)/SoundSpectrum"
mkdir -p "$OUT"
cp -Rp "$PREFS" "$BAK"
stop_audio() { if [[ -n "$PLAYER_PID" ]]; then kill -TERM "$PLAYER_PID" 2>/dev/null || true; wait "$PLAYER_PID" 2>/dev/null || true; PLAYER_PID=""; fi; }
restore() { stop_audio
            pkill -TERM -f "G-Force Standalone.app" 2>/dev/null || true; pkill -TERM -f "G-Force Toolbar.app" 2>/dev/null || true; sleep 3
            rm -rf "$PREFS"; cp -Rp "$BAK" "$PREFS"; echo "Prefs restored from $BAK"
            "$HERE/install.sh" --remove; }
trap restore EXIT
"$HERE/install.sh"

setpref() { grep -v "^$1 = " "$P" > "$P.tmp" || true; echo "$1 = $2" >> "$P.tmp"; mv "$P.tmp" "$P"; }
setpref Screenshot.Dir "\"$OUT\""
setpref UI.ShowOnStartup 0
setpref ColorMap.MapFunction '"i"'   # default for the probes; PROBE_PREFS may override it (run20)
# Extra per-run prefs: PROBE_PREFS="Key=value;Key2=value2" (standalone file), PROBE_GLOBAL_PREFS (Global Preferences.txt)
IFS=';' read -r -a extra <<< "${PROBE_PREFS:-}"; for kv in "${extra[@]:-}"; do [[ -n "$kv" ]] && setpref "${kv%%=*}" "${kv#*=}"; done
GP="$PREFS/G-Force/Global Preferences.txt"
IFS=';' read -r -a gextra <<< "${PROBE_GLOBAL_PREFS:-}"; for kv in "${gextra[@]:-}"; do [[ -n "$kv" ]] && P="$GP" setpref "${kv%%=*}" "${kv#*=}"; done
for k in WaveShape ColorMap FlowField Particle Sprite; do setpref "$k.Slideshow.EnableOnStartup" 0; done
setpref '"FlowField Union.Slideshow.EnableOnStartup"' 0

open -g "/Applications/G-Force/G-Force Standalone.app"
sleep 8
if [[ "${1:-}" == "--auto" ]]; then
  osascript -e 'tell application "G-Force Standalone" to activate' -e 'delay 1' \
            -e 'tell application "System Events" to keystroke "r" using shift down' \
            -e 'delay 0.5' -e 'tell application "System Events" to keystroke "5"'
  if [[ -n "${PROBE_AUDIO:-}" ]]; then
    # Timeline 0:00 = the keystroke above. The sync click in the file aligns the rest exactly.
    { echo "timeline=$(basename "$TL") audio=$PROBE_AUDIO delay=${PROBE_AUDIO_DELAY:-2} keys_sent_epoch=$(date +%s)"
      sleep "${PROBE_AUDIO_DELAY:-2}"; exec "$PLAYER" --device "$AUDIO_DEVICE" "$PROBE_AUDIO"; } > "$OUT/player.log" 2>&1 &
    PLAYER_PID=$!
  fi
else
  echo ">>> In the G-Force window press Shift+R, then 5 (waiting 30 s)"; sleep 30
fi
sleep "${PROBE_SECONDS:-60}"
stop_audio
[[ -f "$OUT/player.log" ]] && cat "$OUT/player.log"
echo "Captures:"; ls -la "$OUT"

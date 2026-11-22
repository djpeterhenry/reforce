#!/usr/bin/env bash
# Manage reforce probe files inside the G-Force install, so normal G-Force use is never affected.
#
#   sudo probes/install.sh          One-time setup: create user-owned probe folders + Scripts/Key 5.py (empty).
#   probes/install.sh               Install probe configs + timeline (done automatically by run.sh).
#                                   Timeline = probes/timelines/$PROBE_TIMELINE.py (default run2).
#   probes/install.sh --remove      Remove probe configs, blank Key 5.py (done automatically by run.sh on exit).
#   sudo probes/install.sh --purge  Delete everything we ever added (folders + Key 5.py) -> install back to stock.
#
# Between runs only empty "zz reforce probes" folders and an empty Key 5.py remain (no configs, nothing in
# G-Force's slideshow or menus). --purge removes even those.
set -euo pipefail
GF="${GFORCE_DIR:-/Library/Application Support/SoundSpectrum/G-Force}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OWNER="${SUDO_USER:-$(id -un)}"
MODE="${1:-install}"

TYPES=(FlowField WaveShape ColorMap Particle Union Sprite)
dest() { case "$1" in
  FlowField) echo "$GF/Packages/G-Force.FlowField.package/zz reforce probes" ;;
  WaveShape) echo "$GF/Packages/G-Force.WaveShape.package/WaveShapes/zz reforce probes" ;;
  ColorMap)  echo "$GF/Packages/G-Force.ColorMap.package/zz reforce probes" ;;
  Particle)  echo "$GF/Packages/G-Force.WaveShape.package/Particles/zz reforce probes" ;;
  Union)     echo "$GF/Packages/G-Force.FlowField Union.package/zz reforce probes" ;;
  Sprite)    echo "$GF/Packages/G-Force.Sprite.package/zz reforce probes" ;;
esac; }
SCRIPT="$GF/Scripts/Key 5.py"
MARK="# reforce black-box"

case "$MODE" in
  --purge)
    [[ $EUID -eq 0 ]] || { echo "--purge needs sudo"; exit 1; }
    for t in "${TYPES[@]}"; do d="$(dest $t)"; [[ -d "$d" ]] && rm -rf "$d"; done
    [[ -f "$SCRIPT" ]] && { [[ ! -s "$SCRIPT" || "$(head -c ${#MARK} "$SCRIPT")" == "$MARK" ]] && rm -f "$SCRIPT"; }
    echo "Purged: G-Force install has no reforce files."
    ;;
  --remove)
    for t in "${TYPES[@]}"; do d="$(dest $t)"; [[ -d "$d" ]] && find "$d" -mindepth 1 -delete; done
    [[ -f "$SCRIPT" && -O "$SCRIPT" ]] && : > "$SCRIPT"
    echo "Probe configs removed (empty folders + empty Key 5.py remain; 'sudo $0 --purge' removes them)."
    ;;
  install)
    if [[ $EUID -eq 0 ]]; then
      for t in "${TYPES[@]}"; do mkdir -p "$(dest $t)"; chown "$OWNER" "$(dest $t)"; done
      if [[ -e "$SCRIPT" && -s "$SCRIPT" && "$(head -c ${#MARK} "$SCRIPT")" != "$MARK" ]]; then
        echo "Refusing to overwrite existing $SCRIPT"; exit 1
      fi
      [[ -e "$SCRIPT" ]] || : > "$SCRIPT"; chown "$OWNER" "$SCRIPT"
      echo "Setup done (folders + Key 5.py owned by $OWNER, empty). run.sh installs/removes probes per run."
      exit 0
    fi
    for t in "${TYPES[@]}"; do
      d="$(dest $t)"; src="$HERE/$t"; [[ $t == Union ]] && src="$HERE/FlowField Union"
      if [[ $t == Sprite ]]; then
        # Optional (5F.6 movie sprites; G-Force ignores movies in ExternalSprites.Path): the folder exists only after
        # 'sudo probes/install.sh' was re-run with Sprite in TYPES. Content: PROBE_SPRITE_DIR (default probes/sprites).
        src="${PROBE_SPRITE_DIR:-$HERE/sprites}"
        [[ -d "$d" && -O "$d" ]] || { echo "(no user-owned $d: sprite probes not installed)"; continue; }
      fi
      [[ -d "$d" && -O "$d" ]] || { echo "Missing $d: run 'sudo $0' once"; exit 1; }
      rsync -a --delete "$src/" "$d/"
    done
    cp "$HERE/timelines/${PROBE_TIMELINE:-run2}.py" "$SCRIPT"
    echo "Probe configs + Key 5.py installed."
    ;;
  *) echo "usage: $0 [--remove | --purge]"; exit 2 ;;
esac

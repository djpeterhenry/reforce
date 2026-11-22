#!/bin/bash
# One-line scores for comparing beat detector settings (see docs/beat-detection.md).
#
#   tools/beat_eval/quick.sh songs NAME [--set beat.X=V ...]   # recent 2,000 + 600 held-out songs (~2.5 min)
#   tools/beat_eval/quick.sh mixes NAME [--set ...]            # 400 simulated DJ mixes of recent songs (~1 min)
#   tools/beat_eval/quick.sh nudges NAME [--set ...]           # ... with a quick +-1-3 BPM tempo nudge instead of rides
#                                                              #     (10-80 s after the blend; --nudge-at 30: the old place)
#
# Build first: cmake --build build --target reforce-beat-eval. Outputs go to build/beat-eval/NAME_*.
set -euo pipefail
cd "$(dirname "$0")/../.."
mode=$1 name=$2
shift 2
py=tools/beat_eval/beat_eval.py
summ() {
    python3 -I -c '
import re, sys
t = sys.stdin.read()
g = lambda p: re.search(p, t).group(1)
print("%s tempo %s (2/3 %s, 0 %s) on-beat %s off %s" % (sys.argv[1], g(r"ok ([\d.]+)%"), g(r"x2/3 ([\d.]+)%"),
      g(r" 0 ([\d.]+)%"), g(r"on the beat ([\d.]+)%"), g(r"off-beat ([\d.]+)%")), end="")' "$1"
}
summ_mix() {
    python3 -I -c '
import re, sys
keys = [("A alone", "A"), ("blend, 1st", "blend1"), ("blend, 2nd", "blend2"), ("B, first 30", "B30"),
        ("B, 30-90", "B90"), ("B while", "ride"), ("B: tempo nudge", "nudge")]
out = []
for line in sys.stdin:
    m = re.search(r"TEMPO right +([\d.]+)%  on the beat +([\d.]+)%  off-beat +([\d.]+)%", line)
    for k, short in keys:
        if m and line.strip().startswith(k):
            out.append("%s %s/%s" % (short, m.group(1), m.group(2)) + (" off %s" % m.group(3) if short == "nudge" else ""))
print("%-12s tempo/on-beat: %s" % (sys.argv[1], "  ".join(out)))' "$1"
}
case $mode in
songs)
    a=$(python3 -I $py --split all --recent 2000 --name "${name}_r" --worst 0 "$@" 2>/dev/null | summ recent)
    b=$(python3 -I $py --split test --sample 600 --name "${name}_t" --worst 0 "$@" 2>/dev/null | summ held-out)
    printf '%-14s %s | %s\n' "$name" "$a" "$b"
    ;;
mixes)
    python3 -I $py --recent 2000 --split all --mixes 400 --name "${name}_m" --worst 0 "$@" 2>/dev/null | summ_mix "$name"
    ;;
nudges)
    python3 -I $py --recent 2000 --split all --mixes 400 --nudge --name "${name}_m" --worst 0 "$@" 2>/dev/null |
        summ_mix "$name"
    ;;
*)
    echo "usage: $0 songs|mixes|nudges NAME [--set ...]" >&2
    exit 2
    ;;
esac

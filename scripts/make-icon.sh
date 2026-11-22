#!/bin/sh
# Regenerates resources/AppIcon.icns from tools/icon/make_icon.m (original
# artwork drawn with CoreGraphics). The .icns is committed; run this only
# after changing the generator.
set -eu

repo=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reforce-icon.XXXXXX")
trap 'rm -rf "$work"' EXIT

clang -fobjc-arc -O2 -Wall -framework Foundation -framework CoreGraphics -framework ImageIO \
    -framework UniformTypeIdentifiers "$repo/tools/icon/make_icon.m" -o "$work/make_icon"
"$work/make_icon" "$work/AppIcon.iconset"
mkdir -p "$repo/resources"
iconutil --convert icns --output "$repo/resources/AppIcon.icns" "$work/AppIcon.iconset"
echo "wrote $repo/resources/AppIcon.icns"

#!/usr/bin/env bash
# Copy G-Force scene files + reference docs from the local install into ./scenes/.
# scenes/ is git-ignored: G-Force content is SoundSpectrum's and must not be committed.
set -euo pipefail

SRC="${GFORCE_DIR:-/Library/Application Support/SoundSpectrum/G-Force}"
DEST="$(cd "$(dirname "$0")/.." && pwd)/scenes"

if [[ ! -d "$SRC/Packages" ]]; then
  echo "G-Force install not found at: $SRC (set GFORCE_DIR)" >&2
  exit 1
fi

mkdir -p "$DEST"
for d in Packages Themes Categories Documentation; do
  rsync -a --delete --exclude "zz reforce probes" "$SRC/$d/" "$DEST/$d/"
done
rsync -a --delete "$SRC/Resources/Shaders/" "$DEST/Shaders/"

# Personal images and movies are NOT copied here any more: they live in ~/reforce/{images,movies}
# and reforce reads them directly (reforce.ExternalSpritesDir / reforce.VideoDir, set by install-app.sh).
# The --delete above also removes any old personal-image mirror from scenes/.

echo "Synced G-Force content from '$SRC' -> '$DEST'"
find "$DEST" -name '*.txt' -path '*Packages*' | wc -l | xargs echo "Scene .txt files:"

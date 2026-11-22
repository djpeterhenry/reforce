#!/bin/sh
# Builds a Release reforce.app and installs it to ~/Applications so it can be
# launched from Finder / Spotlight / the Dock (drag it there yourself; this
# script never touches Dock preferences). Idempotent: run it again to update.
#
#   scripts/install-app.sh [--dest DIR] [--open] [--now]
#
# What it does:
#   1. configure + build Release in build-release/ (CFBundleVersion = git describe)
#   2. replace ~/Applications/reforce.app (only if the existing copy is ours:
#      CFBundleIdentifier com.reforce.app). A running copy is never quit (it may be
#      feeding OBS live): the build is staged and a background waiter installs it
#      as soon as reforce quits (build-release/install-pending.log). --now waits
#      in the foreground instead.
#   3. ad-hoc sign it with the identifier-only designated requirement
#      `identifier "com.reforce.app"` (see docs/usage.md "Install" about the mic permission)
#   4. if this repo has a scenes/ copy (scripts/sync-scenes.sh), merge reforce.ScenesDir = <this repo>/scenes into
#      ~/Library/Application Support/reforce/settings.json (other keys kept); otherwise reforce reads an
#      installed G-Force directly. If ~/reforce exists, it also becomes the media folder (images, movies).
#   5. --open: show the folder in Finder (off by default: a Finder window coming to
#      the front disturbs a live show)
#
# No sudo. If ~/Applications exists but is not writable (some installers create
# it owned by root), it installs to /Applications instead (writable for admin
# users) and says so; `sudo chown "$USER" ~/Applications` once restores the
# default. --dest DIR picks the folder explicitly.
set -eu

BUNDLE_ID=com.reforce.app
repo=$(cd "$(dirname "$0")/.." && pwd)
build="$repo/build-release"
dest_dir=""
open_finder=0
wait_mode=""   # "": stage + background waiter when running; now: wait here; waiter: (internal)
while [ $# -gt 0 ]; do
    case "$1" in
        --open) open_finder=1 ;;
        --no-open) open_finder=0 ;;
        --now) wait_mode=now ;;
        --waiter) wait_mode=waiter ;;
        --dest) [ $# -ge 2 ] || { echo "usage: $0 [--dest DIR] [--open] [--now]" >&2; exit 2; }
                dest_dir=$2; shift ;;
        *) echo "usage: $0 [--dest DIR] [--open] [--now]" >&2; exit 2 ;;
    esac
    shift
done
if [ -z "$dest_dir" ]; then
    dest_dir="$HOME/Applications"
    if [ -d "$dest_dir" ] && [ ! -w "$dest_dir" ]; then
        echo "note: $dest_dir is not writable by $USER (owned by $(stat -f %Su "$dest_dir"))." >&2
        echo "      Installing to /Applications instead; 'sudo chown \"\$USER\" ~/Applications' fixes this once." >&2
        dest_dir=/Applications
    fi
fi
dest="$dest_dir/reforce.app"

plist_get() { /usr/libexec/PlistBuddy -c "Print :$2" "$1/Contents/Info.plist" 2>/dev/null || true; }

version=$(git -C "$repo" describe --always --dirty 2>/dev/null || echo unknown)
src="$build/reforce.app"
staged="$build/reforce-staged.app"   # what a waiter installs (a later build may change $src meanwhile)
running() { pgrep -f "^$dest/Contents/MacOS/reforce" >/dev/null 2>&1; }

if [ "$wait_mode" = waiter ]; then
    # Background waiter: install the staged build once the running reforce has quit.
    while running; do sleep 1; done
    src="$staged"
    version=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$src/Contents/Info.plist" 2>/dev/null || echo unknown)
    echo "==> $(date '+%H:%M:%S') reforce quit; installing the staged $version"
else
    echo "==> Building reforce $version (Release) in $build"
    cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE=Release \
        -DREFORCE_BUNDLE_ID="$BUNDLE_ID" -DREFORCE_BUNDLE_VERSION="$version" \
        -DREFORCE_BUILD_TESTS=OFF -DREFORCE_BUILD_PROBE=OFF >/dev/null
    cmake --build "$build" --target reforce -j "$(sysctl -n hw.ncpu)"
fi
[ "$(plist_get "$src" CFBundleIdentifier)" = "$BUNDLE_ID" ] || {
    echo "error: $src does not have bundle id $BUNDLE_ID" >&2; exit 1; }

mkdir -p "$dest_dir"
if [ -e "$dest" ]; then
    existing=$(plist_get "$dest" CFBundleIdentifier)
    if [ "$existing" != "$BUNDLE_ID" ]; then
        echo "error: $dest exists and is not reforce (CFBundleIdentifier '$existing'); not replacing it" >&2
        exit 1
    fi
    # Never quit a running copy: it may be live (feeding OBS). Stage the build and install it once it quits.
    if running; then
        if [ "$wait_mode" = now ]; then
            echo "==> reforce is running; waiting for it to quit (it is not asked to)"
            while running; do sleep 1; done
        else
            rm -rf "$staged"
            ditto "$src" "$staged"
            pidfile="$build/install-pending.pid"
            if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
                echo "==> reforce is running: not replaced. The waiter already pending will install this build when it quits."
            else
                nohup "$repo/scripts/install-app.sh" --waiter ${dest_dir:+--dest "$dest_dir"} >"$build/install-pending.log" 2>&1 &
                echo $! >"$pidfile"
                echo "==> reforce is running: not replaced (it is never quit). This build ($version) is staged and"
                echo "    installs by itself when reforce quits (log: $build/install-pending.log)."
            fi
            exit 0
        fi
    fi
    rm -rf "$dest"
fi

echo "==> Installing $dest"
ditto "$src" "$dest"
# Re-sign the installed copy (same identifier and designated requirement as the build).
codesign --force --sign - --timestamp=none --identifier "$BUNDLE_ID" \
    -r="designated => identifier \"$BUNDLE_ID\"" "$dest"
codesign --verify --strict "$dest"
# Let LaunchServices pick up the new bundle (icon, version).
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
    -f "$dest" >/dev/null 2>&1 || true

echo "==> Settings"
# Scenes: a local copy in this repo's scenes/ (scripts/sync-scenes.sh) wins; without one reforce reads an installed
# G-Force's scene folder directly (src/app/main.cpp defaultScenesDir), so nothing needs to be set.
gforce_dir="/Library/Application Support/SoundSpectrum/G-Force"
if [ -d "$repo/scenes/Packages" ] || [ -d "$repo/scenes/packages" ]; then
    "$dest/Contents/MacOS/reforce" --write-setting "reforce.ScenesDir=$repo/scenes"
elif [ -d "$gforce_dir/Packages" ]; then
    echo "  G-Force scenes: read from your G-Force install ($gforce_dir)"
else
    echo "  No G-Force install found: one built-in base scene plus reforce's own content (see README)"
fi
settings="$HOME/Library/Application Support/reforce/settings.json"
# Optional media home: if ~/reforce exists, its images and movies become sprites (reforce.ExternalSpritesDir; subfolders
# are searched) and ~/reforce/movies feeds the video picker (reforce.VideoDir). Only set when missing, so a custom path
# survives reinstalls. Create ~/reforce/images and ~/reforce/movies and run this again to opt in.
media="$HOME/reforce"
if [ -d "$media" ]; then
    for kv in "reforce.ExternalSpritesDir=$media" "reforce.VideoDir=$media/movies"; do
        if ! grep -q "\"${kv%%=*}\"" "$settings" 2>/dev/null; then
            "$dest/Contents/MacOS/reforce" --write-setting "$kv"
        fi
    done
fi

echo
echo "Installed reforce $version to $dest"
echo "  Drag it from Finder to the Dock to keep it there."
echo "  Log:      ~/Library/Logs/reforce/reforce.log"
echo "  Settings: ~/Library/Application Support/reforce/settings.json"
if [ "$open_finder" -eq 1 ]; then open "$dest_dir"; fi

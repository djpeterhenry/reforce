#!/bin/sh
# Builds the web dancer preview (dancer_wasm.cpp + reforce's dancer code) into one dancer.js (the WebAssembly is
# inlined) next to dancer_gl.js. Needs Emscripten (brew install emscripten).
#   tools/dancer_web/build.sh [OUT_DIR]      default OUT_DIR: build-web/dancer
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-$root/build-web/dancer}
mkdir -p "$out"
em++ -std=c++20 -O3 -I"$root/src" \
    "$here/dancer_wasm.cpp" \
    "$root/src/engine/DancerRig.cpp" "$root/src/engine/DanceMoves.cpp" "$root/src/engine/DanceClips.cpp" "$root/src/engine/Dancers.cpp" \
    "$root/src/engine/DancerInstances.cpp" "$root/src/engine/Meshes.cpp" \
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createDancerModule -sSINGLE_FILE=1 -sENVIRONMENT=web \
    -sALLOW_MEMORY_GROWTH=1 -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF32 \
    -sEXPORTED_FUNCTIONS=_dancer_moves,_dancer_set,_dancer_frame,_dancer_parts,_dancer_head_top,_dancer_bent,_dancer_floats,_malloc,_free \
    -o "$out/dancer.js"
cp "$here/dancer_gl.js" "$out/dancer_gl.js"
# The web sample of mocap moves (3 basic moves and a routine per style), fetched by dancer_gl.js.
"$root/tools/aist_import/run" web_moves.py "$root/content/Moves/aist.moves" "$out/moves.bin"
ls -l "$out"

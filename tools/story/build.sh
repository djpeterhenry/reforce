#!/bin/sh
# Builds "The Making of reforce", one self-contained HTML file for friends: tools/story/story.html with the dancer
# preview (WebAssembly + WebGL renderer + mocap sample), the organics picture, two 3D solid clips rendered by reforce (tools/story/solid_clips.py; needs the built app)
# and the dance-clip examples
# (tools/story/clips.py, from ~/reforce/movies) inlined as base64.
#   tools/story/build.sh [OUT_FILE]      default: build-web/story/reforce-story.html
# Needs Emscripten for the dancer preview (tools/dancer_web/build.sh).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-$root/build-web/story/reforce-story.html}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$root/tools/dancer_web/build.sh" "$work/dancer" >/dev/null
python3 -I "$here/clips.py" "$work" >/dev/null
sips -Z 1600 -s format jpeg -s formatOptions 78 "$root/docs/prototypes/organics-overview.jpg" --out "$work/organics.jpg" >/dev/null
python3 -I "$here/solid_clips.py" "$work" >/dev/null
mkdir -p "$(dirname "$out")"
python3 -I - "$here/story.html" "$work" "$out" <<'EOF'
import base64, sys, pathlib
src, work, out = sys.argv[1], pathlib.Path(sys.argv[2]), sys.argv[3]
b64 = lambda p: base64.b64encode((work / p).read_bytes()).decode()
html = pathlib.Path(src).read_text()
for key, path in [("@DANCER_JS@", "dancer/dancer.js"), ("@DANCER_GL@", "dancer/dancer_gl.js"),
                  ("@MOVES_BIN@", "dancer/moves.bin"), ("@ORGANICS_JPG@", "organics.jpg"), ("@DODECA_MP4@", "dodecahedron.mp4"), ("@SPHERE_MP4@", "sphere.mp4"), ("@LUMA_MP4@", "luma.mp4"), ("@BEEPLE_MP4@", "beeple.mp4")]:
    assert html.count(key) == 1, key
    html = html.replace(key, b64(path))
pathlib.Path(out).write_text(html)
print(f"{out}: {len(html) / 1e6:.1f} MB")
EOF

# 3D models: design (reforce extension)

Status: built 2026-11-22 (PLAN.md 5R.1-5R.5); Peter picked both looks (A and B). Peter still to try it with his own model.

## What it is

A **model Object** draws a textured 3D model that you supply (glTF binary, `.glb`) in real time, seen from a moving
camera: slow dollies along the hull, passes, orbits, sweeps underneath, beat cuts. It is a new solid
mesh kind (`Mesh="model"`), so it is started like any Object (panel, `switch:object:NAME`, Themes, the slideshow) and
gets the solid slot, fades, `PBar` holds and the `SYNC_*` beat clock for free.

**Models are never part of reforce.** The code is a generic glTF loader; models and the scene files that name them
live in a local folder outside the repo. Most models people find online are fan art or carry licenses that don't mix
with GPL-3.0 (CC-BY-NC-SA, "editorial use"), and the repo is public. A scene whose model is missing is left out of the
catalog with one log line, so a fresh checkout behaves exactly as today.

## Two looks (Peter: both)

| | A. Visuals (`Look=0`) | B. Color (`Look=1`) |
|---|---|---|
| What you see | the model in the palette's colors, trailing and flowing with the feedback | the model's own textures and lighting over the visuals |
| Drawn into | the solid slot target as luminance, composited by the sprite pass (Pen / EPen) like every solid | the colorized frame, after colorize (like the glass puzzle and the rubik's color pass) |
| Trails | yes (Pen) | optional: `Pen` > 0 also stamps a luminance copy into the feedback, so the color model leaves palette trails |
| Glow | the window / light texture lifts the luminance, so lit windows streak | emissive color, plus an optional soft bloom |
| Syphon alpha | keyed like any visual | `ColorKey = 0` raises the alpha by the model's coverage (solid over a camera backdrop) |

A is the reforce-native look; B shows the model as made. Both run from the same mesh, textures and camera, so a scene
picks one with a key, and the same move can exist as an A and a B scene.

### Look A shading (`model_fs`, luminance)

```
albedo = luma(baseColor)                       // the texture's brightness, not its hue
n      = normal map in tangent space -> world
lum    = Fill + (1 - Fill) * albedo * mix(1, key(n) + .25 * rim(n), Shad)
lum    = max(lum, Glow * luma(emissive))       // lit windows and lights read as bright
out    = (saturate(lum), 1)                    // MSAA-resolved, alpha = coverage
```

### Look B shading (`model_color_fs`, sRGB)

A small fixed rig, no image-based lighting: a warm key light (direction `LtX/LtY/LtZ`, default upper front left), a
cool fill from below (.2), a rim light from behind the model (.35) so the silhouette separates from busy visuals,
Blinn-Phong specular from the glTF roughness / metallic (or a specular-in-alpha map, see the prep script), normal maps,
and emissive x `Glow`. Exposure `Expo` and a light ACES-style tone curve. Bloom on the emissive only (`Blom`,
default .6; 0 = off; a half-size threshold + the glass pass's separable blur, reused): window lights and engine glows
halo softly. On by default (Peter 2026-11-22) because it only blooms emissive light: a model without emissive maps
or emission is unchanged, and the half-size pass is skipped when no material emits.
`Tint` (0..1) pulls the albedo towards the current ColorMap's mid color, so a color model can still sit in the
palette.

## The camera

The model is normalized on load: centered on its bounding box, scaled so its bounding sphere has radius 1, front along
-z, up +y. The scene's `Front` / `Up` keys say which way the model in the file faces (the prep script prints the
`Front` line). Every camera position below is in
these units, so a move works for any model.

### Camera paths: keyframes (engine/CameraMoves, pure C++; Peter 2026-11-22)

A path is keyframes, in the scene file itself or in a preset file. Each keyframe says where the camera is and what it
looks at, at a time t from 0 to 1 over the move (`MDur` seconds or `MBar` bars):

```
//   t    eye x y z       aim x y z    fov  [roll]
K1="0     2.0 0.4 -2.3    0 0 0        40"
K2=".5    1.9 0.6  2.1    0 0 -.3      35   .05"
K3="1     0.3 2.9  0.4    0 0 0        40"
Ease=".4"
```

- Between keyframes the camera follows a Catmull-Rom spline (eye, aim, field of view and roll alike); past the first
  and last keyframes the path continues straight, so two keyframes glide evenly. `Ease` (0..1) smoothsteps the
  timeline, so the move starts and stops gently.
- `Step="1"`: beat cuts instead of a glide: each keyframe is held for `CutB` beats (4), in order, or shuffled with
  `Rand="1"` (by `Seed`, never the same one twice in a row); `Push` (e.g. .06) pushes in slowly while each is held.
- `Side`, `Dist`, `Hite`, `Roll` and `Fov` still apply on top (mirror, distance, height, extra roll, a fixed lens).

**Presets.** `Move="name"` plays the preset `Camera/name.txt` from the model folder (your own) or from reforce's
`content/Camera/` (the same keys, nothing else). A scene's own `K` lines win over its Move preset; its own `Ease`,
`Step`, `Rand` and `Push` override the preset's. An unknown preset falls back to orbit (logged). Shipped presets,
each the move it replaced (tests check them against the old code):

| Preset | Shot |
|---|---|
| `dolly` | starts just above the hull near the back, glides forward low over the surface, then rises and pulls back to show the whole model |
| `pass` | the camera holds still off to one side; the model approaches, slides past and recedes while the camera pans to follow it |
| `orbit` (default) | a slow half orbit from the front three-quarter view, tilted slightly up, with a gentle roll sway (13 keyframes) |
| `under` | starts low in front, sweeps beneath the model and rises behind it, looking back along it |
| `flyby` | a fast close pass: the model rushes at and past the camera (a wide lens sells speed) |
| `cuts` | beat cuts: eight framings, shuffled, a new one every `CutB` beats, pushing in while held |

Other keys: `MDur` (seconds) or `MBar` (bars, when the beat clock runs; the move ends on a bar line, like `PBar`),
`Loop` (1 = repeat, 0 = hold the last frame).

### Free camera

`Move="free"` hands the camera to expressions: `CamX CamY CamZ` (eye), `AimX AimY AimZ` (target), `Roll`, `Fov`, all in
model units and evaluated per frame with the `B#` vars and `SYNC_*`. Any path also takes `CamX..AimZ` as
offsets added to it, so a scene can add a beat sway to `orbit`.

### The model's own motion

`Spin` (rad/s about up) and `Bank` (radians). Engine glow pulse on the beat: `Glow` is an expression, e.g.
`Glow="1 + .5 * BEAT"`.

## Assets

### Prep script (`tools/model_prep/prep.py`, run with Blender headless)

`blender -b IN.blend -P tools/model_prep/prep.py -- OUT.glb [--map MAP.json] [--drop NAME...] [--tex 2048] [--front -y] [--decimate R]`

1. Applies all modifiers (Mirror, Edge Split, ...), removes cameras and lights, and drops objects by name (`--drop`,
   fnmatch patterns, e.g. `"Interior*"` for interiors that only show through windows).
2. Rewrites every material as one Principled BSDF the glTF exporter understands. A per-model `MAP.json` names the
   images per material (names may be patterns), for renderer-specific node trees that can't be converted
   automatically:
   `{"Hull_*": {"base": "HullColor.png", "normal": "HullNormal.png", "emissive": "HullLights.png", "roughness": 0.45}}`.
   Without an entry, a simple Principled material is kept; otherwise the images are guessed by name (normal /
   emiss|glow|light|window / the rest is base color), the diffuse color is kept and an Emission shader becomes
   emissive.
3. Halves textures until they fit `--tex` (default 2048), optionally decimates (`--decimate .5`), exports `.glb` with
   tangents.
4. Prints a report (objects, triangles, what happened to each material, texture sizes) and the `Front="..."` line for
   the scene file: the model is not rotated; `--front` is its forward axis in Blender (default -y, Blender's usual
   front) converted to glTF axes.

The MAP.json files belong with the model in the local folder, never in the repo.

### Loader (`engine/ModelFile`, cgltf)

- cgltf (single header, MIT) in third_party. Reads meshes (positions, normals, tangents, uv0), materials (base color,
  normal, emissive, metallic-roughness, factors), node transforms; ignores animation, skins and cameras.
- Textures decode through the existing image path (render/ImageFiles, ImageIO), sRGB for base / emissive, linear
  for normals, uploaded with full mips. Memory at 2048: about 16 MB per texture with mips; a 10-texture model about
  200 MB. `reforce.Model.MaxTexture` (2048) caps it at load.
- Vertices: 48 bytes (pos, normal, tangent.xyzw as half4, uv), 32-bit indices, one draw per primitive. A 500k
  triangle model is about 20 MB of vertices.
- Loading runs on a worker (app/ModelLoader), never on the render thread. At startup the models that model scenes name
  are preloaded, up to `reforce.Model.Cache` (2), so the slideshow finds them resident. A model scene started before
  its model is resident waits invisibly and starts (fade-in and move from the beginning) once it is; a model that
  fails to load ends the scene with a log line. Resident models beyond the cache are evicted least recently used; a
  live scene keeps its own reference.

### Folders

- `reforce.Model.Dir` (default `~/Library/Application Support/reforce/Models/`): `.glb` files, their MAP.json, and
  model scene files (`.txt`, ids `Model/...`). Scenes name their model with `File="name.glb"` (relative to the folder).
- **In the default Objects slideshow** when their model is installed (Peter 2026-11-22; unlike the other solids):
  the catalog lists them with the content Objects, under one-at-a-time (`reforce.Slideshow.OneFeature`). The control
  panel's Objects card lists them too, in a **Models** group (content/Categories/Object/10 Models.txt) that matches by
  catalog id (`Include.Id = "Model/.*"`, a reforce extension to categories), with the `Objects: Models` theme behind
  its Play button (Peter 2026-11-22).
- `reforce.Model.Dir = none` turns the folder off; `--no-settings` runs (the tests) default to none, so a user's models
  never leak into test runs.

## Pipeline

- **Look A**: `SolidPipeline::encode` gains MeshKind::Model: indexed draws per primitive into the slot target with
  MSAA and depth, `model_fs`, the camera's view-projection instead of the solid convention's, the screen box from the
  model's projected bounding box (corners, clipped; when the camera is inside the bounding sphere the box is the whole
  frame). Composited by the sprite pass as today.
- **Look B**: `encodeModelColor` after colorize, before the backdrop / dancers / text (the glass slot): draws into
  the slot target with MSAA and depth (RGBA8 sRGB, premultiplied), optional emissive bloom, then over the frame with
  alpha. With `Pen` > 0 the luminance copy is also drawn (look A's pass) for the trails.
- Depth: Depth32Float memoryless, as for solids; near plane .02, far 50 (model units), so the camera can skim the hull.

## Performance (target 60 fps at 1080p on Apple Silicon)

| Cost | Estimate |
|---|---|
| 500k triangles, 4x MSAA, model covering the frame | 0.6-1.2 ms GPU (vertex-bound; one pass) |
| Measured (1080p, M-series, ~500k triangles, tests/test_render_models.cpp) | look A 0.65 ms, look B with bloom 0.85 ms GPU; 0 render allocations |
| Look B with bloom | + 0.2 ms (half-size threshold and blur) |
| Look B with trails (Pen) | + the look-A pass |
| CPU | the camera spline and a few expressions; no per-frame allocations |
| Memory | textures (about 200 MB at 2048) + vertices (about 20 MB) |

If a model is too heavy, the prep script can decimate (`--decimate .5`), which mostly costs fine greebles.

## Testing

- No model, nothing new encoded: the render goldens stay bit-identical.
- Tests generate their own small glTF (a textured, normal-mapped box with an emissive stripe, written by the test with
  cgltf's writer or by hand), so no third-party asset is ever committed.
- Unit tests: the loader (counts, bounds normalization, front / up), every camera move is finite, continuous (no jump
  between frames at 60 fps) and keeps the model's center in front of the camera; `MBar` moves end on bar lines.
- Render tests: look A probes (lit face, emissive stripe brighter than the hull, coverage edge), look B probes (the
  texture color lands within a tolerance, the rim light outlines the silhouette), 0 render allocations with a model
  live, the GPU cost at 1080p.

## Phases

| Phase | Work | Verify |
|---|---|---|
| 1 | cgltf, `engine/ModelFile` loader + normalization, the generated test model, unit tests | tests pass; goldens unchanged |
| 2 | Look A: MeshKind::Model, render/ModelPipeline (`model_lum_fs`), free camera (model scenes replace the planned `--solid-test` hook) | probes; cost; 0 allocations |
| 3 | Camera moves (engine/CameraMoves) + scene keys, `reforce.Model.Dir`, catalog, worker loading and fade-in | move tests; Peter tries his model |
| 4 | Look B: color pass, rim light, emissive bloom, Tint, trails from Pen, ColorKey | probes; Peter looks |
| 5 | Prep script + MAP.json, usage.md section, perf report at 1080p and 4K | Peter preps his own model end to end |

## Decisions (Peter 2026-11-22)

- Both looks, A and B.
- Bloom on by default (emissive only, so it is a no-op for models without glow).
- Model scenes join the default Objects slideshow when their model is installed.

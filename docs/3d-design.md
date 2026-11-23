# 3D solids: design (reforce extension)

Status: design agreed with Peter on 2026-11-15; to be built in phases (PLAN.md 5L). G-Force output stays the default:
solids only appear when started from the control panel, a Theme, or a slideshow interval you set.

## What it is

A **Solid** is a reforce-only spawnable layer, a little like a sprite:
- It is a polyhedron (tetrahedron, cube, octahedron, dodecahedron, icosahedron) or a geodesic sphere, torus or prism.
- It is animated by G-Force-style expressions.
- Its faces are textured with the **current WaveShape**.
- It is drawn into the intensity buffer **before the FlowField feedback**, so it leaves trails, takes the ColorMap
  palette and flows like everything else.

## Pipeline (option "a + c")

1. **WaveShape tile.** Each step, the current WaveShape output (`wsOut_`) is mapped again into a 512x512 R8
   tile with its own `FrameMapping`, and drawn with the existing analytic line pipeline. Line widths are scaled for
   the tile, and the tile gets mipmaps against shimmer. The main WaveShape still draws in the frame as usual
   (Peter: keep both).
   Built (phase 3):
   - **Mapping:** the tile shows the config's own square, [-1, 1]^2 in authored units (normalized x times
     `X_EXTENTS`, y times `Y_EXTENTS`; blended during a morph), centered. With `Aspc=0` that is the whole frame,
     unstretched, so a circle the config draws is round on the faces. With `Aspc=1` it is the square on the short
     axis; the long axis beyond ±1 falls outside the tile, but faces show only the tile's inscribed circle anyway.
   - **Lines:** `ScenePipeline::encodeTileLines` draws with `line_vs` / `line_fs` in the analytic AA mode (one
     over-blend per segment) into the R8 tile, with its own R8Unorm pipeline state, so it works for both intensity
     formats. Per-path AA is not used: joints that blend twice don't show once the tile is minified.
   - **Widths:** the frame's widths × tile / frame height (the content scale) × 2, at least 3 tile px. The 2x boost
     and the 3 px floor came from the pictures: at 1x the face lines read as faint hairlines after minification.
   - **Mips:** a blit `generateMipmaps` after the lines; `solid_fs` samples trilinear (clamp to edge).
   - **Resources:** the tile textures, the pipeline state and 8 extra 1 MB line buffers (`enableTileLines`, the
     tile draws once per step besides the frame) are made at startup, so there are no render-thread allocations.
   - **Cost:** the tile pass is encoded only when a live solid uses `Text=0`.
1b. **Frame tile (`Text=1`, phase 3).** The previous intensity frame (`intensity_[current_]`, the frame this
   step's flow pass reads; `ScenePipeline::previousIntensity`) is copied as its **centered square crop** (the short
   side of the frame fills it, aspect kept) into a second 512x512 R8 tile with a 4-tap filter, then mipmapped. The
   crop was chosen over the full frame: it keeps the aspect, and the center is where the action and usually the
   solid are, so the recursion shows. Encoded only when a live solid uses `Text=1`.
2. **Solid pass.** Each live solid is drawn into its own RG8 (luminance, alpha) target:
   - perspective projection (40° vertical FOV, camera at z = 4; X/Y +-1 = the frame edges at z = 0, like sprite
     coordinates; `Pers=0` gives orthographic)
   - 4x MSAA with memoryless color and depth, resolved into the target, so the anti-aliased edges land in alpha
   - back-face culling, and a depth buffer for non-convex meshes.
3. **Composite.** The target is registered as a sprite texture and drawn by the **existing sprite pass**. So each
   solid has its own blend flags, Pen/EPen layering, fades, lifetime and oldest-on-top order (Peter: own blend per
   solid). The quad covers only the solid's screen bounding box, projected on the CPU.
   Built (phase 2): the targets are frame-sized; each solid is drawn at its target's top-left corner with the
   render area limited to its pixel box (clip space shifted by the box origin), and the quad samples that
   sub-rectangle (`SpriteQuad::u0..v1`). The MSAA resolve leaves premultiplied luminance, which the sprite shader
   un-premultiplies (alpha = coverage). Solids draw after the sprites (on top), with sprite Mix 0.

`solid_fs`:
- `lum = fill + tex(uv) * (1 - fill)`
- multiplied by `mix(1, max(0, n·light), shade)`
- plus an edge term (distance to the face border, carried in the mesh's `edge` value):
  `lum = max(lum, Edge * (1 - smoothstep(0, edgeWidth, edge)))`, edgeWidth 0.08 (phase 2)
- output `(lum, 1)`; the light direction is fixed at normalize(0.4, 0.6, 1) (towards upper right, front).

**`Fill` (default about 0.15)** keeps faces visible when the wave is sparse (Peter: own blend + fill).

**Face texture source** (`Text=`): `0` = the current WaveShape (default), `1` = the previous frame (a
recursive, Droste-like look), `2` = flat shading. Per-solid WaveShape configs are not planned.

## Meshes (`engine/Meshes.{hpp,cpp}`, pure C++)

- The Platonic solids are at circumradius 1. Vertices are duplicated per face for flat normals. Each face's UVs place
  its polygon on the tile's inscribed circle, so a centered WaveShape sits centered on every face. Pentagons are
  triangulated as fans.
  Phase 3: the tile is unmirrored seen from outside, and one edge lies flat at the bottom (squares upright,
  triangles and pentagons point up). The bottom edge is the one facing world down (-y projected into the face; -z /
  +z for faces normal to y), so faces read upright at the rest pose. Phase 1 put the first vertex up with a mirrored
  u axis: cube faces showed the tile mirrored and turned 45°.
- The geodesic sphere (a subdivided icosahedron), torus and prism use parametric UVs.
- Everything goes into one vertex buffer at startup (under about 10k vertices).
- Tests: V − E + F = 2, every edge shared by exactly two faces, outward normals, unit radius.

## Scene files: `reforce.Solid.package/` (phase 4, built)

Solid scene files are our own content and are committed in `content/Packages/reforce.Solid.package/` (seven
solids; docs/usage.md "3D solids"). `content/` is laid out like a G-Force scene root (Packages, Themes, Categories) and is
read next to `scenes/` (`reforce.ContentDir`, default the source tree's `content/`; `--content DIR|none`); a scene
root's own `reforce.Solid.package` and the folder `reforce.Solid.Dir` (ids `Extra/...`) add more. Themes and
categories work as for sprites, and content themes join G-Force's in the theme list.

`SceneType::Solid` and `Layer::Solid` (a spawn layer like Particles and Sprites: `engine::isSpawnLayer`) are added.
`scene::SolidInstance` evaluates the config: `Mesh` is a literal name (`CompiledKey::text`, never compiled),
`Text` / `Pers` / `Flag` / `PDur` load once, then per frame the `B#` vars (persistent) and `X Y Z Scal RotX RotY
RotZ Fill Shad Edge Pen EPen`. `engine::SolidRun` adds the lifetime (FADER, PDur or `Object.Duration`) and clamps
the outputs; the Visualizer gives each live solid a target slot (MaxLive + 1 targets: `reforce.Object.MaxLive` at
full strength, the oldest fading out under a new one) and composites it like the test hook. Key reference: docs/scene-format.md §5.7.

`DT` is the age as a fraction of the lifetime (0..1), as for sprites and particles, not a frame step: motion uses
the age `t - START_TIME`. An example (content's Dodecahedron Beat, abridged):

```
Vers=100
Mesh="dodecahedron"  // literal name
Text="0"             // 0 = current WaveShape (default), 1 = frame, 2 = flat
B0="t - START_TIME"  // age in seconds
B1="wrap(SYNC_BEAT)"
B2="SYNC_ON * 1.2566 * (floor(SYNC_BEAT) + B1^5)"  // a fifth of a turn per beat while the clock is locked
X=".3 * sin(.21 * B0)"  Y=".15 * sin(.29 * B0)"  Z=".4 * sin(.13 * B0)"
Scal=".5 * (1 + .1 * BEAT)"
RotX=".4"
RotY=".25 * B0 + B2"
```

Defaults: `Scal` .5, `Fill` .15, `Shad` .6, `Edge` .8, `Pers` 1; no `Pen` and no `EPen` = Pen 1 (a keyed copy into
the feedback, so the solid trails); `Flag` 0.

## Rubik's cube (`Mesh="rubik"`, Peter 2026-11-22)

A Rubik's cube solved on every 8-bar line: it scrambles itself in two beats (30 turns), then undoes those turns one
per beat, the last landing on the next 8-bar line (content's Rubik Cube). Keys: scene-format §5.7.

- **Puzzle** (`engine/Rubik`): 26 cubies with a sticker color per direction. A scramble is `Moves` random face
  turns from `Seed` (quarter turns both ways and half turns; never the same axis twice in a row). The cycle is the
  scramble followed by its inverse, so `Turn = 2 * Moves` is solved; the state is a pure function of
  (Seed, Moves, Turn), recomputed every frame (at most 64 moves of 26 cubies; no allocations).
- **Drawing** (`SolidPipeline`, `rubik_fs`): MeshKind::Rubik is one cubie (body + six raised stickers). The solid
  pass draws it 26 times with the cube's model x the turning layer's rotation x the cubie's position, and the
  sticker intensities per direction as fragment bytes (`< 0` = plastic). The screen box comes from the cube's
  corners and the turning slab's corners.
- **Colors** (`engine/StickerColors`): solids are intensity, so a sticker's color is the ColorMap at its intensity.
  Measured over the 212 PNG maps (closest pair of stickers or sticker vs background, CIELAB): evenly spaced fixed
  levels give a median of ~9-10 and 100-123 maps under 10 (hard to tell apart); the best single fixed set is no
  better. Picked per map, the median is 31 and only 3 maps stay under 10 ((Black), Cloudscape, Black on White).
  So the Visualizer's StickerPicker picks six intensities from the effective palette (MapFunction LUT, then the
  palette), net of each level's "wobble" (color change within +-3 levels, so anti-aliasing and trails don't fringe
  into other colors), clear of the plastic (intensity 0); the closest pairs go on opposite faces (never adjacent
  on a solved cube, like white / yellow on a real one), and faces keep their colors as far as possible when the
  map changes. Hysteresis (a new set must score 10% + 1.5 better), eased over 0.5 s; 0.09 ms per pick, at most
  every 0.1 s and only while the palette changes. Log: `Rubik colors: ...`.
- **Drawing over the frame** (`Over = 1`): the sticker colors only hold if the intensities land exactly. The keyed
  copy (Flag 0) drops the dark plastic, and AlphaBlend (16384) is G-Force's screen blend F + x (1 - F), which pushes
  a cube redrawn over its own trail towards white. Over = 1 uses the sprite pass's premultiplied over with the
  solid's coverage as alpha for Pen (feedback) and EPen (display).
- **Anti-aliasing in color** (Peter saw dotted lines at the sticker borders, 2026-11-22): a border pixel resolves to
  an intensity between the sticker's and the plastic's, and the ColorMap gives that its own color (China: pink dots
  along a blue sticker). So the rubik is also drawn in color over the colorized frame (`solid_color_fs` in
  encodeColorize): rubik_fs writes the sticker share to the target's blue channel (targets are RGBA8 since), and
  each pixel mixes the ColorMap colors of its sticker (intensity = r / b) and of the plastic by their shares,
  alpha = coverage. The intensity copy still goes into the feedback for the trails. Test: a palette black / green /
  red shows no green on the cube.
- **Color key** (`ColorKey = 0`): the camera backdrop and Syphon alpha key out dark colors, which would punch
  holes in the dark stickers and the plastic. A ColorKey = 0 solid's coverage x opacity raises the tracked alpha in
  encodeColorize (alpha-only max blend from the solid target's alpha), so the whole cube stays solid over the camera.
  The cube's trails in the feedback are still visuals and keyed. The same path could serve luminance movie sprites
  (Color = off), which are always keyed today.

## 15 puzzle (`Mesh="fifteen"`, Peter 2026-11-22)

A 15 puzzle solved on every 8-bar line (content's Fifteen Glass; the solid-block Fifteen Puzzle scene was removed once Peter chose the glass, 2026-11-22, but `Glas` 0 still draws blocks): on the line the tiles leap from the solved board
into a scramble (the first three quarters of the beat, hopping towards the viewer so they pass over each other), then
slide back one tile per beat, 31 slides landing on beats 2..32. Keys: scene-format §5.7.

- **Puzzle** (`engine/Fifteen`): a 4 x 4 board. A scramble is a random walk of the blank from `Seed` (never straight
  back); of 24 walks the one leaving the tiles farthest from home (summed Manhattan distance) is kept, and the
  solve is that walk backwards. `Turn` 0 = scrambled .. `Moves` = solved; `Shuf` 0..1 = the leap (tiles from their
  solved places to the scramble). A pure function of (Seed, Moves, Turn, Shuf).
- **Drawing** (`fifteen_fs`): MeshKind::Fifteen is one box with uv on its top face; the solid pass draws the 15
  tiles as .4-thick cuboids in their color, top and sides (no tray: Peter, the gaps and the blank show the visuals).
  Each solved-board row has its own color (the puzzle colors' 0, 2, 4, 1: one of each opposite pair first, the
  farthest apart). The numbers are analytic seven-segment digits (anti-aliased with fwidth), centered on the tile by
  their lit strokes; a right-hand "1" is spaced as half a digit wide ("11" looked cramped).
- **Numbers that always read, without flipping** (Peter: per-tile dark / light numbers flipped and mixed, which was
  distracting): r holds the tile's intensity under the strokes too and b the share outside them, so the color pass
  knows the tile color in every pixel and draws the strokes in it pushed far darker (x .15). The row colors come
  from their own picker (engine::TilePicker, not the rubik's): four colors, all light (L* >= 45), so dark numbers
  always read. Only on a map without good light colors (the light set scoring under 8 and the dark one better) the
  whole board takes four dark rows (L* 15..40) with light numbers (80% towards white). Decided only when the
  ColorMap changes, with hysteresis; colors and numbers fade over 0.5 s. Over the 212 PNG maps: light 176, dark
  36. Log: `Fifteen colors: ...`.
- **Shading in color** (both puzzles): shading an intensity changes its color, so rubik_fs / fifteen_fs write the
  shading factor (Shad) to the target's alpha channel and leave the intensity alone; the color pass multiplies the
  color by it (a / coverage). Tops and sides then differ in brightness, not hue. The intensity copy for the trails
  is unshaded.

## Glass (`Glas = 1`, 15 puzzle; exploration, Peter 2026-11-22)

Peter wanted softer, translucent tiles that don't hide the visuals (the WaveShape), maybe liquid-glass blur.
Content's Fifteen Glass. A glass puzzle is not drawn into the visuals (no Pen / EPen stamp, no solid target): the
Visualizer keeps a `GlassDraw` per step and `colorize` calls `SolidPipeline::encodeGlass` right after the colorize
pass (before the backdrop, dancers and text):

1. The colorized frame at half size (4 bilinear taps), then a separable 13-tap Gaussian (sigma = Fros at 1080p) into
   a second half-size texture. Three half-size BGRA textures, made at startup.
2. The buttons in 3D (Peter: the first, flat panes looked flat from the side and their edges aliased): each tile is
   an `engine::makeGlassButton` mesh (rounded square .94 wide, corner radius .17, .32 high, top edge rounded with
   radius .13, flat cap), drawn with depth and 4x MSAA into the puzzle's solid slot target (cleared, resolved
   premultiplied), then laid over the frame (`glass_composite_fs`). `glass_fs` uses the real surface normal: its
   tilt across the view moves the sample (Bend; the rounded edge and the walls show what lies beside the button,
   like a lens edge); half sharp, half frosted (Fros); one light tint for every button (Peter: all the same color,
   mostly letting the visuals through), a light color of the ColorMap halfway to white (Tint, a little more through
   the walls); glass at a grazing angle darkens slightly and catches a Fresnel sheen; a broad soft highlight from
   the upper left; the number on the cap.
3. **Readable numbers on any background**: etched near-white numbers, the same on every button, on a soft dark halo
   (60% darker within .1 of the strokes, applied after the clamp so it still shows over white). An earlier version
   kept tinted panes readable with a brightness floor / ceiling per row; with clear glass the halo does it.

Cost: three half-size passes per colorize, the 15 buttons (540 triangles each) with MSAA, one full-frame composite. ColorKey 0 raises the Syphon alpha by the panes'
coverage. Buttons see the frame without the other buttons (one passing over another during the leap shows the frame through
it, not the button below).

## New variables, for all scene types (Peter)

`SYNC_BEAT`, `SYNC_BPM`, `SYNC_LOCK` and `SYNC_ON` come from `sync::BeatClock`: Link when a session runs, else the
audio phase lock. They are reforce extensions next to `LINK_*` (src/expr/Vars.cpp), native for every per-frame scene
type: WaveShapes, Particles, Sprites and Solids (ColorMaps and FlowFields are not evaluated per frame with audio, so
they read 0 there, like `LINK_*`). The clock is stepped first in each sim step, so every config sees the same
values. G-Force scenes don't reference them, so nothing changes for them (scene-format §3.5).

## Slideshow, panel and settings

- **Slideshow:** solids are not in the default slideshow (Peter: manual + Themes). They are started by command
  (`switch:solid:NAME`, `next:solid`, `kill:solid`), the panel (phase 5) or a Theme:
  `Object.Slideshow.Interval.Duration` defaults to 0, and a theme file may set it (reforce key), e.g. content's
  "Solids" theme (30 s). Switching to a theme without it restores the setting's interval.
- **Settings:**
  - `Object.Slideshow.Interval.Duration` (0), `Object.Duration`
  - `reforce.Object.MaxLive` (1; was 3 until 2026-11-22), `reforce.Solid.TileSize` (512), `reforce.Solid.Msaa` (4 | 1)
  - `reforce.Solid.Dir`, an extra folder of solid scene files.
- **Control panel:** a "Solids" card cloned from Sprites: library, start, kill, interval and duration. It reuses the
  `Switch` / `Kill` / `SetSlideshowInterval` / `SetSpawnDuration` commands with `Layer::Solid`.

## Performance (target 60 fps at 1920x1080 on Apple Silicon)

| Cost | Estimate |
|---|---|
| WaveShape tile lines | ~0.02 ms GPU |
| MSAA solid covering ~30% of the frame | ~0.1–0.2 ms |
| Bounding-box composite | < 0.05 ms |
| CPU | a few expressions per solid |
| Memory | 3 frame-size RGBA8 targets (8 MB each at 1080p, 32 MB at 4K; RG8 until the rubik's color pass) + a 256 KB tile |
| Measured (phase 2, 1080p, M-series) | solid pass 0.03 ms, with the composite 0.10 ms GPU per solid |
| Measured (phase 3, 1080p, tile 512) | WaveShape tile (4096 segments + mips) 0.055 ms, frame tile 0.058 ms; tile + a Text=0 icosahedron composited 0.13 ms GPU |

- Pipeline states are created in the constructor (or prewarmed on a worker), so there are no render-thread
  allocations (`perf::renderAllocs() == 0`).
- Measure with `--perf-report`, and with `--stress` once solid spawns are added to it.

## Testing

- With no solids nothing new is encoded, so the existing render goldens stay bit-identical.
- `tests/solid_render.cmake`: a Void FlowField, the Gray ColorMap and a cube with `Text=2` at a fixed rotation.
  It checks pixel probes and the silhouette area within a tolerance; MSAA can differ across GPUs, so no hashes.
- Unit tests for the meshes, plus a CPU projection reference for the bounding box.
- Phase 3, in `tests/solid_render.cmake`: a cube left of center and a WaveShape of vertical bars in the frame's right
  half. With `Text=0`, the face's left part is flat at the Fill floor (.15 -> 38, .4 -> 102) and its right part
  carries the bars. With `Text=1`, the left part shows the frame (the cube itself). In `reforce_render_tests`: the
  tile's lines and mip chain, the Fill floor before any tile is drawn, the cost at 1080p, and 0 render
  allocations with a Text=0 solid live.

## Phases

| Phase | Work | Verify |
|---|---|---|
| 1 | Meshes and math (+ unit tests), projection and bounding-box reference | tests pass; goldens unchanged |
| 2 | `render/SolidPipeline` with a flat texture: a hard-coded solid behind `--solid-test`, MSAA and depth, composite via the sprite pass | solid_render probes; 0 render-thread allocations; < 0.3 ms GPU |
| 3 (built) | WaveShape tile (`Text=0`) and frame texture (`Text=1`) | Peter looked (2026-11-15): good; prefers Text=0; 0.5 rad/s spin |
| 4 (built) | Scene files, `SolidRun`, the `SYNC_*` variables, package, settings, Themes | every content solid compiles without a diagnostic and stays finite; a scene file renders bit-identical to the test hook at the same pose; a moving Text=0 solid renders the same twice; a theme starts solids; Dodecahedron Beat lands on whole beats (tests/test_engine_solids.cpp, solid_render.cmake) |
| 5 | Control panel Solids card, `--stress` spawns, perf report at 1080p and 4K | perf numbers |
| later | A full-color 3D layer after colorize; depth or normals feeding the FlowField warp | — |

## Risks

- **Keyed copy:** it drops pixels darker than about 10/255, which `Fill` handles. Strong FlowFields smear solids;
  an EPen-only solid draws a clean object.
- **Aliasing:** thin tile lines alias when minified, which the tile's mipmaps handle.
- **Layer list ripple:** adding a layer changes `kLayerCount`, which ripples into the web UI arrays and the Director.

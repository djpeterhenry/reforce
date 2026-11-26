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

## Knight's tour (`Mesh="knight"`, Peter 2026-11-23)

Peter: "a Knight's tour with a nice stylistic Knight". A chess knight visits all 64 squares of a board, one hop per beat,
so a tour fills a 16-bar phrase; the 64th hop returns it to its first square (a closed tour) exactly on the phrase line,
leaving a web of lines through every square. Then the web unravels during the first hop of the next tour. Content's
Knight's Tour (any closed tour, grid lines since 2026-11-23, three-quarter view) and Knight's Web (symmetric tours, grid lines
only, seen from higher up). Keys: scene-format §5.7.

- **Tours** (`engine/Knight`): squares row by row from a1; every tour is closed and starts on g1 (`kHome`), so tour
  after tour continues where the knight stands. `Sym 0`: randomized Warnsdorff from g1 (the unvisited neighbor with the
  fewest onward moves, ties at random, and early on now and then one move worse: pure Warnsdorff finds only a few dozen
  closed tours from one square), up to 3000 tries until the last square is a knight move from g1 (~6 % of tries close;
  about 15 µs a tour). `Sym 1`: half-turn symmetric tours (square s and 63 - s are 32 hops apart): a depth-first search
  over half tours of 32 squares holding one of each pair {s, 63 - s} whose end is a knight move from the start's
  partner, neighbors in Warnsdorff order (ties shuffled), at most 3000 nodes per try and 48 tries (median ~40 nodes).
  Quarter-turn symmetric closed tours don't exist on 8 x 8 (only on 4k + 2 boards), so half-turn is the most symmetric
  web there is. Each tour then gets one of the board's eight symmetries (more variety), rotated to start on g1; a fixed
  symmetric tour is the fallback (never needed in the tests: 600 seeds). `TourCache` (the Visualizer's) keeps the last
  four, so a tour is searched for once.
- **Hops** (`frameAt`, a pure function of tour, previous tour and `Turn`): hop k lands on the whole turn k. Within a hop
  (phase p): the landing settles (squash .13 over p .. .22), the knight turns towards the next square (p .3 .. .75,
  smoothstep, the shorter way; at turn 0 from the previous tour's closing heading, so there is no jump at the phrase
  line), crouches (.36 .. .5) and flies (.5 .. 1): eased along the ground, a parabola .85 squares high, stretched 6 %,
  nose up then down. `Hovr` lifts it .28 with a bob (a build). Junk turns clamp to 0..64; NaN is 0.
- **The knight** (MeshKind::Knight, `engine::knight::makePiece`): modeled in code as a distance field and polygonized
  once at startup with surface nets (12.5 mm cells on a 1-unit piece, two Newton steps onto the surface, normals from
  the field's gradient): a lathed base (foot, bead, cove, collar), the head and neck as a rounded extrusion of a
  Catmull-Rom side profile (chest, throat cut under the jaw, muzzle reaching forward and down, forehead, poll) whose
  half width narrows to the muzzle and swells at the cheeks, two ears leaning out, a mane ridge along the back carved
  with slanting grooves, eye sockets with eyeballs, nostrils and a mouth groove. ~40k triangles, closed (every edge
  shared by exactly two triangles), 1.07 high, base radius .375; drawn 1.1 squares per unit, facing its heading.
  An earlier version with bead-like mane locks, a jaw-line groove and a mouth cut right through looked lumpy and aliased
  close up; the field's sharp creases (clamped width terms) showed as seams, so the widths are smoothsteps.
- **Board and web** (`knight_fs`, drawn into the solid's target like the other solids, so Pen puts it into the
  feedback: trails and the ColorMap; the content scenes add EPen so it also stays crisp on top): the squares as one
  mesh (Brd: grid lines .022 squares each side of a border, light squares .16, dark .07; nothing drawn where both are 0,
  so the visuals show through); the web as flat quads .075 squares wide just above the board (each newer one a little
  higher, so it stays on top where lines cross), level `.68 + .32 exp(-age / 6 hops)` (the newest brightest), the hop in
  flight drawing its line under the knight; dots on the visited squares, a ring on the start square and a ring spreading
  from each landing (half a hop). The previous tour's web unravels from its first line during the first .75 hop.
  `Flsh` lifts the web towards 1 and widens it 70 %. Line art in the solid pass rather than through an OrganicCanvas: it
  shares the board's perspective, depth (the knight passes in front of and behind lines) and the solid's Pen / EPen /
  fade with no new integration.
- **Camera**: the solid's pose (RotX tilts the board back, Scal = the board's corner), `Orbt` turning the board about its
  own normal first (an orbiting camera), `Folw` sliding the board back by part of the knight's offset (follows it a
  little; the web stays readable). The screen box is the board's square up to 2.6 squares above it (the whole frame
  when a corner is behind the camera).
- **Cost** (1080p, M-series, `solid knight` pass with the whole web, 4x MSAA): ~0.2 ms GPU; about 200 small draws.

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

## Labyrinth (`Mesh="labyrinth"`, Peter 2026-11-23)

Peter: "a top-down labyrinth style puzzle ... 3D rendered with the board tilting and the ball rolling with some actual
physics through a maze to get to the goal. The board tilts or ball hitting the wall ... aligned with beats, and
completion of the whole puzzle at the end of a phrase." The wooden tilt-maze toy, our own design: a board with raised
walls, a steel ball, a goal hole. Content's Tilt Maze (11 x 11, solved on every 16-bar phrase line; it was Tilt Maze Long until 2026-11-23,
when Peter kept it over the 8 x 8, 8-bar one). Keys: scene-format §5.7.

- **Maze** (`engine/Labyrinth`, `makeRoute`): a perfect maze (a spanning tree of the grid) by a recursive backtracker
  that keeps its direction 55% of the time where it can (longer straight runs, fewer turns), from `Seed`. The start
  is a dead end at one end of the tree's longest path; the goal is the cell whose route fills the run best (below),
  then a dead end, then the longest route. Six candidate mazes per seed, the best kept. Trap holes (`Traps`) go into
  dead ends off the route, away from the start and the goal: the classic look, but the ball never comes near one
  (nobody loses).
- **Route and timing**: the route is cut into straight segments, each ending against a wall (the cell beyond is walled:
  a clack), braked (the corridor goes on past the turn: the board tilts back and the ball stops on the cell) or in the
  goal. Every segment starts on a whole beat (the board tilts) and ends on a whole beat (the clack, the stop, the drop).
  Each gets at least the beats its distance needs at the steepest planned tilt (.65 of the limit, through the board's
  response; a braked stop at least two beats), then beats up to its "natural" length (3.5 cells / beat^2); beats left
  over become pauses after a segment (the ball rests, the board eases off), spread evenly. The goal is chosen so the
  route fits the run (11 x 11 / 64 beats: ~27 segments; 30 before the realism pass below, so a few more two-beat rolls
  and rests).
- **Physics, in beat time** (`Player::step`): a rolling ball on a tilted plane, a = 5/7 g sin(tilt) per axis
  (g = 40 cells / beat^2: at 120 BPM a 6 cm cell), rolling resistance, a speed limit of 60 cells / beat. The walls are
  boxes (the cell borders, thickness .16; ball radius .28, so .14 of play either side of a corridor's center line).
  Fixed step 1/240 beat; moves are cut into sub-steps of at most .04 cells and the time of impact is bisected, so the
  ball cannot pass a wall at any speed (tested with random kicks up to 1e9 cells / beat and tilts far beyond the
  limit: never inside a wall, never across one). The goal hole: over it the ball rests on the rim, is pushed inwards
  and drops; the drop is its center passing half its radius inside the rim. Running in beats makes the motion a pure
  function of the beat position: a tempo change needs no re-plan (the ball just rolls faster or slower, as if g
  changed with the tempo).
- **Collisions** (Peter 2026-11-23: "collisions are dead"): restitution .42 (steel on wood) above .12 cells / beat
  into the wall (slower: resting contact, no bounce), and Coulomb friction along the wall, at most .12 x the normal
  impulse, so a glancing hit keeps sliding (1 into, .5 along: it leaves at .42 out, ~.33 along) instead of stopping. A
  head-on clack under the hand's lean rebounds and comes back in smaller ticks ("clack-tick-tick", tested: three hits,
  each softer, then at rest against the wall); in a corridor the ball can rattle between the side walls.
- **The board** (Peter: "the board snaps"): it follows the hand like a wrist, a second-order response (natural frequency
  7.5 rad / beat, damping .9: about .13 beats to get there) with its tilt rate limited to 1.5 per beat (sin of the tilt).
  No snapping and no jolt: the old version pressed at the steepest tilt after every clack; now the hand just keeps
  leaning into the wall and eases off. The tilt is continuous and its rate bounded (tested: the fastest change in the
  sweep .66 / beat).
- **The player** (`Player::simulate`; Peter: "the ball moves on rails"): a human hand instead of a robot. Along the
  segment it commits to one tilt and plays a planned move with it (`tiltShape`): into a wall it tilts, on a long roll
  tilts back mid-way so the clack is not too hard (`brakeShare`: about 1.7 cells / beat), and leans into the wall again
  a quarter beat before the clack (the board takes that long); for a braked stop it tilts, tilts back, and as soon as
  the ball would stop within .2 beats it levels the board so the ball comes to rest on a level board (then holds it
  there loosely); for the goal it tilts and eases to level as the ball rolls in. It starts each tilt .15 beats before
  the beat (the next segment's opening is played at the end of the one before), so the board is moving on the beat.
  Across it is loose: it sees the ball .04 beats late, leaves drifts under .035 cells alone, corrects bigger ones
  gently (2.5 rad / beat, times a per-segment 0.75..1.25: it over- and under-corrects), with a slow tremor (a few
  thousandths of tilt); right after a clack it first keeps the ball against that wall. Lining up for the goal it is
  careful (no dead band). So the ball wanders across its corridor and grazes the side walls (tested: mean drift .058
  cells, at most the full .14 of play, never off the route's cells). The hand is seeded per run and segment, so
  everything stays a pure function of (Seed, Grid, Beats, Traps). The locking is a root search on the committed tilt
  (secant steps, bisection once bracketed): for a clack or the drop the event time against the beat line, for a brake
  where the ball comes to rest against the cell's center.
- **Accuracy** (180 runs: grids 4..12, 16 / 32 / 64 beats), before / after the realism pass: the drop 1.4e-4 / 4.8e-5
  beats off the line; clacks 1.6e-6 / 5.0e-5 beats off their beats; braked stops at rest within .004 / .087 cells of
  the cell's center (the looser hand; well inside the cell's .5).
- **Lazy, cached planning**: a segment is simulated and stored (5 floats per step: ball x, y, z, tilt x, y) when the run
  first reaches it; a frame is then a lookup with linear interpolation. A 64-beat 11 x 11 run costs ~10 ms to plan in
  all (~.35 ms a segment; the robot's took ~4 ms), so in normal play a frame pays for at most one segment. A call plans
  at most two segments: after a jump (an object started mid-phrase) the plan catches up over a few frames (8 frames
  from beat 37 of 64), the board level and the ball resting meanwhile (`Frame::catchingUp`, logged once per solid).
  The plan depends only on (Seed, Grid, Beats, Traps), so it is deterministic, and frames played one by one are the
  frames of planning ahead (tested over whole runs, start and run change included: never behind).
- **Cost per frame** (Peter 2026-11-24: a 118 ms stall at every new maze, ~7 dropped frames per phrase live): choosing
  the goal asked for segment durations thousands of times, and each one simulated the tilt through the board's
  response (`tiltGuess`): ~60 ms per route, twice at a run change (the new maze and the one sinking). The durations
  depend only on the stop and the length, so they are a table made once (~.3 ms, the first time); a route now costs
  ~.2-.5 ms, and the sinking maze is the run just played (kept, not made again). No worker thread was needed: the
  most expensive `SolidRun::evaluate` over two content runs, start, run changes and a mid-phrase start included, is
  ~.9 ms (test, PERF_BUDGET 3 ms); `scripts/perf-sweep.py --seconds 40 --only Tilt --no-crossfade`: CPU worst 1.24 ms
  (was 118), p99 .48 ms.
- **Between runs** (the interlude, the first two beats of a run): the solved maze (the previous seed's) sinks into the
  board over half a beat while its ball finishes dropping, its holes close; the new maze's walls rise and its holes
  open over the next half beat; the ball is thrown up out of the start (a small ball return, too small to fall into)
  at beat 1, lands and bounces once; the first tilt is on beat 2. The goal segment lets the board level out, so the
  tilt is continuous across the line.
- **Drawing** (`labyrinth_vs` / `labyrinth_fs`): the board is a .3-thick slab, its corner at the circumradius (`Scal`);
  the walls are one box per run of walls along a grid line (a 12 x 12 maze: ~50 boxes); the holes are black discs and
  the goal and start rings flat rings (`engine::makeFlatRing`, real geometry so MSAA smooths them); the ball the
  geodesic sphere. All tilted about the board's center by the physics' tilt times `Tilt` (content 2: 3° of physics
  reads as 6°; the ball's place on the board is unchanged). The ball is polished steel in its color: a bright "sky"
  above, darker below, a white highlight, a glint around the rim just after a clack; it casts a soft shadow on the
  board.
- **Colors** (`engine::TrioPicker`): three intensities picked for the current ColorMap, as far apart as possible
  (net CIELAB distance, wobble as for the stickers) and all clear of black (the holes); by lightness the darkest is
  the board, the lightest the walls (their lit tops read against the board), the third the ball. Hysteresis and a
  0.5 s ease as the stickers; ~0.1 ms per pick (an exhaustive search over every third level, pruned). Log:
  `Labyrinth colors: ...`.
- **In color** (`solid_color_fs` mode 2): as the puzzles (r = intensity x share, g = coverage, b = share, a = shading),
  with two changes: share 0 is black (the holes), and the shading is stored halved, 0..2, so above 1 the color mixes
  towards white (the ball's highlight). An anti-aliased edge between two of the three intensities takes a mix of those
  two colors, not the ColorMap's colors between them (a ball over the board had a dark ring on some maps). The board's
  top writes `Floor` as its coverage, premultiplied, so the color pass lays it over the visuals at that opacity: the
  visuals show through (Peter liked that for the puzzles). Content draws with `Over = 1`, `Pen = 0` (nothing into the
  feedback; Over with Pen 0 now means that, rather than a dark copy) and `ColorKey = 0`.
- **Drops and builds** are content: a drop makes the board jump (Scal), a build tips it towards the viewer (RotX). The
  timing is untouched by them.
- Cost at 1920x1080: the solid pass .04-.2 ms GPU (12 x 12, MSAA 4; shared GPU), CPU per frame with planning under
  .3 ms. Tests: test_engine_labyrinth.cpp (perfect mazes, timing on beat and phrase lines, no tunneling, determinism,
  junk input, the content scenes at two tempos), test_render_solids.cpp (labyrinth, in color, cost).
## Soma cube (`Mesh="soma"`, Peter 2026-11-23)

Peter, from the list of puzzles that could be solved on beat and phrase: "I'm curious to see what the Soma cube looks
like." Seven pieces fly in and lock together on the beat into a figure; on the line it bursts apart and builds the
next (content's Soma Cube: the cube every round, a new solution each time; Soma Figures, a different figure every 8 bars, was removed 2026-11-23: Peter liked the cube). Keys: scene-format
§5.7.

- **Pieces** (`engine/Soma`): the L-tricube V and the tetracubes L, T, Z, the screws A and B (mirror images of each
  other) and the branch P; 27 cubes. Pieces turn by the 24 proper rotations only, so A and B stay distinct.
- **Solver**: exact cover with Dancing Links (Algorithm X), 7 piece columns + 27 cell columns, a row per placement
  (each distinct orientation at each translation inside the figure; the cube has 688). Fixed storage (~0.6 MB, made
  at load), so a solve allocates nothing. Counts, tested (tests/test_engine_soma.cpp): the cube has **11,520**
  solutions with the cube fixed in space, **480** up to its 24 rotations and **240** up to rotations and reflections
  (a reflection turns screw A into screw B, so a reflected solution is again a solution with the pieces' own
  rotations: 11,520 = 48 x 240, Conway and Guy's "240 essentially different" solutions). Enumerating all takes about
  0.1 s; the player never does: each round walks the solutions of one search with the placements tried in an order
  shuffled by a hash of the round, until one assembles with every piece dropped in from above and a clear burst
  (below; mean 0.3 ms for the cube, worst about 2 ms), so rounds of the cube show different solutions (29 essentially
  different ones in 40 rounds).
- **Figures** (27 cells each, y up, centered on their bounding box): cube, pyramid (5 x 3, 3 x 3, a ridge of 3),
  steps (2, 3, 4 high), bathtub (5 x 3 floor, a rim), sofa, wall (6 x 2 x 2 with three merlons), tower (3 x 3 base,
  2 x 2 four floors up, a 1 x 2 roof). Every one solves and assembles for every seed tested.
- **Assembly order**: bottom-up and physically plausible. A depth-first search over the 7 pieces keeps an order in
  which each piece, when placed, rests on the floor or on a placed piece (a cell directly below) and has a clear way
  in: nothing placed above it (dropped from above) or else nothing beside it along one horizontal direction (slid
  in). Ties go to the lowest piece, then the round's hash. Plans where every piece drops in from above are preferred
  (every cube plan tested is one).
- **The round** (Turn = beats into it, Seed = the round; `Len` 32, `Hold` 8, `Each` 2): beats 0..8 the previous
  figure stands complete (the scene's slow orbit; a glow and a thump from the lock on the line); 8..9 it bursts into
  the **ring**; then the pieces hover and tumble (each about its own axis) until their turn; piece k flies for 2
  beats and locks on `Len - (6 - k) Each` = beats 20, 22, .., 32. On the lock the piece lights up (the shading
  flattens for a moment) and the placed pieces dip 0.08 cells, decaying. The state is a pure function of (figures,
  Seed, Turn, timing, Expl, Tens): a clock jump just shows the right frame, solving at most two rounds (the player
  caches the last four).
- **No two pieces meet** (Peter 2026-11-24: pieces sometimes passed through each other). Measured first
  (tests/test_engine_soma.cpp sweeps every 1/32 beat of 48 cube rounds, 14 rounds of every figure and 24 rounds with
  Expl / Tens, every pair of pieces as their unit cubes, oriented boxes by separating axes, touching faces allowed):
  7,069 overlapping pairs in 49,200 cube frames (flight / placed 2,744, burst / burst 2,059, flight / hover 975,
  hover / hover 667, approach / hover 531, hover / placed 93; with Expl also placed / placed 2,638). Now 0 in all of
  them, by construction and by checks made when a round is planned:
  - *The ring* is in the figure's own space (it was an ellipse in view space, which moved with the camera and could
    not be checked ahead): a flat ring a little below the figure (seen from above at 3/4 it frames the figure and
    its front pieces stay below it), its radius keeping every piece's bounding sphere outside a vertical cylinder
    around any of the player's figures (plus room for Expl, Tens and a .3-cell margin) and the seven slots apart by
    two bounding spheres (cube: 5.7 cells). Hovering, bobbing, trembling and tumbling stay inside those spheres. The
    slots go round in the order of the pieces' angles around the figure's vertical, so each bursts out on its side.
  - *The burst* keeps each piece's orientation and is checked when the round is planned, cube by cube every 1/128 of
    it: first an exploded view (each piece out along its own direction from the figure's center, 2.5 or 4 cells, up
    bias 0..2, together or staggered top / outer / bottom first), then on to its slot; or straight to the slot. The
    first mode that keeps the pieces apart is kept (the plain exploded view for almost every cube plan); a solution
    without one is skipped.
  - *A flight* is a crane's path: it rises straight up beside the ring while it turns into place (its sphere stays
    clear of its neighbors' and of the figure's cylinder), crosses over the top (its sphere above the figure, and
    anything Expl lifts, by the margin), then drops straight down into place, accelerating into the lock: its column
    is clear because no earlier piece lies above it (the assembly order). A piece that slides in comes down beside
    the figure, clear of its bounding box, and slides in.
  - *Expl* (0..1) moves the placed pieces (and a piece coming down into place) a little way along their own checked
    burst paths, about a cell at 1, so they part without meeting.
- **SYNC_DROP / SYNC_BUILD**: `Expl` opens the figure up along the burst paths (above; a drop opens it and it closes
  as SYNC_DROP decays); `Tens` makes the hovering pieces tremble (3 Hz, .07 cells) and pulls the ring in by .25 cells.
- **Drawing** (`soma_fs`): MeshKind::Soma is one full unit cube with per-face uv and its face direction in `edge`;
  the solid pass draws each of the 27 cubes with the figure's model (RotY first, then RotX, RotZ: an upright orbit)
  x the piece's place x the cube's cell. The cubes of a piece meet flush; each face's four borders come from
  `engine::soma::edgeCodes` (per piece and cube, from the piece's shape): the piece's outer edges are rounded (the
  normal tilts towards the edge over .09 of the face, 45 degrees at it) with a dark groove (x .4 over .012), so the
  pieces read apart; a seam between two cubes of one piece is a faint line (x .82 over .008); concave folds get
  nothing. The light comes from the upper left front (tops brightest), with an ambient of .38.
- **Colors** (`engine/PieceColors`, PiecePicker): seven intensities picked per ColorMap like the rubik's stickers
  (CIELAB distances net of the wobble, greedy farthest-point picks and refinement), with no plastic but the
  background (intensity 0) to keep clear of (x 1.25), and only colors with L* >= 40 (lowered to 30, 20, 0 on maps
  without 21 such levels), since the shading darkens them. Hysteresis and a 0.5 s ease as for the stickers; each
  piece keeps its color as far as possible (the best of the 5040 assignments); on a map that changes over time it
  also switches when a piece's color has turned dark. Shading goes to alpha, the color pass multiplies the color
  by it (as for the rubik), so tops and sides differ in brightness, not hue. Log: `Soma colors: ...`.
- **Cost** (1080p, M-series, tests/test_render_solids.cpp): the soma (27 cubes, 4x MSAA, from the hold to the build)
  drawn and composited 0.15 ms GPU; no render-thread allocations.
- **Later options**: a glass variant like the 15 puzzle's `Glas = 1` (each cube as a glass block over the frame);
  more figures (any 27-cell set that solves and assembles; the tests check every figure in the list).

## Jigsaw (`Mesh="jigsaw"`, Peter 2026-11-23)

Peter, from a list of puzzles solved on the beat and phrase: "Jigsaw of the live frame sounds interesting. I'd be
curious what that looks like." The frame is cut into jigsaw pieces that always show the visuals from their home place,
so a scattered piece is a moving patch of picture out of place and the picture assembles as pieces land, while what is
under it keeps moving. Content's Jigsaw and Jigsaw Mini (Objects: Puzzles). Keys: scene-format §5.7.

- **The cut** (`engine/JigsawShape.inc`, one source compiled as C++ for the engine and tests and stringified into
  the Metal shaders, so both cut the same pieces): `Pcs` pieces in a grid fitted to the frame's aspect (32 = 8 x 4
  at 16:9, 12 = 4 x 3; at most 16 x 12 = 160). The grid's corners are jittered (7.5% of a cell; border corners only
  along their border), each interior edge bends in a gentle wave that is 0 at its corners and carries a knob in its
  middle 50%: a round head on a narrower neck, joined by smooth unions (polynomial smin), sticking out to a random
  side, with a little random size, height, skew and neck width, so it looks hand-cut. Each edge is a signed field
  (< 0 on one side); a tab is smin(F, K), a blank the same curve seen from the other side, and a piece is the max of
  its four edges with their sides' signs, so the pieces tile the frame exactly (tested on a grid of points: every
  point in exactly one piece; every tab in its neighbor's blank). Away from its knob an edge returns the line at
  once (an exact bound), which keeps the GPU cost down. A new cut per `Seed`.
- **The cycle** (`engine/Jigsaw`, a pure function of Seed, the config and `Turn` = beats into the cycle, so any jump
  of the clock shows the right state; plans are cached per seed, no allocations per frame): whole from the line (the
  previous cycle's cut, its seams flashing and fading out over two beats) until `Brk` (4: bar 2 of the phrase); then
  every piece lifts and bursts out in about a beat (fast at first, high in the middle so pieces pass over each other,
  a little staggered) to a resting spot over another piece's slot, turned up to ±.4 rad, layered; then the pieces fly
  home in solve order (`Ordr`), each in .85 beat: lift, glide on an arc while turning upright and tilting into the
  motion, then drop and snap in exactly on its beat, with a flash on its seams. The first lands on `Brk` + 2, the last
  on the next line (`Cycl` = 32 beats). More pieces than beats: the first beats take one more each (a rush of easy
  pieces: 32 pieces from beat 6 to 32 = two per beat on 6..10, then one per beat); fewer: pauses spread evenly.
- **The pile** keeps the assembled part clear: each piece rests over a slot that is still empty (fewest others on
  it, preferably one filled late), and when a piece is about to land on a slot, whatever lies on it hops aside to
  another empty one (.4 beat, ending .1 beat before the snap). Only the last piece or two, with nothing else empty,
  stay where they are. With edges first the frame forms around a pile in the middle, like a person doing it.
- **Solve order** (`Ordr`): 0 like a person (the four corners, the border growing from them, then inwards where most
  neighbors are placed; default), 1 growing from a random corner, 2 row by row, 3 random.
- **Drop and build**: `Kick` 0..1 pops the picture apart, every piece out from the middle, lifted and turned, settling
  back as Kick falls (content: `clamp((SYNC_DROP - .4) / .6) ^ 2`, back in about 3.5 beats); `Shak` 0..1 makes the
  pile tremble (content: `SYNC_BUILD`).
- **Drawing** (`render/JigsawPipeline`, over the colorized frame right after encodeColorize, like the glass; before
  the glass, the backdrop, dancers and text): the frame is copied (blit) with a half-size sharp and blurred copy;
  a **cut map** (RGBA8Uint: the piece at home in each pixel, the nearest other one, the signed distance to the outline
  in 1/256 px) is rendered only when the cut changes (about once a cycle, two kept for a cross-fade); the **board**
  pass draws, per pixel, home pieces as the frame with the cut (a bevel lit from the upper left, a fine dark seam
  line, a light rim just inside it so the cut reads on black too, the snap's flash) and empty slots as the **table**;
  then the loose pieces bottom to top, each a soft drop shadow (offset towards the lower right and blurred by its
  lift) then the piece: a quad in 3D (turned, tilted, lifted towards a camera at 2.2 frame heights, so lifted pieces
  grow a little) whose pixels look up their home position, their coverage (bilinear over the cut map, anti-aliased)
  and the frame there. A whole frame (every piece home, seams and flash faded) encodes nothing, and every piece home
  with the seams faded gives back the frame exactly (tested).
- **The empty table** (`Gap`): 0 frosted (the blurred frame at half saturation, .45 bright; Jigsaw), 1 gray (the
  sharp frame without color, .5; Jigsaw Mini), 2 dark (.14: an empty table), 3 ghost (the frame a little soft, .7);
  `Dim` overrides the brightness, `Fros` the blur (10 px at 1080p). Slots are recessed: darker towards their edges.
- **Alpha**: `ColorKey` 1 (default) leaves the Syphon alpha as the visuals keyed it (a moved piece keeps the alpha of
  where it lies); 0 makes the whole frame opaque.

Cost at 1920x1080 (M-series, tests/test_render_jigsaw.cpp): the copies, board and 32 loose pieces with shadows
0.5-1.0 ms GPU; the cut map about 2.5 ms more, once per cycle. Memory: a frame-size BGRA8 copy, three half-size
BGRA8 and two frame-size cut maps (25 MB at 1080p), made at startup.

## Rain on glass (`Mesh="rain"`, Peter 2026-11-23)

Peter: "simulated raindrops landing on the screen and causing distortion and refraction of what's underneath ... the
screen of the monitor becomes a glass window that is getting distorted by rain on it? Or maybe like it's a glass floor
with rain falling on it? ... good modeling of the distortion will be key." Both are built, as `Rain = window | floor`
(content's Rain Downpour, Rain Floor and Rain Pool, in the Overlay group; Rain Window, a sparser window, was removed 2026-11-23). Keys: scene-format §5.7.

Like the glass puzzle it is not drawn into the visuals: the Visualizer keeps a `RainDraw` per step and `colorize`
calls `RainPipeline::encodeComposite` after the glass, so the visuals keep running under the water. One rain at a
time (its fields are shared): a newer rain replaces an older one at once.

**Water is a height field over the frame**, and every pixel looks through it:

- **Drops** (engine/Rain, CPU): window beads, runs and residuals; floor beads and crown droplets. Splatted per
  colorize into a full-size RG16F target by max blending (merges happen on the CPU on contact, so overlaps last one
  substep): a spherical cap of the contact angle `Bead` (height = r tan(θ/2)) in r, and a halo just outside the
  contact line in g. Runs are stretched along their motion (√ across) with a longer, tapering back half; a merge
  wobbles along the merge axis (amplitude ½ ∛(V_small / V), decaying over .16 s, ringing faster for smaller
  drops); a fresh drop squashes flat and recoils in its first ~80 ms. The tilt is a Sobel filter of the heights
  (3 x 3), so a drop a few pixels wide still reads round.
- **Floor waves** (GPU): `h' = (2h - h_prev + c² L h - b L L h + ν L (h - h_prev)) · damp - impulses` on a
  sim-size R32F grid (half the render size, at most 540 rows; three textures in a ring). `b` is capillary dispersion
  (b = .5 c², cells²): short ripples run ahead, so a strike leaves a train of rings, not one; `ν` damps grid chatter;
  the dry glass between puddles damps every substep and the frame edges absorb. `engine::rain::waveStep` picks the
  substeps (c ≤ .28 cells a substep, at most 12; past that the waves slow rather than blow up) and keeps
  8c² + 64b + 16ν ≤ 4, the von Neumann bound of the scheme (3.33 at the largest c). A strike is a crater with its rim,
  `amp (1 - q²) e^(-q²)`: no net push, so the mean level never drifts (a plain Gaussian kick made the flat mode grow
  without bound in the long CPU run). Puddles: `engine::rain::puddle`, seeded value noise thresholded by `Wet`, the
  same function in C++ and MSL, so the CPU knows where a drop falls into water; their meniscus adds its own slope.
  The edges are not still (Peter 2026-11-24: "too static"): two more octaves creep over the sim's creep time
  (`Flow` x seconds; each a crossfade between seeded noise layers 9 s and 4 s apart: ~6 % of the glass changes over
  20 s, the cover stays the same), and the cover swells while rain pours in (+.015 in Rain Floor's rain, +.08 in a
  downpour, at most +.1) and drains over ~20 s. The GPU draws the cover once per step into a sim-size R16F mask the
  wave substeps read (the noise is not evaluated per substep).
- **Window fog** (GPU): condensation in an RG32F sim-size pair (r: fog 0..1 easing back to full over `Refg` seconds,
  g: wet film fading over 2.5 s). Each running drop's capsule this step wipes it clear and leaves film. Shown as the
  frosted half-size copy (`SolidPipeline::encodeFrost`, the glass's first step, Gaussian `Fros`), a little milky,
  wherever no water clears it; the film's edges add a faint lens.

**Optics** (`rain_composite_fs`): N from the total slope; the view ray refracts into the water (Snell, n 1.331 /
1.334 / 1.339 for r / g / b: a touch of dispersion at the rims), out through the flat glass into air, and runs `Dpth`
frame heights to the visuals; the sample offset is its lateral run. So a bead is a real thick lens: focal length ≈
R_curvature / (n - 1) ≈ 3.5 r at 62°, so at Dpth .5 a 10 px bead shows a ~300 px patch of the visuals behind it,
minified and upside down, as on a real window (test: a bead over a frame bright above / dark below is dark at its
top and bright at its bottom). Thin film barely bends, ripples bend by a few pixels. Minified samples come from a
mipmapped copy of the frame by the sample's screen-space derivatives (trilinear), so the image in a small drop does
not shimmer. Rays that run off the frame see the dark room; total internal reflection reads as dark. Fresnel adds
only what the tilt adds over flat water (Schlick minus F0: flat water, like bare glass, leaves the frame exactly as it
was); a dim environment; a highlight (sharp core + broad sheen) from a soft light at the upper left (higher over the
floor, whose ripples are shallow). The contact line darkens with the drop's steepness and a soft halo lies just
outside each drop (`Rim`; drops only, not waves), so drops read over white as well as black (like the buttons' halo).

**Window** (engine/Rain, 1/120 s substeps from the start, seeded):
- Spawns on the 16th grid (below). A fresh drop may splash 1-4 micro droplets around it (their volume from it).
- Beads merge on contact: volume conserved (V ∝ r³ at the contact angle), volume-weighted center and momentum, a
  wobble. A drop past its own threshold (`Slid` ± 18 %) runs: driven by `2.2 (1 - (thr / r)²)`, drag, stick-slip
  (each 1/160 frame height of glass may pin it for 25-250 ms, small drops more often), a seeded meander and a pull
  towards wet glass below (a coarse grid of when each cell was last run over, drying over ~10 s), so later runs follow
  earlier trails. It sweeps up what it meets, sheds a residual droplet every ~1-4 radii and ~6 % of its volume per
  frame height as film, stops when it shrinks below .72 x its threshold, and leaves only past the bottom edge.
  Residuals under ~1.3 px dry up. `Max` bounds the count (the smallest evaporate first).
- Phrase lines (`Wipe`): thresholds drop to 40 % for half a second, so the big beads let go together (a cascade
  that clears streaks through the fog).

**Floor**: a strike in a puddle is an impulse (radius ~r, depth capped) plus a crown of 5-10 droplets thrown 2.5-7
radii out over 80-220 ms; they land as small impulses, or as beads on dry glass. Beads merge like the window's but
never run, dry slowly and are taken in when a puddle reaches them. No drop ever sits across a puddle's edge
(Peter 2026-11-24): a drop is in the water when its center or any point of its contact line (12 points, a pixel
out) is (`Sim::touchesPuddle`; `kPuddleSlope` bounds the field's gradient, so drops far from any edge cost one
sample); a strike or landing touching the water joins it (the impulse where they touch), and after every substep's
merges a bead the water reaches (a creeping or swelling edge, or a merge that grew it) is taken in with a small
ripple. A crown droplet in the air is drawn as a small lens with no contact line or halo (it is not on the glass).

**Music**: every 16th line of the beat clock (phrase-aligned `SYNC_*` while it is on, else 120 BPM from the start):
`Dens` drops on beat lines, `Clus` clusters (a 1.9x drop and 3-7 satellites) on bar lines, `Drz` small drops on the
other 16ths. A build (`SYNC_BUILD`) thins the beat drops (to 45 %) and thickens the drizzle (up to 4x): the rain
building; a drop (`SYNC_DROP`) is a downpour (up to 6x the drops, a cluster on every beat). Deterministic: spawns are
a pure function of (seed, line index), physics runs on fixed substeps, and without a clock the state is the same at
any frame rate (tested at 30 / 60 / 144 fps). A step simulates at most 30 substeps and spawns at most 16 lines; a clock
that jumps re-bases; junk (NaN, inf, 1e300) is clamped or read as no clock.

**Cost** (1920x1080, M-series, test_render_rain.cpp, 700 drops): window 0.57 ms GPU (frame copy + mips, frost, drop
splat, fog step, composite), floor 1.2 ms (10 wave substeps at 60 fps, ~.07 ms each). CPU ~0.09 ms per 60 fps step with
700 drops (worst 0.75 ms; a sort-and-sweep merge per substep). Resources at startup: the frame copy (BGRA8 + mips, 11 MB), drop
heights (RG16F, 8 MB), the wave ring (3 x R32F) and fog pair (2 x RG32F) at 960x540 (~14 MB). They are cleared on
the rain's first step, so poison mode reads nothing undefined.

**Window or floor?** The window is the rain-on-glass look (the inverted world in every bead, runs, wiped streaks
through fog): atmospheric, readable on any visuals, and calm enough to leave the visuals in charge. The floor is the
more spectacular in motion: interfering ring trains bend the whole frame on every beat. Both are kept; Peter picks.
## Bubbles (`Mesh="bubbles"`, Peter 2026-11-26)

Soap bubbles drifting over the visuals, with thin-film iridescence, refracting and mirroring the visuals, popping on
the beat. Content: Bubbles Drift, Bubbles Burst (Overlay group; Bubbles Giant, a few big ones inflating in place with Src air, removed 2026-11-26, Peter). Keys: scene-format §5.7.

Like the rain and the glass puzzle they are not drawn into the visuals: the Visualizer keeps a `BubbleDraw` per step
and `colorize` calls `BubblePipeline::encodeComposite` after the rain, so the visuals keep running underneath (and
the bubbles mirror and shift the rain too). The pass keeps no state between frames (everything lives in the CPU sim),
so several bubble objects may run at once.

**Optics** (render/BubblePipeline, `bubble_fs`). A soap bubble is a thin shell, not a lens. Each bubble is one
instanced quad drawn straight over the frame with framebuffer fetch (what is already there, other bubbles included,
shows through it). Per pixel the unit sphere's point comes from the outline (the shape modes below), and the shell
has two films, the front one and the back one seen through it:
- **Thickness**: the bubble's mean (`Film`, thinning by up to 45 % as it ages) times a drainage profile (`Drn`: thin
  at the top, thick at the bottom), times swirling 3D value noise on the sphere, plus finer streaks on bubbles big
  enough to show them. The film flows (Peter 2026-11-26: it looked static on the big bubbles): each height circles
  the bubble at its own speed (faster at the top, against the turn lower down), so the bands shear and wind into
  swirls, and a two-level moving domain warp (the second warped by the first) curls them into eddies, the whole
  pattern also draining downward; `Swrl` scales both the flow and the warp. A planned pop drains a
  black film down from the top over the beat before it (thinner than a quarter wave reflects nothing: the black
  film real bubbles show before they burst).
- **Interference, computed physically**: the Airy reflectance of a symmetric soap film (n 1.33; the single-surface
  Fresnel reflectance at the angle, s and p averaged; the phase 2 pi 2 n d cos(theta_t) / lambda with the half-wave
  flip, so a vanishing film is black) at ten wavelengths 406..694 nm, weighted into linear RGB by the CIE 1931 color
  matching functions (Wyman, Sloan and Shirley's multi-lobe Gaussian fit) and the sRGB matrix, normalized so a flat
  spectrum is white. This gives the Newton series (black, silver-white, gold, magenta, blue, green, pink, ... washing
  out to white past ~1.5 um) and the mirror at grazing incidence. `Iris` scales the saturation. A CPU copy
  (`render::filmReflectance`) is tested.
- **What the films reflect**: the front film is a convex mirror, the back film (seen from inside) a concave one, so its
  reflections are upside down on the other side. A reflected ray running towards the screen meets the visuals `Dpth`
  behind (sampled from a mipmapped copy of the frame, rays off the frame see a dark room); one running towards the
  viewer sees the room: the visuals mirrored and soft (mip 4.5) and a sky brighter above. The space between the
  bubbles and the visuals is lit a little, so the rims read over black. A soft, round light up and to the left
  (`Spec`) gives the classic highlight pair (front upper left, back lower right), colored by the film like the rest
  (a four-pane window until 2026-11-26: Peter was unsure about it).
  The light is white, tinted a little towards the ColorMap's color at .8 (`Tint`), so they sit with the palette.
- **Composition** (in linear light): `under (1 - Rf)(1 - Rb) + 2 (Rf L_front + .75 (1 - Rf) Rb L_back)`. The middle,
  where the film's reflectance is lowest, stays nearly clear (the room's reflection also fades a little towards the
  middle); the rim goes to a strong Fresnel reflection of the visuals behind (`Rim` widens that band: cos theta is
  taken to the power 1 + .8 Rim).
- **Through the shell** the visuals shift only slightly, most near the rim (`Bend`; q^4 R .045 px outward): the shift
  is taken from the frame copy and added to what is under the pixel, so overlapping bubbles keep each other.

**Shapes and walls.** The outline is r (1 + a2 cos 2t + b2 sin 2t + a3 cos 3t + b3 sin 3t) from the sim's shape modes.
A cluster's shared wall: both bubbles are cut at the plane where their spheres meet (a = (d^2 + r1^2 - r2^2) / 2d from
the first center, the wall's circle of radius sqrt(r1^2 - a^2)); the lower id draws the wall, a film seen edge-on: a
bright line with its own interference color, along it varying; the other gives the wall its pixel. A wall ends where
it meets the bubble's other walls (the planes of three bubbles meet in one line, so a cluster's walls meet at one
point, as in real foam), and within .4 radii of a wall the outline is the plain sphere (the shape modes fade out), so
both bubbles' edges meet the wall's ends at the same point (Peter 2026-11-26: the two sides did not match).

**Popping.** The film tears from one seeded point on the face turned to us (a little more often higher up, where it
is thinnest), so the hole opens in the face as a growing disc and races out over the sphere, the far side's film
closing last as a small shrinking ring. The hole is the cap within an angle of the tear that grows to pi in
kPopSeconds = 45 ms (about 3 frames at 60 fps), accelerating out of the tear, for both films. Its edge is ragged
(noise round the tear, the tongues shrinking again as the last film closes); the retracting rim is a band of gathered
liquid, thickening as it goes and faintly beaded; a ripple runs through the film just ahead of it. Real rims run at
the Culick speed (~7 m/s for a 1 um film: a 3 cm bubble in ~4 ms); slowed so it can be seen. (Until 2026-11-26 the
tear was near the top and the smooth cap took 75 ms: Peter saw it as a scroll down through the giant bubbles.) Each
pop throws a spray (12-60 droplets from the shell, each appearing when the rim reaches its spot, flung along the
rim's travel and outward, falling, fading over .3-.8 s), drawn as tiny lenses (the visuals upside down) with a glint.

**Motion** (engine/Bubbles, 1/120 s substeps from the start, seeded):
- Air: the curl of a seeded noise potential (two octaves, each a crossfade between noise layers 7 s and 3.7 s
  apart), so the flow neither converges nor diverges (tested); `Air`. They rise gently (`Rise`) and relax to the air's
  velocity with a time constant growing with size, so a wand's bubbles carry their blow for a second or two. `Wind`
  adds a breeze whose heading and strength wander slowly (seeded sines over tens of seconds), so the drift goes every
  way, not only up (Bubbles Drift: 1).
- Soft walls at the sides and the bottom (the edge source's bubbles come up from below it); the top is open: they
  drift out of the frame there unless they pop first.
- Shape modes n = 2 and 3 (cos and sin each): damped oscillators (zeta .12, f2 = 2.6 Hz (55 px / r)^1.5, f3 = 2.24 f2)
  driven by smooth noise (the air), a build's tremble and a coming pop, and squashed while two bubbles touch; a
  wand's bubble starts stretched along its blow.
- Contacts: two touching bubbles bounce softly (pushed apart by the overlap, losing most of the approach), or, once per
  pair (seeded, `Clst`) when they meet gently, stick: a bond whose spring holds the centers sqrt(r1^2 + r2^2 - r1 r2)
  apart, the distance at which three films meet at 120 degrees (equal radii: one radius apart), as in a real double
  bubble. At most three walls per bubble; a bond breaks when the air pulls them apart or one pops.

**Music** (the 16th grid of the beat clock, phrase-aligned `SYNC_*` while it is on, else 120 BPM from the start):
- Blowing: on each bar line a puff of `Blow` bubbles joins a queue, released over the bar's first 16ths (a stream from
  the wand); sizes lognormal around `Size`. The wand: each bar's puff from a new seeded spot anywhere in the frame,
  aimed at a random point well inside it, swaying a little over the bar (until 2026-11-26 one spot near a lower corner,
  aimed up at the middle).
- Popping: every pop lands on a beat line. On beat line k the victims for line k + 4 are planned: `Pop` of them (fewer
  while a build holds its breath), every bubble past `Life` beats, and the extras over `Max`, the oldest first with a
  seeded jitter. Their film drains black from the top over that beat (and they tremble a little), and on line k + 4
  they tear, in the substep the line is crossed. A plan far ahead of the clock (it jumped back) is re-planned.
- A drop (`SYNC_DROP` rising past .5): on the next beat line 60 % of the bubbles pop at once (no warning) and a burst of
  2 Blow + 4 is blown in, faster and wider (it may overfill `Max` by half; the extras pop on the next lines).
- A build (`SYNC_BUILD`): blowing up to 3x (build^2) and trembling (the shape modes driven harder and faster); early in a
  breakdown (calm = min(1, 4 build)(1 - build^2)) the air slows (to 40 %), they rise slower and come up to 60 % bigger.
- Deterministic: lines and pops are a pure function of (seed, line index), physics runs on fixed substeps, and
  without a clock the state is the same at any frame rate (tested at 30 / 60 / 144 fps for each source). A step
  simulates at most 30 substeps and visits at most 16 lines; a clock that jumps re-bases; junk (NaN, inf, 1e300) is
  clamped or read as no clock. At most 96 bubbles and 640 droplets, whatever the keys say.

**Cost** (1920x1080, M-series, test_render_bubbles.cpp; other agents' GPU tests were running at the same time, so
these are upper bounds): Bubbles Drift's crowd (40 bubbles of 55 px, popping and spraying) 0.5 ms GPU best, ~1.0 ms
mean; 7 giants of 150 px (Src air) 0.4 / 0.5 ms (frame copy with mips, then the bubbles). Nothing is encoded while
there are none. CPU ~0.02 ms per 60 fps step (worst 0.1 ms) with up to 60 bubbles. Resources at startup: the frame
copy (BGRA8 + mips, 11 MB); 0 render allocations (tested).

## Ferrofluid (removed)

A pool of glossy ferrofluid over the visuals whose spikes rose on the beat and refracted the visuals like the
rain (`Mesh="ferrofluid"`, built 2026-11-26): removed the same day (Peter: "I just don't like those"), with its
engine, pipeline, tests and the three scenes (Ferrofluid Pool, Crown, Twins). It is in the git history.

## Shatter (`Mesh="shatter"`, Peter 2026-11-26)

The screen is a pane of glass a little in front of the visuals. Cracks spread across it on the kicks; on the phrase
line it shatters into shards that fall away (or, on a drop, fly apart towards you), leaving the clean visuals; then a
fresh pane slides down and the cycle repeats. Content's Shatter Drop and Shatter Hail (Overlay group; Shatter Web, one impact on a lighter web, removed 2026-11-26, Peter).
Keys: scene-format §5.7.

Like the rain and the glass puzzle it is not drawn into the visuals: the Visualizer keeps a `ShatterDraw` per step and
`colorize` calls `ShatterPipeline::encode` after the rain and the color models, so the visuals keep running behind the
glass. One pane at a time (its crack maps are shared): a newer shatter replaces an older one at once.

**The fracture** (`engine/ShatterShape.inc`, one source compiled as C++ for the engine and the tests and stringified
into the Metal crack-map shader, like the jigsaw's cut, so both break the pane into exactly the same shards):
- `Imp` impacts per pane (one a little above the middle; several spread apart by rejection sampling). Each owns its
  Voronoi cell; the boundary between two is their bisector with a gentle wander (up to .012 frame heights), the seam
  where two webs meet.
- Around an impact a spider web in polar coordinates: `Spk` ± 2 radial cracks, jittered in angle (.28 of a sector)
  and kinked by a piecewise-linear wander in log radius (a kink every third of a log unit, .11 of a sector); forks: from
  a ring about a third of the way out a radial crack may branch into its sector (`Fork`), leaving its parent where that
  meets the ring and curving out to its share of the sector over a band (a Y), and two rings later each half may branch
  again; ring cracks at radii r0 g^(j - 1) (r0 = .04 `Size`, .026 with several impacts; g = 1.95 - .5 `Rng`, at least
  1.45), their ends on each spoke jittered by 10 % (so they kink at the spokes), nearly straight between the spokes
  (from the arc towards the chord as far as the next ring's margin allows), with a little bow.
- A shard is one sector of one band (`slot`); the jitters and the straightening are bounded so ring j + 1 stays outside
  ring j everywhere and spokes never cross: every point has exactly one shard (tested on grids for one to six impacts,
  and the owner along any ray steps out one band at a time). The crack between two shards is a **crack element**: a
  shard's left spoke piece, its inner ring piece, or a bisector, with a position along it (outwards along a spoke,
  across along a ring, from the middle along a bisector).
- **The shard table** (engine `makePlan`, on the CPU, once per pane, no allocations): each sector's outline sampled
  (spokes every .1 log units, rings in 6 steps), clipped to the frame and the impact's cell (Sutherland-Hodgman) for
  its centroid, its quad (bounds + .012 margin; tested: every grid point lies in its own shard's quad) and its
  boundary elements. At most 640 shards: a denser web is thinned deterministically (fewer forks, then fewer spokes).

**The schedule** (`engine::shatter::Player`; beats on the phrase-aligned clock, 4 x `SYNC_BAR`, or 120 BPM from the
start without it; a cycle of `Cycl` beats from one line to the next):
- On the line the pane shatters. 2 beats later a fresh pane slides down over 1.5 beats (its thick bottom edge
  refracting and catching the light; `Arrv=1` fades it in instead), its glass catching a little more light as it
  settles.
- `Hit` beats after the line (4) the first impact lands; the others on later bar lines over the pane's first three
  quarters. On its beat the impact's inner cracks shoot out (the crushed spot, a flash); then on every beat up to the
  one before the line more cracks grow: each impact's elements in order (spokes outwards band by band, rings a little
  after the spokes around them, some jitter), a share per beat, so every kick adds or extends cracks. A spoke piece
  starts after the piece below it (within a beat it continues its growth), a ring piece from the spoke that reached
  it first. Each starts exactly on its beat line, grows in about a tenth of a beat (ease out) and flashes as it lands.
  By the line `Crk` of the web shows; the rest are hidden cuts that show only as the pane breaks. The bisectors appear
  well after both their impacts landed (60 % of the way to the line), from the middle out over a beat and a half.
- The pieces a crack has cut loose (their boundary mostly grown) tilt a hair (.006-.016 rad x `Tilt`), so the visuals
  shift by a few pixels across each crack.
- A build (`SYNC_BUILD`): the cracks due in the next 4 beats creep ahead on the 16th lines (up to 40 %, never back),
  glints tick along the cracks on the 16ths, and the loose pieces creak (their tilts jitter per 16th).
- A drop (`SYNC_DROP` rising past .5) while a pane is up: it explodes on the next 16th line, and a fresh pane arrives 3
  beats later and cracks over the beats left until the next line (at least 12; else it runs to the line after). A drop
  within 2 beats after a gentle shatter blows the falling shards apart from that 16th line.
- The shards' flight is closed-form in the beats since the shatter (no integration): a gentle collapse (`Brst` .3: a
  little push out from the impact, pieces near it first, toppling forward about a mostly horizontal axis, gravity
  1.4 `Grav` frame heights per beat²; gone in about 1.5 beats) or an explosion (a drop, or `Brst` 1: thrown out from
  the impact and towards the camera, spinning about random axes; past the camera or off the frame in about 2 beats).
  Each starts from the tilt it had in the pane, so the break is continuous. A shard is gone when it is below or beside
  the frame or past the camera; the poses are sorted back to front.
- Deterministic: plans are pure functions of (seed, config, aspect, the pane's beats); the frame is a function of the
  beat and those, so it is the same at 30 / 60 / 144 fps (tested; only a drop's 16th line and a build's creep depend
  on when they are seen). A clock that jumps back (more than a 16th) or ahead (more than 16 beats) re-bases: the pane
  due at the new beat (one arriving on the next beat line when it is mid-cycle). Junk (NaN, inf, 1e300) is clamped or
  read as no clock. Three plans are cached (the pane, the falling one, a spare).

**Drawing** (`render/ShatterPipeline`):
1. The frame is copied with mips (a blit), so the refraction samples it by the ray's derivatives.
2. **The crack map** (RGBA16Uint, frame size: the shard, the nearest crack element, the position along it, the
   distance to it in 1/32 px), rendered from the shared fracture code only when a plan is new; two kept (the pane's
   and the falling shards'). Tested against the CPU: the same shard everywhere off the cracks.
3. **The pane** (full screen): each pixel looks through the glass. Where the nearest crack has grown past it: a fine
   silvery line (light scattered in the fracture face, its brightness wandering along it) with a darker fringe, so it
   reads over black and white; the chipped edges lean into the crack (a narrow lens: the view ray refracts through
   them, Snell with n 1.50 / 1.52 / 1.545, and runs `Dpth` frame heights to the visuals, as in the rain, so the visuals
   bend and split into colors along every crack); glints twinkle along it (`Sprk`; more tick on the 16ths in a build),
   and it flashes as it lands. Everywhere the shard's tilt shifts the view a little. Fresnel adds only what a tilt adds
   over flat glass (the intact pane leaves the frame exactly as it is; tested), a highlight from the upper left on the
   chipped edges; each impact's crushed spot (white, mottled) and its flash; a faint broad sheen (`Gls`).
4. **The shards** (instanced quads, back to front, premultiplied over): each a quad in 3D (rotated, lifted towards a
   camera 2.2 frame heights above the pane, so a shard thrown at it grows) whose pixels look up their home in the
   crack map (coverage anti-aliased from the bilinear distance; the frame's edge cuts it without a bevel). They show
   what lies behind them now: the visuals shifted sideways through the thick slab (.018 frame heights x `Thck`) as it
   turns, bent and split by the beveled edges (3 + 6 `Thck` px at 1080p), a Fresnel reflection of the room (the frame,
   blurred and mirrored) that flashes as a shard turns edge-on, a highlight from the upper left, the green of the
   glass at the edges and through more of it as it turns, the lit edge and the shatter's flash. Flat at home a shard is
   exactly the pane (tested: at the moment of breaking the frame shows through the shards unchanged off the cuts, so
   there are no gaps and no overlaps).
- **Alpha**: `ColorKey` 1 (default) leaves the Syphon alpha as the visuals keyed it; 0 raises it under the cracks and
  the shards.

**Cost** (1920x1080, M-series, test_render_shatter.cpp, nothing else running): the cracked pane 0.45 ms GPU (one impact;
0.43 ms with five), the shards falling 0.34 ms (103 shards; 0.25 ms for 411 smaller ones), the crack map 2.7 ms once per
pane (1.5 ms with five impacts), as the pane slides in. CPU: a plan about 1 ms (one impact; 4-5 ms with five)
once per pane as it arrives, a frame a few microseconds. Resources at startup: the frame copy (BGRA8 + mips, 11 MB), two crack maps (RGBA16Uint, 33 MB), eight 150 KB
shared state buffers (the crack and tilt arrays, the shard instances).

**Peter's choices** (2026-11-26): every scene explodes on the line (`Brst` 1, now the default), the pane gets fully
cracked (`Crk` 1, now the default), new panes slide down (`Arrv` 0); the collapse and the fade stay as options.

## Fractal zoom (`Mesh="fractal"`, Peter 2026-11-23)

Peter: "Zooming in on one or more 3D fractals. I'm thinking maybe a 3D Sierpinski, where we can move the camera around
it in 3D such that we loop exactly when we've zoomed in so it can go on forever." Built as a solid kind (an Object,
spawned and faded like every solid, composited through the sprite pass with Pen / EPen / Flag / Over, so it trails
and takes the ColorMap), raymarched instead of rasterized: `engine/FractalZoom` (the maths, the paths, the clock;
pure C++), `render/FractalPipeline` (the shader), `app/VisualizerFractals.cpp`. Keys: scene-format §5.7. Content's
Sierpinski Apex, Sierpinski Vortex and Menger Tunnels (Objects group Fractals).

**Why a solid kind** (not a GPU organic like the Mandelbrot dive): it is a 3D object seen by a camera, like a model;
it reuses the solid slots, the solid fade / MaxLive / Over / ColorKey handling and the sprite composite unchanged, and
its own pass and shader live in their own files (nothing in SolidPipeline's mesh pass changes).

**The seam, by self-similarity.** The fractals are attractors of contractions `f_i`: the Sierpinski tetrahedron (4
maps, scale 1/2, vertices (1,1,1), (-1,-1,1), (1,-1,-1), (-1,1,-1)) and the Menger sponge (20 maps, scale 1/3, the cube
[-1, 1]^3). A *zoom step* is a similarity `g = f_w o sigma` (a word of maps after a rotation `sigma` of the hull), so
`g(S)` is one of S's own pieces:

    g(x) = p + lambda Q (x - p)        p = g's fixed point, lambda = scale^|w|, Q = a turn by `angle` about `axis`

What is drawn is not S but **S_inf = union over n >= 0 of g^-n(S)**: S grown outwards about p for ever. Because g(S)
is a piece of S, S_inf is *exactly* invariant: g(S_inf) = S_inf. So a camera pose P and the pose g o P see the same
picture. The camera runs along `g^u` (u = 0..1: lambda^u, a turn by u x angle about the axis through p), and at u = 1
it is reset to u = 0: nothing visible changes, no precision is lost, for ever. (A finite S would not loop: from g o P
you also see S's other pieces around g(S).)

- **Distance to S_inf** (`deInf`): pick the integer m with lambda^m |x - p| in (lambda R, R], R = rho / 2, map
  y = g^m(x), and take lambda^-m DE_S(y), capped by |x - p|. rho is the radius about p within which S_inf equals S:
  lambda^-1 x the distance from p to S's other pieces at g's level (through their hulls: a lower bound), so the cap
  (a step from y may go rho - |y - p| >= |y - p|) is a valid bound, and in world units it is the same in every shell
  (continuous). The same y and level serve x and g(x), so the estimate is exactly g-equivariant (tested in double to
  1e-15; `fractalDistanceInf` is the shader's twin).
- **Prefractal level** per point: the smallest pieces `Detl` pixels wide at their distance from the camera, computed
  in y units (lambda^m x the pixel angle x the distance), with the two neighboring levels blended (Sierpinski: the
  fold DE, then the tetrahedron at the level; Menger: iq's cross DE, iterated in local cell coordinates so it stays
  precise in float). Detail relative to the camera is then constant over the step, and identical at u = 0 and u = 1.
  Caps: 22 levels (Sierpinski), 12 (Menger: float loses a factor 3 per level).
- **Everything else is relative to the camera** too: the fog distance and the march's far end are multiples of the
  camera's distance to p, the hit threshold and the normal's tap are a share of a pixel at the hit distance, the
  occlusion samples a share of the hit distance, the key light is in camera space. Two traps found on the way
  (both made the Menger vortex's seam visible, mean error 12/255): normals from taps on world axes are not invariant
  under a quarter turn (the taps are now a tetrahedron in the camera's frame); and the DE of S is only a bound off
  the surface, which differs between two neighboring shells (the cube's outside on one side, a tunnel of crosses on
  the other), so the occlusion showed each shell boundary as a ring: shading uses `deSmooth`, the two valid mappings
  blended smoothly across each shell (R0 = lambda rho), continuous everywhere; the march keeps the cheaper `deInf`.
- **Detl default 2.5 px**: at 1 px the regular hole grids alias into moire rings (sub-pixel holes beating with the
  pixel grid); 2.5 is crisp and cheaper.

**The paths** (`Path`; the camera's base pose B at u = 0; its closest approach to the fractal over a step, as a share
of its distance to p, is tested with the default sway):

| Frac | Path | Step g | What you see | Closest |
|---|---|---|---|---|
| sierpinski | `apex` (default) | f_0 o a third of a turn about the axis through the vertex; x2 | the tip of an endless Sierpinski pyramid from outside, the camera circling a third of the way round per step (Peter's idea) | .58 |
| sierpinski | `dive` | the same | inside the pyramid down its axis, through the triangular windows from room to room, rolling a third per step | .11 |
| sierpinski | `vortex` | f_2 f_3 o a third of a turn about (-1,1,-1); x4 | spiralling in on a point deep inside the gaps (fixed point (.079, -.270, -.683), found by a search over words and rotations for the widest clearance) | .34 |
| menger | `corner` (default) | the corner cube o a third of a turn about the diagonal; x3 | the corner of an endless sponge from outside, turning a third per step | .94 |
| menger | `vortex` | the cube (-1,-1,0) o a quarter turn about -z; x3 | through the tunnels, spiralling in a quarter turn per step; the sponge's outer face (the void beyond it) stays at the left | .42 |
| menger | `tunnel` | the cubes (0,-1,1) then (0,1,-1), no turn; x9 | straight down the tunnels toward (0, -.5, .5), surrounded by sponge on every side | .18 |

A vertex fixed point (apex, dive, corner) gives a cone-shaped S_inf (the pyramid / the octant), empty outside; an
interior one (the vortices, tunnel) fills all space. Inside the Menger octant every ray from the corner touches the
sponge at Cantor-like points, so there is no Menger "dive"; the tunnel path is the inside flight instead.

**The camera** is `G(u) o W o B`: B the path's base pose, W the scene's sway (`Orbt` turns the eye about the path's
orbit axis through p; `LkX` / `LkY` turn the view; `Roll`; `Fov`), G(u) the zoom. W may be any continuous function
of time: the picture only depends on the pose up to g, so the sway never breaks the seam. Default sway: a slow orbit
(`.25` rad apex-like paths, `.1`–`.12` inside) over 32 beats and a look-around over 64, both moving in a step on each
beat (`Stpb`).

**Music** (`engine::FractalClock`, a function of the beat clock plus two small bounded states):
- One self-similar step per `ZBar` bars (default 2; content 2 for the apex, 4 inside), landing on those bar lines.
  Within each bar the motion surges right after the bar line and settles into the next (`Puls` .5: speed 2x the mean
  on the line, .5x at its end); with ZBar < 1 or fractional, per step instead of per bar.
- Beats: a glow on the surfaces' rims and a halo along near misses, `Glow` x a pulse that decays over the beat; the
  sway steps on the beats.
- A drop (SYNC_DROP rising) rushes through `Drop` extra steps (2) over `DrpB` beats (4), eased in and out. A build
  (SYNC_BUILD) slows the zoom to 1 - `Hovr` (.75) at its top; after it the zoom catches up to the next whole step in
  about a beat, so the steps land on bar lines again. Both offsets are kept mod 1 (only the fraction is drawn).
- Without a confident clock: 120 BPM from the start, no glow. A scene's own `Zoom` expression (in steps; its fraction
  counts) replaces all of this.
- Deterministic (a pure function of the inputs and the frame order); NaN, infinite, 1e300 and jumping clocks stay
  bounded (the zoom always in [0, 1), the stepped beat count mod 65536).

**Composite.** `frac_fs` writes (lum x a, a): a = the fog (coverage of the hit), lum = Fill + lit (key light, a soft
headlight, a rim, ambient occlusion). Content uses `Over="1"` (the fractal opaque over the visuals, the fog fading
into them; Pen 1 so it trails). Rendered at `Res` (.5) of the frame into the slot's top-left, upscaled by the sprite
pass's bilinear sampling (the quad's uv inset half a texel).

**Cost** (tests/test_render_fractals.cpp, 1920x1080, Res .5, Detl 2.5, M-series): 1.4–1.9 ms GPU mean per frame
through a whole step (peaks 1.8–2.3 ms), every path; 0 render-thread allocations. Res 1 is about 4x.

**Tests:** test_engine_fractals.cpp (every step maps S onto a piece; S_inf = S within rho; DE equivariance; a CPU
raymarch hits at the same relative distances from g o B as from B, to 1e-14; the camera seam; clearance; the clock's
bar lines, drops, builds, determinism, junk inputs; the content scenes; unknown names; the green note),
test_render_fractals.cpp (GPU: the frame at the end of a step vs its start, mean |diff| under .01 of 255 for every
path, the same frame twice identical, cost; `REFORCE_FRACTAL_PREVIEW=DIR` writes PNGs).

## Tower of Hanoi (`Mesh="hanoi"`, Peter 2026-11-26)

Peter: a Tower of Hanoi solved on the beat, one disc move per beat, finishing exactly on a phrase line, drawn as glass
that refracts the visuals behind it, in the spirit of the glass 15 puzzle and the rain. Content's Hanoi Tower (6 discs,
63 moves: a 16-bar phrase with one beat for the reset), group Puzzles. Keys: scene-format §5.7.

Peter 2026-11-26: Hanoi Tower (springing open and carrying on round the pegs) over Hanoi Mini (removed); the glass well
frosted, bending the visuals a decent amount, every part one color (`Mono` .7: discs, pegs and base all the ColorMap's
color at .7, as deeply tinted); nothing special on drops (no `Lift` / `Flip` in the scenes; a build still opens the
stacks). The frosted share now grows with `Fros` (.2 + .02 Fros, at most .85; it was a fixed .3, so more frost only
blurred that 30 % and barely showed). Of three looks compared (`Fros` / `Bend` / `Dpth` / `Tint`: 22 / 1.4 / .65 / .4,
a milky 34 / 1.15 / .55 / .3, and the "Lens" 16 / 2 / .85 / .45) Peter picked the Lens: Hanoi Tower now has it, clearer
glass with every rim and peg a strong lens. He also wanted it to move more: it drifts side to side (X ~±.14) and bobs
(Y ±.07), turns up to ~45 degrees each way (RotY), rocks (RotX ±.1) and tilts a little (RotZ ±.05), on slow sines of
unrelated periods. `REFORCE_HANOI_PREVIEW` also writes the look (hanoi_look_tower.png).

- **Solve** (`engine/Hanoi`, `Plan`): the minimal solve of `Disc` discs from peg 0 to peg 2, made once (an explicit
  stack of sub-towers, no recursion) with the peg and level of every disc after every move (a table: the state after k
  moves is a lookup). A run maps those pegs onto its own: with `Rset` 0 run r goes from peg 2r mod 3 to the next peg
  round to the left (0 to 2, 2 to 1, 1 to 0), so each run starts where the last one ended and every run is a correct,
  minimal solve; with `Rset` 1 every run goes left to right. Seeds are the clock's run count, so consecutive runs chain
  across the line (tested for negative and large seeds).
- **Timing**: a run of `Beats` beats; the first `lead = Beats - moves x Each` are the reset, then move k lands on
  `lead + k x Each`, the last on the run's end. Everything is a pure function of (settings, run, beat), so a clock
  jump, a mid-phrase start or a tempo change is just another lookup: nothing is planned ahead or simulated (the
  labyrinth needs lazy planning for its physics; here the state is closed-form). The only state is the style of the
  move in flight (`Lift`, `Flip`), which SolidRun reads when the move starts, so a value that changes mid-flight (a drop
  arriving) never jumps the disc.
- **A move** (`carry`): straight up the peg until the disc's bottom clears the peg tops (`kClear`), over a half
  ellipse (vertical at both ends, so the path is smooth) whose height grows with the distance (.3 + .1 per peg, x
  `Lift`), straight down onto the stack. Time along the path's arc length is eased in from rest and arrives at .9 x the
  mean speed: the disc lands with a little speed exactly on the line (tested: a hair before it is above its place, on
  the line it rests on it exactly, for 3..8 discs, Each .5 / 1 / 2, both resets, several runs). In flight it swings
  (the leading edge up, then down; no more than the arc leaves room for) and, on a drop, tumbles forward `round(2 x
  Flip)` half turns between .3 pi and .7 pi of the arc, whose height then grows by 1.25 radii so the turning disc clears
  everything. On landing it squashes a few percent about its bottom and rings back (~.2 beats) and its rim flashes
  (glint).
- **Resets**: `Rset` 0: the solved tower springs open (each disc up .14 per level, the top the most, a parabola) and
  shuts again, every disc landing on the reset's line; `Rset` 1: the whole stack is lifted off the right peg, carried
  back over the pegs and dropped onto the left one, landing on the line. The reset is one beat (`Each` beats when
  longer); with a longer lead the tower rests until the first move.
- **No collisions** (tested over whole runs at 1/512 beat, every disc count and reset, with no lift, a drop's style and a
  full build): the carried disc never cuts a peg (it rides threaded on its own peg until its bottom clears the tops) or
  another disc (as a tilted box), and moves continuously.
- **Music** (content): `Lift = 1 + 1.5 SYNC_DROP` (a drop flicks the discs higher), `Flip = SYNC_DROP > .6` (the moves
  starting in a drop's first two beats flip right over), `Hovr = SYNC_BUILD (1 - SYNC_DROP)` (a build: the stacks open
  up, .06 per level, and bob); a build also tips the tower towards the viewer (RotX). Timing is never touched.

**Drawing** (`render/HanoiPipeline`; like the glass puzzle and the rain it is laid over the colorized frame by
`colorize`, after the glass puzzle and before the rain, so the visuals keep running behind it):

- **Geometry**: every part is a lathe around a short segment, made in the vertex shader from a profile (no vertex
  buffers): a disc is a glass washer (an annulus from the hole, radius .17, to its outer radius .5..1, .26 high, with
  half-round inner and outer rims), a peg a rod (radius .085) with a round top, the base a stadium slab (.22 thick, the
  pegs' line +- 2.3 with round ends of 1.14) with rounded edges. 4x MSAA and depth into the object's slot target
  (resolved premultiplied), then over the frame by `SolidPipeline::encodeGlassComposite` (the glass puzzle's last
  step, factored out of `encodeGlass` and now able to cover a box). The pass and the composite cover only the tower's
  screen box (the parts' bounding boxes projected; clip space shifted so the box sits at the target's corner, as the
  solid pass does).
- **Optics** (`hanoi_fs`): each part also has its exact distance field (the same shape). The mesh's point is moved
  onto the true surface along the field's analytic normal; the view ray refracts in (Snell, n = 1 + .5 `Bend`), runs
  through the glass and refracts out, then on to the visuals, a plane `Dpth` frame heights behind the tower; that point
  is projected back onto the screen and sampled, so a straight ray samples exactly the pixel behind it. Where the ray
  leaves through a flat face of a disc or the base (the path staying in their solid core) or anywhere on a peg (a
  capsule) the exit is analytic; the rims are traced (sphere tracing inside the field, which never steps past the
  surface, then bisected). A total internal reflection bounces once more (followed only when it then leaves
  analytically). Dispersion: red and blue refract with n -+ .018 `Bend`. So a rim is a strong lens (the visuals behind
  it minified and flipped), the flat tops shift them a little, a peg is a rod lens (render test: over a frame bright
  left / dark right of the peg, the peg shows dark on its left and bright on its right).
- **Sampling**: from a mipmapped half-size copy of the frame (its own downsample pass + mips), at the ray's screen
  footprint (no shimmer where the glass minifies), with 30% of it frosted (a coarser level of the same copy, about a
  Gaussian of `Fros` px). The first version cost 2.2 ms: the full-frame passes and composite (the box: -.2 ms), the
  Gaussian frost of `encodeFrost` (-.4 ms), traced exits everywhere with a traced entry and numeric normals (analytic
  ones: -.8 ms); a separate pass for the base and pegs, so the discs could see them, was dropped (no visible gain).
- **Look**: colored glass absorbs along the path (`Tint`; the tint to the power `Tint x path / disc height`, so the
  rims are deeper); Fresnel (Schlick, F0 from n) reflects a dim room, lighter above, tinged with the glass's color; a
  soft highlight from the upper left (`Spec`); the transmitted light darkens a little towards the rims and a sheen runs
  along them, so the glass reads over white (darker rims, colored discs) and over black (lit rims, highlights); rays
  that leave the frame see the dark room; a landing's glint lights the rim. Each part sees the frame behind the tower,
  not the other parts (as the glass puzzle's buttons).
- **Colors**: the discs take the soma's seven piece colors (`engine::PiecePicker`: clearly different, light enough),
  sorted in ColorMap order and spread over the discs (the smallest the first); the pegs and base a faint neutral glass.
- **Cost** (1920x1080, M-series, test_render_hanoi.cpp, 6 discs, a move in flight, Scal .9): .72 ms GPU (downsample and
  mips, the glass pass, the composite); 2.8 MB for the half-size copy, the solids' MSAA color, depth and slot target
  shared. CPU per frame: a lookup and one path (~32 arc samples), negligible.
- Tests: test_engine_hanoi.cpp (the minimal legal solve, landings exactly on the beat lines, solved on the run's end,
  runs chaining, no collisions and continuous motion, a drop's style, determinism, jumps, junk, the content scenes at
  two tempos and a drop arriving mid-flight), test_render_hanoi.cpp (the rod lens, readable over white and black,
  opacity 0, the cost; `REFORCE_HANOI_PREVIEW=<dir>` writes frames to look at).

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
| Measured (fractal zoom, 1080p, Res .5, Detl 2.5) | 1.4–1.9 ms GPU mean (peaks 2.3 ms), every path; 0 render allocations |
| Measured (Tower of Hanoi, 1080p, 6 discs) | .72 ms GPU (half-size copy, glass pass, composite over the tower's box) |

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

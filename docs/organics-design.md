# Organics: procedural growth (PLAN 5N)

A new spawn layer, **Organics**, for procedural algorithms that grow, evolve and die over a lifetime. The first
algorithm is **trees** (space colonisation). Later ones (Physarum, reaction–diffusion, Lenia, domain-warped noise)
fit the same layer: one scene file per config, `Kind` picks the algorithm.

Peter's decisions (2026-11-17): an Organics layer, trees first.

## Where it sits

- `SceneType::Organic` (scene files in `reforce.Organic.package`, committed under `content/Packages/`) and
  `Layer::Organic`, a spawn layer like Particles, Sprites and Solids: `switch:organic:NAME`, `next:organic`,
  `kill:organic`, `life:organic:SEC`, `interval:organic:SEC`. Settings `Organic.*` like `Solid.*` (interval 0 = not in
  the default slideshow, Duration 40, fade 4); a Theme may set `Organic.Slideshow.Interval.Duration` (the content
  theme "Organics" does). Up to `reforce.Organic.MaxLive` (3) live at once; a fourth replaces the oldest.
- Trees are **line art drawn into the intensity buffer** with the WaveShape line pass: each branch segment is a
  round-capped capsule (`engine::LineSegment`) whose radius tapers from trunk to tip. So trees trail in the FlowField
  and take the ColorMap like everything else, and there is no new GPU code. Segments go after the WaveShape's and the
  particles' (drawn on top, overwrite, probe #15); each tree is one path, so per-path AA max-combines overlapping
  branches instead of adding them.
- Growth runs on the render thread in `engine::TreeGrowth` (pure C++, deterministic: seeded RNG, growth stepped in
  simulation time). Cost is bounded: at most `reforce.Organic.MaxNodes` (6000) nodes per tree and 8 growth iterations
  per frame, nearest-node search on a uniform grid.

## Trees: space colonisation

Runions et al. 2007, "Modeling Trees with a Space Colonization Algorithm":

1. Scatter `Pts` **attraction points** (the crown's shape) and plant `Rts` **roots**.
2. Each growth iteration, every attraction point pulls the nearest node within `Infl`. Every node that is pulled grows
   one segment of length `Seg` towards the average pull, bent by the **tropism** (`TrpX`, `TrpY`: light, gravity, wind)
   and a little random **jitter** (`Jitr`). Nodes pulled two ways split into branches.
3. Attraction points within `Kill` of a node are used up.
4. Before any point is in reach, the tips grow straight on (the trunk). The tree is complete when the points are used
   up or nothing can grow.

The shape of the point cloud is the species: a wide dome is an oak, a tall ellipse a poplar, a cloud below the root
gives roots or lightning, several roots in a field give neurons or coral.

**Thickness** follows the pipe model: a node's width grows with the number of tips it feeds,
`w = Tip + (Wdth - Tip) * sqrt(tips / rootTips)`, so the trunk thickens as the crown fills in.

**Growth in time**: `Grow` iterations per second (a per-frame expression, so it can burst on the beat:
`Grow="12 * (1 + 3 * exp(-6 * wrap(SYNC_BEAT)))"`). The newest generation is drawn at the fraction of its length
reached so far, so growth is continuous rather than stepping a segment at a time.

**Sway** (`Sway`, radians at the thinnest tips; a per-frame expression, e.g. bass-driven): each node turns about its
parent by a small angle that grows with thinness, travelling up the branches as a slow wave. Trunks barely move,
tips move most.

**Buds** (`Bud`, `BudW`): a dot on every tip at `Pen * Bud`, e.g. blossoms that open on the bar line.

**End of life** (`Wthr`): with `Wthr=1` (default) the tree withers during its fade-out: it ungrows, newest
generations first, back to the root. With `Wthr=0` it just dims with `FADER`.

## Coordinates

Tree units: y = ±1 at the bottom / top of the frame, x in the same units (x = ±1.78 at the edges of a 16:9 frame), so
circles stay round. `X`, `Y`, `Scal`, `Rot` move, scale and turn the whole tree (about the origin, after sway).

## Keys (`Kind="tree"`)

| Key | Tier | Default | Meaning |
|---|---|---|---|
| `Kind` | literal | `tree` | Algorithm. Only `tree` so far; unknown kinds refuse to start. |
| `PDur` | Pre | Organic.Duration | Seconds at full strength. |
| `Pts` | Pre | 300 | Attraction points (10..4000). |
| `PX`, `PY` | per point | an ellipse | Each point's position, evaluated once per point at the start with `s` = k / (Pts - 1) and `rnd`. Default: an ellipse .9 x .55 centred at (0, .3), evenly filled. |
| `Rts` | Pre | 1 | Roots (1..64). |
| `RtX`, `RtY`, `RtA` | per root | 0, -1, PI/2 | Each root's position and initial direction (radians, PI/2 = up), with `s` = k / (Rts - 1) and `rnd`. |
| `Seg` | Pre | .02 | Segment length. |
| `Infl` | Pre | .3 | Influence radius of a point. |
| `Kill` | Pre | 2 x Seg | Kill distance. |
| `Jitr` | Pre | .15 | Random wobble of each new segment's direction. |
| `Wdth` | Pre | 8 | Trunk width (LWdt units, like WaveShapes). |
| `Tip` | Pre | 1 | Tip width (LWdt). |
| `BudW` | Pre | 4 | Bud diameter (LWdt). |
| `Wthr` | Pre | 1 | 1 = ungrow during the fade-out, 0 = dim only. |
| `A#` | Load | | Once at start. |
| `C#` | per point / root | | Before each point's `PX`/`PY` and each root's `RtX`/`RtY`/`RtA`, ascending, so both coordinates can share one random angle and radius (`C0="2 * PI * rnd(1)"`, `PX="C1 * cos(C0)"`). |
| `B#` | Frame | | Per frame, persisting. `D#` is not evaluated. |
| `Grow` | FrameOutput | 12 | Growth iterations per second. |
| `TrpX`, `TrpY` | FrameOutput | 0, 0 | Tropism added to each new branch segment's direction (unit = the pull of the points; trunks ignore it). Below ~.5 it mostly curves the branches (the points pull them back), about 1 leans the whole crown, above ~1.5 it overwhelms the pull and the tree stops short. |
| `Sway` | FrameOutput | .15 | Sway at the thinnest tips, radians. |
| `X`, `Y`, `Scal`, `Rot` | FrameOutput | 0, 0, 1, 0 | Move, scale, turn the whole tree. |
| `Pen` | FrameOutput | 1 | Intensity of the branches (× `FADER` when `Wthr=0`). |
| `Bud` | FrameOutput | 0 | Intensity of the tip buds relative to `Pen`. |

Variables: as for Solids (`t`, `START_TIME`, `END_TIME`, `DT`, `FADER`, the audio scalars, `LINK_*`, `SYNC_*`), plus
`s` in the per-point and per-root keys.

## Phases

| # | Deliverable | Done when |
|---|---|---|
| 1 | `engine::TreeGrowth` + unit tests | deterministic, bounded, branches reach the crown, pipe-model widths |
| 2 | Scene type, `OrganicInstance`, `OrganicRun` (sway, wither, buds, segments) + tests | every key evaluates, segments finite and in frame |
| 3 | `Layer::Organic` through Director, Visualizer, commands, settings, themes, web API | `switch:organic:Oak` renders offline |
| 4 | Control panel Organics card, content configs, Organics theme, docs | Peter tries it |

Content (`content/Packages/reforce.Organic.package`): Oak, Willow, Roots, Lightning, Neurons, Kelp Forest, Radial
Bloom; theme `Organics` (one every ~30 s).

Cost (Release, 1920x1080): three trees growing at once (Neurons, Kelp Forest, Oak) peak at 6,412 segments; CPU
p99 0.27 ms for the whole step, GPU 0.17 ms average (the line pass).

## Later

Physarum, reaction–diffusion, Lenia and domain-warped noise as further `Kind`s (GPU compute, drawn through the sprite
pass like solids); L-system ferns; a 3D tree in the Solids camera.
Kinds beyond trees (pendulums, phyllotaxis, harmonograph, automata, Game of Life, L-systems, attractor): docs/rhythm-design.md, keys in docs/scene-format.md §5.8.

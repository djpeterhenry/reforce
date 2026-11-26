# Organics: procedural growth (PLAN 5N)

A new spawn layer, **Organics**, for procedural algorithms that grow, evolve and die over a lifetime. The first
algorithm is **trees** (space colonisation). Later ones (Physarum, reaction–diffusion, Lenia, domain-warped noise)
fit the same layer: one scene file per config, `Kind` picks the algorithm.

Peter's decisions (2026-11-17): an Organics layer, trees first.

## Where it sits

- `SceneType::Organic` (scene files in `reforce.Organic.package`, committed under `content/Packages/`) and
  `Layer::Organic`, a spawn layer like Particles, Sprites and Solids: `switch:organic:NAME`, `next:organic`,
  `kill:organic`, `life:organic:SEC`, `interval:organic:SEC`. Settings `Organic.*` like `Solid.*` (interval 0 = not in
  the default slideshow, Duration 40, fade 4); a Theme may set `Object.Slideshow.Interval.Duration` (the content
  theme "Organics" does). Up to `reforce.Object.MaxLive` (1) at full strength: a new one fades the oldest out under it, and a slideshow start waits until the live one begins its fade-out, so a long one (`PBar`) is never cut short.
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
parent by a small angle that grows with thinness, traveling up the branches as a slow wave. Trunks barely move,
tips move most.

**Buds** (`Bud`, `BudW`): a dot on every tip at `Pen * Bud`, e.g. blossoms that open on the bar line.

**End of life** (`Wthr`): with `Wthr=1` (default) the tree withers during its fade-out: it ungrows, newest
generations first, back to the root. With `Wthr=0` it just dims with `FADER`.

## Trees on the beat (2026-11-22)

Peter: Lightning should strike repeatedly, Oak sway every 2 beats, Radial Bloom vibrate on the beat, and Neurons not
just sit there once grown. Four generic tree features, all on the **beat clock** (`SYNC_BEAT` while it is locked,
else 120 BPM from the start), each off by default so the other trees are unchanged:

- **Strikes** (`Strk`, `SGap`, `SAln`): the tree grows `Strk` times (1..16) over its life. The first growth starts on
  the first strike line, every `SAln` beats on the phrase-aligned bar clock (1 = the next beat line, 4 = the next bar
  line; at once when the clock is on one), each next one `SGap` beats later (SGap 4 = a bar apart). Every strike replaces the last: the A vars run again (so a strike can pick its own place,
  `A0="1.3 * (2 * rnd(1) - 1)"`, `RtX="A0"`), the points and roots are evaluated again (rnd runs on) and it grows
  from its own seed, so each one has a different shape, deterministic per seed. A jumping clock re-bases on the beat
  line just passed. In the frame keys `s` = beats since the current growth began, so a config shapes each strike's
  flash and fade itself (Lightning: dim while the leader grows, full on landing at .6 beats, flickering away over two
  beats: `Pen="(.5 + .5 * (s >= .6)) * exp(-1.8 * max(0, s - .6)) * ..."`). Without `Strk`, `s` is the beats since
  the tree started.
- **Beat sway** (`SwyB`): instead of the free 0.3 Hz wave whose phase travels along the branches, the whole tree
  sways back and forth on the clock, one cycle per `SwyB` beats (Oak: 4 since Peter asked for it twice as slow, over
  to one side for two beats, back for two; first 2, over on one beat, back on the
  next). The root stays put; every node turns about its parent by `Sway`-scaled
  `(.15 + .85 thin^2) cos(2 pi beat / SwyB - .5 depth)`: the trunk bends a little, the thin branches most, the tips
  lag slightly behind, so it bends rather than slides.
- **Vibration** (`Vib`, `VibF`): every node moves out from its root and back by `Vib` × its distance, as
  `cos(2 pi VibF (.6 + .8 depth) frac(beat))`: all outwards together on the beat line, then ringing at different
  rates by depth (inner branches slower, tips `1.4 VibF`), so the bloom shivers. The envelope is the config's
  (Radial Bloom: `.09 exp(-5 wrap(beat))`, doubled on the bar line, tripled on a drop).
- **Pulses** (`Puls`, `PlsB`, `PlsE`, `PAll`): action potentials. On every line of `PlsE` beats one root fires: a
  bright, thicker band runs out along its branches from the cell body (which flashes) and reaches its deepest tip
  after `PlsB` beats; each tip flashes as the pulse arrives (a synapse). The next root to fire is one whose branches
  meet the last one's (nodes of both within ~2 segments, found on a hash grid), not the one before if there is
  another, so the firing cascades through the network; when `PAll` rises past .5 every root fires at once (Neurons:
  `PAll="SYNC_DROP"`). Pulses brighten up to `Pen + Puls` (clamped), so the resting branches need `Pen` below 1
  (Neurons: .55). At most 256 pulses run at once; they end `PlsB + 2` beats after firing. A pulse's front runs by
  depth (segments from its root), so it also times right on branches that grew later. A drop fires every root at 1.6 x
  strength (a brighter, thicker front: the front's bump is scaled by the strength, the width by `1 + .8 bump`).
- **Bars and phrases** (`PlsL`, `PlsP`; Peter 2026-11-22 "How do neurons line up with bars and phrases?"): on the
  phrase-aligned bar clock (`SYNC_BAR`, `SYNC_PHRASE`; without the clock 4 beats a bar and 16 bars a phrase from the
  start). On every bar line a **lead** root restarts the cascade with a `PlsL`-strong pulse (Neurons 1.7: brighter,
  thicker, a bigger cell-body flash), the leads taking turns in a seeded order of the roots, so each bar reads as a
  new wave; the plain beats continue the cascade from it. On every phrase line every root fires together at `PlsP`
  (Neurons 1: softer than a drop). A beat-line fire within half a beat of one of these is left out.
- **Rewiring** (`Rwir`, `RwrB`; Peter: "Neurons rewiring: good idea!"): on each phrase line (once the tree is grown,
  not while it fades out) a seeded share `Rwir` of the nodes is cut: side branches (a node whose parent branches) of
  3 nodes up to 12% of the tree, in a seeded order, none inside another. The cut sub-branches wither back over `RwrB`
  bars, tips first; then `TreeGrowth::prune` removes them (roots never; the survivors keep their order, positions and
  depths) and `addPoints` scatters `Pts x Rwir` new attraction points from the `PX` / `PY` keys again (rnd runs on;
  points already within `Kill` of a node are skipped), so new branches grow from the nearest nodes over the next bars.
  Each phrase shows a different network; the cell bodies never move and the pulses keep running on whatever is there.
  Bounded by `MaxNodes` (nodes) and 200,000 points; deterministic per seed. Neurons lives `PBar="32"` (two phrases) so
  it rewires once. Cost: Neurons at most 2,265 segments, 0.036 ms per step on average, 0.42 ms on the frame that cuts
  or prunes.

- **Leaves** (`Leaf`, `LeafW`, `LeafD`, `LeafA`, `LeafU`, `LFlt`; Peter 2026-11-22 "is there some way it could grow
  leaves?"): a leaf on every `LeafD`-th node of the outer twigs (nodes feeding at most 4 tips, once the crown feeds 8,
  so never on a bare trunk) and on every tip, chosen by a hash of the node and the seed (deterministic). Each is a
  pointed oval `LeafW` long (±20% per leaf): two hard-edged capsules, a body (radius .2 L) and a narrower tip (.075 L).
  Hard edges because a fat anti-aliased rim takes unrelated ColorMap colors (a speckled fringe in steep maps such as
  Metal Blood). A leaf sits on its twig's displayed position and direction, so it rustles with the sway, turned
  `LeafA` radians off it on alternating sides (tips almost straight on), plus a flutter of `LFlt` radians (per frame,
  e.g. `.25 exp(-6 wrap(beat))`: a shiver on the beat), each leaf out of step. It unfurls over `LeafU` beats from the
  moment its node qualifies (growing from nothing and opening from along the twig), so the crown leafs out as it
  grows, and stays once started even if its twig branches on. Leaves are drawn as a path of their own over the
  branches, at `Pen × Leaf`, so the ColorMap gives them their own color. While the tree withers (`Wthr=1`) they come
  loose, staggered over the first half of the fade-out, and fall about .9 units turning and shrinking; with `Wthr=0`
  they dim with the rest. At most 1,500 leaves (3,000 segments); Oak has about 400 (1,698 segments in all at most,
  0.024 ms per step on average).

**Neurons, what was considered.** (a) Pulses with a cascade and a drop firing them all (chosen): it changes every
beat, reads as a nervous system at a glance, keeps the grown network as the stage, and the timing is exact (fired on
the beat, arriving on the next). (b) Slow pruning and regrowth: keeps the shape changing but slowly (a change every
few bars is hard to notice and does not move with the music) and needs node removal in TreeGrowth. (c) Tips flashing
on the beat (as before): everything at once, no direction or travel. Pruning was added on top later (Rewiring,
tied to the phrases so the change comes on a musical line).

Cost (Release, offline, 40 s at 60 fps with the clock): Neurons at most 2,266 segments, step 0.044 ms average,
0.17 ms worst; Radial Bloom 1,476 / 0.029 ms; Oak 862 / 0.012 ms (1,698 / 0.024 ms with its leaves); Lightning 513 /
0.007 ms.

## Coordinates

Tree units: y = ±1 at the bottom / top of the frame, x in the same units (x = ±1.78 at the edges of a 16:9 frame), so
circles stay round. `X`, `Y`, `Scal`, `Rot` move, scale and turn the whole tree (about the origin, after sway).

## Keys (`Kind="tree"`)

| Key | Tier | Default | Meaning |
|---|---|---|---|
| `Kind` | literal | `tree` | Algorithm. Only `tree` so far; unknown kinds refuse to start. |
| `PDur` | Pre | Object.Duration | Seconds at full strength. |
| `PBar` | Pre | | Bars at full strength instead (wins over `PDur`): see scene-format §5.8. |
| `Pts` | Pre | 300 | Attraction points (10..4000). |
| `PX`, `PY` | per point | an ellipse | Each point's position, evaluated once per point at the start with `s` = k / (Pts - 1) and `rnd`. Default: an ellipse .9 x .55 centered at (0, .3), evenly filled. |
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
| `A#` | Load | | Once at start, and again at each strike after the first (`Strk`). |
| `Strk` | Pre | 1 | Growths (strikes) over the life, 1..16: each from fresh points and roots, on the beat clock. |
| `SGap` | Pre | 4 | Beats between strikes. |
| `SAln` | Pre | 1 | The first strike waits for a line every this many beats (1 = the next beat, 4 = the next bar line). |
| `SwyB` | Pre | 0 | Beats per sway cycle on the beat clock, about the fixed root; 0 = the free wave. |
| `VibF` | Pre | 3 | Vibration cycles per beat at the tips (.6 x at the root). |
| `PlsB`, `PlsE` | Pre | 1, 1 | Pulses: beats to reach the farthest tip; a root fires every `PlsE` beats. |
| `PlsL`, `PlsP` | Pre | 0, 0 | Strength of the lead pulse on every bar line, of all roots firing on every phrase line (0 = none). |
| `Rwir`, `RwrB` | Pre | 0, 4 | Share of the tree cut and regrown on every phrase line (0 = none; at most .9); bars the cut branches wither over. |
| `LeafW` | Pre | .05 | Leaf length (tree units; ±20% per leaf). |
| `LeafD` | Pre | 2 | A leaf on every `LeafD`-th node of the outer twigs (and on every tip). |
| `LeafA` | Pre | .9 | Leaf angle off its twig, radians, on alternating sides. |
| `LeafU` | Pre | 4 | Beats for a leaf to unfurl. |
| `C#` | per point / root | | Before each point's `PX`/`PY` and each root's `RtX`/`RtY`/`RtA`, ascending, so both coordinates can share one random angle and radius (`C0="2 * PI * rnd(1)"`, `PX="C1 * cos(C0)"`). |
| `B#` | Frame | | Per frame, persisting. `D#` is not evaluated. `s` = beats since the current growth (strike) began. |
| `Grow` | FrameOutput | 12 | Growth iterations per second. |
| `TrpX`, `TrpY` | FrameOutput | 0, 0 | Tropism added to each new branch segment's direction (unit = the pull of the points; trunks ignore it). Below ~.5 it mostly curves the branches (the points pull them back), about 1 leans the whole crown, above ~1.5 it overwhelms the pull and the tree stops short. |
| `Sway` | FrameOutput | .15 | Sway at the thinnest tips, radians. |
| `X`, `Y`, `Scal`, `Rot` | FrameOutput | 0, 0, 1, 0 | Move, scale, turn the whole tree. |
| `Pen` | FrameOutput | 1 | Intensity of the branches (× `FADER` when `Wthr=0`). |
| `Bud` | FrameOutput | 0 | Intensity of the tip buds relative to `Pen`. |
| `Vib` | FrameOutput | 0 | Radial vibration: each node out from its root by Vib x its distance, ringing on the beat. |
| `Puls` | FrameOutput | 0 (off) | Pulse brightness added to `Pen` (defining it turns pulses on). |
| `Leaf` | FrameOutput | 0 (none) | Leaf intensity relative to `Pen`. |
| `LFlt` | FrameOutput | 0 | Leaf flutter, radians (each leaf out of step). |
| `PAll` | FrameOutput | 0 | Every root fires when it rises past .5 (e.g. `SYNC_DROP`). |

Variables: as for Solids (`t`, `START_TIME`, `END_TIME`, `DT`, `FADER`, the audio scalars, `LINK_*`, `SYNC_*`), plus
`s` in the per-point and per-root keys (k / (count - 1)) and in the B vars and frame keys (beats since the current
growth began).

## Phases

| # | Deliverable | Done when |
|---|---|---|
| 1 | `engine::TreeGrowth` + unit tests | deterministic, bounded, branches reach the crown, pipe-model widths |
| 2 | Scene type, `OrganicInstance`, `OrganicRun` (sway, wither, buds, segments) + tests | every key evaluates, segments finite and in frame |
| 3 | `Layer::Organic` through Director, Visualizer, commands, settings, themes, web API | `switch:organic:Oak` renders offline |
| 4 | Control panel Organics card, content configs, Organics theme, docs | Peter tries it |

Content (`content/Packages/reforce.Organic.package`): Oak, Lightning, Neurons, Kelp Forest, Radial Bloom (Willow and
Roots removed 2026-11-22, Peter); theme `Objects: Trees` (one every ~30 s).

Cost (Release, 1920x1080): three trees growing at once (Neurons, Kelp Forest, Oak) peak at 6,412 segments; CPU
p99 0.27 ms for the whole step, GPU 0.17 ms average (the line pass).

## Later

Physarum, reaction–diffusion, Lenia and domain-warped noise as further `Kind`s (GPU compute, drawn through the sprite
pass like solids); L-system ferns; a 3D tree in the Solids camera.
Kinds beyond trees (pendulums, double pendulums, phyllotaxis, harmonograph, automata, Game of Life, L-systems, attractor,
dragon): docs/rhythm-design.md, keys in docs/scene-format.md §5.8.

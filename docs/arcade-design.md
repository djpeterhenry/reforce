# Arcade organics: game-inspired visuals on the beat (PLAN 5Q)

Peter (2026-11-22): video-game inspired visuals (an invader march, a pyramid hopper, light cycles; then vector rocks and a brick wall) as inputs to the
flow field, taking beat-synchronised actions. Settled the same day:

- **Drawn into the feedback.** Each game is an Organic kind (`Kind="invaders"`, `"hopper"`, `"cycles"`): line art
  through the OrganicCanvas into the intensity buffer, like Game of Life. So it trails in the FlowField and takes the
  ColorMap, and every frame draws the game crisp over its own smear. (Later, maybe: the game stirring the warp
  itself.)
- **Beat-synchronised.** Moves land on beat / bar / phrase lines of the beat clock (`OrganicStep::beat`, `bar`:
  the phrase-aligned clock when confident, else 120 BPM from the start). Breakdowns (`SYNC_BUILD`) and drops
  (`SYNC_DROP`) get their own moments.
- **Nobody loses.** The invaders never shoot back; the player never dies. Rounds end on musical lines, not on
  failure. (Exception, Peter 2026-11-22: a boxed-in light cycle may crash and explode, then rezzes in again.)
- **Our own art and names.** Inspired by the games' mechanics, never their bitmaps, characters, logos or titles (the
  repo is public).
- **Deterministic.** State is seeded (the organic's seed) and stepped from the beat count, so `--deterministic`
  renders repeat, and a step that jumps several beats catches up a bounded number of moves.

Shared groundwork (5Q.0): `engine/arcade/Arcade.hpp` (factories, key helpers, `dropOf` / `buildOf` / `onsetOf`),
filled shapes on the canvas: `fillPolygon` (even-odd, any shape), `rect`, `bitmap` (pixel art from strings, runs
merged), all as horizontal round-capped strokes ~2 px apart in frame pixels; `newPath` so a character overwrites the
floor under it. Tests: tests/test_engine_arcade.cpp, tests/arcade_harness.hpp.

Coordinates as for every organic: tree units, y = ±1 at the bottom / top of the frame, x = ±1.78 at the edges of
16:9; `X`, `Y`, `Scal`, `Rot`, `Pen` per frame. Every game fits its field to the frame's real aspect (`OrganicCanvas::halfWidth`), so a
square or ultra-wide output works too; at 16:9 nothing changes.


## Chase (`Kind="chase"`, 5Q.8)

Peter (2026-11-22): "something more inspired by the [light-cycle] movies. A 3D view following a motorcycle
dynamically." The light-cycle game itself (`CyclesSim`, the same rules, AI, crashes and rounds as `cycles`) seen in
3D from a camera chasing one cycle. Engine: src/engine/arcade/Chase.cpp / Chase.hpp (`ChaseView`: the game, the
director and the drawing; `ChaseCamera`: the projection, tested on its own); tests:
tests/test_engine_arcade_chase.cpp; content: Chase Normal (4 cycles, 40 x 24), Chase Swarm (8 on a wide fine grid at
Chase Normal's speed, walls and turns, a higher camera cutting every bar); Chase Duel was removed 2026-11-22.

- **3D on the CPU.** World units are cells (X along the columns, Z along the rows, Y up). A perspective camera (eye,
  look-at, vertical FOV, roll) projects to tree units; lines are clipped at the near plane, then to the frame
  (Liang-Barsky), polygons likewise (Sutherland-Hodgman), so nothing near the camera blows up. The frame's real
  aspect (`halfWidth`) sets the horizontal extent; the FOV is vertical.
- **The floor.** Grid lines on every cell line, the arena's border brighter, pulsing a little on each beat; beyond
  the border the floor goes on, dimmer and every other line, towards the horizon. Everything fades with distance
  (fog: about a tenth left at `Fog` cells, further out while the camera is craned up).
- **Walls** are vertical ribbons `WalH` high along each trail, coming out of the machine's tail: a dim filled body,
  a bright top edge, a dimmer foot, fading with age as in `cycles`, sinking as they retract, flashing on a drop. A
  crashed cycle's wall dissolves as outlines sinking into the floor. Right at the camera (within about 1.5 cells) a
  wall turns see-through (outlines only), so a camera riding beside a wall never stares into a slab.
- **The machines** (our own designs, `Bike`; Peter 2026-11-22: "refine the bike rendering ... it doesn't look very
  cool", then: the wheels clearly circular and glowing from the side, spinning at the right speed or not at all, more
  glow; a headlight was tried and removed). About 1.8 cells long, pointing along their motion and leaning into their turns, the body
  settling a little on each beat. Each is drawn as:
  - a light pool on the floor under it;
  - the body: a dark shell (the hull of its outline from the camera), its smooth top line on both flanks as the
    silhouette's highlight, a glowing seam along each side, the bottom line dimmer, a bright tail-light bar across
    the back;
  - the **wheels over the body**, always: dark discs, thick bright rims on both edges of the tyre, a hub ring and hot
    centre, and tread marks (short arcs just inside the rim) **rolling with the distance ridden** (the angle is the
    cells ridden, `Spd` per beat, over the wheel's radius: about 1.5 turns a second at 120 BPM, smooth, with four
    marks it never strobes; a drop's boost is not counted, so they never jump). From the side they are near-circles.
  - Glow: a wide faint bloom under the brightest lines (the rim edge nearer the camera, the top line, the tail light,
    the lamp), a halo under every glowing line, then the cores, each layer its own path. At speed (a drop's boost, a
    breakdown) two streaks trail off the tail. The light wall starts under the rear wheel.
  - Level of detail: a machine whose wheels are under about 12 pixels across gets single rims of 12 pieces, no bloom,
    tread marks, hub ring, seam or bottom line; its fills (pool, discs) and beam bands give way when the frame's fill
    budget is spent.

  Designs (Peter picked Arc for every config on 2026-11-22; Blade and Orb stay available with `Bike`):
  - `0` **Arc** (default): a low enclosed fairing arching from the tail over the rider to the nose, a seam along each
    flank.
  - `1` **Blade**: a long low wedge with a sharp nose and a tall swept tail fin, smaller wheels standing out of it.
  - `2` **Orb**: two huge open hubless rings (see-through, six tread marks), a slim spine with a rider hump riding
    high between their tops.

  Rezzing in: a shrinking diamond on the floor.
- **Occlusion** by the painter's algorithm: wall chunks (at most 3 cells long), machines, sparks and explosions are
  sorted by distance and drawn far to near; each wall chunk and machine starts a new path (its fill), then another
  (its edges), so nearer surfaces overwrite farther ones. Fills are budgeted (about 4200 fill strokes per frame,
  nearest walls first); walls past the budget get two horizontal lines for a body instead.
- **Explosions** in 3D: the white-hot flash, a light pillar shooting up, the ring racing out over the floor, sparks
  thrown up and falling, wall fragments tumbling.

The camera (the director), all on musical lines and seeded (the organic's seed and the cut line's number), so the
same beats give the same film:

- **The chase.** Behind and above its cycle, a little to one side (so the wall reads as a ribbon), looking ahead of
  it. The heading the camera follows is a spring lagging the cycle's (about a third of a beat), and the eye and the
  look-at point are springs too, so it swings wide round each 90-degree turn and rolls a little into it.
- **Beats.** On every beat line a FOV kick (+7 %) and the camera dipping (a fifth of `CamH`), stronger on bar lines
  and with the onset (`BEAT`), never down past the top of the walls.
- **Cut lines** (every `Cut` bars, aligned to the bar clock): it picks the cycle to chase (preferring cycles near
  others and near a fresh explosion, a little away from the one it had) and a shot: the chase (most often), low and
  fast beside the wall, alongside (a tracking shot from the side), ahead looking back at the cycle and its wall
  streaming out, or high behind. A new shot (or a far cycle) is a cut; the same shot on another cycle is a glide
  (the springs carry the camera across).
- **Phrase lines** (the round's reset, `Rsd` bars): from two beats before, the camera cranes up to a high wide shot
  over the whole arena, holds it as the walls burst and the new formation appears, drifting round a little, then
  swoops back down behind a cycle over the next three beats (alternating sides from phrase to phrase).
- **A drop** (the game's boost: twice the speed for a bar): a low, close, fast angle (just off the tail, +22 degrees
  FOV) for most of the bar, with speed lines streaming past; speed lines also through a breakdown (`SYNC_BUILD`).
- **A crash** of the chased cycle: the camera orbits the explosion, rising and pulling back, and cuts to another
  cycle on the first beat line 2.5 beats later.
- **Time:** the camera springs step in 1/60 beat substeps from the beat count; a jump of more than 8 beats (or back)
  snaps the camera to its shot instead of swinging across. Cost: about 0.07 - 0.15 ms per step (under 1 ms worst), at
  most about 7300 segments per frame at 1080p (6700 with the defaults, 7300 the swarm, 6000 close tall walls; a
  round's formation of machines close to the camera is up to about 5500 of them: lines, halos, blooms and fills).

| Key | Tier | Default | Meaning |
|---|---|---|---|
| `Num`, `Cols`, `Rows`, `Spd`, `Turn`, `Tail`, `Rsd`, `Mirr`, `Out` | Load | as `cycles` | The game, as for `Kind="cycles"`. |
| `Cam` | Load | 4.2 | Camera distance behind the chased cycle, cells (.5..60); the other shots scale with it. |
| `CamH` | Load | 1.5 | Camera height in the chase shot, cells (.05..60). |
| `FOV` | Load | 62 | Vertical field of view, degrees (15..130). |
| `WalH` | Load | .8 | Wall height, cells (0..10). |
| `Fog` | Load | 42 | Distance (cells) at which lines have faded to about a tenth (2..1000). |
| `Grid` | Load | .45 | Floor grid intensity (0..1; 0 = only the border). |
| `Cut` | Load | 2 | Bars between camera changes (.25..1024). |
| `Wdth` | Load | 2.4 | Line width (LWdt units) at the chase distance; nearer lines are wider, farther thinner. |
| `Shot` | Load | 0 | 0: the director mixes shots; 1..5: always that shot: 1 chase, 2 low, 3 alongside, 4 ahead, 5 high (cycles still change on cut lines). |
| `Bike` | Load | 0 | The machines' design: 0 Arc (every Chase config), 1 Blade, 2 Orb. |
| `X`, `Y`, `Scal`, `Rot`, `Pen` | Frame | 0, 0, 1, 0, 1 | As for every organic. |


## Rocks (`Kind="rocks"`, 5Q.5)

Vector rocks drift and slowly turn through a field that wraps at the frame's edges (a rock's width outside the frame,
so a rock is wholly off screen when it jumps to the other side: nothing pops). Our own ship sits in the middle: a
stretched hexagonal hull, twin barrels, swept fins, a cockpit dot. It turns to aim and fires; every shot connects.
Everything is thin line art with an optional halo (`Glow`): the halos go in a first path, the cores in a second over
them, since within one path the stroke with the most coverage wins. Engine: src/engine/arcade/Rocks.cpp; tests:
tests/test_engine_arcade_rocks.cpp; content: Rock Field, Rock Storm.

Beat use (the game runs on a grid of 48 ticks per beat, so it is the same at any frame rate):

- **Shots** leave on the fire lines (`Fire` per beat: 8ths by default) and fly about half a beat (a whole number of
  fire steps), aimed at where the target rock will be when they arrive. The shot arriving on a **hit line** (`Hits`
  per beat: each beat by default) splits the rock: large → two medium → two small → a burst of its own outline. The
  shots arriving between hit lines chip it (a flash and a spray of sparks off the rim). So the splits land exactly on
  the beat lines. The next target is one of the nearest rocks that will be on the frame then (small ones slightly
  preferred, so a field ends in bursts rather than stragglers).
- **Debris**: a split throws every other edge of the rock's outline as a short turning line plus sparks (dots); a
  burst throws every edge. They fly, slow and fade over 1–2 beats, which is what fills the FlowField's trails.
- **Thrust** on each beat line (and the flame on onsets): a flame and a push along the heading; the shot's recoil
  pushes back; a spring pulls the ship to the middle and it veers away from rocks that come close. It never dies.
- **Waves**: fresh large rocks drift in from the edges on the phrase lines (`Wave` bars, aligned to the phrase clock),
  and on the next bar line once the field is clear. The first wave is on screen from the start.
- **Drop** (`SYNC_DROP` rising past .6): a shockwave ring from the ship sweeps past the corners in a beat, shattering
  every rock it passes into its outline's edges; the ship holds fire while it is out, and the field refills on a later
  bar line.
- **Breakdown** (`SYNC_BUILD` over .35): the fire rate doubles (8ths → 16ths, more chips per hit) and the rocks speed
  up (× 1 + 1.5 `SYNC_BUILD`).
- **Catch-up**: a step that jumps ahead advances at most 4 beats of ticks; a longer jump (or a jump back) drops the
  shots in flight and carries on from the new beat.

Intensities (× `Pen`), apart so the ColorMap colours the parts differently: rocks .6 (a chipped rock flashes to .86),
ship .92, flame .74, shots 1, edge debris .5, sparks .8, shockwave .7; debris fades to 0.

| Key | Tier | Meaning |
|---|---|---|
| `Num` | Load | Large rocks per wave (4; 1..12). |
| `Fire` | Load | Shots per beat (2 = 8ths; one of 1, 2, 3, 4, 6, 8, 12, 16). Doubled through a breakdown. |
| `Hits` | Load | Hit lines per beat (1; .125..16, on the fire grid): the shots arriving on them split a rock. |
| `Spd` | Load | Large rocks' drift in tree units per beat (.12; 0..2); smaller rocks part faster. |
| `Wdth` | Load | Line width in LWdt units (1.1; 0..20). |
| `Size` | Load | Scale of the rocks and the ship (1; .2..4): a large rock's radius is .2 × Size tree units, the ship .2 × Size long. |
| `Wave` | Load | Bars between fresh waves (16, a phrase; 0 = only when the field is clear). |
| `Glow` | Load | Halo strength (.35; 0 = none, 1): a dim stroke 3.2 × as wide under each line. |
| `X`, `Y`, `Scal`, `Rot`, `Pen` | Frame | As for every organic. The field wraps at the frame's edges (its real aspect), so `Rot` or a large `X` / `Y` shows the wrap edges. |

Bounds: at most 48 rocks (a wave only comes while there is room for it to split fully), 640 pieces of debris (the
oldest give way), 32 shots; under 1,000 segments a frame even in a stress run (12 rocks a wave, 16 shots and 16 hits a
beat, a wave every bar, drops and breakdowns), about 6 µs a step.


## Bricks (`Kind="bricks"`, 5Q.6)

A wall of bricks at the top, a paddle at the bottom, a ball flying between them at a constant speed: every paddle
and brick hit lands on a tick line (`Rate` per beat; side-wall bounces land wherever they fall), the paddle is always
there in time, each hit breaks one brick, and the last brick breaks exactly on the cycle line (`Cyc` bars, a phrase
by default), like the Rubik cube solved on its 8-bar line. On the line the walls flash and a new wall (a new seeded
pattern) drops in over a beat. Content: Brick Wall (the defaults: 10 x 3, beats, 16 bars, `Spd` 1.5), Brick Storm
(16 x 4, 8ths, 16 bars, a fast ball, `Spd` 3), Brick Drift (7 x 4, beats, 32 bars, a slow ball, `Spd` .9, tilting
gently).

- **Constant speed, hits on the beat** (Peter 2026-11-22: the earlier per-bounce speed changes looked weird). The
  ball flies at `Spd` units per beat. A leg between two on-beat hits takes a whole number of ticks, so its path must
  be exactly `Spd` x ticks long: the planner picks the angle, and when a straight shot would be too short, a path off
  one or more side walls. The walls mirror the field, so such a path is a straight line in the unfolded plane (x past
  a wall continues in its mirror image, folded back); its length is sqrt(dy² + dx²) with dx the unfolded sideways
  distance, solved for. A volley is chosen as a pair at the brick hit: down to a paddle point at the ball's speed (any
  x will do: dx follows from the ticks) and up to the lowest brick of a column (present when the ball gets there),
  for every duration and side, the up leg's speed nudged at most 3 % (at the paddle only) so the folded landing point
  falls under the brick; paths must pass below the bricks, at most 3.5 sideways per unit up or down (6 if nothing
  else fits). The volley before the last ones looks ahead (on a copy of the plan, undone) so that the volleys up to
  the line still fit at the base speed. If nothing fits, the rules bend step by step (5 %, then paths through bricks,
  10 %, any speed): never at the content configs without a drop; after a drop about 1 volley in 400 (multiball
  near the line), and about 1 in 1000 over the tests' odd sizes.
- **Planned ahead, deterministic.** Volleys are decided in time order, a few beats (6) ahead of the clock, so the work
  is spread over the frames; the decisions are the same however far ahead it plans. Drawing is a pure function of the
  plan and the beat: a step that jumps many beats (or back) costs at most a new plan from the cycle's start, so
  joining mid-phrase shows the same game.
- **One brick per hit** (Peter 2026-11-22: lots of bricks breaking at once feels unnatural). The count works out
  through time instead: one brick is kept for ball 0's final hit on the line, and the rest are paced over the time
  before it (aiming to be done a volley or two early). Each volley prefers a length near the pace wanted (beats left
  / bricks left): longer volleys (flatter angles, more side-wall bounces) when there are few bricks, the quickest
  ones when there are many. With even fewer bricks, some hits only knock (the ball hits a brick kept for later, which
  flashes, and nothing breaks), and when there is nothing to aim at, the ball goes up through a gap in the wall to the
  ceiling and back to the paddle (the ceiling bounce off the beat, like the side walls). Only when the wall has more
  bricks than the time has hits at the quickest pace does a dithered share of the hits break two (the target and its
  nearest free neighbour), spread evenly over the cycle; three or more only for walls far too big for their cycle.
  The volleys just before the line look ahead (on a copy of the plan) so the final one reaches the last brick at
  the base speed, knocking instead of breaking if that keeps a brick more to aim at. Hits steer away from a seeded
  "finale" brick high in the wall, so the last one stands there. At the content configs (12 seeds x 2 cycles each):
  99.4–99.6 % of breaking hits break one brick, never three, and one brick breaks on the line in 21–24 of 24 cycles
  (two otherwise); before this change it was 6 % (Brick Wall), 0 % (Brick Storm) and 88 % (Brick Drift), with four or
  more bricks on every line.
- **The paddle** glides between ball 0's hits (smoothstep over the middle 60 % of the interval, so it waits under
  the ball) and meets it off-centre, on the side that sends it towards the next target; it glows for a moment on
  each hit.
- **Drop (SYNC_DROP rising past .6): multiball.** The plan is redone from each ball's next bounce: `Mult` extra balls
  split off ball 0 there, each hitting on the tick lines at the same speed and taking its share of the bricks, one per
  hit. Extra balls bounce off a faint shield along the bottom (one paddle cannot meet them all); near the line (or
  when nothing is left for them within the rules) the shield takes them, on a tick, so ball 0 alone breaks the last
  brick on the line. They come back on the next cycle when the drop came in its last two bars.
- **Breakdown (SYNC_BUILD past .35):** the ticks get `Bld` times finer (beats → 8ths) from the next bounce on, until
  the next cycle line: hits land on 8ths and shorter volleys are preferred, at the same speed.
- **Pieces.** A broken brick bursts into `Frag` spinning pieces (from a 3 x 2 grid over it) that fly up and out,
  fall and fade over `FLif` beats, shrinking: a pure function of the break time, so they survive into the next
  cycle and trail well in the FlowField. Rows take different intensities (a seeded scheme per wall: brighter up,
  down, alternating, waved), so the ColorMap colours them apart; a fresh wall lands bright.
- **Walls** (seeded per cycle unless `Lay`): 0 full, 1 narrowing downwards, 2 chequer of double bricks, 3 diamond,
  4 pillars under a beam, 5 a wave of column heights, 6 two blocks under a beam, 7 worn (a fifth missing).
- **Budget:** at the defaults at most ~1800 segments at 1920 x 1080 (filled rects are strokes ~2 px apart), mean
  step 0.004 ms, worst ~0.1 ms (a volley with its lookahead); jumping into the middle of a 40 x 16, 64-bar cycle at
  4 x 2 ticks per beat with 7 balls ~1.3 ms.
- **Fitting:** the field is `FldW` x `FldH` (default ±.9 of the frame's half width x ±.92: ±1.6 at 16:9), so it
  fits any aspect; a written `FldW` is used as is.

| Key | Tier | Meaning |
|---|---|---|
| `Cols`, `Rows` | Load | The wall (10 x 3; 1..40 x 1..16). Bricks get thinner when the rows would reach the paddle's half. One brick per hit fits when the wall has about as many bricks as the cycle has volleys (Brick Wall: 30 in 16 bars at `Spd` 1.5). |
| `Cyc` | Load | Bars per wall (16; 1..64): the last brick breaks on this line (phrase-aligned with the clock). |
| `Rate` | Load | Ticks per beat, where paddle and brick hits land (1; .25, .5, 1, 2, 3 or 4). |
| `Bld` | Load | Ticks x this through a breakdown (2; 1..4, 1 = off). |
| `Spd` | Load | Ball speed in tree units per beat (1.5; .2..8), constant (within 3 %, changed only at the paddle). Quickest when a tick's flight just covers the paddle-to-wall height (~1.4): 1.5 at beats, 3 at 8ths. |
| `Ball` | Load | Ball diameter (.06). |
| `Pad` | Load | Paddle width (.46). |
| `Gap`, `BrkH` | Load | Gap between bricks (.018) and brick height (.075). |
| `FldW`, `FldH` | Load | Half width and half height of the playfield (.9 x the frame's half width: 1.6 at 16:9; .92). |
| `Mult` | Load | Extra balls on a drop (2; 0..6, 0 = none). |
| `Frag`, `FLif` | Load | Pieces per broken brick (6; 0..12) and how many beats they fly (2). |
| `Lay` | Load | Wall pattern 0..7 (-1 = a seeded one per cycle). |
| `Wall` | Load | Intensity of the side walls and ceiling relative to Pen (.35); they flash to 1 on the line. |
| `X`, `Y`, `Scal`, `Rot`, `Pen` | Frame | As for every organic. |


## Invaders (`Kind="invaders"`, 5Q.1)

An invader march (src/engine/arcade/Invaders.cpp; content: Invader; Invader Swarm was removed 2026-11-22). A formation
of `Cols` x `Rows` aliens marches sideways one step per beat; at the edge it steps down a row instead and turns. A
cannon at the bottom slides under a target and fires on the 8ths; a hit alien bursts into its own pixels, which fly
out, slow down, sag and fade (lovely in the trails). A saucer crosses the top once per phrase and is shot down on a
bar line in a big burst. Nobody shoots back and nobody loses: a formation that reaches the cannon's row beams back up
to the top, and rounds end on musical lines.

**Our own art.** Three species of 11 x 8 pixels, two frames each, one per row band: the *gazer* (top row: a one-eyed
jelly whose tentacles ripple; its pupil looks the way the march goes), the *moth* (the next rows: wings up, wings
down) and the *boxbot* (the bottom rows: a square robot with a grille mouth, antennae and legs shuffling). The cannon
is a domed *turret* on treads that roll as it slides; the saucer is a *halo craft* (a hollow dome over a hull whose
lights chase on the 16ths). Hits flash a small starburst. All drawn with `OrganicCanvas::bitmap`.

**On the beat.** Everything happens on a 16th-note tick grid (4 ticks per beat), stepped from the beat count; moving
things (shots, debris, the saucer, the cannon's slide) are drawn from closed-form positions in beat time, so the
picture is the same at any frame rate.

- *March*: one step per beat × `Rate`. A sweep is 15 sideways steps plus the step down, 16 steps: 4 bars at Rate 1,
  so the steps down land on bar lines. Through a breakdown the march hurries, quantised: `SYNC_BUILD` ≥ 1/3 doubles
  it (8ths), ≥ 2/3 doubles it again (16ths). The last eighth of a formation hurries one notch too, like the classic.
  Each step toggles the animation frame.
- *Cannon*: `Fire` shots per beat (2: the 8ths; 4: the 16ths). After a shot it lingers for half the gap, then slides
  (eased) under its next target, arriving on the next shot's tick; the target is the nearer of two random open
  columns, aimed where the column will be when the shot lands. A shot goes for the lowest alien of its column, and
  lands on a 16th line (its speed is set so it arrives exactly then). `Aim` is the share of shots aimed at an alien;
  the rest go through the gaps and off the top.
- *Saucer*: per 16-bar phrase it flies in at bar 10 (from a random side) and is shot down exactly on bar 12's line
  (the cannon moves under it a beat before its shot): ~110 bits of its pixels and a ring of sparks.
- *Waves*: a fresh wave descends from the top in 8th-note steps over one bar. On `Wave`-bar lines (16: the phrase
  lines) a new wave comes in and what is left of the old one dissolves upwards; a cleared formation is replaced on
  the next bar line.
- *Drop* (`SYNC_DROP` rising through .5): every remaining alien explodes, cascading outwards from the one nearest the
  cannon, within one beat; a fresh wave comes in on the next bar line.
- *Reaching the cannon's row*: the formation beams back up to its starting height over one beat (16th steps, with
  tractor beams), and marches on. No invasion, no game over.
- *Catch-up*: a frame may advance at most 64 ticks (4 bars); a bigger jump starts a fresh game 4 bars before now and
  plays those; a jump back by a bar or more starts a fresh game. Without the beat clock: 120 BPM from the start.

**Intensities**: every species at `Aln` (all rows alike, so none fades into the background; `RShd` darkens each species below the top one by that much, e.g. .12), the cannon at `Cann`, shots and flashes at `Shot`,
debris starting at its alien's level × `Debr` and fading, the saucer at Pen; so the ColorMap gives them different
colours (in palettes made for full-intensity lines, levels near 1 read best).

**Cost**: at 1920 x 1080 the default (11 x 5) draws at most ~3500 segments per frame (bursts and a drop included),
a 16 x 6 swarm at most ~5800; a step costs ~0.03 ms. Debris is capped at 2400 dots, shots at 32.

The run's log line (start and end) carries a tally: waves, hits, saucers, beam-ups, drops, aliens left.

| Key | Tier | Default | Meaning |
|---|---|---|---|
| `Cols`, `Rows` | load | 11, 5 | The formation (1..24, 1..8). Rows: gazers on top, then moths, then boxbots. |
| `Rate` | load | 1 | March steps per beat (.25..4), before the build speed-up; quantised to the 16th grid. |
| `Fire` | load | 2 | Shots per beat (0..4; 0 = no regular shots, the saucer is still shot down), on the 16th grid. |
| `Aim` | load | .7 | Share of shots aimed at an alien (0..1); the others go through gaps. |
| `Wave` | load | 16 | Bars per wave (0 = only when cleared; else at least 1). |
| `Pix` | load | .0115 | Alien pixel size in tree units (.002 to .05); shrunk so the formation fits (≤ 2.6 wide, ≤ .9 tall). |
| `Sauc` | load | 1 | 1 = the saucer each phrase, 0 = none. |
| `RShd` | load | 0 | Row shading: each species below the top one this much dimmer (0 = all rows alike; .12 = the first look). |
| `Aln`, `Cann`, `Shot`, `Debr` | frame | 1, 1, 1, 1 | Intensities (× Pen) of the aliens, cannon (the aliens' colour by default), shots / flashes, debris. Shots are bold bolts: 2 pixels wide, 7 long, a bigger head and a fading tail. |
| `X`, `Y`, `Scal`, `Rot`, `Pen` | frame | | As for every organic. |

(The pixel size is `Pix`, not `Px`: `PX` is the trees' per-point key.)


## Pyramid hopper (`Kind="hopper"`, 5Q.2)

A round little creature (a ball body on two feet with a two-leaf sprout, eyes towards where it hops, none from behind)
hops one cube per beat over an isometric pyramid of cubes. **Always dark to bright** (Peter 2026-11-22): every cycle
starts with all tops dark, and each landing steps the top it lands on one state brighter (`Levels` 3: dark, middle,
bright, so every top takes two landings). The route is planned so the **last top turns bright exactly on the cycle
line** (`Cyc` bars, like the Rubik cube solved on the 8-bar line); on the line the finished board flashes and goes
dark again for the next cycle. Nobody loses: balls bounce down past the player, never onto it. Content: Pyramid
(9 rows, 16 bars, off-beat balls; Pyramid Hop and Pyramid Mini were removed 2026-11-22).

- **The board** (engine/arcade/Hopper.hpp: the model, apart from the drawing so tests check the rules). `Rows` rows,
  cube (r, c) with c = 0..r; a hop goes to a diagonal neighbour one row up or down. Flat line art: per cube a top
  diamond in its state's intensity and two side faces at fixed darker intensities, all inset a little over the
  pyramid's dark silhouette (one path, max-combined), so dark seams separate the faces. Fitted to the frame.
- **Beat use.** The hop clock is the beat clock's beats, shifted by whole beats onto the phrase-aligned bars, so
  landings are on beat lines and cycles start on bar lines (no clock: 2 beats per second of age). The creature sits
  on its cube for the first fifth of a beat (squashed by the landing, crouching), then arcs over the rest and lands
  on the next beat line; its shadow stays on the cubes. A landing flashes the top it lands on.
- **The line** (the first beat of a cycle): the finished, all-bright board flashes twice in the first half beat (the
  sides light up on the 16ths), then the tops go dark in a quick cascade down the rows (apex at half a beat, the
  bottom row at .85), before the first landing.
- **The route** (once per cycle: ~0.05 ms for 7 rows, under ~2.5 ms for any size and `Levels`). The pyramid's graph
  is two-coloured by row parity and every hop changes colour, so each colour gets a fixed share of the 4 x `Cyc`
  landings; `Cyc` doubles until both shares cover their cubes `Levels` - 1 times (7 rows fit 8 bars exactly with 2
  levels: the 16 even-row cubes take every even-row landing, the 12 odd-row cubes share 16; 8 rows need 16 bars; 3
  levels double that: 7 rows 16 bars, 4 rows 8). A seeded depth-first search with Warnsdorff ordering finds a walk
  from where the last cycle ended that lands on every cube `Levels` - 1 times, the last of those steps exactly on the
  line (cuts: reach, parity shares of the steps left, a step left for the last landing, bottom corners reachable only
  through a neighbour whose last step then cannot be the cycle's last; short searches restarted with fresh seeded
  orders, as the backtracking is heavy-tailed). Every cycle has its own route. Landings beyond a cube's steps (on a
  bright top) change nothing.
- **Balls** drop onto the apex on bar lines (`Ball` per bar; not on the cycle line, and only while they can bounce
  off the bottom before the next one) and bounce down one row per beat, on the beat or on the off-beat (`Offb`). Each
  bounce goes left or right (seeded), searched so a ball never lands on the player's cube, never swaps cubes with it
  over a beat, and off-beat never lands where the player just was or is about to be; a ball with no such path is not
  dropped. Past the bottom row it falls away.
- **A drop** (`SYNC_DROP` rising): every ball on the board bursts into sparks, a flash runs down the rows over all the
  faces, and a spinning disc picks the creature up where it is, swings out over the nearer edge, round and up, and
  drops it on the apex two beat lines later, when a route from the apex still brightens every remaining top by the
  line (always at the start of a cycle), else back onto its planned route (searching anew if the missed landing would
  have stepped a top). With no route either way the next hop is a high spinning jump instead. One per 4 beats.
- **A breakdown** (`SYNC_BUILD`): hops lift up to 2.6 x and a glint runs over the tops (only ever brighter, so a top
  never looks unturned).
- **Determinism.** Routes and balls are seeded per cycle by the organic's seed; the board is a pure function of the
  plan and the position in the cycle, so a jump of any size plans once (from where the player stood) and draws.
- **Cost** (1080p, the 7-row default board): at most ~5000 segments (the dark silhouette ~450 strokes, faces ~4400, creature and
  balls a few hundred); ~0.06 ms per step on average (a route plan, once per cycle, ~0.05 ms; worst ~0.6 ms).

| Key | Tier | Meaning |
|---|---|---|
| `Rows` | load | Pyramid rows, 3..10 (7: 28 cubes). |
| `Cyc` | load | Bars per board (8); doubled until the board fits (see above). |
| `Levels` | load | Top states, 2..3 (2): every cycle starts all dark, each landing steps a top one state brighter. |
| `Ball` | load | Balls per bar, 0..4 (1; .5 = every other bar). |
| `Offb` | load | 1: balls bounce on the off-beat (0). |
| `Size` | load | Cube width in tree units (0 = fit the frame). |
| `Hop` | frame | Hop height in cube widths (.55); balls hop .75 of it. |
| `TLo`, `THi` | frame | Top intensity of the dark and the bright state (.6, 1), relative to Pen. |
| `SidL`, `SidR` | frame | Left and right side intensities (.22, .4). |
| `Edge` | frame | The seams, outlines and shadows (.04). |
| `CPen`, `BPen` | frame | Creature body (.9) and ball (.72) intensity. |
| `X`, `Y`, `Scal`, `Rot`, `Pen` | frame | As for every organic. |


## Light cycles (`Kind="cycles"`, 5Q.3)

Racers on a grid seen from above, each leaving a wall behind it (engine/arcade/Cycles.cpp; the game itself is
`CyclesSim` in Cycles.hpp, separate from the drawing so the tests check it move by move). Content (manual only, parenthesized): (Chase Normal Top) (Chase Normal's
game from above: 4 on 40 x 24) and (Chase Swarm Top) (Chase Swarm's: 8 on 64 x 36); the Trail configs were removed
2026-11-22.

- **The arena.** `Cols` x `Rows` cells (48 x 27: square cells in 16:9), centred, at most 3.36 x 1.88 tree units at
  16:9 (the width follows the frame's aspect), a border around it and optional faint grid lines that pulse on every
  beat.
- **Riding on the beat.** A cycle moves one cell per tick, `Spd` ticks per beat, gliding between cell centres, so
  every head is exactly on a cell centre on every beat line. It turns 90 degrees only on turn lines: beats (`Turn` 1),
  8ths (2) or 16ths (4), where they fall on a cell centre. Every cell it rides through is wall until `Tail` bars
  later; the oldest end of the wall shrinks away smoothly, so the arena never fills.
- **Drawing.** Each cycle at its own intensity (spread over .62 .. 1, so the ColorMap tells them apart): the wall a
  little dimmer than the head and fading towards its retracting tail, drawn as a few long lines (straight runs
  merged, a vertex only at a corner); the head a dart of our own (an arrowhead with swept fins and a notched tail)
  with a hot core, swinging round over the first part of a move after a turn. All of it goes into the feedback.
- **The AI.** At each turn line a cycle looks at going straight, left and right: the path to the next turn line must
  be free (walls, the arena's edge, and the paths the other cycles reserved this tick), then a depth-first look-ahead
  over the next six turn lines (walls counted where they will still stand) and the room behind (a flood fill over
  the points it can turn at). Turns by choice are seeded: likeliest on a bar line (about half the time), now and then
  on a beat, rarely in between, earlier rather than later when a wall is coming up, towards open ground. Pairs start
  as mirror images (left-right, up-down or about the centre, with the formation) and the partner copies its leader's
  moves mirrored while it safely can (`Mirr`: how often a pair does), which gives the choreographed look.
- **Crashes** (Peter 2026-11-22: some may crash, and explode). The AI keeps trying to survive, so a cycle crashes
  only when it is truly boxed in (no safe way at its last turn line: it then rides the way that keeps it going
  longest, hoping a wall retracts or the next turn line opens a way) and the next cell is wall. Two heads meeting
  both crash; a cycle whose path a boxed-in one cut across crashes too (counted as rammed). A crash is an explosion
  where the head meets the wall: a white-hot flash (a third of a beat), a ring racing out (three quarters of a
  beat), 36 sparks and 10 short wall fragments spinning out (fading over one to two beats); the crashed cycle's wall
  dissolves from the crash back to its tail over a beat, each cell a spark as it goes. The cycle is out for `Out`
  bars (2) and rezzes in on the first bar line after that, at a free cell one in from the border with a clear run
  ahead, heading in, marked by the shrinking diamond (no room: the next bar line); a phrase line first brings it
  back with the formation. Occasional: in 1000 beats with the defaults 4 cycles crash about 4 times, the swarm about
  4 times. Eight cycles with four-bar walls crowd the default 48 x 27 arena (60 crashes in 1000 beats): use a finer
  grid or shorter walls.
- **Rounds.** Every `Rsd` bars (16: on the phrase lines with the phrase clock) the round ends: every wall bursts into
  a spray of sparks (dots flying out and fading over 1.5 to 3.5 beats, up to 2400) and the cycles start again from a
  formation (racers facing in from the sides, a pinwheel from the corners, a burst from the centre, or columns from
  the bottom), each marked by a shrinking diamond for a beat.
- **Drop** (`SYNC_DROP` rising past .5): from the next beat line, twice the speed for a bar (still on cell centres
  at every beat line); the walls and border flash, fading over the bar.
- **Breakdown** (`SYNC_BUILD` at least .5, from the beat line on): twice the speed, and turns on twice as many lines
  (beats become 8ths).
- **Time** is the beat count only (the beat clock when confident, else 120 BPM from the start), so the same beats
  give the same game. A step that jumps ahead replays up to 8 beats; more (or the clock jumping back) starts a fresh
  round on the new beat line instead. Crashes, explosions and rezzing in are seeded and stepped the same way. Cost:
  about 0.004 ms per frame for 4 cycles, 0.01 ms for the swarm (0.2 ms on the worst frames); at most about 600
  segments per frame (the heads' fills are most of them; sparks are capped at 4000).

| Key | Tier | Default | Meaning |
|---|---|---|---|
| `Num` | Load | 4 | Cycles, 1..8. |
| `Cols`, `Rows` | Load | 48, 27 | The grid (8..400, 6..300). |
| `Spd` | Load | 2 | Cells per beat, 1..8. |
| `Turn` | Load | 1 | Turn lines per beat: 1 (beats), 2 (8ths), 4 (16ths). |
| `Tail` | Load | 4 | Bars a wall stands before it retracts (.25..64). |
| `Rsd` | Load | 16 | Bars between rounds (0 = never). |
| `Mirr` | Load | 1 | 0..1: the chance that a pair rides as mirror images in a round. |
| `Out` | Load | 2 | Bars a crashed cycle is out; it rezzes in on the bar line after (.25..64). |
| `Grid` | Load | 0 | Grid line intensity (0 = none), pulsing on the beat. |
| `Brdr` | Load | .5 | Border intensity. |
| `Wdth` | Load | 3 | Wall width in LWdt units (grid lines .3 of it, the border .6). |
| `X`, `Y`, `Scal`, `Rot`, `Pen` | Frame | 0, 0, 1, 0, 1 | As for every organic. |

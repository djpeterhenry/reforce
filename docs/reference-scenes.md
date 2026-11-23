# Reference scenes (PLAN task 1.3)

These are the test scenes to bring up first, for each scene type. Within each type they are
ordered from simple to complex. Each one is chosen to exercise something the earlier ones
don't. Paths are relative to `scenes/Packages/`, the local, git-ignored copy of the G-Force
content (see `scripts/sync-scenes.sh`). Scene contents are SoundSpectrum's and must not be
copied into the repo. Committed tests should use excerpts or scenes we write ourselves.

Feature names follow [scene-format.md](scene-format.md). Counts come from
[corpus-stats.md](corpus-stats.md) and from a per-file feature scan.

## WaveShape

| # | Scene | Why |
|---|---|---|
| 1 | `G-Force.WaveShape.package/WaveShapes/Simple Vertical.txt` | Smallest real WaveShape: one path from `mag(s)`, constant `Pen`/`LWdt`, `Aspc=1`, `ConB=1`, default `Stps`. Exercises the basic draw path. |
| 2 | `G-Force.WaveShape.package/WaveShapes/Rotating Corridor.txt` | The documented WaveShape [RC]. It covers all three tiers (A at load, B per frame, C per step), two paths, `Stps=NUM_SAMPLE_BINS`, and the `#waveshape-particle-also` tag, and shows `B0` defined but read as `b0` (case-insensitive names). |
| 3 | `G-Force.WaveShape.package/WaveShapes/DT/Bass Circle.txt` | Minimal **B-var persistence**: `B0` reads its own previous value (a one-pole smoother over `mag()`), which needs a defined starting value. Polar shape with `Aspc=1`, and lines with leading whitespace. |
| 4 | `G-Force.WaveShape.package/WaveShapes/Genesis Device.txt` | **Instances plus shared state**: `NUM` is randomized, B vars are shared across `ID` with an `ID == 0` guard, B vars refer forward/to themselves (an integrator: `B3` uses `B3`, `B2` uses `B3`), `level` (undocumented), `wrap`, empty `B4=""`, `A0="t"` start-time capture, and `LWdt`/`Pen` from A/C vars. |
| 5 | `G-Force.WaveShape.package/WaveShapes/Lifeshift.txt` | First **vector-VM** case: `{...}` array literals in A vars indexed per instance with `map1D(…, ID/(NUM-1), 0)`, `pow`, `fft(trwv(...))`, `Stps="3 * NUM_FFT_BINS"`, quoted `NUM="8"`, and spaces around `=`. |

Stress tests once those pass: `JRM/Flakes.txt` (heavy matrix use: `vectorLR`/`row`/`col`/`select`/`choice`, `PDur`, `X_EXTENTS`) and `JRM/Adapter.txt` (a 21 KB expression, the largest in the corpus). Both are under `G-Force.WaveShape.package/WaveShapes/`.

## Particle

| # | Scene | Why |
|---|---|---|
| 1 | `G-Force.WaveShape.package/Particles/Raindrops.txt` | Smallest particle: dots only (`ConB=0`), and `X0`/`Y0`/`LWdt` come straight from `rnd()`, so it tests whether `rnd` gives a new value at each step (open question 4). Has no `Pen` envelope, so the engine's fade behavior shows. |
| 2 | `G-Force.WaveShape.package/Particles/Spinners.txt` | The documented particle [SP]: `NUM="2 + rnd( 5 )"` (truncated), `ID`, and the "A once, B per ID" rule. It is the baseline for spawn and lifetime handling with the default duration. |
| 3 | `G-Force.WaveShape.package/Particles/Gold/Ring.txt` | Deterministic randomness per instance: `srand(B0 + A0)` in a B var reseeds `rnd()` for each `ID`. Also uses `dt` (life progress 0→1) in the projection, empty values (`A2=""`, `C0=""`), and a long C-var chain (3D rotation and perspective). |
| 4 | `G-Force.WaveShape.package/Particles/JRM/Dots.txt` | **`PDur`** (60 s), the `end_time < 1e30` particle check, and `s` used as a vector inside an **A** var (`{cos(2πs), sin(2πs)}`, unpacked with `row()`). Also `Num`/`Lwdt` in non-canonical case and a `/* */` header that contains the `#waveshape-particle-only` tag. |
| 5 | `G-Force.WaveShape.package/Particles/Ted/Fern.txt` | IFS fractal. C vars must **carry over between `s` steps** (`C5 = f(C7, C8)`, then `C7="C5"`), with `rnd` at each step choosing the transform. If step evaluation is wrong, this is the scene that shows it (open question 3). |

## FlowField

| # | Scene | Why |
|---|---|---|
| 1 | `G-Force.FlowField.package/Expand and Turn.txt` | Simplest **polar** field: `srcR=".99 * r"`, `srcT="theta - .015"`, `Aspc=1`. Tests the polar → source conversion and the aspect handling. |
| 2 | `G-Force.FlowField.package/Linear Spread.txt` | The documented field [LS]: **cartesian** `srcX`/`srcY` with `rnd()` **per pixel**. |
| 3 | `G-Force.FlowField.package/Flee.txt` | **C var evaluated once per row**. `C0="rnd( A1 ) - A0"` must be a single random value per scan line, not per pixel [FL]. Uses A-var chains. |
| 4 | `G-Force.FlowField.package/Swirly.txt` | Per-pixel **D vars** (6) built on 18 A vars [SW], with lower-case `a0` references. A realistic example of the "compute the source-coordinate texture at load" approach. |
| 5 | `G-Force.FlowField.package/Gold/Noise Mosaic.txt` | **Vector-VM FlowField**: `noise`, `smooth`, `map2D`, `vectorLR`, `vector`, `X_EXTENTS`. Its comments document the row-wise evaluation model [NM]. |

Extra baseline: `G-Force.FlowField.package/Nowhere.txt` (`Aspc=0`, the identity field `srcX="x"`, `srcY="y"`). It isolates the per-frame decay and the sampling filter.

## FlowField Union

| # | Scene | Why |
|---|---|---|
| 1 | `G-Force.FlowField Union.package/Horizontal Strip.txt` | Simplest `ALFA`: a cartesian strip from `abs(y)`, with random A vars. |
| 2 | `G-Force.FlowField Union.package/Birth Cone.txt` | The documented Union [BC]: a radial `ALFA="A1 * ( A0 - r )"` disk with a soft edge. ALFA values outside 0..1 must clamp. |
| 3 | `G-Force.FlowField Union.package/Sectors.txt` | `ALFA` from `theta`. Deliberately overshoots to just outside [0,1] (the comment explains why), which tests clamping at both ends. |
| 4 | `G-Force.FlowField Union.package/Vesica Piscis.txt` | **D vars** in a Union, `Aspc=1`, `sqwv`/`hypot`, and the rare XOR operator **`^^`** (`ALFA="D0^^D1"`). |
| 5 | `G-Force.FlowField Union.package/Sines of Life.txt` | Most complex Union: a D var with `wrap`/`theta`, `ALFA` mixing `r` and `theta`. It also has a **duplicate key** (`A0` defined twice), which forces a first-wins vs last-wins decision. |

## ColorMap

| # | Scene | Why |
|---|---|---|
| 1 | `G-Force.ColorMap.package/ColorWheel.txt` | The documented **HSV** map [CW]. Only `H=".03 * t"` is set, so it tests the default `S`/`V`, the time-only recompute, and **H wrapping**. |
| 2 | `G-Force.ColorMap.package/Gold/Lust.txt` | HSV as a function of `i` with random A vars and fractional powers (`(1-i)^.3`, `i^A1`), which checks the 256-entry sampling of `i`. |
| 3 | `G-Force.ColorMap.package/Clear Blue Water.txt` | The documented **RGB** map [CBW]: B and C vars, `R`/`G`/`B` keys (and `B` the color vs `B0` the var). Also has the lexer trap of a `"` inside a `//` comment that follows a quoted value. |
| 4 | `G-Force.ColorMap.package/Gold/CyanoChroma.txt` | **`CCal` override** (`.0333`), a per-frame B var driving a per-`i` C var, `clamp(pow(sin(…), 80))` (a steep exponent, so precision matters). HSV. |
| 5 | `G-Force.ColorMap.package/Ted/Neon II.txt` | Most complex text map: RGB from 8 B vars and 5 C vars, using `trwv`/`sqwv`/`clip` and lower-case var references. |

Also cover one **PNG ColorMap**: `G-Force.ColorMap.package/Charcoal.png`, a horizontal N×1 strip whose leftmost pixel is black.

## Sprite

Only four Sprite `.txt` files exist, so this list is all of them plus one image-only sprite.

| # | Scene | Why |
|---|---|---|
| 1 | `G-Force.Sprite.package/Space/Comet.jpg` | Image with no sibling `.txt`, so it runs entirely on `_default_sprite_params.txt`. Tests default lookup and image loading. |
| 2 | `G-Force.Sprite.package/_default_sprite_params.txt` | The documented sprite params [DS]: `X`/`Y`, `Sped` from an A var, `EPen="1"`, `Flag=7` (ShrinkToFit, EnlargeToFit, MaintainAspect), `Invt`, and an unused B var. |
| 3 | `G-Force.Sprite.package/G-Force Logo.txt` | Sibling `.txt` overriding the defaults for `G-Force Logo.png`. `Flag="16397"` turns on AlphaBlend, DontCrop, MaintainAspect and ShrinkToFit, and `EPen=".4"` gives a partial trail. |
| 4 | `G-Force.Sprite.package/Space/Astronaut.txt` | Random placement from A vars and `EPen="FADER"` (the undocumented fade envelope). |
| 5 | `G-Force.Sprite.package/Space/Moon.txt` | Motion: a per-frame B var from `t` minus the captured start time (`A5="t"`), `PDur="10 + rnd( 50 )"`, and lower-case var references. |

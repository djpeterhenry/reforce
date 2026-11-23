# G-Force Scene ("Config") Format Specification

Status: reverse-engineering reference for the reforce reimplementation.
Sources: the SoundSpectrum documentation bundled in `scenes/Documentation/`, the
documented example configs that ship with G-Force, and observations of the local
content corpus in `scenes/Packages/`. All of that content belongs to SoundSpectrum.
This document paraphrases it and quotes only short fragments.

## Citation keys

| Key | Source |
|---|---|
| **[CP]** | `Documentation/config-programming.html`: syntax and language reference |
| **[CU]** | `Documentation/customizing.html`: prefs (slideshow, transitions, ColorMap map function) |
| **[BG]** | `Documentation/background.html`: "How G-Force Works" |
| **[FAQ]** | `Documentation/faq.html` |
| **[TS]** | `Documentation/troubleshooting.html` |
| **[VH x.y]** | `Documentation/version-history.html`, entry for version x.y |
| **[VC]** | `Documentation/VectorC.h`: VectorC VM built-in declarations |
| **[RC]** | `Packages/G-Force.WaveShape.package/WaveShapes/Rotating Corridor.txt` (the documented WaveShape) |
| **[SP]** | `Packages/G-Force.WaveShape.package/Particles/Spinners.txt` (the documented Particle) |
| **[CW]** | `Packages/G-Force.ColorMap.package/ColorWheel.txt` (the documented HSV ColorMap) |
| **[CBW]** | `Packages/G-Force.ColorMap.package/Clear Blue Water.txt` (the documented RGB ColorMap) |
| **[LS]** | `Packages/G-Force.FlowField.package/Linear Spread.txt` (the documented FlowField) |
| **[SW]** | `Packages/G-Force.FlowField.package/Swirly.txt` (FlowField D vars) |
| **[FL]** | `Packages/G-Force.FlowField.package/.../Flee.txt` (FlowField C vars) |
| **[NM]** | `Packages/G-Force.FlowField.package/Gold/Noise Mosaic.txt` (row-wise FlowField evaluation) |
| **[BC]** | `Packages/G-Force.FlowField Union.package/Birth Cone.txt` (the documented FlowField Union) |
| **[DS]** | `Packages/G-Force.Sprite.package/_default_sprite_params.txt` (the documented Sprite) |
| **[PKG]** | `Packages/*/_package.py` |
| **[corpus]** | Observed in the local scene files. This is evidence of what shipped content relies on, not documented behavior. |

[CP] lists seven "Documented Configs" as the canonical tutorials: ColorWheel, Clear Blue
Water, Linear Spread, Rotating Corridor, Spinners, Default Sprite, and Birth Cone. All of
them except "Default Sprite" are present locally. Its content now lives in
`_default_sprite_params.txt` ([DS]).

---

## 1. Overview and frame pipeline

G-Force has five config types, plus two Python-based types (TrackAnimation and Library) that
are out of scope here:

| Type | Package dir | Role |
|---|---|---|
| WaveShape | `G-Force.WaveShape.package/WaveShapes` | Line/dot drawing from audio. One active at a time, or a morph between two. |
| Particle | `G-Force.WaveShape.package/Particles` (+ listed WaveShapes) | WaveShape-format configs that spawn randomly. Several can be active at once. |
| FlowField | `G-Force.FlowField.package` | Per-pixel "source coordinate" warp applied to the previous frame |
| FlowField Union | `G-Force.FlowField Union.package` | Per-point blend mask (ALFA) between the outgoing and incoming FlowField |
| ColorMap | `G-Force.ColorMap.package` (`.txt` or `.png`) | Intensity (0..1) to RGB palette |
| Sprite | `G-Force.Sprite.package` (images/movies + optional `.txt`) | Image particles |

**The frame loop ([BG]):**
1. Apply the current FlowField to the previous frame to produce a new frame.
2. Sample audio.
3. Draw the current WaveShape and any running particles (and sprites) onto the new frame.
4. Colorize the new frame with the current ColorMap.
5. Present the frame.

The working image is a **single-channel intensity buffer**. [BG] describes it as 8-bit
(0..255). During pixel flow, each destination pixel takes the intensity at its source location
and "decrease[s] i a little" ([CW] pseudocode). The decay is how trails fade to black. Drawing
writes intensities (Pen) into this buffer. Colorization is a palette lookup. The shipped
shaders do exactly that: `G-Force Colorizer.pixel.glsl` samples the intensity texture and
uses the value as the coordinate into a ColorMap texture. The HLSL version uses
`float2(intensity, 0)`. `ColorizePixels.pixel.glsl` does the same for directly colorized
geometry, with a scale and offset (`gParams[1]`).

---

## 2. Lexical and syntax rules

### 2.1 Basic form ([CP])
- A config is a plain-text, **unordered** list of key/value pairs.
- Each pair is a key of **1 to 4 characters** (the doc says "case-sensitive", but see 2.4),
  then `=`, then the value.
- A value is either an **unquoted number** or a **double-quoted expression**. In [CP]'s words,
  anything that "isn't a number (with the exception of a leading '-' or '.')" must be quoted.
  Example forms: `Pen=.8`, `Ans=-42`, `X0=".5 + sin( .3 * t )"`.
- "All whitespace or new lines not inside quotes are ignored". Pairs are delimited by whitespace,
  not by newlines. Before v2.7.0, configs used commas as delimiters [VH 2.7.0].
- Both C comment styles are supported: `// ...` to end of line, and `/* ... */` (multi-line).
  No whitespace is required between a value and a following comment [VH 2.7.1]. Example
  from [CP]: `ALFA="1 / exp( C1 ^ 2 )"/* comment */`.
- Since v2.6.4, non-integer numbers no longer have to be quoted [VH 2.6.4].
- Parameters that are omitted default to 0 or `"0"`. Specific keys override this (Pen, LWdt,
  NUM, Stps; see section 5) [RC].
- If a config fails to load, G-Force falls back to an internal "factory" config. Mismatched
  quotes or comment delimiters are the usual cause [TS]. Since 2.6.8 the evaluator is "more
  restrictive for invalid/illegal syntax" [VH 2.6.8].

### 2.2 Observed in the corpus ([corpus]); a parser must accept all of these
- **Spaces around `=`**: `NUM = "3"` (K0sMose files).
- **Multi-line quoted expressions.** A string literal can span several lines. JRM's vector
  literals rely on this:
  ```
  B1="(A2 + ...) * { cos(B0)
                   , sin(B0)
                   }"
  ```
- **Empty expressions**: `A5=""`. Treat as 0, or as "not defined".
- **Comments inside quotes** are part of the expression string, but quoted text in comments
  is ignored. Example: `B1=" A1"  // * ( 1.1 + cos( .08 * t ) )"` ([CBW]). The trailing `"`
  sits inside a `//` comment, so the comment stripper must recognize quotes and the string
  scanner must recognize comments, in the right order: outside a string, `//` and `/*` start
  comments; inside a string they do not.
- **Commas between pairs**: `AVRG=11, DCAY=.7` (JRM/Triskele). The legacy comma delimiter
  should be tolerated as whitespace.
- **Unquoted integers used as named constants**: `Side=5`, `Bnds=24`, `PACE=4`. These are not
  engine keys. Expressions reference them by name (see 3.6).
- **Line endings**: mostly LF; about 62 files use CRLF, and some mix CRLF with CR or LF.
  Treat CR, LF, and CRLF all as whitespace.
- **Encoding**: mostly ASCII. A few files contain UTF-8 or ISO-8859-1 bytes, but only in
  comments. Read the file as bytes or Latin-1.
- **Comment tags with meaning**: `#waveshape-particle-also` and `#waveshape-particle-only`
  inside comments (see 5.2).
- **Scientific notation** inside expressions: `end_time < 1e30`.
- **Hex numbers** (`0x5165EC00`) and `\t`-separated strings appear in Theme and Category
  files, not in scene configs (see section 8).

### 2.3 Numbers
Accept a leading `-` or `.`, decimals, and exponents (`1e30`) at minimum. Unquoted numeric
values in configs are always plain decimals in the corpus.

### 2.4 Case sensitivity: conflicting evidence
- [CP] says keys are case-sensitive.
- Identifiers **inside expressions** are evidently case-insensitive. The corpus freely mixes
  `A0`/`a0`, `PI`/`pi`/`Pi`, `ID`/`id`/`Id`, `NUM`/`Num`/`num`,
  `NUM_S_STEPS`/`Num_S_Steps`, `X_EXTENTS`/`X_Extents`, `BASS`/`bass`/`Bass`, and
  `END_TIME`/`end_time`. The documented examples do this themselves: [RC] defines `B0` and
  uses `b0`; [SW] uses `a0`.
- Some **keys** appear in non-canonical case: `Num=` (37 files), `stps=` (9), `Lwdt=` (9),
  and `pen=` (4). Several of those configs (for example JRM/Flakes and JRM/Dodecahedron) only
  work if `Num` is honored as `NUM`.
- **Recommendation:** match both keys and identifiers case-insensitively. In ColorMaps, `B`
  (blue) and `B0..Bn` (B vars) are distinct because of the numeric suffix.

---

## 3. Expression language

### 3.1 Data model: VectorC
Since v3.1, WaveShapes, FlowFields, and ColorMaps are compiled to SoundSpectrum's
**VectorC** VM [VH 3.1]. VectorC values are **matrices** (cols × rows) of floats. A scalar is
a 1×1 matrix. Arithmetic is elementwise with broadcasting, and many built-ins accept or return
vectors [VC]. Most configs only use scalar math. About 50 files rely on vector semantics
([corpus]: mostly JRM, Gold, and K0sMose WaveShapes, plus 4 FlowFields), for example
`vectorLR`, `{...}` matrix literals, `row`/`col`, `select`, `choice`, `map1D`/`map2D`,
`noise`, and `smooth`. The 3.6 release added "lazy" expression evaluation [VH 3.6].

**Matrix literals** ([corpus], undocumented): `{ a, b, c }` is a row vector.
`{ {..}, {..} }` and `{ expr, expr }` stack rows. Literal elements can be expressions,
and whole literals take part in arithmetic: `{ ... } * .4`.

### 3.2 Operators ([CP])
| Operator | Meaning |
|---|---|
| `+ -` | add / subtract (also unary minus) |
| `* /` | multiply / divide |
| `%` | modulo ("integer remainder", e.g. `11 % 3 == 2`) |
| `^` | power |
| `== != < > <= >=` | comparisons; result 1 or 0 |
| `&& \|\|` | logical AND / OR |
| `& \| ^^` | bitwise AND / OR / **XOR** (`^` is power, not XOR) |
| `<< >>` | shifts |
| `(int)`, `(float)` | unary casts |
| `!` | logical NOT. Not in [CP], but used in the corpus (`!A0`, `!(end_time < 1e30)`). |

**Precedence is not documented.** Recommended (C-like, with `^` above unary minus):
1. Primary: literal, identifier, call, `( )`, `{ }`
2. `^` (right-associative; see the note below)
3. Unary `- + ! (int) (float)`
4. `* / %`
5. `+ -`
6. `<< >>`
7. `< <= > >=`
8. `== !=`
9. `&`
10. `^^`
11. `|`
12. `&&`
13. `||`

The corpus contains `-1^(round(...))` (JRM/Flakes) and `(-1) ^ ID` (Pollock Dots). Authors
who wanted sign alternation added explicit parentheses, which hints that unary minus binds
looser than `^` (as in math). See Open Questions. `%` on non-integers and `% 0` behavior are
also undocumented. One author's comment asks "(x % 0) illegal??" (JRM/Adapter).

**Measured (corpus sweep 5B.4, [docs/sweep.md](sweep.md)):** `%` is an **integer remainder**: both
operands are truncated toward zero first, so `5.5 % 2 == 1` and `-5.5 % 2 == -1`. "Bits Between Bits"
(`(A1 * abs(y)) % 2 - .5` flips the flow direction on every other pixel row), "Split and Twist" and
"Glass Blocks" only match G-Force with it; C `fmod` leaves them at mad 11-16. `x % 0` is still a guess (0).

### 3.3 Built-in functions
From [CP], plus [VC] where noted. All trig functions use radians.

**Common**
| Function | Semantics |
|---|---|
| `abs(x)` | \|x\| |
| `dim(a,b)` | `max(a-b, 0)` |
| `sqr(x)` | x² |
| `sqrt(x)` | √x |
| `exp(x)` | eˣ |
| `log(x)` | natural log |
| `log10(x)` | base-10 log |
| `pow(x,y)` | xʸ [VC] |
| `sin cos tan(x)` | radians |
| `asin(x)` | range −π/2..π/2 |
| `acos(x)` | range 0..π |
| `atan(x)` | range −π/2..π/2 |
| `atan2(y,x)` | range −π..π |
| `hypot(x,y)` | √(x²+y²) |
| `hypot2(x,y)` | x²+y² |
| `leg(h,x)` | `sqrt(max(h²−x², 0))` |
| `wyvill(r)` | −4/9 r³ + 17/9 r² − 22/9 r + 1. [VC] names the parameter `r2` (squared radius?). |

**Logical / discrete**
| Function | Semantics |
|---|---|
| `pos(x)` | x if x > 0, else 0 |
| `sign(x)` | 1 if x ≥ 0, else −1 (so `sign(0) == 1`) |
| `clamp(x)` | clamp to [0, 1] |
| `sqwv(x)` | 1 if \|x\| ≤ 1, else 0 |
| `select(c,a,b)` | elementwise `c ? a : b` [VC] |
| `choice(x)` | random column of x, e.g. `choice({4,5,6,7})` [VC] |
| `largest(x)` | column whose top element is largest [VC] |

**Miscellaneous**
| Function | Semantics |
|---|---|
| `rnd(x)` | random real in [0, x] inclusive. Same shape as x; each element is scaled by U[0,1] [CP][VC]. |
| `trunc(x)` | drop the fractional part (toward 0) |
| `floor(x)` | floor; `floor(-2.7) == -3` |
| `round(x)` | nearest integer |
| `wrap(x)` | `x - floor(x)`; `wrap(-2.7) == .3` |
| `flip(x)` | sawtooth through …(−3,−1),(−1,1),(−1,−1),(1,1),(1,−1)… (period 2, range ±1; see Open Questions) |
| `trwv(x)` | triangle wave through (0,0),(1,1),(2,0), periodic |
| `min(a,b)`, `max(a,b)` | |
| `srand(x)` | seed the RNG from the bits of x; returns x. [VC]: affects `rnd()` and `noise()` and is per-thread. |
| `clock()` | system time in ms |
| `time()` | seconds since midnight |
| `print(x)` | print to console [VC] |

**Vector / numeric utilities [VC]** (needed for the vector-heavy configs): `vector(N,v)`,
`vectorLR(N,l,r)` (inclusive linspace, N×1), `rows(x)`, `cols(x)`, `transpose(x)`,
`subrange(x,c1,r1,c2,r2)` (zero-based; negative indices count from the right/bottom,
one-based; clamped), `row(x,n)`, `col(x,n)`, `noise(cols,rows,octaves)`,
`resample(x,cols,rows,interp)`, `map1D(f,x,interp)` (x wrapped into [0,1] across a row of f),
`map2D(f,x,y,interp)` (x,y in [0,1] clamped; (0,0) is the top-left of f),
`smooth(x,sigma)` (Gaussian; negative sigma wraps), `HSLToRGB`, `RGBToHSL`, `dot`,
`dot_recip`, `dist`, `dist_recip`, `normalize`, `transform`, `cross`, `rotate`,
`cylindrical`, `spherical`. Of these, the corpus uses `vectorLR`, `vector`, `row`, `col`,
`rows`, `cols`, `transpose`, `subrange`, `select`, `choice`, `largest`, `map1D`, `map2D`,
`noise`, `smooth`, `resample`, and `dot`.

**Deprecated aliases ([CP])** that must still be supported:

| Alias | Equivalent | Uses in corpus |
|---|---|---|
| `sgn(x)` | `sign(x)` | 208 |
| `clip(x)` | `clamp(x)` | 113 |
| `flor(x)` | `floor(x)` | |
| `trnc(x)` | `trunc(x)` | |
| `seed(x)` | `srand(x)` | |
| `atan2(x)` (one argument) | `atan` | |

**Undocumented names found in the corpus:** `ceil(x)` (Gold/Spin Me) and `rnd16` (Roger
Bigod/Sphere Turn; probably a typo for `rnd(16)`, which would make it an undefined
identifier). See Open Questions.

### 3.4 Constants and globals ([CP])
| Name | Meaning |
|---|---|
| `PI` | 3.141592653 |
| `NUM_S_STEPS` | Number of X/Y/C evaluations per frame (the resolved `Stps`) |
| `NUM_FFT_BINS` | Number of bins in `fft()` (pref `Audio.FFT.NumBins`; was 155, 185 since [VH 3.1]) |
| `NUM_SAMPLE_BINS` | Number of values in `mag()` (pref `Audio.PCM.NumBins`) |
| `WIDTH`, `HEIGHT` | Output frame size in pixels. Can change at any time, so treat as time-dependent. |
| `X_EXTENTS`, `Y_EXTENTS` | The corners of the frame are (±X_EXTENTS, ±Y_EXTENTS). Both are 1 when `Aspc=0`. Can change at any time. |

### 3.5 Audio inputs ([CP], [CU], [RC])
| Name | Semantics |
|---|---|
| `mag(x)` | PCM waveform. x in 0..1 indexes the `NUM_SAMPLE_BINS` samples (`mag(0)` is the first, `mag(1)` the last). Lookups are discrete (nearest bin, no interpolation): "mag(.4) and mag(.40001) refer to the same data value" [RC]. The domain wraps: `mag(-.2) == mag(.8)` [VH 2.5.0]. Smoothing is set by `Audio.PCM.Smooth`. |
| `fft(x)` | Spectrum. x in 0..1 over `NUM_FFT_BINS` bins (each bin averages a band). The domain wraps [VH 2.5.0]; an edge fix stops `fft(≈1+)` from wrapping to `fft(0)` [VH 2.7.1]. Auto-normalized since 4.3 [VH 4.3]. Smoothed per `Audio.FFT.Smooth`; range set by `Audio.FFT.FreqLo/FreqHi` [VH 5.9.7]. Scaled by `Audio.Response.Scale` [CU]. |
| `BASS` | "A single scalar value that approximates the overall beat" [CP]. Range undocumented. Corpus usage such as `min(BASS*3+.5, 8)` and `log(4*BASS+1)` suggests roughly 0..1+. |
| `level`, `energy` | **Undocumented** but used in shipped configs (Genesis Device and MCP: `B1="4 * level"`; K0sMose: `LWdt="1.5 + energy"`). Presumably smoothed overall loudness. |

`mag()` and `fft()` accept vector arguments and return vectors. For example,
`fft(vectorLR(NUM_S_STEPS,0,1))` (JRM/Flakes). Output ranges are undocumented. `mag()` is
assumed to be signed (around 0) and `fft()` non-negative.

**Measured (Phase 5C, G-Force 5.9.7; details in `docs/audio-probe-results.md`):** `mag(x)` is the raw
sample (a full-scale sine reads ±1); `fft(x)` bins are log-spaced from 120 Hz to 5.5 kHz, broad, adaptively
normalized and clamped at ~0.93; `LEVEL` ≈ 1.3 × the mean of `fft()` (0..~0.65 in practice); `BASS` = 4 × `LEVEL`
(not a low-band measure); `ENERGY` is a slow (~1.5–2.5 s) follower of recent `LEVEL` peaks, 0..1.

**reforce extensions** (not in G-Force; G-Force scenes never use them, so they do not affect parity). Available
wherever `BASS` is (WaveShape, Particle, Sprite expressions), case-insensitive:

| Name | Semantics |
|---|---|
| `MID` | Mid band 250 Hz–2 kHz: band amplitude from the analyzer's raw spectrum divided by its own peak envelope (fast attack, 4 s release), smoothed (10 ms attack, 120 ms release). ~1 when the band is at its recent loudest, ~0 when quiet; 0..2. |
| `TREBLE` | Same for 2–16 kHz. |
| `BEAT` | Onset pulse: 1 at each detected beat, then `exp(-age / 0.12 s)`; 0 when no beat for ~0.6 s. Detection: spectral flux of the log magnitude spectrum (1024-point FFT every 10 ms) in 30–200 Hz (weight 1) and 200 Hz–4 kHz (weight 0.5), a local maximum above mean + 1.5 σ of the last 1.5 s, at least 120 ms after the previous beat. |
| `TEMPO` | Tempo in BPM, folded into 80–160 (a half-time 62 reads 124, drum and bass 174 reads 87; near 80 / 160 it keeps its octave), from the autocorrelation of the onset strength over the last 8 s with a log-normal preference around 120 BPM (one octave wide), smoothed over ~2 s; needs ≥ 4 beats in the window. With `reforce.BeatClock.SteadyTempo` (default) the tempo is kept through breakdowns (up to 60 s; 0 after 3 s of silence) and a different tempo takes over only after winning for ~8 s; off: 0 when unsure (silence, steady sound, no rhythm for 4 s). |

Parameters: `AnalyzerConfig` / `BeatConfig` in `src/audio/Analyzer.hpp` and `src/audio/BeatTracker.hpp`.

**Ableton Link** (reforce extension, Phase 5H). reforce joins any Link session on the local network (rekordbox in
Performance mode with LINK on, Ableton Live, Traktor, …) and reads its tempo and beat grid. Same availability as
above:

| Name | Semantics |
|---|---|
| `LINK_ON` | 1 while a session with at least one other app is running, else 0. All other `LINK_*` are 0 when `LINK_ON` is 0. |
| `LINK_BPM` | Session tempo in BPM (e.g. rekordbox's master deck). |
| `LINK_BEAT` | Running beat count on the session timeline (e.g. 1234.37): the integer part ticks on every beat, `frac(LINK_BEAT)` is the position within the beat. |
| `LINK_PHASE` | Position in the bar, 0 ≤ `LINK_PHASE` < quantum (`reforce.Link.Quantum`, 4): 0 on the bar line ("the one"). |

While `LINK_ON` (and `reforce.Link.DriveBeat`, default on), `BEAT` and `TEMPO` follow Link instead of the audio
estimate: `TEMPO` = `LINK_BPM` and `BEAT` = `exp(-age / 0.12 s)` after each Link beat, so scenes that use them lock to
the DJ's beat grid. Values are sampled `reforce.Link.OffsetMs` ahead of the frame (display latency).

Examples: a flash on every beat `exp(-4 * frac(LINK_BEAT))`, a rotation of one turn per bar
`2 * PI * LINK_PHASE / 4`, an accent on the one `LINK_ON * (LINK_PHASE < .25)`.

**Beat clock** (reforce extension, PLAN 5L.4): the best beat source there is, the same clock that drives
beat-synced movies (`sync/BeatClock.hpp`, `reforce.BeatClock.Source`): a Link session when one runs, else the audio
phase lock (the audio `TEMPO` plus a phase fitted to the onsets). Same availability as above, plus Solids. All 0
while the clock has no source.

| Name | Semantics |
|---|---|
| `SYNC_ON` | 1 while the clock is confident (Link: always; audio: once the onsets agree on a phase), else 0. |
| `SYNC_BPM` | The clock's tempo in BPM. |
| `SYNC_BEAT` | Running beat count; whole numbers fall on the beats (`wrap(SYNC_BEAT)` = position within the beat). It only moves forward while one source runs; switching sources starts a different count. |
| `SYNC_LOCK` | Audio: how consistently the onsets fall on one phase, 0..1 (`SYNC_ON` switches on at .5, off below .3). Link: 1. |
| `SYNC_BAR` | Running bar count, aligned to the phrase clock (PLAN 5O.1): whole numbers on the bar lines. With Link the bar lines are Link's; with the audio clock, bar 1 is wherever the phrase clock puts the phrase start. |
| `SYNC_PHRASE` | Running phrase count (`reforce.Phrase.Bars` bars each, 16): `wrap(SYNC_PHRASE)` = position in the phrase, whole numbers on the phrase lines (auto-aligned to loudness changes, or set with "Phrase starts now"). |
| `SYNC_BUILD` | 0..1 through a breakdown (much less bass than usual for 4+ beats), rising over 32 beats: the tension of a build-up. 0 otherwise (PLAN 5O.5). |
| `SYNC_DROP` | 1 at a drop (the bass coming back after a breakdown), falling to 1/e over a bar. |

Unlike `LINK_*`, these work without Link (audio only). Without a lock, fall back on something smooth, e.g.
`SYNC_ON * SYNC_BEAT + (1 - SYNC_ON) * 1.75 * t`.

### 3.6 User variables and key references
- Variable families: `A0..An`, `B0..Bn`, `C0..Cn`, `D0..Dn`. The corpus maxima are A30, B31,
  C24, and D16. Treat the families as unbounded or allow at least 32 of each.
- **Any key's value is readable by name in expressions** ([corpus]). For example,
  `Stps="Bnds*16*2"` after `Bnds=23`; `A1="Stps/Bnds"`; `Num="1 + Side"`;
  `B0="((t/PACE) % B2)"`. Engine keys (`NUM`, `Stps`, `PDur`) are readable too. Authors
  exploit the 4-character key slot as named constants ("limited to 4char Constant name",
  JRM/Dodecahedron).
- Undefined identifiers: behavior unknown (probably 0, or the config is rejected).
- `Meta="reactive=4 detail=4 ..."` appears in 3 files. It is a string payload, not an
  expression. Its words (`reactive`, `detail`, ...) show up in the identifier scan only
  because they sit inside quotes. `Meta` has been unused since 5.3 [VH 5.3].

---

## 4. Evaluation model (tiers)

Each family is evaluated at a fixed **tier**. An expression can reference anything from its
own tier or an outer (less frequent) tier.

| Tier | Family | When | Can see |
|---|---|---|---|
| Load | `A#` | Once, when the config is loaded or instantiated ([RC]: "only when a config is loaded/executed"; [CW]: "when the ColorMap is first loaded and never again") | `t`, other A vars, `rnd`, `srand`, constants, `NUM`/`NUM_S_STEPS` (WaveShape), `ID`?, and **`s` as a vector** in VectorC configs ([corpus] JRM/Divisions `A1="(s - ...)"`) |
| Frame | `B#` | Start of every frame (WaveShape, Sprite); before each ColorMap recompute (ColorMap) | + `t`, `mag()`, `fft()`, `BASS`, `ID`, `END_TIME`/`START_TIME`/`dt` (particles) |
| Step | `C#` | WaveShape: for each value of `s`. ColorMap: for each value of `i`. FlowField: once per **scan row** ([FL]). | + `s` or `i` (or the row's `y`) |
| Pixel | `D#` | FlowField and Union: for each pixel ([SW]) | + `x`, `y`, `r`, `theta` |
| Output | type keys (`X#`/`Y#`, `Pen`, `LWdt`, `srcX`, `ALFA`, `H`/`S`/`V`, ...) | At the innermost tier relevant to the type | everything above |

Details:

- **Ordering.** The file is unordered [CP]. Within a family, evaluate in **ascending index
  order**: A0, A1, ... The corpus relies on this. `A2="(...)/ a0"` [SP] needs A0 first.
  Placement in the file does not matter: Lizy's Starry Night defines `A2` after `Pen`.
- **B (and C) vars are persistent state.** A B var keeps its last value between frames, and an
  expression that references itself or a higher-numbered B var reads the **previous frame's**
  value. 75 forward or self references exist in the corpus. Shipped configs build integrators
  and edge detectors this way:
  - `B3="min( B3 + B0 * B2, A13 )"` (angular velocity, Genesis Device)
  - `B0="B0 + B1 * (1 - sqwv(...BASS...))"` (beat counter, Trance Snap)
  - `B5="wrap( B5 + ... )"`

  The initial value before the first evaluation is presumably 0.
- **B vars are shared across `ID` instances.** With `NUM > 1`, the B, C, X, Y, Pen, and LWdt
  expressions are evaluated once per instance per frame, with `ID` = 0..NUM−1 [SP]. The B
  storage is shared, so accumulators guard with `ID == 0`. Genesis Device comments
  `B0="ID == 0"` with "1 only when ID == 0 (so we don't multiply accumulate B3)", and
  JRM uses `select(ID, B2, <update>)`. A vars are evaluated once and are the same for every
  instance [SP]. The only per-instance distinction is `ID` itself, so per-instance randomness
  is created with `srand(ID + A0)` in a B var (Lizy's Starry Night).
- **C-var self-reference within a frame.** Fern (an IFS particle) uses `C5 = f(C7, C8)`,
  then `C7="C5"` and `C8="C6"`. That only produces a fern if C vars carry over from the
  **previous s step**, which implies sequential per-step evaluation, or VectorC equivalence
  across frames. See Open Questions.
- **Vectorized evaluation (VectorC, ≥3.1).** In practice, VectorC evaluates a whole tier as
  vectors:
  - **WaveShape:** `s` behaves like a vector of `NUM_S_STEPS` values (presumably
    `vectorLR(NUM_S_STEPS, 0, 1)`), and C/X/Y can be vectors over s. B vars that mention `s`
    become vectors. `X0="row(B1,0)"` takes an X path from a 2×N B-var matrix (JRM/Divisions).
  - **FlowField:** "FlowFields are evaluated by VectorC row by row: x is a vector that goes
    from −X_EXTENTS to X_EXTENTS and y is a scalar value that changes from Y_EXTENTS to
    −Y_EXTENTS with each successive evaluated row" [NM]. So C vars are per row (scalar y),
    and D vars and `srcX`/`srcY` are row vectors. `rnd(k)` in a D or src expression produces
    **per-pixel** noise ("rnd() acting in a piecewise fashion with x, y, and D values").
    In a C var it returns **one scalar per row** [FL].
  - A conforming scalar implementation must give these semantics. For the vector-heavy
    configs (about 50 files), a small matrix VM is needed.
- **Time `t`.** "system time index (in seconds)" [CW][RC]. Since v4.0, `t` "always start[s]
  from 0" [VH 4.0]. That is, from engine start, not from config load. Configs capture their
  own start time with `A0="t"` (Pollock Dots, Genesis Device).
  **Measured for ColorMaps (corpus sweep 5B.4):** a ColorMap's `t` is **seconds since that ColorMap
  started**. All 25 t-dependent corpus maps, shot 0.7 s after a cut to them at script times 5-197 s,
  match reforce's palette at t = 0.7 (mean |diff| 0.2-1.1 levels) and none at the global clock. Whether
  WaveShapes (and their `A0="t"` idiom) see a global or a per-config clock is not measured yet.
- **Lazy evaluation [VH 3.6].** No details are documented. Inference: unused expressions
  may be skipped, so side effects from `srand`/`rnd` in unreferenced vars cannot be relied on.

---

## 5. Scene types

### Common keys
| Key | Meaning |
|---|---|
| `Vers` | Unquoted integer: the "G-Force version this was written for, times one hundred" [RC][LS][CW]. Corpus values: 100, 200, 209, 250, 260, 265, 268, 270, 271, 280, 300, 310, 311, 350, 355, 391, 400, 500, 530, and 540 (100 and 270 are the most common). No documented behavioral effect (section 9). |
| `Aspc` | 0 = stretch coordinates so the frame corners are exactly (±1, ±1). 1 = force a 1:1 aspect, so one axis extends past ±1 at the corners [RC][LS]. Default 0. |
| `A#` | Load-time vars (all types) |

**Coordinate system (all types):** (0,0) is the frame center. The top-right is (1,1) and the
bottom-right is (1,−1), so **+y is up** [RC]. With `Aspc=1`, the shorter axis spans ±1 and the
longer axis spans ±(long/short) (FlowFields: ±(long−1)/(short−1), see the FlowField pixel grid). This is inferred from "making either the x or the y greater
than 1 at the corners" [RC], with X_EXTENTS/Y_EXTENTS exposing the result [CP].

### 5.1 WaveShape

A WaveShape draws one or more polylines (paths), or dots, whose points come from `s` stepping
over 0..1.

| Key | Tier | Default | Meaning |
|---|---|---|---|
| `NUM` (`Num`) | once, before A vars? | 1 | Number of instances. The non-integer part is truncated [SP]. `ID` = 0..NUM−1. Valid for WaveShapes since 2.5.0 [VH 2.5.0]. |
| `Stps` | once at load | `NUM_SAMPLE_BINS` | Number of s steps. Evaluated once and stored in `NUM_S_STEPS`. **Cannot access A vars** [RC]. 0 or omitted gives the default. Old name: `NumS` [VH 2.6.0]. |
| `A#` | load | | |
| `B#` | per frame (× per ID) | | |
| `C#` | per s step | | |
| `X#`, `Y#` | per s step | | Path #k point = (X#k, Y#k). Paths are paired by index. "v1.0 allows up to X14" [RC]; the corpus uses up to X19/Y19. |
| `Pen` | per step (per path; `PATH_ID` usable) | 1 | Draw intensity 0..1, clipped at 1 [RC]. Negative values are presumably clamped to 0. |
| `LWdt` | per step | 1 | Line or dot thickness in pixels [RC]. The final width is `LWdt * WaveShape.LineWidth.Scale + WaveShape.LineWidth.Offset` [CU]. With `WaveShape.AutoLineScale`, Scale is proportional to the frame size [CU][VH 3.1]. *Measured (5.9.7, probe #38):* width = LWdt·Scale·s(W, H) + Offset px with s = 0.733 + 8.67e-4·min(W, H) + 5.10e-4·max(W, H) in G-Force frame pixels (frames above ~5.2 MP are rendered at half size); `AutoLineScale = 0` has no effect. |
| `ConB` | constant | 0 per the omitted-key rule, though nearly every file sets 1 | 1 = connect successive points with lines; 0 = dots only [RC]. |
| `PDur` | once, before the particle starts | pref `PREF_PARTICLE_DURATION` | Particle lifetime in seconds. Can see `NUM` and `NUM_S_STEPS` [VH 2.6.0][SP]. Only meaningful when running as a particle. |
| `Meta` | | | Obsolete since 5.3 [VH 5.3]. |

**Variables visible:** `s` (0..1), `t`, `ID`, `NUM`, `NUM_S_STEPS`, `mag()`, `fft()`, `BASS`,
`level`/`energy` (undocumented), `PATH_ID` (undocumented: the index k of the X#/Y# path
being evaluated, used in Pen and LWdt in "Hollow Circle"), `WIDTH`, `HEIGHT`, `X_EXTENTS`,
`Y_EXTENTS`, `PI`, `END_TIME`/`START_TIME`/`dt` (particles; see 5.2).

**Stepping.** s goes over 0..1 in `NUM_S_STEPS` steps [RC]. Whether the endpoints are
inclusive (i/(N−1)) or not (i/N) is not stated. `vectorLR` is inclusive, and JRM's
`Stps="... +1"` idiom (with the comment "Why +1, rounding or error ???") suggests i/(N−1).
For each frame and each ID: evaluate B, then for each s evaluate C, then each X#/Y# pair,
Pen, and LWdt. Each path k draws `NUM_S_STEPS` points, connected if `ConB=1`.

**Typical idiom:** `Stps="NUM_FFT_BINS"` when using `fft(s)`, and the default
(`NUM_SAMPLE_BINS`) when using `mag(s)` [RC][CU].

**Morphing between WaveShapes** ([VH 3.1], [VH 2.6.5], [VH 2.6.0], [VH 2.4.3]):
- Transitions are morphs: corresponding segments (paths and points) interpolate from the old
  shape to the new. "During a transition, pen intensity goes from/to 0 for non-morphing
  segments" [VH 2.6.5]. So when the path counts differ, unmatched paths fade.
- Shapes passing through (0,0) are slightly offset during a "dilation transition" [VH 2.6.0].
- A WaveShape (and ColorMap) morph also fires on a track change [VH 2.4.3]. Ordering a
  change mid-morph is handled gracefully [VH 3.1].
- Some configs warn that they "make a nasty morph" (Trance Snap). Morph quality is
  visual-only and not configurable from the config.
- The exact point-correspondence rule when `NUM_S_STEPS` or `NUM` differ is undocumented.

### 5.2 Particle (WaveShape particle)

Same format as a WaveShape. Differences ([SP], [BG]):
1. Particles start at random times ("when G-Force rolls some dice"), and several can be alive
   at once alongside the current WaveShape.
2. `END_TIME`: the time (in `t` units) when the particle expires. `START_TIME`: when it started.
   **Measured (probe #43), not what the docs say:** END_TIME = START_TIME + 5 + (D + duration)/2, with D the
   Particle.Transition.Duration and duration = PDur (or the StartParticle argument, which overrides PDur). It is not
   the end of the particle's life (that is START_TIME + 2D + duration).
3. `dt` = `(t − START_TIME)/(END_TIME − START_TIME)`, going 0 to 1 over the particle's life. **Measured (probe
   #43):** dt goes 0 to 1 over the whole visible life (fade-in, duration, fade-out) = age / (2D + duration), so the
   formula above does not hold. In a plain WaveShape dt is 0.
4. `NUM` defaults to 1 for configs in the Particles folder [SP].
5. Lifetime comes from `PDur` if present, otherwise from the particle-duration pref.
   `StartParticle()` from a script can override it [VH 3.0.1].
- Particles "typically fad[e] to and from the background color" [SP]. Whether the engine
  applies a fade envelope to Pen automatically is undocumented (see Open Questions).
- **Particle-vs-WaveShape detection idiom** ([corpus], JRM): when a config runs as a
  WaveShape, `END_TIME` is huge, so `A0="(end_time < 1e30)"` is 1 only for particles. An
  implementation should set `END_TIME` ≥ 1e30 (or +inf) and `dt` to 0 for regular WaveShapes.
- **Which configs are particles** ([PKG], [VH 5.0], [VH 5.3]): everything in
  `WaveShape.package/Particles` is a Particle, and everything in `WaveShapes` is a
  WaveShape. The `alsoParticles` dictionary in `G-Force.WaveShape.package/_package.py` lists
  29 WaveShapes that are additionally registered as Particles. The comment tags
  `#waveshape-particle-also` and `#waveshape-particle-only` express the same thing (they are
  described as signals to "G-Force 6"). In the corpus, 117 of the 119 Particles carry `-only`
  and 27 WaveShapes carry `-also`. Treat the tags as authoritative.
- **Names in parentheses** (e.g. `(G-Force Logo).txt`) are excluded from the automatic
  shuffle/slideshow and are only started by script [VH 3.1][FAQ].

### 5.3 FlowField

A FlowField maps each destination pixel coordinate to a **source** coordinate in the
previous frame ([BG] chalkboard analogy, [LS]).

| Key | Tier | Meaning |
|---|---|---|
| `Aspc` | constant | see Common keys |
| `A#` | load | randomization per instance ("Sunburst - Few") |
| `C#` | per scan row | rarely used ([FL]: "C Vars are identical to D vars except calls to rnd() return a scalar") |
| `D#` | per pixel | temporaries [SW] |
| `srcX`, `srcY` | per pixel | Cartesian source coordinate, **or** |
| `srcR`, `srcT` | per pixel | polar source (radius, angle) |

**Pixel grid (measured, corpus sweep 5B.4):** pixel column c of a W-pixel-wide frame is evaluated at
`x = (-1 + 2c/(W-1)) * X_EXTENTS`, row r (from the top) at `y = (1 - 2r/(H-1)) * Y_EXTENTS`: the edge
pixels sit **exactly on the frame edges** (like the inclusive `s` of WaveShapes), not at pixel centers or
corners. Sources map back the same way (`px = (sx/X_EXTENTS + 1) * (W-1)/2`). Evidence: "Bits Between Bits"
(per-row parity of `HEIGHT/2/Y_EXTENTS * abs(y)`), "Digital Staircase" (`wrap(50 y)` is never exactly an
integer on a row), "TriGrid" / "Unholy Ritual" (1/sin singularities between rows) and "Fran 10a"
(`clip()`ed sources on the edges) pass only with this grid; overall 264 of 352 fields got closer (sweep).
**With `Aspc=1` the steps are square**, 2/(short−1) on both axes, so `X_EXTENTS` = (W−1)/(H−1) for a landscape
frame (1979/1259 at 1980×1260, not 1980/1260; portrait: `Y_EXTENTS` = (H−1)/(W−1)). "Nebula" (`1/wrap(232 r)`,
a texture that decorrelates under a 0.1-px offset or a 1e-4 change of scale) pins this; W/H failed it (mad 3.89 →
0.58) and 110 other fields got closer (probe-results #36). Whether `X_EXTENTS` as read by expressions is this value
or W/H is not distinguishable from the captures; reforce exposes the grid's value. Expressions are evaluated in
double precision; single precision (`reforce.FlowPrecision = float32`) was tried and is not measurably closer.
WaveShape lines, in contrast, pass through the **center of pixel `(x+1) * W/2`** (x = 0 is the center of
column W/2): G-Force's probe-grid lines at x = 0 / y = 0 cover columns 987-993 / rows 627-633 at 1980x1260.

**Variables:** `x`, `y` (destination, center origin, +y up), `r` (distance from the origin),
`theta` (angle from the +x axis, range −π..π, per "Scattered Flow Out"), `X_EXTENTS`,
`Y_EXTENTS`, `WIDTH`, `HEIGHT`, `PI`.

**No time and no audio:** no shipped FlowField references `t`, `mag`, `fft`, `BASS`, or B
vars [corpus]. FlowFields are computed **once** per instance into a vector field. They are
precomputed "in another thread" for the oncoming field [VH 4.0], and incrementally over
frames (pref `PREF_FLOWFIELD_LINES_PER_FRAME` [VH 3.0.1], `ConfigPrepTime` [CU]). The field is
then applied every frame. So A vars, and every `rnd()`, are frozen per field instance.

**Pixel-flow step** ([CW] pseudocode): for each output pixel (x,y), take the source location
(sx,sy) from the field, read the intensity there from the previous frame, "decrease i a
little", and write it. Since 2.8 the algorithm reduces "color granularity" [VH 2.8], which
suggests sub-pixel or bilinear sampling. Since 4.0 the field has a higher vector resolution
[VH 4.0]. The source-out-of-frame policy, the decay amount, and the interpolation are
undocumented.

**Transitions:** FlowFields morph [VH 2.0.0], presumably by interpolating the two vector
fields with the transition weight. Related prefs: `PREF_FLOWFIELD_SMOOTH_SWITCH` [VH 3.0.1]
and `PREF_FLOWFIELD_TRANS_DISABLE` [VH 3.0].

### 5.4 FlowField Union

Introduced in 2.5.0 [VH 2.5.0]. Documented in [BC]:

- When the Union interval elapses (pref `PREF_UNION_INTERVAL`, which replaced
  `PREF_UNION_PROBABILITY` [VH 2.7.1]), G-Force takes the next Union from the Union playlist
  and uses it to combine the outgoing and incoming FlowFields.
- `ALFA` is evaluated per point. At ≤ 0, use the outgoing field's vector; at ≥ 1, the
  incoming field's vector; in between, linearly interpolate the two vectors.
- Keys: `A#`, `D#`, `ALFA`, and `Aspc` (1 file). Variables are the FlowField set (`x`, `y`,
  `r`, `theta`, ...).
- Example: `ALFA="A1 * ( A0 - r )"`, a disk of the new field with a soft edge.
- It is unclear whether the union result persists as the "current field" or whether it is a
  transition shape (a spatially varying morph). [BC] reads as the latter: it defines how
  each point blends during the transition.

### 5.5 ColorMap

Maps intensity `i` ∈ [0,1] (0 = black/background, 1 = brightest/foreground) to RGB ([CW], [BG]).

| Key | Tier | Meaning |
|---|---|---|
| `A#` | load | |
| `B#` | per recompute | "only t, never i" [CW] |
| `C#` | per i value | [CW][CBW] |
| `H`, `S`, `V` | per i | HSV, each 0..1. **H wraps** ("3011.34 == 42.34 == .34") [CW]. The hue wheel is Red 0, Yellow 1/6, Green 1/3, Cyan 1/2, Blue 2/3, Magenta 5/6. |
| `R`, `G`, `B` | per i | RGB alternative, 0..1 [CBW] (since 2.6.0) |
| `CCal` | expression | Recompute interval in seconds. The factory value is a little under 0.1. Rarely overridden (e.g. `.0333` in Tri-Chroma) [CW]. |

- **Variables:** only `i` and `t` (plus A/B/C vars and constants) [CW].
- A ColorMap is effectively a palette of 256 entries [BG]. Evaluate C/H/S/V (or R/G/B) for
  each of the 256 values of i and upload the result as a 256×1 texture.
- **Out-of-range S/V/R/G/B:** presumably clamped (undocumented). H is documented to wrap.
  HSV maps with wrapped hues were fixed in 2.6.8 [VH 2.6.8].
- **PNG ColorMaps:** an image strip `N×1` (the corpus has widths 120..256 at height 1). The
  leftmost pixel is i=0 and the rightmost is i=1. **Measured (corpus sweep 5B.4):** the strip is read
  like a linearly filtered, edge-clamped texture: entry k samples `x = k/255 * N - 0.5` (texel centers at
  `(j + .5)/N`), not `k/255 * (N-1)`. All 212 PNG maps pass with it; endpoint-to-endpoint resampling
  failed 8 strips with sharp steps (Grape, Seguro, Pacific Smiles, ...) by up to 14 levels. Verified: leftmost pixels are black in
  Charcoal, China, and 105 Degrees. [CW] says "1xN, N is 255 or less", but shipped strips
  are horizontal and up to 256 wide. These replaced `.MAP` files in 3.9 [VH 3.9]. The
  `-icon.png` files are UI thumbnails, not ColorMaps.
- **Domain remap pref** `ColorMap.MapFunction` (formerly `PREF_COLORMAP_I_MAP` [VH 2.8]): an
  expression in `i` (and `t`) applied to each pixel's intensity before the lookup. Examples:
  `"i^2.5"`, `"wrap( i + .1 * t )"`, `"(1-i)^.7"` [CU].
- **Transitions:** ColorMaps morph smoothly [VH 2.6.5][VH 2.8]. Presumably this is a lerp of
  palettes by the transition weight. A new change during a transition starts a new transition
  instead of jumping [VH 2.6.8]. During a fast morph, recomputation runs faster than CCal [CW].

### 5.6 Sprite

Image or movie particles. Documented in [DS] and [FAQ].

- **Files:** an image or movie (`.jpg`, `.png`, ...) in the Sprite package. A sibling `.txt`
  with the same basename (`Space/Astronaut.txt` next to `Astronaut.jpg`) overrides the
  defaults in `_default_sprite_params.txt` [FAQ]. A trailing `~` in the filename inverts the
  image [FAQ][DS]. Names in parentheses are script-only [FAQ]. Text sprites were removed in
  5.3 [VH 5.3].

| Key | Tier | Meaning |
|---|---|---|
| `A#` | load | |
| `B#` | per frame | |
| `X`, `Y` | per frame | Center of the image rectangle, in frame coordinates (±1 corners). For example, (1,−1) is the bottom-right corner. **Measured (probe #41):** the center lands at X · max(0, 1 − sprite width / frame width) (Y likewise), so ±1 puts the sprite's edge on the frame's edge and a sprite that fills an axis stays centered on it. |
| `Sped` | once, after the A vars | Speed of the built-in bouncing `XPOS`/`YPOS`. Only used by those. |
| `Pen` | expression | Drawn intensity. Omit it for a "background-only" sprite. **Measured (probe #40):** the non-black pixels are copied at Pen × luminance into the feedback buffer, so they flow with the FlowField and leave trails; each frame's stamp replaces (fade × Pen × lum, #41) where that reaches ~10 levels, so Pen 0 draws nothing (#42). |
| `EPen` | expression | "Erase pen". Each frame, G-Force redraws the sprite at this intensity to erase what it drew, so EPen controls the trail. Should be ≤ Pen. `EPen=0` leaves no trail. Example: `EPen="dt ^ 1.5"`. **Measured (probe #40), contrary to this wording:** EPen is drawn on the displayed frame only and never changes the trail. Without Pen: out = F + ½·max(0, envelope·EPen·lum − F) (a half-way lighten, no trail, #42); with Pen: EPen is drawn on the display like Pen (default: envelope × EPen × lum replaces where it reaches ~10 levels; EPen 0: nothing drawn, #41). |
| `Flag` | integer | Bit flags (below). Ignored if `StartSprite()` passes its own flags. |
| `Invt` | 0/1 | Invert the image |
| `PDur` | expression | Duration in seconds. Overrides the pref; removed from the default params so the pref applies [VH 3.5.4]. |
| `Fit` | (legacy) | Shrink image to fit [VH 2.6.0]; superseded by flags |

**Variables:** `t`, `dt` ([DS] uses it in EPen), `XPOS`/`YPOS` (built-in bouncing-ball
position), `FADER` (undocumented; used as `EPen="FADER"`. [VH 2.7.2] mentions a "smoother
FADER" for sprite and particle fade in/out, so it is a 0..1 fade envelope).

**Flag bits ([DS]):**

| Value | Name |
|---|---|
| 1 | ShrinkToFit |
| 2 | EnlargeToFit |
| 4 | MaintainAspect |
| 8 | DontCrop |
| 16 | Invert |
| 1024 | ThresholdCopy |
| 2048 | AdditiveBlend |
| 4096 | SelectiveBlend |
| 8192 | AlphaMask |
| 16384 | AlphaBlend |

**Measured (probe #42)**, with x = envelope × pen × luminance and F the target: default and ThresholdCopy = replace
with x where x ≥ ~10 levels (an EPen-only default draw is the half-way lighten F + ½·max(0, x − F) instead);
SelectiveBlend = that half-way lighten; AdditiveBlend = min(1, F + x); AlphaMask = F·(1 − x); AlphaBlend =
F + x·(1 − F). The image's alpha channel is ignored (luminance of the straight color, Rec. 601). With several
sprites the one started first draws on top.

Default `Flag=7` per [DS], but a sprite .txt without `Flag` gets 0 (native size; probe #46: a sprite's own .txt
replaces `_default_sprite_params.txt` completely). Example: `Flag="16397"` = AlphaBlend + DontCrop + MaintainAspect +
ShrinkToFit ("G-Force Logo"). [DS] has a typo: its example says "Flags=20" for
MaintainAspect + Invert, but the key is `Flag`. Pen/EPen behavior changed in 3.1 [VH 3.1].
Four blending modes have existed since 3.0 [VH 3.0].

Background sprites auto-fit the frame [VH 3.0]. A new background sprite fades out the running
one [VH 3.6]. Sprites and WaveShape particles are separate playlists [VH 2.6.5]. **Measured (probe #44):** "background" means EPen-only (no Pen), whatever its size: a new
EPen-only sprite fades out every running EPen-only sprite, in a straight line from its current value to 0 over 1.3 s
(independent of Sprite.Transition.Duration, .Quick and the frame rate). Pen sprites never replace or get replaced
(a full-frame Pen sprite started over another keeps both).

**Movie sprites.** G-Force "auto-loads QuickTime if it sees video files present in the Sprites folder" [VH];
a movie is a sprite like an image. reforce (Phase 5F) plays `.mov .mp4 .m4v .mpg .mpeg .avi .qt .3gp` files
from the Sprite package (and `reforce.ExternalSpritesDir`) with the same rules as images: sibling `.txt` or
package defaults, flags/fit (the movie's frame size is the "image" size), Pen/EPen, fades and lifetime, `~`
inversion, script-only parentheses, Themes. The texture is the movie frame at the sprite's age (looping by
default), converted to Rec. 601 luminance, with no alpha channel (luminance doubles as alpha for AlphaBlend).
GUESS: G-Force's movie frame timing and color handling were not probed (PLAN 5F.6).
reforce-only options (PLAN 5K, all off by default): a sprite Mix per kind (`reforce.Sprite.Mix`,
`reforce.MovieSprite.Mix`) that dims the target under the sprite and raises the half-way lighten, movie luminance
levels (`reforce.MovieSprite.Gamma`, per-movie Black / White / Gamma), and full-color movie sprites drawn over the
colorized frame (`reforce.MovieSprite.Color = over`); see usage.md, "Sprite visibility".

---

### 5.7 Solid (reforce extension, PLAN 5L)

*Since 2026-11-22 solids and organics are one layer, Objects (`switch:object:NAME`, `Object.*` settings and theme
keys, `reforce.Object.MaxLive`; docs/usage.md "Objects"); the scene types and their keys below are unchanged. Any object may set `SBar=N`: from the slideshow it starts only on a line every N bars of the phrase clock.*
3D solids: a polyhedron, sphere, torus or prism wearing the current WaveShape (or the frame), drawn into the
intensity buffer through the sprite pass so it trails and takes the ColorMap (design: docs/3d-design.md). Scene
files live in `reforce.Solid.package` (reforce's committed `content/Packages/`, a scene root's `Packages/`, or the
folder `reforce.Solid.Dir`). Solids are Objects, a spawn layer like Particles: `switch:object:NAME`, `next:object`,
`kill:object`, `life:object:SEC`; `Object.Slideshow.Interval.Duration` is 0 (none) unless a Theme sets it (§8).

| Key | Tier | Meaning |
|---|---|---|
| `Mesh` | literal | `tetrahedron`, `cube`, `octahedron`, `dodecahedron`, `icosahedron` (default), `sphere`, `torus`, `prism`, `rubik` (a Rubik's cube: see below), `fifteen` (a 15 puzzle: see below). A name, not an expression; unknown names refuse to start. |
| `Text` | Pre | Face texture: `0` the current WaveShape (default), `1` the previous frame (Droste), `2` flat. |
| `Pers` | Pre | `1` perspective (default; 40° vertical field of view, camera at z ≈ 2.75), `0` orthographic. |
| `Flag` | Pre | Sprite blend flags (§5.6); absent = 0, a keyed copy. |
| `PDur` | Pre | Seconds at full strength (else `Object.Duration`, 40). |
| `PBar` | Pre | Bars at full strength instead (wins over `PDur`). With the beat clock the fade-out starts on the bar line nearest to that many bars after the fade-in, or on the nearest phrase line when it is a whole number of phrases (never under half of it); without it, bars at 120 BPM. The tempo at the start counts. |
| `A#` | Load | Once at start. |
| `B#` | Frame | Per frame, persisting (like sprites). `C#`/`D#` are not evaluated. |
| `X`, `Y`, `Z` | FrameOutput | Center. Sprite convention: ±1 = the frame edges at z = 0; +z comes towards the viewer (stay below ~2). Default 0. |
| `Scal` | FrameOutput | Circumradius in frame half-heights (default .5). |
| `RotX`, `RotY`, `RotZ` | FrameOutput | Radians, applied X, then Y, then Z (default 0). |
| `Fill` | FrameOutput | Floor of the face texture, 0..1 (default .15): faces stay visible where the wave is sparse. |
| `Shad` | FrameOutput | 0 flat .. 1 full Lambert shading (default .6). |
| `Edge` | FrameOutput | Brightness of the face-border highlight (default .8; 0 = none). |
| `Pen`, `EPen` | FrameOutput | As for sprites (§5.6): Pen draws into the feedback (trails), EPen on the display only. No Pen and no EPen = Pen 1. |
| `Over` | Pre | `1`: Pen / EPen draw the solid opaque over the frame with its coverage as alpha (exact intensities, dark parts covering what is behind), instead of the sprite blend `Flag` picks; `0` (default): by `Flag`. |
| `ColorKey` | Pre | `1` (default) keyed like the visuals in the Syphon alpha / camera backdrop; `0` the whole solid stays opaque there, dark parts included (docs/syphon-alpha.md). |
| `Moves` | Pre | `rubik`: scramble length, 1..32 (30); `fifteen`: 1..64 (30). |
| `Turn` | FrameOutput | `rubik`: position in the cycle, in turns: `0` solved, `Moves` fully scrambled (the scramble's moves done one after another), `2 * Moves` solved again (the same moves undone in reverse order); the fraction is how far the current turn has gone. Absent: solved. |
| `Seed` | FrameOutput | `rubik`, `fifteen`: picks the scramble (its whole part; 0). Change it once per cycle for a new scramble. |
| `Glas` | Pre | `fifteen`: `1` = liquid glass (drawn over the colorized frame, not into the visuals; the gaps and what is under the panes keep moving). |
| `Fros`, `Tint`, `Bend` | FrameOutput | `fifteen` glass: blur in pixels at 1080p of the frosted half (12; content 8), how much the glass's light tint shows 0..1 (.35; content .15), how strongly the button's shape bends the view 0..4 (1). |
| `Shuf` | FrameOutput | `fifteen`: the leap into the scramble, 0 = tiles on their solved places .. 1 = scrambled (default 1). |

**Rubik** (`Mesh="rubik"`): 26 cubies, their state a pure function of `Seed`, `Moves` and `Turn`
(engine/Rubik.hpp), so any jump of the clock simply shows the right cube. `Text`, `Fill` and `Edge` are not used;
`Shad` shades the puzzles in color (the color pass darkens each color by it), so it never changes a sticker's
color. The six sticker intensities are picked
for the current ColorMap (engine/StickerColors.hpp, docs/3d-design.md); the plastic is intensity 0. Use
`Over="1"`, so the stickers keep their exact intensities and the dark plastic is drawn rather than keyed out (no
`Flag` does that: AlphaBlend is a screen blend that pushes a cube redrawn over its own trail towards white), and
`ColorKey="0"` so the cube stays solid over the camera. Content's Rubik Cube times one cycle per 8 bars:

```
B2="SYNC_ON * 32 * wrap(SYNC_BAR / 8) + (1 - SYNC_ON) * 32 * wrap(B1 / 32)"   // beats into the 8-bar cycle
B4="clamp(B2 / 2)"  B5="max(0, B2 - 2)"
Turn="(B2 < 2) * 30 * B4 * B4 + (B2 >= 2) * (30 + floor(B5) + clamp((wrap(B5) - .55) / .45) ^ 2)"
Seed="SYNC_ON * floor(SYNC_BAR / 8) + (1 - SYNC_ON) * floor(B1 / 32)"
```

**Fifteen** (`Mesh="fifteen"`): a 15 puzzle on a tray (engine/Fifteen.hpp). `Turn` runs from `0` (scrambled) to
`Moves` (solved), one tile slide per whole turn; the scramble is a walk of the blank from `Seed`. Each row of the
solved board takes its own color, picked for the current ColorMap: the tiles are colored cuboids down to the
board (there is no tray: the gaps show the visuals), shaded by `Shad` (content: .5); the numbers are a dark
shade of their tile's color on four light row colors (or, on a ColorMap without good light colors, a light
shade on four dark ones: one choice for the whole board). Use
`Over="1"` and `ColorKey="0"` as for the rubik. Content's Fifteen Glass (with `Glas="1"` instead of `Over`): `Shuf="clamp(B2 / .75)"`,
`Turn="floor(B4) + clamp((wrap(B4) - .55) / .45) ^ 2"` with `B4="max(0, B2 - 1)"` (B2 = beats into the 8-bar cycle).

Variables: `t`, `START_TIME`, `END_TIME`, `DT` (age / lifetime, 0..1, as for sprites), `FADER`, the audio scalars,
`LINK_*` and `SYNC_*`. `t - START_TIME` is the solid's age, handy for motion that starts the same every time:

```
Mesh="icosahedron"  Text="0"
B0="t - START_TIME"
X=".45 * sin(.31 * B0)"  Y=".22 * sin(.47 * B0)"  Z=".7 * sin(.19 * B0)"
RotX=".35"  RotY=".5 * B0"
```

**Model** (`Mesh="model"`, PLAN 5R, docs/model-design.md): your own textured 3D model (a `.glb`) seen from a moving
camera. Model scenes live with the models in `reforce.Model.Dir` (default `~/Library/Application Support/reforce/Models`;
ids `Model/...`) and are left out when their model file is missing. Units: the model is centered and scaled to a
bounding sphere of radius 1, front along -z, top along +y. `X Y Z Scal Rot*` and `Text` don't apply; `Fill`, `Shad`,
`Pen`, `EPen`, `Flag`, `Over`, `ColorKey`, `PDur`, `PBar` and `Seed` do.

| Key | Tier | Meaning |
|---|---|---|
| `File` | literal | The model, relative to the model folder (or absolute). |
| `Look` | Pre | `0` = into the visuals as luminance (palette colors, trails; default). `1` = the model's own colors over the visuals; add `Pen` for palette trails behind it. |
| `Front`, `Up` | literal | The model's forward and top axes in the file: `+x`, `-z`, ... (default `-z`, `+y`; the prep script prints the Front line). |
| `Move` | literal | A camera preset: `dolly`, `pass`, `orbit` (default), `under`, `flyby`, `cuts` (content/Camera), or your own in the model folder's `Camera/`; `free` = expressions only (`CamX..AimZ`). |
| `K1`, `K2`, ... | literal | Camera keyframes, `"t eyeX eyeY eyeZ aimX aimY aimZ [fov [roll]]"`, t 0..1 over the move; they win over the Move preset (docs/model-design.md "Camera paths"). |
| `Ease`, `Step`, `Rand`, `Push` | literal | Smoothstep the timeline (0..1); beat cuts holding each keyframe `CutB` beats; shuffled order; push in while held. They override the preset's. |
| `MDur` / `MBar` | Pre | The move's length in seconds (24) / in bars on the beat clock (wins while the clock runs). Without `PDur` / `PBar` the scene lasts one move. |
| `Loop` | Pre | `1` = repeat the move (default), `0` = hold its last frame. |
| `Dist`, `Hite`, `Side` | frame | Camera distance scale (1), height offset (0), `-1` mirrors the path left / right. |
| `Roll`, `Fov` | frame | Extra camera roll (radians); a fixed field of view in degrees (0 = the path's own). |
| `CutB` | frame | Beat cuts (`Step`): beats per keyframe (4). |
| `CamX..CamZ`, `AimX..AimZ` | frame | `free`: the camera position and aim point; otherwise offsets added to the path. |
| `Spin`, `Bank` | frame | The model turning about its up axis (rad/s) and rolling (radians). |
| `Glow` | frame | Emissive (window / engine) brightness (1); e.g. `Glow="1 + .5 * BEAT"`. |
| `Blom`, `Expo`, `Tint` | frame | Look B: bloom on the emissive light (.6; 0 = off), exposure (1), pull towards a palette color (0..1, 0). |
| `LtX..LtZ` | frame | Towards the key light in camera space (-.45, .6, .65: upper left, from the camera's side). |

```
Mesh="model"  File="my-model.glb"  Look="1"
Move="orbit"  MBar="16"  Hite=".2"
Glow="1 + .4 * BEAT"
```

### 5.8 Organic (reforce extension, PLAN 5N)

*Since 2026-11-22 solids and organics are one layer, Objects (`switch:object:NAME`, `Object.*` settings and theme
keys, `reforce.Object.MaxLive`; docs/usage.md "Objects"); the scene types and their keys below are unchanged. Any object may set `SBar=N`: from the slideshow it starts only on a line every N bars of the phrase clock.*

Procedural growth, drawn as tapered lines into the intensity buffer through the WaveShape line pass, so it trails and
takes the ColorMap (design: docs/organics-design.md). Scene files live in `reforce.Organic.package` (reforce's
committed `content/Packages/`, or a scene root's `Packages/`). Organics are a spawn layer like Solids:
`switch:object:NAME`, `next:object`, `kill:object` (trees wither: they ungrow), `life:object:SEC`;
`Object.Slideshow.Interval.Duration` is 0 (none) unless a Theme sets it (§8). Up to `reforce.Object.MaxLive` (1)
at full strength (a new one fades the oldest out; the slideshow waits for a fade-out); a tree stops growing at `reforce.Organic.MaxNodes` (6000) nodes.

The only `Kind` so far is `tree`: space colonisation. `Pts` attraction points (the crown's shape) pull the nearest
node within `Infl`; pulled nodes grow a segment of `Seg` towards them; points within `Kill` are used up. The trunk
grows from each root until it reaches the points.

| Key | Tier | Meaning |
|---|---|---|
| `Kind` | literal | `tree` (default), or one of the other kinds below. A name, not an expression; unknown kinds refuse to start. |
| `PDur` | Pre | Seconds at full strength (else `Object.Duration`, 40). |
| `PBar` | Pre | Bars at full strength instead (wins over `PDur`), ending on a bar or phrase line as for Solids (§5.7). |
| `Pts`, `Rts` | Pre | Attraction points (300; 10..4000) and roots (1; 1..64). |
| `C#` | per point / root | Before each point's and each root's keys, ascending (with `s` = k / (count - 1) and `rnd`). |
| `PX`, `PY` | per point | A point's position. Default: an ellipse .9 x .55 at (0, .3). |
| `RtX`, `RtY`, `RtA` | per root | A root's position and initial direction in radians (0, -1, PI/2 = bottom center, up). |
| `Seg`, `Infl`, `Kill`, `Jitr` | Pre | Segment length (.02), influence radius (.3), kill distance (2 × Seg), direction jitter (.15). |
| `Wdth`, `Tip`, `BudW` | Pre | Trunk width, tip width, bud diameter in LWdt units (8, 1, 4). Widths follow the pipe model. |
| `Wthr` | Pre | 1 (default): ungrow during the fade-out; 0: dim with `FADER`. |
| `Strk`, `SGap`, `SAln` | Pre | Strikes: grow `Strk` times (1; 1..16), each from fresh points and roots (A vars, points and roots evaluated again), the first on the next line every `SAln` beats (1; 4 = bar lines), the next every `SGap` beats (4). |
| `SwyB` | Pre | Beats per sway cycle on the beat clock, about the fixed root (0 = the free 0.3 Hz wave). |
| `VibF` | Pre | Vibration cycles per beat at the tips (3; .6 x that at the root). |
| `PlsB`, `PlsE` | Pre | Pulses: beats for a pulse to reach the farthest tip (1); a root fires every `PlsE` beats (1), the next one a root whose branches meet the last one's. |
| `PlsL`, `PlsP` | Pre | On every bar line (`SYNC_BAR`) a lead root restarts the cascade with a pulse this strong (0 = none; 1.7 = brighter and thicker); on every phrase line (`SYNC_PHRASE`) every root fires at `PlsP` (0 = none). A drop (`PAll`) fires all at 1.6. |
| `Rwir`, `RwrB` | Pre | Rewiring: on every phrase line a share `Rwir` (0 = none) of the side branches withers back over `RwrB` bars (4), is removed, and `Pts × Rwir` new points (`PX`, `PY` again) grow new branches; the roots stay put. |
| `LeafW`, `LeafD`, `LeafA`, `LeafU` | Pre | Leaves: length in tree units (.05), on every `LeafD`-th node of the outer twigs and every tip (2), angle off the twig in radians, alternating sides (.9), beats to unfurl (4). |
| `A#` | Load | Once at start, and again at every strike after the first. |
| `B#` | Frame | Per frame, persisting. `D#` is not evaluated. `s` = beats since the current growth (strike) began. |
| `Grow` | FrameOutput | Growth iterations (one segment per tip) per second (12). |
| `TrpX`, `TrpY` | FrameOutput | Tropism added to each new branch segment's direction (0; the trunk ignores it). |
| `Sway` | FrameOutput | Sway at the thinnest tips in radians (.15). |
| `X`, `Y`, `Scal`, `Rot` | FrameOutput | Move, scale and turn the whole organic (0, 0, 1, 0). |
| `Pen`, `Bud` | FrameOutput | Branch intensity (1) and tip-bud intensity relative to Pen (0). |
| `Vib` | FrameOutput | Radial vibration (0): each node out from its root by Vib × its distance × cos(2π VibF (.6 + .8 depth) wrap(beat)). |
| `Puls`, `PAll` | FrameOutput | Pulse brightness added to Pen (absent = no pulses); every root fires when `PAll` rises past .5 (0). |
| `Leaf`, `LFlt` | FrameOutput | Leaf intensity relative to Pen (0 = no leaves); leaf flutter in radians (0). Leaves are hard-edged pointed ovals drawn over the branches; they fall as the tree withers. |

Coordinates: y = ±1 at the bottom / top of the frame, x in the same units (±1.78 at the edges of 16:9), so circles
stay round. Variables: as for Solids, plus `s` in the per-point and per-root keys and, in the B vars and frame
keys, the beats since the current growth began. The beat clock is `SYNC_BEAT` while locked, else 120 BPM from the
start.

**Other kinds** (PLAN 5O). Every kind takes `PDur`, `A#`, `B#`, and per frame `X`, `Y`, `Scal`, `Rot`, `Pen`. Time is
the beat clock when it is locked (`SYNC_BEAT`, and `SYNC_BAR` aligned to the phrase), else 120 BPM from the start.

| Kind | Load keys (default) | Frame keys (default) |
|---|---|---|
| `pendulums` | `Num` (15) pendulums, pendulum k swinging `Osc` + k times (`Osc` = `Cyc`) per cycle of `Cyc` bars (16, so they line up on the phrase lines); `View` 0 from above (a row and the snake through it), 1 from the side (strings from y = .9, `Len` 1.4 long), 2 a ring (radius `Len` / 2, swinging in and out); `Sprd` row width (3), `BobW` (10), `Wdth` string / line width (1.5) | `Amp` (.45 / .5 rad / .25), `Strg` line intensity relative to Pen (.35 / .6 / .35), `Phas` cycle position (default: the clock) |
| `phyllotaxis` | `Num` seeds (800), `Rate` seeds per beat (4), `Spc` spacing (.032; seed n at radius Spc √n), `DotW` (6) | `Ang` degrees between seeds (137.508, the golden angle) |
| `harmonograph` | `Stps` (600), `Len` bars of trail (1), `Wdth` (2) | `FX`, `FY` cycles per bar (3, 2), `PhX`, `PhY` (0, PI/2), `AX`, `AY` (1.2, .8), `Damp` (0: decay along the tail per bar) |
| `automaton` | `Rule` (90), `Cols` (101), `Rows` shown (56), `Rate` generations per beat (2), `Seed` 0 = one cell, 1 = random; `Cell` dot size (.8 of a cell) | |
| `life` | `Cols` (64), `Rows` (36), `Rate` generations per beat (1), `Dens` start density (.3), `Rsd` reseed every N bars (16; 0 = only when it dies or stops), `Cell` (.8) | |
| `lsystem` | literal `Axm` (X) and rules `R1`..`R4` ("F=FF"; default: a fern), `Gens` (5), `Rate` bars per generation (2; each new one traced over its first bar), `Wdth` (1.5). Turtle: F G draw, f move, + - turn, \| about-face, [ ] push / pop; fitted to the frame, root at the bottom | `Ang` degrees (25) |
| `attractor` | `Num` points per frame (6000, at most 30000), `DotW` (1.5), `Sprd` scale (.42) | `PA`, `PB`, `PC`, `PD` (-1.4, 1.6, 1, .7): a Clifford attractor x' = sin(a y) + c cos(a x), y' = sin(b x) + d cos(b y) |
| `ripples` | GPU (render/FieldPipeline). `Rate` drops per beat (1; 0 = none), `Rad` drop radius in frame heights (.03), `Spd` wave speed in frame heights per second (.4), `Decy` seconds to 1/e (2.5), `Edge` 0 = the frame's edges absorb, 1 = they reflect, `Flag` sprite blend flags (0) | `DX`, `DY` where the next drop lands (tree units; default random), `Amp` drop height (1), `Rain` extra small drops per second (0), `Gain` brightness of the crests (3), `Pen` |
| `julia` | GPU. `Iter` iterations (120; 8..1000), `Flag` (0) | `CRe`, `CIm` the constant c (-.8, .156), `X`, `Y`, `Scal`, `Rot` (the set's ±1.6 fills the frame height at Scal 1), `Gain` (1), `Band` bands per Iter of escape count (0 = none), `Phas` band phase in radians, `Pen` |
| `mandel` | GPU. A Mandelbrot dive on the clock (engine/MandelDive). `Dive` bars per dive (16: one per phrase), `Dbl` beats per doubling of the magnification (2; 0 = paced to reach `Deep` just before the rush out), `Deep` deepest log2 magnification (34; at most 44), `Out` beats of the rush back out to the whole set at the end of a dive (4; 0 = a cut), `Algn` dives count from the phrase line (every Algn bars) at or before the start (16; 0 = from bar 0), `Ent` doublings a beat while catching up from the whole set after a start mid-dive (6; 0 = jump straight in), `Kick` doublings surging in on each beat line (.3), `Drop` doublings a drop pulls out (6), `Iter` least iterations (200), `Itk` x the iterations the target needs at the depth (1.5), `MaxI` cap (4000), `Prec` 0 auto (float32, perturbation past 2^12), 1 float32, 2 double-float, 3 perturbation, `Res` render scale (1; .5 = a quarter of the pixels, upscaled), `Mini` which endings: 0 all four kinds (spirals, lace, mini-Mandelbrots, the decorations around a mini; taking turns), 1 minis and decorations only, 2 no minis, `Aff` 0 turns off the mini interior shortcut (1), `Flag` (0) | `Tgt` target index (-1 = the shared seeded order, one per dive; 0-4 spirals, 5-6 lace, 7-10 minis, 11-13 decorations), `X`, `Y`, `Scal`, `Rot` (the whole set fills the frame height at Scal 1 and depth 0), `Gain` (1), `Band` escape-count iterations per band (0 = none), `Phas` band phase (default: the bands flow inward one per beat), `Glow` pixels from the edge fully lit (1; 0 = none), `Pen` |
| `invaders`, `hopper`, `cycles`, `rocks`, `bricks`, `chase` | Arcade kinds (PLAN 5Q): an invader march, a pyramid hopper, light cycles, vector rocks, a brick wall, the light cycles in 3D behind one cycle; keys in docs/arcade-design.md | |

The GPU kinds are not lines: each draws into its own frame-sized texture and is composited through the sprite pass
like a solid (Pen, `Flag`, the fade envelope as opacity), so it trails. Ripples solve the wave equation on a grid of
270 rows (the frame's aspect); a drop is a Gaussian bump. Julia shading: the smooth escape count mu, luminance
(mu / Iter)^0.4 x Gain, times .5 + .5 cos(2 pi Band mu / Iter + Phas) when Band > 0; the set itself is dark.
Mandelbrot shading, the same at every depth: d = the distance estimate to the set in pixels, luminance
(1 - log2(d) / 12) x Gain, times .5 + .5 cos(2 pi mu / Band + Phas) when Band > 0 (faded where the bands would
alias), at least (Glow / d)^2 x Gain; the set itself is dark. Dives head for Misiurewicz points (the boundary
looks the same at every depth there), so a dive never ends in a flat frame.

**Groups** (the Objects card's dropdown): `content/Categories/Object/*.txt` (one category per group: Trees,
Pendulums, Curves, Cells, Water, Fractals, Arcade) and a theme per group, `Objects: <group>`
(`Object.CategoryExpr = "+<group>"`, an organic every ~30 s). An organic in no group is listed under "Other".

Example (content's Oak, abridged):

```
Kind="tree"  Pts="420"
C0="PI * rnd(1)"  C1="sqrt(rnd(1))"
PX="1.15 * C1 * cos(C0)"  PY=".1 + .75 * C1 * sin(C0)"
B1="SYNC_ON * SYNC_BEAT + (1 - SYNC_ON) * 2 * (t - START_TIME)"
Grow="8 * (1 + 2.5 * exp(-5 * wrap(B1)))"  SwyB="4"  Sway=".25 + .12 * BASS"  Bud=".9 * exp(-3 * wrap(B1 / 4))"
```

### 5.9 Dancer (reforce extension, PLAN 5P)

One character per file in `reforce.Dancer.package` (docs/dancers-design.md). Every key is a **literal** (no
expressions); unknown values keep the default and are logged.

| Key | Default | Meaning |
|---|---|---|
| `Style` | `pop` | `disco`, `hiphop`, `house`, `pop`, `robot`, `rave`, `retro`, `chill` (also `popping`, `shuffle`, `funk`, `60s`) |
| `Skin`, `Hair`, `Eyes`, `Top`, `Bottom`, `Shoes`, `Accent` | a sample look | colors `"#rrggbb"` (`Accent`: hat, headphones, tie, hair tie) |
| `HairStyle` | `short` | `short` (`crew`, `crop`), `buzz`, `shaved`, `sidepart`, `quiff`, `pompadour`, `slickback`, `undercut`, `coils` (`twists`, `hightop`), `waves` (360 waves), `curtains` (a middle part), `balding`, `manbun`, `mullet`, `bowl`, `spiky`, `mohawk`, `anime` (`messy`), `curly` (`longcurls`), `afro`, `bob` (`lob`), `long` (`wavy`, `locs`), `layers`, `bangs` (long with a fringe), `shag`, `halfup`, `pixie`, `ponytail` (`lowbun`), `sidepony`, `braid` (a side braid), `pigtails`, `bun`, `topknot`, `spacebuns`, `bald` |
| `Outfit` | `tee` | `tee`, `tank`, `hoodie`, `dress`, `suit`, `skirt` (a top over a skirt in the Bottom color), `croptop` (a bare midriff), `jumpsuit` (top and trousers in the Top color, an Accent belt), `longdress` (to the ankles) |
| `Hat` | `none` | `none`, `cap`, `beanie`, `top`, `headphones` |
| `Build`, `Tall` | 1, 1 | width .75..1.3, height .8..1.2 |
| `Head`, `Legs`, `Torso`, `Shoulders`, `Limbs` | 1 | proportions: head size .75..1.35 (big cartoon head > 1), leg length .75..1.25, torso length .8..1.2, shoulder width .8..1.25, arm and leg thickness .75..1.35 |
| `Belly` | 0 | 0..1, a rounder middle |
| `TopPattern`, `BottomPattern` | `none` | print: `none`, `stripes`, `dots`, `plaid`, `gradient`, `zigzag` (a dress is the top) |
| `Ink` | `#ffffff` | the print's color |
| `TopFabric`, `BottomFabric` | `matte` | `matte`, `shiny` (lame: reflects the visuals), `sequins` (sparkle on the beat), `velvet` (sheen at the edges), `neon` (glows by itself, pulses on the beat) |
| `Glasses` | `none` | glasses or sunglasses (see Lens): `none`, `round`, `aviator`, `visor`, `heart`, `star`, `cateye`, `square` |
| `Lens`, `Frame` | dark, black | the lens color (sunglasses), or `Lens="clear"` for ordinary glasses (just the frame, the eyes show through); the frame color; `Mirrored="1"` makes dark lenses mirrors (they reflect the visuals) |
| `GlowSticks` | `none` | a color: a neon glow stick in each hand |
| `EyeShape` | `oval` | `oval`, `round`, `sleepy`, `happy` (^ ^), `big`, `dot` |
| `Brows` | `normal` | `normal`, `thick`, `thin`, `arched` |
| `Mouth` | `smile` | `smile`, `grin`, `small`, `smirk` |
| `Beard` | `none` | `none`, `stubble`, `moustache`, `handlebar`, `goatee`, `circle` (moustache and goatee), `short` (`chinstrap`), `full`, `long` (hair color; all but stubble 3D) |
| `Lashes`, `Freckles` | 0 | `1` to add them |
| `Name` | none | A name tag over the dancer's head (a stream guest's name), shown with `reforce.Dancer.NameTags` = named (default) or all |
| `Blush` | .35 | 0..1 |
| `Lips` | none | lipstick color |
| `Random` | 0 | `1`: a new character each time it starts, generated for `Style` (body, colors, outfit, prints, fabrics, sunglasses, glow sticks, face; the look keys above are ignored). The package has one per style: `Random Disco`, `Random Hip-Hop`, ... |
| `Moves` | none | favorite moves, picked 4x as often (names from engine/DanceMoves.cpp, e.g. `dab,floss,discoPoint`) |
| `Snap` | the style's | 0 loose .. 2 snappy |
| `PDur` | `Dancer.Duration` | seconds on stage |

```
Vers=100
Style="disco"
Skin="#5a3825"  Hair="#3a2416"  HairStyle="afro"
Top="#f4d03f"  Outfit="dress"  Shoes="#f5f5f5"
Moves="discoPoint,shimmy,freeze,grapevine"
```

## 6. Slideshow, transitions, and playlists ([CU])

Per config type (`[ConfigType].` prefix: WaveShape, FlowField, ColorMap, Particle, Sprite):

| Pref | Meaning |
|---|---|
| `Slideshow.EnableOnStartup` | Enable the slideshow at startup |
| `Slideshow.Interval.Duration` | Seconds from the end of one change to the start of the next (default 30 s since 5.6 [VH 5.6]) |
| `Slideshow.Interval.Duration.Deviation` | Relative standard deviation (.1 × 15 s gives σ = 1.5 s) |
| `Transition.Duration` | Morph length for slideshow changes |
| `Transition.Duration.Quick` | Morph length for manual changes |
| `Transition.Duration.Deviation` | Relative standard deviation of the transition duration |
| `Transition.Function` | Expression in `t` (**here t ∈ [0,1] is transition progress**). It must give 0 at t=0 and 1 at t=1, and returns the weight of the incoming config. The suggested shape is `1-(1-t)^1.5`. |
| `ConfigPrepTime` | Seconds allowed for background prep (FlowField precompute) |

- By default, G-Force randomly shuffles WaveShapes, FlowFields, and ColorMaps [BG].
- Prev/Next step alphabetically when the slideshow is off [VH 4.3].
- `U` reloads the current configs from disk. Shift+U rescans folders [CP][FAQ].
- Folders are scanned recursively [VH 2.5.0], via `_package.py` (`EntryRegex`
  `(.*)\.(txt)$`, plus `png` for ColorMaps) [PKG].

---

## 7. FlowField Unions in the slideshow
See 5.4. Unions are a separate playlist that is consulted at FlowField transitions when
`PREF_UNION_INTERVAL` has elapsed. The Toolbar can enable or disable Unions [VH 4.1.2].

---

## 8. Themes and Categories

These are separate file formats: user data, edited via the UI [FAQ]. The [FAQ] warns:
"Be conscious of punctuation, spacing, and quotation marks ... or else G-Force can crash".

**Syntax:** `Key = Value` lines, with spaces around `=`. Values are integers (decimal or
`0x` hex, e.g. `LastModified = 0x5165EC00`, `CategoryFlags = 0x01`) or double-quoted strings.
Strings can span lines (`Include.List`). The two-character sequence backslash-`t` inside
strings is a **literal** separator (it is not a real tab byte in the files).

**Theme** (`Themes/*.txt`):

| Key | Meaning |
|---|---|
| `ThemeName` | Display name |
| `About` | Description |
| `Locked` | 1 for factory themes |
| `Version` | 1 |
| `LastModified` | Hex timestamp |
| `Object.Slideshow.Interval.Duration` | reforce extension (§5.7): seconds between slideshow solids while the theme is on (solids are not in the default slideshow). Absent: the setting's interval (0). |
| `<Type>.CategoryExpr` | Per type (WaveShape, FlowField, ColorMap, Particle, Sprite; reforce: Solid). A `\t`-separated list of `+Category` (include) and `-Category` (exclude) terms, e.g. `"+(Suggested)\t-Bright Background\t-Low Saturation"`. A type whose key is absent is presumably unrestricted. Inferred semantics: union of the `+` sets minus the union of the `-` sets. |

**Category** (`Categories/<Type>/*.txt`):

| Key | Meaning |
|---|---|
| `CategoryTitle` | Name referenced by CategoryExpr. The file name can differ: `Spirt.txt` has the title "Spirit". |
| `About`, `Version`, `LastModified` | |
| `CategoryFlags` | Always 1 in the corpus |
| `Include.List`, `Exclude.List` | Newline-separated config names inside one quoted string |
| `Include.Pattern`, `Exclude.Pattern` | Regex on config names. `(Suggested)` for Sprite uses `".*"` plus an exclude list. |
| `Include.ConfigFlagSelectMask` = 6, `Include.ConfigFlagMatchMask` = 0 | Constant in the corpus. Semantics undocumented. |

`(Suggested)` for WaveShape, FlowField, ColorMap, and Particle has no local file, so it is
presumably built in (maybe "all", or flag-based via the masks).

---

## 9. Version (`Vers=`) compatibility notes

No document states that `Vers` changes evaluation. Changes that affect how old configs
render, from [VH]:

| Version | Change |
|---|---|
| 2.0 | Sprites; spectrum and bass access; FlowField morphing |
| 2.5.0 | FlowField Union type; `ID` and `NUM` valid in WaveShapes; wrapped `fft`/`mag` domains |
| 2.6.0 | "DeltaFields" renamed "FlowFields"; R/G/B ColorMaps; B and C vars in ColorMaps; `flip()`; `PDur` can see `NUM` and `NUM_S_STEPS`; Meta text |
| 2.6.4 | Non-integers need not be quoted |
| 2.6.8 | `asin`, `acos`, `atan2` added; stricter syntax |
| 2.7.0 | Whitespace (not commas) delimits pairs; `WIDTH`/`HEIGHT`/`X_EXTENTS`/`Y_EXTENTS` added |
| 3.0 | FlowField velocity scale reduced by hand in most FlowFields (a content change) |
| 3.1 | VectorC VM; WaveShape morph reimplemented; FFT bins 155 → 185; auto line scale; sprite Pen/EPen change; parenthesized names excluded from the slideshow |
| 3.1.1 | Fix: `rnd()` with a non-scalar argument |
| 3.6 | Lazy evaluation |
| 4.0 | **`t` starts from 0**, which broke some particles that assumed a large `t`; floor() fix on PPC |
| 4.3 | FFT auto-normalization |
| 4.4.0 | `BASS` fix |
| 5.0 | Themes/Categories; Particles folder |
| 5.3 | VisPackage loading; Python scripting; `Meta` unused; text sprites removed |

Practical guidance: ignore `Vers` for semantics, but keep it (and log it) for diagnostics.

---

## 10. Open questions / undocumented

> **Update 2026-11-11:** many of these are now answered by black-box measurement. See
> [probe-results.md](probe-results.md), which takes precedence over the guesses below: precedence, `^` associativity,
> inclusive `s`, var init 0, C vars as per-step vectors, linear decay of 1/255 per frame, clamping, and the RGB, source-point
> and per-index morphs.

1. **Operator precedence and associativity**, especially `^` vs unary minus (`-1^n`),
   `%` on floats and negatives, `%` by 0, and division by zero (inf/NaN handling).
2. **Key case-sensitivity.** [CP] says keys are case-sensitive, but `Num`, `stps`, `Lwdt`,
   and `pen` are common in shipped configs. Were they honored or silently ignored? This
   affects whether those configs render as intended.
3. **Evaluation order and persistence.** Index order is assumed. The initial value of B/C vars
   on the first frame is unknown (0?). Are B vars per-instance or shared across `ID`? The
   corpus implies shared. Do C vars carry over between s steps (Fern) or between frames
   (VectorC)?
4. **`rnd()` broadcasting.** When the argument is a scalar but the context is per-step or
   per-pixel, does `rnd` return per-element values? [FL] says yes for FlowField D and src
   expressions. What about WaveShape C/X/Y? Fern's `C0="floor( rnd( 10000 ) )"` needs a
   per-step value.
5. **s sampling:** i/(N−1) (inclusive) or i/N?
6. **`flip(x)` definition.** The listed points are ambiguous: "(−1,1),(−1,−1)" suggests a
   jump at odd integers. One plausible form is `flip(x) = 2*wrap((x+1)/2) - 1`, possibly
   negated. Verify against reference renders.
7. **`wyvill(r)`**: is the argument r or r²? [VC] names it `r2`.
8. **Audio ranges and scaling:** the numeric range of `mag()` (signed?), `fft()`, and `BASS`.
   What are `level` and `energy` exactly, and their smoothing? Is `mag`/`fft` lookup
   nearest-bin or interpolated (nearest per [RC])? `NUM_SAMPLE_BINS` and `NUM_FFT_BINS`
   default values (FFT 185 per [VH 3.1]; PCM unknown).
9. **Undocumented identifiers:** `PATH_ID`, `FADER`, `XPOS`/`YPOS` dynamics, `level`,
   `energy`, `ceil`, `!`, matrix literals, and `st` (mentioned in [RC]'s list "dt, st, t").
   Also, how undefined identifiers such as `rnd16` resolve.
10. **Pixel flow details:** the per-frame decay amount, the interpolation (nearest vs
    bilinear), the out-of-bounds source policy (clamp, wrap, or black), the vector resolution
    (full-res or grid), and how `srcR`/`srcT` convert (theta convention: angle from +x,
    −π..π).
11. **Aspect mapping with `Aspc=1`:** is the short axis ±1 (assumed) or the long axis? (FlowFields: short axis
    ±1 with square pixel steps, measured: probe-results #36.)
12. **Line rendering:** antialiasing, dot shape when `ConB=0`, `Pen` per point vs per segment,
    and how Pen writes into the buffer (overwrite, max, or additive). *Measured:* overwrite in path order
    (probe #15); points snap to whole pixels, col = W/2 + trunc(x·W/2), row = H/2 + trunc(−y·H/2); each segment is
    a round-capped capsule of perpendicular width LWdt·s(W, H) px (probe #38), and a pixel's coverage is the capsule's
    length along the pixel's center line in the segment's major axis (vertical for x-major segments and dots),
    clipped to the pixel: no threshold between "thin" and "thick" (probe #32, docs/probe-results.md).
13. **Particle lifecycle:** spawn probability and prefs, an automatic fade envelope on Pen,
    and defaults for `PDur` and `NUM` outside the Particles folder.
14. **Morph algorithms:** the WaveShape point correspondence when `NUM_S_STEPS`, `NUM`, or
    the path count differ; ColorMap and FlowField interpolation space (RGB lerp? vector lerp?);
    how Transition.Function's output is applied.
15. **FlowField Union lifecycle:** is ALFA a spatial transition mask during one morph, or does
    the blended field persist? *Measured (probe #45):* ALFA is a per-point blend weight during the morph
    (w·ALFA), and the blended field then persists until the next FlowField transition.
16. **ColorMap clamping** of S/V/R/G/B, and the exact 256-entry sampling of i (i = k/255?).
    The GLSL colorizer samples a 2D table with `intensity.ra`. What is the second coordinate,
    possibly for morph blending?
17. **Theme/Category semantics:** `(Suggested)` for non-Sprite types, the
    ConfigFlagSelect/Match masks, the pattern regex dialect, and the evaluation order of
    `+`/`-` terms.
18. **`CCal` default value** (described only as "a little less than 0.1").
19. **Sprite `Pen` default** when omitted, `Sped` units, and `X`/`Y` tier (per frame
    assumed).

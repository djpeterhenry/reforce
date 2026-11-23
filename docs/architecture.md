# reforce — Architecture

Status: v1 draft, 2026-11-11. Stack: **C++20 + Metal (metal-cpp), macOS only, Syphon output** (see PLAN.md decision log).
Scene semantics are specified in [scene-format.md](scene-format.md); this doc covers how we implement them.

## 1. Guiding decisions

1. **The CPU interpreter is the source of truth; the GPU runs the per-pixel work.** Every scene expression
   runs through one C++ interpreter. Hand-written Metal shaders do the per-pixel work: flowing the
   previous frame along the warp, drawing lines, applying the colormap.
   Generating shader code from scene expressions is an *optional later optimization* (§4.4), because:
   - FlowFields never reference time or audio, so each field is computed **once per scene load**, not per frame.
   - WaveShapes are at most a few thousand points per frame. That's trivial work for a compiled interpreter.
   - Sequential CPU evaluation gives us G-Force's stateful semantics for free: B/C variables that read their own
     previous value, `rnd()` behaving differently per row and per pixel, the VectorC array functions.
2. **Single-channel intensity buffer**, colorized at the end, exactly as G-Force does it. Feedback, decay and
   drawing all work on a float intensity texture. Color only appears in the final palette lookup.
3. **Fixed-size offscreen render**, independent of the window. The final RGBA texture is backed by an IOSurface:
   it is scaled to fit the window and also published through Syphon.
4. **Deterministic mode.** Seeded RNG, fixed timestep and file audio input, so we can render reproducible frames
   for tests and for comparison with G-Force.

## 2. Frame pipeline

```
 audio thread ──► ring buffer ──► Analyzer (vDSP FFT) ──► AudioFrame {wave[], fft[], BASS, LEVEL, ENERGY}
                                                          │
 Scheduler (playlists, slideshow, transitions) ───────────┤
                                                          ▼
 ┌──────────────────────── GPU, per frame (render thread) ─────────────────────────┐
 │ 1. Flow pass     I_new(p) = decay( I_prev( lerp(fieldA(p), fieldB(p), w(p)) ) ) │
 │                  fieldA/B: RG32F source-coordinate textures (precomputed)        │
 │                  w(p): transition weight × optional Union ALFA mask texture      │
 │ 2. Draw pass     WaveShape + particle polylines → thick-line quads → I_new       │
 │ 3. Sprite pass   textured quads (luminance) with G-Force blend flags → I_new     │
 │ 4. Colorize      RGBA = palette( mapFn(I_new) )   palette: 256×1, lerped A→B     │
 │                  → output texture (IOSurface, e.g. 1920×1080 BGRA8)              │
 │ 5. Present       scale-to-fit → CAMetalLayer drawable;  SyphonMetalServer publish│
 └──────────────────────────────────────────────────────────────────────────────────┘
 I_prev / I_new swap each frame (two R16F textures).
```

The CPU side of each frame (render thread, before encoding the GPU passes):
- Advance `t`, then poll the analyzer for the latest `AudioFrame`.
- For each active WaveShape/particle instance: evaluate B (per ID), then step `s` over C, X#/Y#, Pen
  and LWdt → vertex buffer.
- ColorMap: re-evaluate when its `CCal` interval comes due (256 entries) → upload the palette.
- Sprites: evaluate B, X, Y, Pen, EPen.

## 3. Modules

```
src/
  platform/mac/   NSApp/NSWindow/CAMetalLayer view, fullscreen toggle, display link, Syphon server, hotkeys
  render/         Metal device & queues, intensity ping-pong, passes (flow, lines, sprites, colorize, present),
                  shader sources (MSL strings, compiled at startup via newLibraryWithSource)
  audio/          CoreAudio HAL input (device select; Loopback default), file player (ExtAudioFile → also to speakers),
                  lock-free ring buffer, Analyzer (vDSP FFT, smoothing, BASS/LEVEL/ENERGY)
  expr/           lexer, Pratt parser → AST, resolver (case-insensitive names → slots), compiler → bytecode,
                  VM with Value = scalar | matrix (VectorC), builtins table, seeded RNG
  scene/          SceneFile loader (tolerant: CR/CRLF, multi-line quotes, commas, dup keys, Latin-1 fallback, tags),
                  typed configs: WaveShapeConfig, FlowFieldConfig, UnionConfig, ColorMapConfig, SpriteConfig
  engine/         instances & evaluation tiers (A/B/C/D, persistent state, ID instancing),
                  FlowFieldBuilder (worker thread → RG32F field), ColorMapBuilder, ParticleSystem, SpriteSystem,
                  Transition (Transition.Function weights, morph matching), Scheduler/playlists, Themes/Categories
  app/            main loop, prefs (JSON/TOML), CLI flags (--deterministic, --audio-file, --render-frames N)
tests/            doctest: expr unit tests, corpus parse test (skipped if scenes/ absent), golden-image tests
tools/            scan_corpus.py, probe harness (§7)
```

Dependency direction: `app → engine → {expr, scene, audio, render} → platform`. `expr` and `scene` are pure C++ with
no Apple dependencies, so they're unit-testable on their own.

## 4. Expression engine

### 4.1 Pipeline
`text → tokens → AST → resolved AST → bytecode`. Compile once per config load and run many times.
- **Grammar:** a Pratt parser with the operator set from corpus-stats (`+ - * / % ^ == != < > <= >= && || ! ^^`,
  unary `-`/`!`, `{...}` matrix literals, calls). Precedence follows scene-format §3.2. Unsettled cases
  (e.g. `-1^n`) are settled by probing (§7).
- **Names:** case-insensitive. Every identifier resolves at compile time to one of: a built-in function, a built-in
  variable (`t`, `s`, `x`, `BASS`, …), a user-var slot (`A3`, `C12`), or a **key reference** (`NUM`, `Side`, `AVRG`).
  An unknown name is a load-time warning and evaluates as 0.
- **Lenient lexing:** `1L` → `1`, and `.5.5` → a warning plus best-effort parsing. Each fix is recorded in a per-file
  diagnostics list.

### 4.2 Values and the VM
- `Value` is a small-buffer-optimized type: a scalar `double`, or a dense row-major matrix (VectorC). The scalar
  fast path has no allocation. Matrix ops (`row`, `col`, `map1D`, `map2D`, `vectorLR`, `transpose`, `select`, …)
  live in `builtins_matrix.cpp`.
- A stack-based bytecode VM evaluates one expression at a time. Variable slots live in a `Frame` struct
  per instance:
  `A[]` (set at load) · `B[]` (persists across frames, shared across `ID`s) · `C[]` (persists across `s` steps) · locals.
- **Ordering:** within a tier, evaluate in ascending index order. Reading a slot that hasn't been written yet in this
  pass returns its previous value (initially 0). This one rule gives the documented B persistence and the Fern-style
  C carry-over.
- **RNG:** one seeded PCG stream per instance. `srand(k)` reseeds it. The tier decides how many values `rnd` returns
  (per pixel for D/src, per row for C in FlowFields).

### 4.3 Per-type evaluation
| Type | Load | Per frame | Inner loop |
|---|---|---|---|
| WaveShape/Particle | Stps, NUM, PDur, A | B (per ID) | per s step: C, X#/Y#, Pen, LWdt → vertices |
| FlowField | A, Aspc | — | per row: C · per pixel: D, srcX/srcY or srcR/srcT → field texel (worker thread) |
| Union | A | — | per pixel: D, ALFA → mask texel (worker thread) |
| ColorMap | A | B every CCal s | per i (256): C, H/S/V or R/G/B → palette |
| Sprite | A, Sped, Flag | B, X, Y, Pen, EPen | — |

**FlowField build cost:** about 2M pixels × a few dozen ops each takes roughly 0.2–1 s on one core. Mitigations:
build on a worker thread ahead of the transition (as G-Force does with ConfigPrepTime), split rows across cores, and
optionally build at half resolution with bilinear upsampling of the field (G-Force also uses a reduced
"vector resolution").

### 4.4 Optional: generate MSL for FlowFields
If build latency matters (e.g. for quick manual switching), lower the resolved AST to an MSL compute kernel
(scalar ops map 1:1, `rnd` becomes a hash of pixel and seed, and matrix literals become `constant` arrays). The CPU
interpreter remains the reference, and tests compare the two fields.

## 5. Rendering details

- **Intensity textures:** R16Float ping-pong at the render resolution. Flow sampling is bilinear. Sources outside the
  frame return 0 (to verify by probing).
- **Decay:** `I' = max(0, I·k − d)`, with k and d as preferences. Values to be tuned against G-Force captures.
- **Lines:** each polyline segment is expanded into a quad (plus round caps when ConB) in a vertex shader from a
  storage buffer of points. Width = `LWdt × scale + offset`, with auto-scale proportional to frame height. Blend
  mode is **max**, so Pen sets a floor on intensity (to confirm). Dots (ConB=0) become point quads.
- **Palette:** a 256×1 RGBA16F texture. During a ColorMap transition both palettes are bound and lerped in the
  colorize shader. The `ColorMap.MapFunction` preference is a 256-entry 1D lookup texture applied first.
- **Output:** BGRA8 in an IOSurface-backed `MTLTexture`. The present pass scales it into the drawable with letterbox
  bars. `SyphonMetalServer publishFrameTexture` is called once per frame on the same command buffer.
- **Frame pacing:** CAMetalDisplayLink on the window's screen. When the window is occluded or minimized, a timer
  keeps rendering at the target rate so Syphon output never stalls.
- **Window present** runs on its own thread (`render/Presenter`): the render thread blits each finished frame into
  a free copy slot; the present thread waits for drawables, so an occluded window keeps updating whenever the system
  hands out drawables (OBS window capture) and the render/Syphon thread never blocks in `nextDrawable`.

## 5a. Transitions (morphing)

FlowField, WaveShape and ColorMap (and Particle/Sprite fades) each have their **own independent `Transition`**:
`{from, to, t0, duration, shapeFn, weight w ∈ [0,1]}`. One layer can be mid-morph while the others sit still or
morph on different timers. Something drives `w`:
- the slideshow clock, shaped by `Transition.Function` (default `1-(1-t)^1.5`), or
- a manual crossfader per layer (hotkey or MIDI), with the weight set directly. The same machinery handles both.

**Interruptions:** if a new change arrives mid-morph, G-Force starts a new transition rather than jumping. We treat
the in-progress blend as the new `from`:
- FlowField and ColorMap: bake the blend (one GPU blit, or a 256-entry lerp).
- WaveShape: the shapes are live and audio-reactive, so they can't be baked. Keep the composite (A⊕B at the frozen w)
  as the `from` source. Depth is capped at 2: if a third change arrives, the oldest morph snaps to its target.

### FlowField: interpolate the warp, not the image
- Both fields are precomputed source-coordinate textures. The incoming field must be **built before** its
  transition starts: the scheduler preloads the next field on a worker thread.
- The flow pass blends per pixel: `src = interp(fieldA(p), fieldB(p), w(p))`. Blending the *mapping* (rather than
  crossfading two images) gives a feedback trail that warps continuously from one motion into the other.
- **Linear interpolation of source points fails for rotations.** Two fields that rotate by different angles blend
  into a contraction toward the center. For example, +90° and −90° lerped at w=0.5 collapse everything to the origin.
  So we store each field as a **complex ratio** `m(p) = src(p)/p` (scale and rotation about the center) and
  interpolate `log|m|` linearly and `arg m` along the shortest arc. Rotations and zooms then blend as rotations and
  zooms. We fall back to linear blending near the origin and where `|m|` is degenerate.
  Mode `FlowField.Interp = linear | polar | auto`. `auto` picks per pixel, based on how far the two angles differ.
- `w(p)` is the global weight, optionally shaped by a **FlowField Union**:
  `w(p) = clamp(ALFA(p) · ramp(w))`. ALFA is precomputed into a mask texture, so a Union is a spatially varying
  transition (e.g. a disk of the new field growing outward).
- Kaleidoscope/wrap fields have seams. Interpolating near a seam can tear; that's acceptable and matches G-Force.

### WaveShape: point correspondence morph
Both configs are evaluated **every frame** during the morph, so both stay audio-reactive. Then:
1. **Flatten** each config into a list of paths: (ID instance × X#/Y# pair), each a polyline with per-point pen/width.
2. **Match paths by index.** Unmatched paths fade their Pen to/from 0 (documented G-Force behavior).
3. **Resample** each matched pair to a common point count `max(Na, Nb)` by parameter `s` (or by arc length, as an
   option), so paths with different `Stps` line up.
4. **Align closed curves.** If both paths are closed loops, choose the cyclic shift (and direction) that minimizes
   the summed squared point distance. This avoids the "twisting" morph. It's an O(N²) search, cheap for N ≤ ~1000;
   cache the shift at morph start and refresh it occasionally.
5. Lerp position, pen and width by `w`. A connected path morphing to dots (ConB 1→0) fades its connecting segments'
   alpha by `1−w`.
6. G-Force offsets shapes that pass through (0,0) during "dilation" transitions. Probe this (see §7) before copying it.

### ColorMap: interpolate in a perceptual space
- Both palettes stay live (each re-evaluates on its CCal interval). The blended 256-entry palette is computed on the
  CPU each time either source changes.
- An RGB lerp between saturated palettes passes through muddy grays. Instead, lerp each entry in **OKLab**
  (perceptually uniform, keeps vividness). For two HSV maps there's an optional **hue-shortest-arc** mode.
  Mode `ColorMap.Interp = oklab | rgb | hsv`.
- Index 0 (background) is lerped like any other entry, so the background color glides too.

### Particles and sprites
These don't morph: they fade in and out through the `FADER` envelope (0→1→0 over the lifetime), applied to Pen.

### Blend modes are pluggable; `gforce` is the default
Each layer's interpolation is a named strategy in a registry. It can be switched at runtime (web UI, prefs) and
takes effect on the next transition.

| Layer | `gforce` (default) | Alternatives |
|---|---|---|
| FlowField | Our best match to G-Force, calibrated by probes (presumably a linear lerp of the source coordinates; to confirm) | `polar` (complex log/arc), `auto` (per-pixel choice), `crossfade` (image dissolve, for comparison) |
| WaveShape | Index-matched paths, resample by `s`, unmatched paths fade (documented). Match the details by probing. | `aligned` (cyclic-shift alignment of closed curves), `arclength` resampling, `crossfade` |
| ColorMap | Palette lerp in the space G-Force uses (probe RGB vs HSV) | `oklab`, `hsv-short`, `rgb` |
| Weight curve | `Transition.Function` = `1-(1-t)^1.5` | any expression in `t` (`smoothstep`, linear, …) |

The probes (§7) define `gforce`. Each alternative is just another registry entry, so adding experiments is cheap.

## 5b. Control: built-in web UI

The visualizer runs an embedded HTTP server. Open `http://localhost:8411` (or use a phone or tablet on the LAN, if
enabled).

**Server:** [cpp-httplib](https://github.com/yhirose/cpp-httplib) (header-only, MIT) on its own thread.
- `GET /api/library`: every config by type, with category and Theme membership. Built once at startup.
- `GET /api/state`: current/next config per layer, transition progress, slideshow settings, blend modes, fps.
- `GET /api/events`: **Server-Sent Events** stream of state changes. One-way push is all we need; that avoids a
  WebSocket dependency.
- `POST /api/command`: e.g. `{"layer":"flowfield","action":"switch","config":"HAL/Crop Spiral"}`. Commands go onto
  the render thread's command queue (the same path hotkeys use). The HTTP thread never touches engine state.

**UI:** plain HTML/CSS/JS with no build step, compiled into the binary as embedded resources. Three columns,
FlowField / WaveShape / ColorMap (plus Particles/Sprites later). Each column has:
- current → next, and a progress bar during a transition;
- **Switch now**: pick from a searchable list, filtered by category or Theme. It uses the quick transition duration;
- **Hold**: pause the slideshow for this layer only. Also **Next** / **Prev**;
- slideshow interval and transition duration sliders, and a blend mode dropdown (`gforce` / alternatives);
- a manual crossfader (optional; it takes over `w` while held).

Global controls: Theme select, slideshow on/off for all layers, deterministic seed, and a stats panel.
**Look card** (engine/Look.hpp): live A/B of the rendering-quality choices (ThinLines, Intensity, FlowRounding,
FlowPrecision, AA). `look:*` commands go through the same queue; the Visualizer keeps a `LookControl` (current,
saved, two A/B slots) and applies changes to the ScenePipeline at the next step. An Intensity switch allocates the
new ping-pong + displayed-frame textures once and the next encode copies the trail into them (`copy_fs`); the other
format's pipelines are built on a worker at startup. "Save" is handled on the HTTP thread (settings.json via
`mergeSettingsFile`, never on the render thread), which then queues `look:saved:K=V`.
Later: config thumbnails (ColorMap strips are easy; FlowFields/WaveShapes rendered offline by our engine) and a
low-res MJPEG preview.

**Security:** binds to `127.0.0.1` by default. LAN access is opt-in (`--ui-lan`) and requires a random token in the
URL, which is shown in the window title or log.

## 5c. Video (Phase 5F)

One `video::VideoSource` abstraction (`src/video/VideoSource.hpp`, implemented in `VideoSourceMac.mm`) for three
kinds of source, used by two consumers:

```
 file    AVAssetReader (VideoToolbox hw decode: H.264/HEVC/ProRes) ─┐  decode thread per source
 camera  AVCaptureSession (BGRA, delegate queue)                    ├─ IOSurface CVPixelBuffer ─ CVMetalTextureCache
 syphon  SyphonMetalClient (newFrameHandler)                        ┘  → BGRA8 MTLTexture (zero copy) = video::Frame
                         │
   render thread: frameAt(source time) picks a frame (never decodes; try_lock, keeps the last one on contention)
        ├─ (a) movie / live sprite: sprite pass samples the frame, luminance in the shader (Rec. 601, like
        │      toLumaAlpha; `~`/Invt/Flag 16 invert there) → intensity buffer → ColorMap
        └─ (b) color layer: colorize_video_fs = colorize + fit + key + crossfade A→B + blend over/under,
               one pass into the output texture (window + Syphon)
```

- **Files are sampled by simulation time**, not the wall clock: media time `m = offset + age · rate`, modulo the
  duration when looping; the frame shown is the last one with presentation time ≤ m. That is why files use
  AVAssetReader rather than AVPlayer + AVPlayerItemVideoOutput (which run on the host clock): the same mapping
  works for live playback, `rate`/`offset`, and `--deterministic` runs, where `frameAt` waits for the exact frame
  (`reforce_video_tests` checks frame N at t = N/fps for H.264, HEVC and ProRes, loops, offset and rate). Audio
  tracks are never read. Live playback keeps 3 decoded frames ahead and skips frames it can no longer show in
  time. Seeking (start offset) uses the reader's time range, so the first frame is exact.
- **Preroll**: opening never blocks (asset loading, camera permission, Syphon server lookup all run on the
  source's threads). A sprite or the layer adopts a source when its first frame is in (`ready()`); the slideshow
  opens the next movie sprite ahead of time (like image sprite prefetch).
- **Lifetime of frames**: a `FramePtr` holds the CVMetalTexture and CVPixelBuffer; the Visualizer attaches the
  frames used by a command buffer to its completion handler, so the decoder's IOSurface pool cannot reuse a
  surface the GPU still reads. Nothing Metal is allocated on the render thread for video (the per-frame textures
  are CoreVideo wrappers made on the decode thread); `perf::renderAllocs()` stays 0 under `--stress` with videos.
  The only exception is the window composite texture created once when the Syphon output is switched to the
  plain visualizer (`videoset:syphon:off`), counted as one allocation.
- **Syphon output vs window**: by default both get the composite. With `syphon off` the output texture is the
  plain visualizer (published to Syphon) and a second colorize pass writes the composite into a window-only copy.
- **Self-subscription guard**: `video::setOwnSyphonServer` records our server's UUID; a Syphon input is refused
  when the server has that UUID or our app name and server name (`isSelfSyphonServer`, unit-tested), unless
  `reforce.Syphon.AllowFeedback`. The source picker marks such servers and the Video menu hides them.
- **Composite math** lives in `engine/VideoComposite.*` (CPU reference, unit-tested); the shader mirrors it and
  `reforce_video_render` checks the GPU output against it (chroma key, crossfade, screen at 50 %).
- **Perf**: tags `video-start` / `video-stop` in the hitch detector; `--stress` starts/stops/crossfades the layer
  and movie sprites from the video library. Measured (M4 Pro): decode 0.9–1.0 ms/frame at 1080p and 2.9–3.4 ms
  at 2160p on the decode thread (≥ 10× real time for 4K H.264/HEVC/ProRes); the colorize+video pass adds about
  0.1 ms GPU at a 1080p render with a 4K source (0.3 ms at a 4K render); 60 s `--stress` with videos at 1080p:
  0 hitches, p99 work 7.7 ms, 0 render-thread allocations.

## 6. Threads

| Thread | Work | Shares via |
|---|---|---|
| Main (Cocoa) | events, window, menus, hotkeys | posts commands to the render thread's queue |
| Render (`reforce.render`) | its own run loop hosting the display link and the fallback timer; scheduler, CPU evaluation, GPU encoding, Syphon | owns all engine state |
| Present | acquires window drawables (may block while occluded), scales the latest finished frame into them | 4 private frame copies, handed over by command-buffer completion |
| Audio (CoreAudio RT) | copy samples, nothing else | SPSC lock-free ring buffer |
| Audio control | live-device open/close (Audio menu, mic permission), `service()` (reconnects, format restarts) | source pointer swapped atomically; the render thread never waits for CoreAudio |
| HTTP (web UI) | serve UI, SSE state stream, parse commands | command queue in; state snapshot out (published ~20 Hz, swapped in without waiting) |
| Workers (1–N) | FlowField/Union builds **and their texture uploads**, sprite decode + texture creation | futures handed back to the render thread |
| Eval pool (P-cores − 2, user-interactive QoS) | WaveShape/particle step ranges and the aligned-morph shift search, for the render thread, which waits and works along (`scene::WorkerPool`, `--eval-threads`) | per-lane contexts; results written by step index |
| Prepare (2, utility QoS) | parse/compile every catalog config ahead of time, decode .png ColorMaps, sprite params | `ConfigCache` (immutable entries) |
| Logger | writes render-thread log lines (`rlog`) to stderr | preallocated ring; the render thread never does I/O |
| Video (one per source) | file decode (AVAssetReader), camera delivery queue, Syphon client lookup/frames; wraps frames as Metal textures | small frame queue / latest frame under a mutex the render thread only try-locks (§5c) |

Never allocate or lock on the audio thread. Engine state belongs to the render thread alone.

**No hitches (Phase 5D).** The render thread does no file I/O, parsing, compiling, CoreAudio calls or Metal
resource creation in steady state:
- FlowField/Union textures come from pools filled at startup; workers upload into them and the render thread
  swaps handles. A handle returns its texture to the pool, reusable once the GPU has completed every step
  encoded before. An interrupted FlowField transition is baked on the GPU (`bake_fs`, same math as
  `engine::bakeFieldBlend`) before the next flow pass. All pipeline states, the displayed-intensity texture
  and the line buffers are created in the `ScenePipeline` constructor; `perf::renderAllocs()` counts any
  render-thread creation after that (expected: 0).
- Frame pacing: the display link is pinned (`preferredFrameRateRange`) to the simulation rate when the
  screen refreshes at a whole multiple of it (60 Hz sim on a 120 Hz ProMotion screen: 60 callbacks/s), frames
  carry the link's target timestamp, and `FixedStepClock::advanceLocked` snaps each callback to whole callback
  periods, so every callback runs exactly the steps its cadence says (60 on 60: always 1; 60 on 120 unpinned:
  0,1,0,1). Catch-up only follows a real stall. The fallback timer (window occluded/minimized, display asleep)
  fires at the simulation rate and only after the link has been quiet for 250 ms.
- Instrumentation: `perf::FrameStats` keeps per-frame CPU split (sim, colorize, submit), GPU time
  (`GPUEndTime - GPUStartTime`), display interval and steps; a hitch is a frame over 1.5 x the callback budget
  (CPU, GPU or interval) or a cadence break (a sim frame shown longer/shorter than the cadence, or more steps
  than it allows), logged with the cause tags code set that frame (`perf::tag`). `--stress` switches every
  layer every ~2 s (random blend modes, interrupted transitions, particles, sprites, kills); `--perf-report
  SECONDS` prints p50/p99/max, a histogram and hitch counts by kind and cause. The web UI shows p99, the hitch
  count and a compact histogram (`perf` in `/api/state`).
- WaveShape evaluation (Phase 5D follow-up): `WaveShapeInstance` evaluates the `s` steps in parallel on the
  Visualizer's `scene::WorkerPool` (persistent threads, no per-frame allocation in steady state). Steps only
  share state through the per-step C history (`stepStore_[C][step]`, one task runs a step range for every ID
  in order, so step k's chain across IDs is unchanged) and through key slots; a config stays serial when its
  step tier uses the RNG or `print`, or reads a step-tier key before the step writes it (it would see the
  previous step). B runs serially for every ID first (one dispatch per frame) unless B reads C or a step-tier
  key (then one dispatch per ID). Lanes own deep copies of their matrices (shared `shared_ptr` refcounts made
  matrix-heavy shapes slower in parallel than serially). Independently, per-step keys whose program is
  elementwise are **lane-sliced**: a B row over `s` (`B6 * cos(...)`, `row(B6, 0)`) is read at column `step`
  instead of computing the whole row every step and extracting one element (O(N²) → O(N) per ID). Both are
  bit-identical to the serial whole-row evaluation (unit tests + every corpus WaveShape). The aligned morph's
  shift search (every 30 frames, O(n²) per closed loop) runs as candidate chunks on the same pool.
  Cost does not depend on the render size (only on IDs × steps); the 4K hitches came from frames with several
  heavy shapes (a morph keeps both, an interrupted morph three) plus a shift-search frame.

## 7. Resolving undocumented behavior: black-box probes

We use our licensed copy of G-Force **only as a black box**. We run it normally with test configs we write ourselves
and watch the output. No decompiling or disassembling (prohibited by the EULA §2). Method:
- Write probe configs under `probes/` (our own content, committable). Examples:
  - a FlowField of the identity map → measure the per-frame decay;
  - a WaveShape whose Y0 is `-1^2` → read off operator precedence from where the line lands;
  - `Stps=2` with dots → see whether `s` hits 1.0;
  - a ColorMap with `S=2` → see whether values clamp;
  - B/C vars with no initializer → find their starting values.
- Install probes into G-Force's user config folder, show them, and capture frames (G-Force's own output, or via
  OBS/screen capture) for personal comparison only. Captures stay out of git.
- Record each answer in scene-format.md §10 with the probe that settled it.

## 8. Testing

- **Unit:** lexer/parser/VM, each builtin, tier ordering and persistence rules.
- **Corpus:** every file under `scenes/` loads with 0 errors (warnings allowed and counted). Skipped in CI when
  `scenes/` is absent.
- **Golden images:** `--deterministic --audio-file X --render-frames N` writes PNGs for each reference scene
  (docs/reference-scenes.md). These are compared against stored hashes or images of our own output to catch
  regressions. Comparisons with G-Force output stay manual and local.
- **Performance budget:** at 1080p60, under 2 ms GPU and under 3 ms CPU per frame on the render thread, with FlowField
  builds off the render thread.

## 9. Milestones

| M | Deliverable | Proves |
|---|---|---|
| M1 | Window + fullscreen + runtime-compiled test shader + Syphon visible in OBS | output path |
| M2 | Parser + VM; 100% corpus loads; unit tests | language |
| M3 | Static FlowField + ColorMap with a seeded noise image, no audio | flow/decay/colorize |
| M4 | Audio capture + analyzer; WaveShapes drawn | audio-reactive core |
| M5 | Scheduler, slideshow, per-layer transitions with pluggable blend modes (`gforce` + alternatives), Unions | G-Force feel |
| M5b | Web control UI: per-layer switch/hold/next, slideshow and transition settings, blend mode select | control |
| M6 | Particles, Sprites, Themes/Categories | completeness |
| M7 | Probe-driven calibration against G-Force, prefs, hotkeys, performance pass | fidelity |

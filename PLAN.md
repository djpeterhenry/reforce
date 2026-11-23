# reforce — Plan & Progress

A from-scratch, real-time music visualizer for macOS that parses and runs G-Force's scene
`.txt` files (WaveShapes, FlowFields, ColorMaps, Particles, Sprites). Output goes to a window or full screen,
plus Syphon for OBS.

**Status legend:** `[ ]` todo · `[~]` in progress · `[x]` done · `[-]` dropped

---

## ⏳ Waiting on Peter
1. Probe runs are fully automatic (sudo setup done for 5 package folders, Accessibility granted) but need the screen
   **unlocked**.
2. By hand: OBS *window* capture of a covered reforce window; Cmd+Ctrl+F.
3. Audio default is now effectively settled: settings.json has `Audio.InputSource = "DDJ-1000 Loopback"`.
4. Movie review: first pass done 2026-11-12 (13 Keep, copied as APFS clones into `~/reforce/movies`). Iterate any time:
   tag in `python3 tools/media_review/review.py`, then `review.py collect --copy` (adds) / `--prune` (removes copies no
   longer tagged Keep), or delete copies by hand and re-tag the original.
5. Done 2026-11-12: 5G CPU comparison (docs/perf-comparison.md). G-Force uses ~1.2 cores, reforce ~0.13–0.54
   (typically ~0.2). Optional extras (power via sudo `powermetrics`, full screen, fixed scenes) are listed in 5G.
6. **5H.4 Link check with rekordbox:** a short session with rekordbox playing in Performance mode with LINK on, to confirm
   the bar line matches its downbeat and to set `reforce.Link.OffsetMs`.
7. **5B.10 sprite layering**: built. Look card "Sprites and the flow": Overlay (G-Force, built-in default) / Melt
   (reforce's old look) / Overlay + trails. Pick yours and "Save as default".
   Also **`reforce.SpriteAlpha`** (settings.json, #42): G-Force ignores a PNG's alpha channel (the colour under
   transparent pixels shows, often junk); reforce's default `black` treats transparent pixels as black (not drawn),
   `ignore` copies G-Force. **Decided 2026-11-13 (Peter): keep `black` as the default.**
8. **5K sprite visibility**: try the Mix sliders (Image / Movie sprites cards, keys `[` `]`), Full colour (Movie
   sprites card) and movie levels (review.py Black / White / Gamma), then pick defaults for settings.json.
9. Open question: is the Python scripting layer (`_package.py`, `Scripts/`) in scope? (Assumed: no.)
10. (Install: Claude now runs `scripts/install-app.sh` after each finished item; Peter 2026-11-15.)
11. **5J beat sync checks:** with rekordbox (Link on, or audio-only with Link off) play luma_2 / silhouette / a VJ
    loop with Beat sync on; look at bird (~88) and fry_and_laurie in the beat panel; check the dance_beats
    previews by eye; choose shadow_dance's tempo (one section or a tempo map).
12. **5L solids:** Peter liked the Solids theme (2026-11-15). Try the control panel's Solids card (5L.5): Interval,
    Duration, Start next / Clear, library and favourites.
    5L.6: start **Rubik Cube** with the beat clock locked: does it land solved on the 8-bar lines, and do the six
    colours read well across ColorMaps? Check it stays solid over the camera backdrop.
13. **5N organics:** try the Organics card (or the Organics theme) and the seven trees; say which look right and which
    to tune (growth speed, thickness, sway, buds).
14. **5O rhythm** (built 2026-11-17 while Peter was away; start with docs/prototypes.md): the Rhythm section (phrase
    strip, Phrase starts now, quantize on phrase, kick zoom, drops) and the new Organics (pendulums, sunflower,
    harmonograph, automata, Game of Life, L-systems, attractor). Defaults decided 2026-11-18 (keep them).
15. **5O.6 / 5O.7** (2026-11-18): Peter: "keep all for now, looks great". Tune individual organics later if wanted.
16. **5O.8 deck starts** (2026-11-18): tried live, "working decently" (after the Link-settle / bar-snap fix). Watch it
    over a full set: say if hot cues, loops or the fader third misbehave.
17. **5P dancers** (built 2026-11-18): try the **Dance Party** theme or the Dancers card (Start next a few times).
    Look at: the characters' looks, whether each style feels like itself, the moves on the beat, the disco floor,
    Moves = canon / mirror / together, and "Lit by visuals". Then pick friends whose faces should go on them (next).

---

## Ground rules
- **Never commit G-Force content.** Under the EULA, SoundSpectrum owns it. A local copy lives in
  `scenes/`, which is git-ignored and populated by `scripts/sync-scenes.sh`. Committed tests use short excerpts or
  scenes we write ourselves. Captures of G-Force output (`probes/captures/`) stay local.
- No decompiling or disassembling G-Force (EULA §2). Undocumented behavior is settled by black-box probes.
- **Peter's normal G-Force use must never be affected by our work.**
  - `probes/run.sh` backs up the prefs dir, installs the probes only for the run, and on exit restores the prefs and
    removes the probes. Between runs only empty user-owned folders and an empty `Key 5.py` remain;
    `sudo probes/install.sh --purge` returns the install to stock.
  - After every G-Force run, `diff -r` the prefs against the original backup
    `~/Library/Preferences/SoundSpectrum.reforce-backup-20261111-123844`. It was identical, Cache included, after
    every run on 2026-11-11.
- **G-Force also remembers its window rect outside the prefs** (found in run 24, 2026-11-11): it reopens at the rect it
  had when full screen was last entered, which `run.sh` does not restore. Probes that resize or full-screen G-Force must
  put the window back to Peter's rect (738,144 990×658 pt) and toggle full screen once before quitting
  (`probes/timelines/run24_restore.py`, `tools/parity/resize_gforce.sh` does this before going full screen).
- **G-Force renders frames above ~5.2 MP at half size** (#38): Retina full screen on this Mac is really a 1728×1117 frame,
  pixel-doubled. Matters for any full-screen comparison (lines, flow speed per pixel, decay).
- **Match G-Force, but don't copy its flaws blindly** (Peter, 2026-11-11). If matching G-Force means something that's
  intuitively worse (lost precision, aliasing, quantisation, frame-rate dependence, measurement artefacts), build the
  G-Force behaviour as a selectable option, keep or propose a "done right" alternative, and let Peter pick the default.
  Rule of thumb: **scene semantics** that scene authors relied on (operators, tiers, timing, what a config means) should
  match G-Force; **rendering quality** (precision, filtering, edge shapes) is a candidate for doing it right. Options live in
  `settings.json` (`reforce.*`) so a Dock launch can use them. See "Fidelity choices" below.
- PLAN.md is the record of truth. Update it whenever a task changes state or a decision is made.
- Subagents get tasks with clear boundaries and don't commit. The orchestrator reviews, verifies and commits.

---

## Phase 0 — Discovery  `[x]`

| What | Path |
|---|---|
| Main install (v5.9.7 Platinum, root-owned) | `/Library/Application Support/SoundSpectrum/G-Force/` |
| Apps | `/Applications/G-Force/` (Standalone, Toolbar, V-Bar) |
| User prefs | `~/Library/Preferences/SoundSpectrum/G-Force/Preferences (Standalone).txt` |
| User backup (2026-10-11) | `~/Library/Application Support/SoundSpectrum/G-Force.bak.2026-10-11__03-12-13/` |

| Type | Count | Role |
|---|---|---|
| WaveShape | 206 | audio-driven lines/dots |
| Particle | 119 | short-lived WaveShapes |
| FlowField | 352 | per-pixel warp of the previous frame |
| FlowField Union | 15 | per-pixel blend mask during FlowField transitions |
| ColorMap | 37 txt + 249 png | intensity → color palette |
| Sprite | 4 txt + 185 images | image overlays |

**Peter's G-Force prefs to copy as our defaults** (from `Preferences (Standalone).txt`):
- Transition durations: FF 10 s, WS 13 s, CM 10 s.
- Transition functions: FF `1-(1-t)^1.35`, WS `1-(1-t)^1.6`, CM `1-(1-t)^1.5`.
- Slideshow intervals: FF 20 s, WS 25 s, CM 15 s, Particle 120 s, Sprite 240 s, Union 210 s.
- `ColorMap.MapFunction = "i^2.1"`.
- Audio: input `"DDJ-1000 Loopback"`, FFT bins 255, `Audio.FFT.Params` envelope settings, PCM smoothing 0.008.
- `WaveShape.AutoLineScale = 1`, `Graphics.TargetFrameRate = 60`.

**G-Force facts from running it** (2026-11-11):
- No Syphon output: our Syphon listing saw no G-Force server.
- Window capture works (`screencapture -l <id>`, helper in scratchpad) now that Screen Recording is granted.
- G-Force reads packages and scripts **only from its install folder** (Python `LoadPackages('/Packages')`).
- Scripts start only from hotkeys (`Key N.py`) or `Run()`. There's no startup hook without modifying G-Force.
- Script API (documented): `StartConfig(type, name, morphSecs)`, `SetFlowField(name, union, morphSecs, delay)`,
  `TakeScreenshot()`, `SetPref`, `SetSlideshowEnabled`.

## Phase 1 — Spec the scene language  `[~]`
- [x] 1.1 Spec → `docs/scene-format.md`. When variables are evaluated: A at load, B per frame (keeps its value
  between frames), C per step, D per pixel. VectorC matrix VM. FlowFields never use `t` or audio (verified), so each
  field is computed once. Open questions are in §10.
- [x] 1.2 Corpus scan → `tools/scan_corpus.py` + `docs/corpus-stats.md`. 733 files, 54 builtins (18 undocumented),
  `{...}` matrices, case-insensitive names, any key readable as a variable, ~280 self/forward references, messy syntax.
- [x] 1.3 Reference scenes → `docs/reference-scenes.md` (5 per type, simple to complex).
- [x] 1.4 Black-box probes → **`docs/probe-results.md`** (runs 1–4 on 2026-11-11, 19 findings). Also measured G-Force's
  default prefs → `docs/gforce-defaults.md`. Peter's prefs differ in only 3 keys. Run 3–4 additions: decay is per *frame*,
  dilation `1+15.5u^4.16`, Union weight = w·ALFA (held after the transition, #45), particle life = 6 s fade-in + PDur + 6 s fade-out.
  Earlier findings: Main ones:
  `-1^2`=1, `^` is left-assoc, `s` inclusive, vars start at 0, **C vars are per-step vectors persisting across frames**,
  decay is linear at 1/255 per frame, out-of-frame → black, clamped colors, **overwrite** line blending, width 3 px per LWdt at 1260 px
  tall, ColorMap morph = RGB lerp, FlowField morph = linear source lerp, WaveShape morph = per-index lerp plus **dilation of
  unmatched paths `1+13u⁴`**. Run 3 still open: decay vs frame rate, dilation details, particles, sprites, Unions.

## Phase 2 — Architecture & stack  `[x]`
- [x] 2.1 Stack: **C++20 + Metal (metal-cpp), macOS only, Syphon for OBS.**
- [x] 2.2 Architecture → `docs/architecture.md`: the CPU bytecode interpreter is the source of truth; FlowFields are
  built once on workers into source-coordinate textures; hand-written GPU passes; per-layer transitions with
  pluggable blend modes (§5a); web control UI (§5b); probes (§7).

## Phase 3 — Core implementation  `[x]`
- [x] **M1** Output path. Committed `0eb502f`. CMake app bundle, Cocoa + CAMetalLayer, runtime-compiled MSL,
  1920×1080 IOSurface render target, letterboxed present, `SyphonMetalServer` "reforce", full-screen toggle,
  fallback timer for minimized or occluded windows. Deps pinned: metal-cpp 26 (sha256), Syphon `f4761677` (its
  Metal shaders compiled at runtime because we have no Xcode), doctest 2.5.3.
  Verified by me: clean build, tests pass, Syphon probe got 122 frames/s at 1920×1080, window draws the test pattern
  when in front.
  Follow-ups:
  - [x] (fixed in M7b: present thread) When the window is occluded, the present step is skipped (to avoid blocking in `nextDrawable`), so an
    occluded window shows black. That matters only for OBS *window* capture; Syphon is unaffected. Fix: move present
    to its own thread that blits the latest frame whenever a drawable is available.
  - [~] Keyboard shortcuts: hotkey unit tests done (M7b); Cmd+Ctrl+F still to check by hand.
  - [x] OBS Syphon Client check: Peter confirmed on 2026-11-11 that it looks great.
- [x] **M2** (committed `451d9f9`). Scene loader, Pratt parser (single operator table, probe-confirmed precedence), constant
  folding, bytecode VM (~38 ns per typical expression), 63 builtins, probe-confirmed tier semantics. Corpus: 733/733 load,
  0 errors, 24 warnings. Open: 7 Roger Bigod FlowFields produce NaN (sqrt of a negative) → treated as out-of-frame;
  some matrix builtins are guesses (marked in code); `NumS` alias missing.
- [x] **M3 + M4b** (committed `7011100`). R8 intensity ping-pong, flow+decay pass, instanced capsule lines (overwrite + AA),
  palette + MapFunction LUT, multithreaded FlowField build (3–5 ms at 1080p), settings.json + one-time importer, scene and
  audio CLI, deterministic offline PNG rendering. Frame cost: 0.2–2 ms CPU and ≤1 ms GPU at 1080p.
  Open: decay rounding vs G-Force integer behavior; a dark bar in Noise Mosaic (check whether it comes from the field);
  faint AA double-blend at segment overlaps.
- [x] **M5** (committed `6be724d`). Fixed 60 Hz sim tick, scheduler (shuffle, deviation, themes, Union cadence, FlowField
  prebuild gating), per-layer transitions + crossfader + interruption, blend registry (gforce/oklab/hsv-short,
  gforce/polar/crossfade, gforce/aligned/crossfade), CommandQueue + EngineState, hotkeys W/C/F/U/H/1-3, **fullscreen is
  now Cmd+Ctrl+F**. Orchestrator fix: only NaN sentinels block FlowField blending. Open: `FlowField Union.Transition.*`
  keys unused (Union uses the FlowField timing); WaveShape morph of heavy pairs reaches 3.7 ms; hotkeys untested by hand.
- [x] **M4a** Audio (committed `3e04f85`): AUHAL capture with device select (sees `DDJ-1000 Loopback`), file source
  (deterministic), vDSP analyzer interpreting G-Force's `Audio.FFT.Params`, BASS/LEVEL/ENERGY, 17 tests pass. Mic
  permission is authorized. Not yet wired into the app. Open: 2048-pt FFT is coarse at the low end; device-change
  handling untested.
- [x] **M5b** (committed `13efe5c`). Web UI at http://127.0.0.1:8411 (`--ui-port`, `--ui-lan` with a token, `--no-ui`):
  per-layer columns, next/prev/hold, crossfader, sliders, blend dropdown, searchable lists, SSE live state, phone layout.
- [x] **M6** (committed `13efe5c`). Particles (probe #19 lifetime, ≤16 alive), sprites (luminance, flags, EPen, bounce,
  `~` invert), themes for all layers, P/S hotkeys. Calibrated after run 5: EPen-only sprites draw at 0.5× (`9120245`).
  Open: decoding on the render thread (10–20 ms per JPEG); kill is instant (no fade); no movie sprites.
- [x] **M7** Calibration + polish. Probe runs 5–6 done (sprites: cover fit, wipe, 0.5× EPen-only, particle-style lifetime; line
  width ≈ round(H/420)). **M7a parity suite done** (`f25f710`): `tools/parity/` + `docs/parity.md` + ctest `reforce_parity`. **112/115
  measurements pass; 17/19 findings fully.** Fixes: area-calibrated line width, no dots off the right edge, FlowField
  switches one step later (frame order). Known gaps: FlowField mid-morph starts ~0.13 s later in G-Force; particle
  fade-out tail is 3 levels off; G-Force actually runs ~55.6 fps at target 60 (trails ~8% longer) while reforce runs at
  exactly 60. Set `Graphics.TargetFrameRate` to 55.6 to emulate.
  - [x] **M7b polish**: present thread (occluded window still presents: 301 drawables in 5 s while hidden; a visual
    capture is pending because the display was asleep), async sprite decode + prefetch, kill/manual fades over .Quick,
    `NumS`, 0 build warnings, hotkey tests, no per-frame allocation in morphs.
    To check by hand: OBS *window* capture of an occluded reforce window; Cmd+Ctrl+F.

## Phase 5: Packaging, fidelity follow-ups, audio, video  `[~]`  (planned 2026-11-11)

### 5A. Packaging: launch from the Dock  `[x]` (committed `00faab6`)
Installed at **/Applications/reforce.app** (`~/Applications` is root-owned; `sudo chown "$USER" ~/Applications` would let
the installer use it). Original swirl icon; stable signing requirement `identifier "com.reforce.app"`. **Peter confirmed (2026-11-11): in the Dock, launches, mic
granted, reacts to DDJ-1000 Loopback audio.**
- [x] 5A.1 `scripts/install-app.sh`: Release build → `/Applications/reforce.app` (planned `~/Applications`, which is root-owned), stable bundle id
  `com.reforce.app` (not a SoundSpectrum id), ad-hoc signing with a stable designated requirement so mic permission
  survives reinstalls, version stamped from git.
- [x] 5A.2 App icon: our own original artwork (generated, e.g. a swirl rendered by reforce itself) → `.icns`.
- [x] 5A.3 Launch from the Dock has no working dir or CLI flags, so: settings key `reforce.ScenesDir` (set by the
  installer to this repo's `scenes/`), logs go to `~/Library/Logs/reforce/`, window size/position/screen and
  full-screen state persist, single instance (a second launch focuses the first), quit cleanly from the Dock.
- [x] 5A.4 Menu bar: Open Control Panel (opens the web UI), Full Screen, Next WaveShape/ColorMap/FlowField, Hold,
  Audio Input submenu (device list), About.
- [x] 5A.5 Dock: **Peter drags it himself** (decided). The installer opens `~/Applications` and doesn't touch Dock prefs.
- [x] 5A.6 Verify: Dock launch → window + audio from `DDJ-1000 Loopback` + Syphon + web UI, with no terminal.

### 5B. Fidelity follow-ups against G-Force
Known gaps from `docs/parity.md`:
- [x] 5B.1 Probed (run 17, frame-by-frame strip, parity #30): **G-Force's FlowField morph is timed in seconds, on time,
  like reforce's**: offline full-curve fit (w = fn((t - d)/D)) gives D = 9.84-10.02 s (reforce 9.96-10.08), delay within
  +-0.05 s of reforce's; frame-counted (600 frames at the true ~55.6 fps = 10.8 s) fits 2x worse. The old "0.11-0.17 s
  late" was the strip decoder: it timed columns via B0 frames, but G-Force's strip scrolls ~0.95 px/frame (#34), so a
  column is ~1.055 frames. Now read from the clock's time band: #30 3/3 (half-way deltas +0.04/-0.03/+0.06 s). No reforce change.
- [x] 5B.2 Probed (run 18, parity #31): same artefact; timed by the time band the fade-out matches #19 (all 12 within
  +-0.11 s, 224 -> 32 in 3.9-4.1 s vs 4.0). No reforce change.
- [x] 5B.3 Default stays **60 Hz** (G-Force's design; Peter decided). Peter's G-Force really runs ~55.6, adjustable via `Graphics.TargetFrameRate`.
Things we haven't checked yet:
- [x] 5B.4 Sweeps run 2026-11-11 18:15–18:35 (602 G-Force shots). Finding #24 (fixed16 resampling) → **FlowFields 337/352 pass,
  ColorMaps 233/250** (`docs/sweep.md`). Offline follow-up (findings #25–#29: `%` = integer remainder, FlowField grid with
  edge pixels on the edges, lines through pixel centers, PNG strips as filtered textures, config-local ColorMap `t`) →
  **FlowFields 348/352, ColorMaps 250/250**. Open: Nebula (fixed in round 3, #36); 3 rnd-load configs can't be
  compared exactly. **Round 2 (offline, done): ColorMaps 250/250, FlowFields 348/352** (findings #25–#29: integer `%`, corner-aligned field grid,
  +0.5 px lines, PNG strip filtering, per-ColorMap `t`). **Round 3 (offline, done): FlowFields 349/352**: Nebula 3.89 → 0.58
  via finding #36 (`Aspc=1` field grid has square pixel steps, long axis ±(long−1)/(short−1)); 110 other fields
  tighter, none worse. Not float32 (now an option, `reforce.FlowPrecision`, default double). Open: 3 rnd-load configs only. **Corpus sweeps** (big coverage win). Run every FlowField (352) on a fixed test grid and every ColorMap
  (37 + PNGs) on a fixed ramp, in G-Force and in reforce, and diff the screenshots. Configs whose A vars use
  `rnd` can't be compared exactly; flag them and compare the rest. This exercises the whole expression engine,
  including the guessed builtins (map1D/map2D, noise, smooth, flip…).
- [x] 5B.5 Confirmed (run 20, parity #33): MapFunction maps the intensity before the ColorMap (palette(f(i))); reforce
  matches within 1 level.
- [ ] 5B.6 Sub-pixel flow: very slow zoom/rotate fields (8-bit "sticking", bilinear vs nearest sampling).
- [x] 5B.7 Sub-1-px lines (run 19, parity #32): G-Force's line rule re-derived offline (probe-results #32: points
  snapped by truncation toward the center, capsule coverage along the major-axis pixel center line, one rule at
  every width) and implemented as `reforce.ThinLines = gforce`; the default `smooth` keeps AA round dots/sub-pixel
  lines with G-Force's energy (Peter picks the default). Parity runs in `gforce` mode: #32 50/50. Also found (#34):
  a pure 1-px translation decays 2 levels/frame in G-Force (corner-aligned pixel mapping, cf. #26). Remaining: line
  cap shape beyond r = .7 (no probe has a thick line end on screen); WaveShape morph when NUM or Stps differ;
  transition interruption; `Aspc=1` axis. (Sprite blend flags: done, #42. Union end: done, #45.)
- [ ] 5B.8 Fold everything that's measurable into `tools/parity` so it stays checked.
- [x] 5B.10 (built 2026-11-12: `reforce.SpriteLayering = gforce | melt | trails`, Look card "Sprites and the flow";
  built-in default gforce; replay of run 26 in reforce matches #40: lighten rule on 77,271/77,271 band pixels, Pen/both
  bands 255/128 and 128/64, identical trails for Pen and Pen+EPen; `trails` = the overlay plus a max feed at
  EPen × `reforce.SpriteTrails` (0.35). Run 27 (#41) settled the two open points: X/Y map to centre = X·max(0, 1 −
  size/frame) (cover sprites never move), EPen 0 draws nothing; also Pen stamps replace at fade × Pen (not blended).
  Runs 28-31 (#42, 2026-11-13): all blend flags measured (keyed copy at ~10 levels, half-way lighten with the envelope
  on the source, additive, AlphaMask = darken, AlphaBlend = screen), alpha ignored, Rec. 601, oldest sprite on top; run
  27's "Pen 0 erases" withdrawn. Implemented for gforce/trails (melt keeps the old guesses); replay of runs 28-31
  matches all 54 shots within 2 levels. **Peter picks his defaults** (layering, and `reforce.SpriteAlpha`).)
  **Sprite layering (probe run 26, #40, 2026-11-12): reforce has Pen/EPen the wrong way round.** G-Force: Pen
  copies non-black pixels into the feedback (flows, trails); EPen is display-only (EPen-only = half-way lighten
  max(F, (S+F)/2) at full envelope, never darkens, no trail; with Pen: EPen copied over the non-black pixels on the
  display). reforce: Pen display-only, EPen copied into the feedback with opaque black, so default sprites melt into
  the flow and wipe the trails under them. Peter likes reforce's look sometimes → plan: G-Force rule as an option
  `reforce.SpriteLayering = gforce | feedback`, live in the Look card; **Peter picks the default**.
- [x] 5B.9 (done: WaveShape t is now config-local in reforce, `Visualizer::WaveNode::t0`; morph sides each keep their own clock) WaveShape `t` is config-local in G-Force (probe #37, runs 17/18: the clock band restarts at 0 with every
  StartConfig, like ColorMaps #29); reforce passes the global clock. Semantic: switch WaveShapes (both sides of a morph)
  to their own start time; check particles/sprites the same way.

### 5C. Audio processing parity
G-Force facts: the scene language exposes `mag()`, `fft()`, `BASS` (documented as "approximates the overall beat"),
plus `LEVEL` and `ENERGY` (undocumented; 5 and 10 files). There's no MID/TREBLE/BEAT in scenes. Internally the engine
also computes `impulseFast`, `impulseSlow`, `energy`, `beat`, `level`, alternative FFTs `fft1`/`fft2`, and
normalised/"persist" FFT stages (SSEngine.py `AudioData`), which matches the `Audio.FFT.Params` names (Persist*,
Short/Long envelopes, Exciter, FloorRaise). Settings: `Audio.Response.Scale`, `FreqLo`/`FreqHi`, `FFT.Smooth`,
`PCM.Smooth`, bins.
- [x] 5C.1–5C.3 Kit done (`b97b8a8`); **G-Force audio runs 7–14 captured 2026-11-11 17:05–17:10** (strip charts decode with
  rails ≤1.5 px; G-Force ran 52–54 fps). 5C.4 analyzer fit (71.6% of measurements, `docs/audio-parity.md`) + 5C.5 MID/TREBLE/BEAT/TEMPO done (`6c24119`).
  Round 2 (2026-11-11 evening, done): runs 15/16 (tones 125 Hz–4 kHz, 20 s tone, falling sweep) + run 23 (two/three-tone
  chords). **Found: G-Force's fft() axis adapts to the content** (upper edge contracts onto the highest tone, τ ≈ 2 s, back
  toward ~11 kHz in silence; lower edge floor ~240 Hz, rises slowly) — explains tone drift, the "3.5× sweep", the dark
  phases and the low high-band LEVEL (docs/audio-probe-results.md §9). Not modelled yet (exploratory fit too poor for single
  tones). **`mag()` = 7-point moving average** → implemented (`pcmBoxcar`). Audio parity 100 % outside gaps (276 checks).
- [x] 5C.1 Controlled audio route (**Loopback**, decided): Peter creates a Loopback virtual device named `reforce probe`
  (default Pass-Thru source, no monitors, so it's silent). Our player tool outputs test WAVs to it, and it is G-Force's input
  only during probe runs (prefs restored).
- [x] 5C.2 Test-signal set (generated WAVs, committed or regenerated): silence, 1 kHz tone level steps
  (−40…0 dBFS), log sine sweep 20 Hz–20 kHz, band-limited noise (low/mid/high), a kick pattern at 120 BPM, and
  an attack/release step.
- [x] 5C.3 **Strip-chart probes**: a FlowField that scrolls left 1 px per frame plus a WaveShape that draws a dot at
  the right edge at y = f(BASS / LEVEL / ENERGY / fft(k) / mag(k)). One screenshot then shows several seconds of a
  signal's time series. Also snapshot probes of the full `fft(s)` and `mag(s)` curves.
- [x] 5C.4 Fit reforce's Analyzer to the measurements: FFT bin mapping and normalisation, attack/release
  envelopes, BASS/LEVEL/ENERGY definitions and ranges, `mag` scaling and smoothing, `Response.Scale`. Add the
  signals to `tools/parity` (reforce reads the same WAVs deterministically).
- [x] 5C.5 reforce extensions (**Peter chose: yes**): `MID`, `TREBLE`, `BEAT` (onset pulse) + tempo estimate as extra
  scene variables and web UI meters. G-Force scenes don't use them, so they don't affect parity.

### 5D. Smoothness: no hitches  (Peter, 2026-11-11)  `[x]` mostly done
Reinstalled to /Applications (`a1379c7`). Results (3-min `--stress`): 1080p 180 → **0** hitches, exact 16.67 ms cadence on 120 Hz (link pinned to 60 Hz);
4K 284 → 4. Remaining: heavy WaveShape/particle evaluation can take 15–32 ms per step at 4K (aligned morphs) → next:
parallel evaluation **done (round 2)**: lane slicing + persistent WorkerPool + parallel morph shift search, bit-identical;
torture run 4K cpu avg 24.4 → 7.3 ms, worst 37 → 13.5 ms (`--eval-threads N`). ~570 MB RSS at 4K (6 pooled RG32F fields). 30-min runs not yet done.
Goal: every displayed frame on time, during switches, transitions, spawns and UI use. Steps:
- [x] 5D.1 **Instrument first**: per-frame CPU and GPU timing with a ring buffer; a hitch detector (any frame > 1.5×
  budget, or a repeated or skipped sim step) that logs the cause tags active that frame (switch, field bake, sprite
  start, compile, alloc…); `--stress` mode that switches all layers every ~2 s and spawns particles/sprites; frame-time
  histogram in the web UI stats panel.
- [~] 5D.2 **Known or likely hitch sources to remove:**
  - Mid-transition FlowField interruption bakes the blended field **on the CPU, on the render thread (~15 ms)** →
    move it to a GPU pass.
  - Scene parse/compile (WaveShape/ColorMap/particle configs) at switch time → do it on workers ahead of time (preload
    the next playlist item, as FlowFields already are).
  - GPU resource creation at switch time (16 MB RG32F field textures, palettes, sprite textures, pipeline states)
    → texture pool plus warm-up of every pipeline at startup.
  - **Frame pacing**: a 60 Hz sim on a 120 Hz (ProMotion) display can show some sim frames once and others three
    times when the accumulator phase drifts. Ask CAMetalDisplayLink for an exact 60 Hz (or a whole multiple) and lock
    the sim step to vsync; also handle variable refresh. Catch-up (up to 4 steps) should never trigger in normal
    running.
  - Logging, settings writes, web UI state serialisation and SSE: off the render thread, with nothing on the hot path
    allocating.
  - Syphon publish and window present must never block the sim (present thread done in M7b; check Syphon too).
  - Audio device changes/reconnects, full-screen Space switches, display changes, App Nap/timer coalescing,
    thermal throttling (watch GPU time at 4K).
- [ ] 5D.3 Acceptance: in a 30-min stress run at 1080p and at 4K, 0 frames over budget outside full-screen
  toggles, and p99 frame time < budget/2.

### 5E. Antialiased lines  (Peter, 2026-11-11)  `[x]` per-path max-coverage AA (`6c24119`)
Status: present since M3, as analytic per-pixel coverage on capsule quads (`WaveShape.AntiAlias = 1`, the G-Force
default), blended as `pen·cov + dst·(1−cov)` so that overwrite semantics hold in the core.
- [x] 5E.1 Quality review: the faint double-blend where segments of one polyline overlap at joints (each segment
  blends separately), thin lines (LWdt·scale < 1 px) going dim or aliased, dot roundness, and edges on the 8-bit
  intensity buffer (quantised AA ramps that then flow and decay → possible banding or crawling).
- [x] 5E.2 (per-path max coverage in tile memory chosen) Options to evaluate: per-polyline coverage (max-coverage within one path before blending, e.g. a
  per-path coverage pass or depth/stencil trick) vs current; minimum-width clamping with alpha compensation; 4×MSAA
  as a comparison baseline. Match G-Force's edge profile (5B.7 measures it).
- [~] 5E.3 (no hitches attributed to the line pass in stress runs; full AA-mode cost matrix not recorded) Performance: measure GPU time of the line pass with AA off / analytic / per-path / MSAA at 1080p and 4K
  on heavy scenes (e.g. 6k+ segments). Budget target: line pass < 0.5 ms at 1080p. Keep the `--no-aa` switch.
  2026-11-11 (`reforce-line-lab bench`, 5 scenes up to 16k segments): smooth (path) 0.016-0.114 ms at 1080p,
  0.033-0.172 ms at 4K (unchanged within noise by the #32 work); gforce 0.015-0.111 / 0.036-0.224 ms.

### 5F. Video files  (Peter, 2026-11-11)  `[x]` all three parts work
Movie sprites, colour layer (over/under, 4 blend modes, keys, crossfade), Syphon + camera inputs (camera **verified by Peter 2026-11-11**). Files decoded with AVAssetReader by sim time (frame-exact in deterministic mode). 1080p stress with
video: 0 hitches. Open: G-Force's own movie-frame mapping not probed (5F.6). Polish (round 2): minified frames (< 0.6×)
sampled via a half-size box filter + mips (render/VideoMips, ~0.25 ms GPU per new 4K frame); camera permission through
CameraGate (async, one shared prompt; waiting / denied / no-usage-key states in the Video card and menu); failed sources
fade out with the error; a Syphon input whose server quits shows nothing and reconnects. Camera still needs Peter's manual
permission test (the installed app predated NSCameraUsageDescription: reinstall first).
G-Force supports **movie sprites**: video files (MOV/MPEG, …) placed in the Sprite package play as sprites, with the
same params/flags as image sprites (scene-format §5.6; G-Force version history: "auto-loads QuickTime if it sees video
files present in the Sprites folder"). reforce currently ignores them (M6 open issue).
- [x] 5F.1 Scope (**Peter: all three**, 2026-11-11):
  (a) G-Force-compatible **movie sprites** (video → luminance → intensity buffer → ColorMap);
  (b) a **full-colour video layer** composited over or under the visualizer (blend modes: normal/add/screen/multiply,
      opacity, fit, a layer order relative to the colorized visualizer, optional luma/chroma key);
  (c) **live sources**: cameras (AVCaptureDevice) and **Syphon input** (SyphonMetalClient). Each can feed either (a) as a
      "live sprite" or (b) as the colour layer.
  Design: one `VideoSource` abstraction (file / camera / Syphon), producing Metal textures on its own thread, used by
  both the sprite path (a) and the colour layer (b). Order: (a) → (b) → (c). Self-capture guard: never subscribe to
  our own "reforce" Syphon server (feedback loop) unless explicitly allowed.
- [x] 5F.2 Decoding: AVFoundation (built with `AVAssetReader` by sim time instead of `AVPlayer`), hardware decode (H.264/HEVC/ProRes),
  frames as IOSurface-backed CVPixelBuffers mapped zero-copy into Metal with `CVMetalTextureCache`. Decode runs off
  the render thread; the sim samples the frame nearest the sim time. Looping, start offset, playback rate, audio
  muted (or optionally routed).
- [x] 5F.3 Sprite integration: a video is a sprite whose texture updates every frame. Same placement/fit (cover),
  flags, fades (lifetime rule), Pen/EPen, `~` invert, slideshow and Theme membership, a sibling `.txt` for params.
  Sprite catalog: add video extensions.
- [x] 5F.4 Controls: a "Video" section in the web UI and menu: source picker (files, cameras, Syphon servers), start/stop,
  loop, layer mode (sprite vs colour layer), blend/opacity/fit, crossfade between sources; video folders outside `scenes/`
  (an external path, like G-Force's `ExternalSprites.Path`). Camera permission prompt (NSCameraUsageDescription).
- [x] 5F.5 Smoothness: preroll before a video starts (no first-frame stall), pooled textures, decode time
  measured in the 5D perf tooling; 4K ProRes/HEVC test files; no hitches in `--stress` with videos.
- [-] 5F.6 **Dropped (Peter, 2026-11-11): G-Force can't play movies on this Mac at all** (it relies on 32-bit Intel
  QuickTime, unsupported here), so movie playback can't be black-box compared. reforce's behaviour stands as designed.
  Earlier notes: probed (runs 21/22): G-Force does **not** play movies from `G-Force.ExternalSprites.Path` (our PNG there works;
  .mov/.mp4 Card test movies never appear). Needs the Sprite package folder: **Peter: `sudo probes/install.sh`** (now also
  creates `G-Force.Sprite.package/zz reforce probes`), then run21 with `PROBE_SPRITE_DIR=build/probe-movies` (probes/README.md).
  reforce today: frame at the sprite's age, looping, movie's own rate, cover fit, luminance like images.

### 5G. CPU/GPU/power comparison: G-Force vs reforce  (Peter, 2026-11-11)  `[x]` first pass done 2026-11-12
**Result (docs/perf-comparison.md):** normal slideshow, windowed 1980×1316 px, same audio, 2×120 s for G-Force and 3×120 s
for reforce. G-Force: 119–123 % CPU, ~2 % GPU, 228 MB. reforce: 13–54 % CPU (typically ~20 %), ~15 % GPU, 240–270 MB,
60 fps (G-Force ~55.6). In short, reforce uses about 1/5 of G-Force's CPU because it does the drawing on the GPU.
Script: `tools/perfcompare/perfcompare.py` (no sudo). The unchecked items below are optional extras, not reminders.
Goal: an overall picture of how much total CPU (and GPU, power, memory) reforce uses compared to G-Force, under the
same conditions. Never run both at once; one app at a time, machine otherwise idle (no agents/builds running).
- [x] 5G.1 (windowed, slideshow, Loopback audio; not full screen, fixed scenes, Syphon or web UI variants) Matched conditions: same window size (e.g. 1920×1080 windowed and full screen on the same display), same
  audio (a fixed test WAV / music file looped through the `reforce probe` Loopback device), same frame rate target (60),
  a fixed set of scenes plus normal slideshow. For G-Force, only via `probes/run.sh` (prefs backed up/restored).
  For reforce, also with Syphon on/off and the web UI open/closed.
- [x] 5G.2 (CPU, RSS, threads, whole-GPU busy %, reforce fps; not power or per-process GPU, which need sudo) Measure per app, sampled over ≥5 min after warm-up: total process CPU % (all threads, `top -l`/`ps` or
  `proc_pid_rusage` user+system time), GPU busy % and per-process GPU time (`powermetrics --samplers gpu_power,tasks`,
  needs sudo, Peter runs it, or Activity Monitor's GPU column), package/CPU/GPU power (mW, `powermetrics`), energy impact,
  RSS/footprint, thread count, and achieved fps / frame-time spread (G-Force: its own fps readout or screenshot timing).
- [ ] 5G.3 Scenarios: idle-ish scene, heavy WaveShape scene, FlowField morph-heavy slideshow, sprites/particles, 4K
  (or full-screen on the largest display); reforce also with `--eval-threads 0` and with video playing.
- [x] 5G.4 Write up `docs/perf-comparison.md`: table per scenario (CPU %, cores used, GPU %, watts, MB), plus where
  reforce's time goes (perf tooling breakdown) and any easy wins. Script it (`tools/perfcompare/`) so it can be rerun.

### 5H. Ableton Link: beat sync with rekordbox  (Peter, 2026-11-12)  `[~]` built; rekordbox check with Peter pending
Peter's decisions (2026-11-12): GPL Link library OK; own `LINK_*` variables **and** `BEAT`/`TEMPO` follow Link while a
session runs (setting); build steps 1–3, scenes later.
- [x] 5H.1 Link 4.1.1 via FetchContent (`cmake/Dependencies.cmake`); `src/sync/LinkClock` (joins, never proposes tempo);
  sampled per sim step at now + `reforce.Link.OffsetMs`; web UI header LINK badge (BPM, peers, bar dots);
  `reforce.Link.Enabled`, `--no-link`; `NSLocalNetworkUsageDescription`; `reforce-link-peer` test tool.
- [x] 5H.2 Scene variables `LINK_ON`, `LINK_BPM`, `LINK_BEAT`, `LINK_PHASE` (scene-format §3.5); `BEAT`/`TEMPO` follow Link
  (`reforce.Link.DriveBeat`, default 1).
- [x] 5H.3 Beat-aligned switching: slideshow changes on the next bar (`reforce.Link.Quantize` bar|beat|off), manual
  switches on the next beat (`reforce.Link.QuantizeManual`), transitions in beats (`reforce.Link.TransitionBeats`, 0 = off).
  Verified with a 128 BPM test peer: every slideshow change at a multiple of 4 beats, 4-beat transitions = 1.88 s.
- [ ] 5H.4 **With Peter, rekordbox playing (Performance mode, LINK on):** does rekordbox's bar line (LINK_PHASE 0) land on
  its downbeat? Tune `reforce.Link.OffsetMs` by eye (beat flash vs kick) on the window and on the projector/OBS path.
- [ ] 5H.5 (later, Peter's ideas) Beat-aligned scenes using `LINK_*`; maybe slideshow intervals in bars, particle/sprite
  spawns on beats, a Link-only "pulse" layer.
- [x] 5H.6 Follow mode (`3e30b26`, 2026-11-13): Peter saw rekordbox pulled to reforce's 120 BPM. In Link the
  longer-running session wins, and reforce enabled Link at launch. `reforce.Link.Follow` (default 1) keeps Link off and
  listens for discovery announcements, joins 1.5 s after another app appears, leaves when the last peer goes. Panel
  badge: listening / joining / BPM · peers. Not yet checked live with rekordbox.
- [ ] 5H.7 (later, Peter 2026-11-13: "come back to at some point") Tempo control from the DDJ-1000 while Link is on.
  rekordbox locks the TEMPO slider to the Link BPM when a controller is connected; the BPM lives on the Link subscreen
  (right-click the global [LINK]). Ideas, cheapest first: (a) rekordbox MIDI Learn on the Link subscreen BPM +/-;
  (b) reforce reads the DDJ-1000 tempo faders over CoreMIDI (shared input, read-only) and proposes Link tempo
  (relative mapping with pick-up, since the faders are absolute and reforce doesn't know the track BPM; one session
  tempo for two faders). Off by default. First step: read-only MIDI monitor to see what the DDJ-1000 sends while
  rekordbox has it. Open question: whether Link fits Peter's tempo-riding style at all, vs LINK off and audio TEMPO.

### 5I. Spawn timing in the control panel  (Peter, 2026-11-13)  `[x]`
- [x] Particles / Sprites cards: Interval (0 = off) and Duration sliders; Sprites also a Movie interval. Movie sprites
  now have their own shuffle and timer (`reforce.MovieSprite.Slideshow.Interval.Duration`, default = the sprite
  interval, so with movies in the sprite folder there are about twice as many sprites until it is set).
- [x] Every panel slideshow slider (all Interval / Transition / Duration) is saved to settings.json on release.
- [x] Separate Image sprites and Movie sprites cards (each: interval, start next, clear, own library; Loop until
  stopped moved from the Video card to Movie sprites). `reforce.SpriteReplace` = own (default, Peter) | any (G-Force).
- [x] Main cards' libraries collapsible and collapsed at load, like the extra cards (Peter).
- [x] Union card Clear (`kill:union`): cancels a pending Union; a Union shaping the field (mid-transition or held,
  #45) morphs to the plain FlowField over the quick duration. Tested live in all three states.

### 5J. Dance video beat sync  (Peter, 2026-11-14)  `[~]`
Play dance clips locked to the music: find each clip's BPM and beat grid from the dancers' motion (clips are silent),
RIFE-interpolate them for smooth slow-down, and drive the playhead from a beat clock (Link when present, else the
audio tracker with a new phase estimate). Peter may not DJ with Link on, so audio must work on its own.
- [~] 5J.1 `tools/dance_sync/dance_beats.py`: frame timing clean-up (skip repeated frames, even re-spacing,
  2-2-1 style cadence correction), then two motion signals at 50 Hz: body bounce (vertical motion in
  upper/hips/legs bands, hips+legs weighted) and directogram deceleration (hits). Tempo from a whole-clip
  tempogram refined to 0.002 BPM; phase from the peaks. `--preview` renders the clip with flashing beats, a signal
  strip and a click track; `--write` saves `BPM` / `BeatStart` / `BeatsPerBar` to `<stem>.reforce.txt`.
  `selftest.py` (synthetic dancers, incl. 25->15->23.976 bouncing silhouettes) passes. luma 1/2/5 ~90 BPM,
  luma 3/4 ~113 BPM (they were 150/100 before: duplicate-frame artefacts). Peter to check the previews by eye.
  Later: pose keypoints (Apple Vision) for non-silhouette clips.
- [x] 5J.6 Loops and the colour layer (Peter, 2026-11-14):
  - beat-synced movies start from a downbeat (the first at or after Start) and the section is any whole number of
    bars (`sectionEnd` honours `loopUnit`, like the decoder)
  - play-once movies last as long as they take at the synced rate
  - the colour video layer is beat-synced too
  - the setting is renamed `reforce.Video.BeatSync`
  - not done (Peter): best-loop-point search, seam crossfade.
- [x] 5J.5 Survey of the other dance clips (2026-11-14) and three detector fixes:
  - auto keeps the more confident of the bounce-led and hits fits
  - dark-on-light silhouettes are recognised
  - timing cleanup works locally between cuts and holds, with cadence patterns up to 8 frames that must hold steady
    across the clip.
  Results:
  - silhouette **100.0** BPM (confidence 0.82); static **124.9**; both interpolated and written.
  - shadow_dance's 25 -> 29.97 conversion is now undone (interpolated). It is a medley: its sections run at
    129.6 / 129.9 / 126.6 / 127.1 / 134.1, so no single BPM yet (Peter to choose a tempo map or one section).
  - Left out (Peter): reservoir_dance, thought_of_you, silhouette_pole.
  - Poor fits: army_men (stop-motion), crystal_fighters / disney / pole_diplo (montages), samsara, oceans_twelve,
    slowmo_gelatin, time_warp_fuel_girls; slow_and_motion has speed ramps.
  - Need a check by eye: bird (~88), fry_and_laurie (9.5 s; set BPM by hand).
  - Cut timing is not beat-locked in any montage (tested against shuffled cuts), so cuts are not used as beats.
- [x] 5J.7 Other beat-syncable clips: VJ loops (2026-11-14).
  - Seamless loops get their tempo from the loop length, the tempo in the file name is used, and interpolation fills
    the loop's seam. reforce no longer cuts a nearly whole bar off a loop.
  - Found a RIFE frame-spacing error: output i sits at i * inputs / count, not i / factor. Now it asks for
    factor * inputs frames. The 8 interpolated files were redone and the beat values re-saved (copy + original).
  - luma_5: auto picked hits (105.2, confidence 0.56) over bounce. Kept bounce, 90.35, which matches every earlier
    measurement; auto can still pick a stray hits fit.
  - Kept and collected (Peter):
    - the 7 locked VJ loops (Loop = 1)
    - weapon_of_choice at 99.4 (Peter: close to 100)
  - Candidates:
    - locked loops: ignite_running_man 96, beeple harvest 128.1 / popbottles 110.3 / space_crystals 150 /
      purple_cycle 120.1 / signal_barrel 102.9, bg_spinning_red_earth 120
    - also: weapon_of_choice 149.1 or 99.4 (Walken), bg_matrix 99.6, bg_numbers_flicker 152.5
- [ ] 5J.8 Open items (2026-11-15):
  - shadow_dance tempo map (sections 129.6 / 129.9 / 126.6 / 127.1 / 134.1) or one section; waits on Peter.
  - [x] auto signal choice picked a stray hits fit over bounce (luma_5: 105.2 vs the right 90.35). A plain margin
    can't fix it: luma_5's wrong hits fit leads by 0.10, static's right one by 0.085 (red_earth 0.08, signal_barrel
    0.02). Fixed by clip type: on a silhouette (bounce measured on the real figure) bounce stays unless hits leads by
    0.2 (silhouette's real hits fit leads by 0.39); other clips unchanged. All 15 written clips re-checked: auto
    now matches every written value except weapon_of_choice (149.1 in its range; written 99.4 by hand, as before).
  - later: pose keypoints (Apple Vision) for non-silhouette clips; best-loop-point search and seam crossfade
    (Peter: not now).
- [ ] 5J.9 More content (research 2026-11-15, links unverified, nothing downloaded). No source sells full-body
  dancer silhouettes that are seamless and BPM-labelled, so packs go through dance_sync / the beat panel.
  - Visual 3xplosiv3 (sites.google.com/view/visual3xplosiv3): 300+ free loops incl. 4K silhouettes; licence allows
    live VJ / streaming. **First pick.**
  - Gumroad loops with a stated tempo (length-lock syncs them): rtificial "Endorphin" (69 loops, 130 BPM, EUR 4;
    first pick), DizArt "Pulse Geometry" (122 BPM), Jiagual Essentials (120 BPM, alpha), NonVisualCult free packs.
  - Silhouette dancers (no BPM): AEJuice "Shadow Dancers" (69, 4K, mattes), Motion Array, VideoHive "Dancer Pack".
  - Resolume Footage (clearest live licence): WhiteDancer, MoCapMayhem.
  - Beeple resources page: C4D projects, some BPM-named (render needed).
  - Reddit: r/vjing (Gumroad roundups; freestockfootagearchive.com, Prelinger archive), r/VJloops (sellers).
    Next: search r/vjing for "silhouette" / "dancer".
  - Licence watch-outs: Envato / VideoHive are per project; Pexels / Pixabay / Mixkit are not seamless or clearly
    OK for venues.
- [x] 5J.2 Beat panel in tools/media_review (button or key b; `?beats=ID` opens one):
  - plays with a beat flash, bar dots and a click scheduled on the video's clock (also at 1/2 and 3/4 speed)
  - a +-3 s strip of the bounce/hits signals with the grid
  - BPM field, x2 / /2, nudge (+-10 ms, shift = 1 frame), "This beat is 1" (key d), Tap (key t)
  - Analyze (dance_beats.py, on the interpolated copy when there is one; alternatives clickable)
  - Save / Clear write the original's and the ~/reforce/movies copy's .reforce.txt
  - Tested: API round trip on a scratch clone, headless Chrome render with no script errors.
- [x] 5J.3 `tools/dance_sync/interpolate.py`: restore real timing (drop repeats; RIFE recreates the frames a 2-2-1
  cadence dropped), then RIFE x4 -> `<stem>.rife4x.mov` beside the original (HEVC 10-bit q85, same start and length).
  All five luma clips done (25 -> 100 fps); their BPM/BeatStart re-analysed on the smooth files and written to
  the originals' and the copies' `.reforce.txt`.
- [x] 5J.4 `sync/BeatClock` (Link > audio phase loop: strength-weighted onset phase average, confident only when
  onsets agree) and `engine/MovieBeatSync` (playhead from beat deltas, octave 1/2,1,2 with hysteresis, 0.5-2x,
  slewed phase, start in phase, bar-length loops via `SourceSpec::loopUnit`). Interpolated copies play in place of
  the original and are hidden from lists. Panel: Beat sync toggle + clock readout. Live test with a 100 BPM kick
  file: audio clock locked (0.9998), luma_2 at 1.110x, phase 0.000. Peter: check with rekordbox Link
  (my test peer can't reach the network from this shell), and by eye.
- [x] 5J.10 Audio tempo for house (Peter, 2026-11-15: "detected at half speed, 62"; "jumping around during a
  breakdown"):
  - TEMPO folded into 80-160 BPM (BeatConfig::foldMinBpm), octave hysteresis at the edges.
  - Steady tempo (`reforce.BeatClock.SteadyTempo`, default on). BeatTracker: tracks the current tempo's own peak
    (+-4 %), a challenger needs ~8 s of confidence-weighted wins, holds 60 s without a reading, 0 after 3 s of
    silence. BeatClock: stays locked while the tempo is held; while locked only onsets within 1/4 beat correct the
    phase, slew slowed by the kick share of the evidence (BeatTracker::lastOnsetLowShare, band-total flux; kick
    ~0.4, stabs 0.05-0.3); 8 kick-like onsets off the grid in a row re-acquire.
  - Tests (synthetic 124 BPM house: kick, 32 s breakdown of random stabs + noise, kick): old tracker jumped to 82 / 99
    and lost the lock; steady holds 123.8-124.0, clock within 0.024 beat through the breakdown and after the drop;
    124 -> 128 in 5.75 s, 124 -> 100 in 12 s (none before); shifted-grid kick re-acquired (0.004 beat).
  - Peter: try in a set with Link off; if real kicks read below kickShare (0.35), the lock falls back to the
    slower all-onset correction.
  - Peter (same day): "still didn't keep the correct tempo through a longer breakdown". His log showed 83 for 125
    (2/3: a dotted-eighth riff). Reproduced (90 s dotted arpeggio breakdown: 125 held 25 s, then 83). Fix: kick
    gate (30-120 Hz onset level vs its recent peak, BeatTracker::kickPresent): no kick = tempo frozen, clock
    freewheels; the clock learns a tempo trim from kicks near the beat only (+-0.5 %). Now: 125 throughout, clock
    within 0.002 beat through 90 s, after a 30 s breakdown a new 100 BPM track still read in 8.75 s.
  - Log: "audio tempo A -> B BPM" on changes > 3 %, and a "beat:" line every 5 s (clock, lock, kick, TEMPO,
    offset), to check Peter's next set.
- [~] 5J.12 Beat detection scored against Peter's DJ library (Peter, 2026-11-15). **Details, how to measure,
  results, open issues and the round-by-round log: [docs/beat-detection.md](docs/beat-detection.md).** Done: eval
  tool on the library's warp-marker grids (read-only) + simulated DJ mixes; learnt on-beat model, comb tempo search,
  beat fold, phase filter. Trusted grids, recent 2,000: TEMPO 96.5 %, on the beat 92.8 % (from 77.5 / 42.2 %
  held-out at the start). Next: **quick 1-3 BPM tempo nudges** (Peter does these often): 74.9 -> 77.6 % on the beat
  after the kick-grid tempo (2026-11-16); with the true tempo it would be 96 %, so faster tempo tracking is the lever
  (docs §5.1). Then the remaining 2/3 locks (~1 %).
- [x] 5J.11 Beat offset nudge (`reforce.BeatClock.OffsetMs`, top bar "Beat ‹ ›", live, saved; Link sampled ahead,
  audio count shifted) and the `(Beat Clock)` debug solid (never in a slideshow) to set it by eye.


### 5K. Sprite visibility  (Peter, 2026-11-15)  `[~]` built; Peter to try
Sprites are hard to see: a default (EPen-only) sprite is only a half-way lighten, F + 0.5·max(0, x − F), which the
ColorMap's MapFunction (default `i^2.1`) pushes into the dark end of the palette; keyed copies drop pixels under
10/255; movies get no levels. All new settings default to G-Force (render tests unchanged).
- [x] 5K.1 **Sprite Mix**, one per sprite card: `reforce.Sprite.Mix` (images), `reforce.MovieSprite.Mix` (movie and
  live sprites), 0..1, default 0. With p = mix, e = envelope, c = coverage: F' = F·(1 − p·e·c), then the sprite's own
  blend with the lighten fraction 0.5 + 0.5p (keyed copies keep their key). p = 0 is G-Force exactly, p = 1 shows the
  sprite itself in its rectangle. Display draws only (trails stay G-Force); a Pen sprite without an EPen draw gets
  a display copy of its stamp; melt puts it on the feedback draw. Command `spritemix:sprite|movie:W` (+/- nudges),
  panel Mix sliders (saved on release), keys `[` `]` (Shift: movies; session only). Render test
  `reforce_sprite_visibility_render` (mix 0 = today byte for byte, mix 1 takes over, 0.5 halves the field).
- [x] 5K.2 **Movie levels** on video sprite luminance (shader `sprite_la`, before `~` inverts):
  l = saturate((l − Black) / (White − Black))^Gamma. Global `reforce.MovieSprite.Gamma` (default 1); per movie
  `Black` (0..1, default 0), `White` (0..1, default 1, must be above Black or both are ignored), `Gamma` (> 0,
  default 1) in `<stem>.reforce.txt`. **The per-movie Gamma multiplies the global one** (global = taste, per movie =
  fix). Interpolated copies share their original's options. `tools/media_review/review.py`: Black / White / Gamma
  fields per row (original + `~/reforce/movies` copy, other keys kept). Unit tests (MediaOptions) and render tests
  (Gamma 0.5 lifts gray 64 → 90; per-movie Gamma 2 × global 0.5 = unchanged; Black/White stretch).
- [x] 5K.3 **Full-colour movie sprites** (option): `reforce.MovieSprite.Colour = off` (default, G-Force) `| over`.
  With `over`, movie / live sprites skip the intensity buffer; their frames are alpha-over composited in the
  colorize pass (after the colour video layer, so on top of it; in the window and Syphon alike), at the sprite's
  rectangle, alpha = envelope × (0.5 + 0.5 × movie Mix), levels per channel, `~` inverts; Pen/EPen and blend flags
  unused. Panel "Full colour" toggle on the Movie sprites card (`videoset:spritecolour:over|off`, saved). Fixed on
  the way: the API rejected `videoset` keys the Visualizer handles itself, so the panel's Beat sync toggle got a
  400 and never reached the engine.
- [ ] 5K.4 Peter: try Mix on the Image / Movie sprites cards and Full colour live; pick defaults for settings.json.

### 5L. 3D solids  (Peter, 2026-11-15)  `[~]`
Polyhedra (and a sphere, torus and prism) wearing the current WaveShape, drawn into the intensity buffer before the
FlowField feedback, so they trail and take the palette like everything else. Each composites through the sprite pass
with its own blend, fades and lifetime. Design: docs/3d-design.md.

Peter's decisions:
- The main WaveShape still draws in the frame.
- Not in the default slideshow (manual + Themes).
- `SYNC_*` beat-clock variables for all scene types.
- Each solid has its own blend, plus `Fill`.
- Faces wear the current WaveShape; `Text=1` uses the frame, `Text=2` is flat.

- [x] 5L.1 Meshes and math (`engine/Meshes`) + unit tests; projection / bounding-box reference. Faces from a
  convex hull of each vertex set, fanned around their centres (UVs on the tile's inscribed circle, edge 1 -> 0),
  geodesic sphere, torus; sprite-coordinate model and view-projection matrices, clipped screen bounds.
- [x] 5L.2 `render/SolidPipeline`: flat-shaded solid behind `--solid-test`, MSAA + depth, composite via the
  sprite pass. One vertex buffer (ranges per MeshKind), PSO / depth state at startup, `reforce.Solid.MaxLive` (3)
  frame-size RG8 targets registered as sprite textures, a shared memoryless 4x MSAA colour + Depth32Float pair
  (`reforce.Solid.Msaa` 4 | 1), back-face culling, depth test. The solid is drawn at its target's top-left with the
  render area limited to its pixel box (projectBounds + 1 px), so clear / resolve scale with its size; the sprite quad
  covers the box and samples that sub-rectangle (new `SpriteQuad::u0..v1`, SpriteP 80 -> 96 bytes; defaults = whole
  texture, goldens unchanged). The resolve leaves premultiplied luminance; the sprite shader un-premultiplies solid
  textures and uses alpha as coverage. Solid quads go after the sprites (on top), sprite Mix 0. Hook: CLI
  `--solid-test SPEC` (repeatable; README "3D solids"), not an engine command. Tests: `tests/solid_render.cmake`
  (extent within 2 px, hull area within 3 % (measured 0.8 %), face luminance within 16 levels (measured <= 1), outside
  black; MSAA 4 and 1), `reforce_render_tests` "solids:*" (layout, coverage = hull area within 1 %, culling, torus
  depth, parser, cost, 0 render allocations). Cost at 1920x1080, 3 solids, M-series: solid passes 0.03 ms/solid,
  0.10 ms/solid including the composite (Pen stamp + display draw; most of it is the displayed-frame copy any display
  sprite costs). On the way: FlowParams was 36 bytes vs MSL's 40 (Metal API validation failed); padded.
- [x] 5L.3 WaveShape tile (`Text=0`) and frame texture (`Text=1`). Each step with a live `Text=0` solid,
  `wsOut_` is mapped again into a `reforce.Solid.TileSize` (512) R8Unorm tile with mips. The tile shows the
  config's square [-1, 1]^2 in authored units (x · X_EXTENTS, y · Y_EXTENTS; circles stay round for Aspc 0 and 1),
  centred. The lines are drawn by `ScenePipeline::encodeTileLines` (line_vs / line_fs, analytic AA, its own
  R8Unorm PSO), at the frame's widths × tile / frame height × 2, min 3 tile px. Then `generateMipmaps`, and
  trilinear sampling in solid_fs.
  `Text=1`: the previous intensity frame (`previousIntensity`, the frame this step's flow reads), its centred square
  crop downsampled with 4 taps into a second mipmapped tile. Gives the Droste look.
  solid_fs: `lum = fill + (1 - fill) * tex`, then shading and edge as before; `Text=2` is flat.
  Everything is made at startup: the tiles, the PSOs, 8 extra 1 MB line buffers. A source that was never drawn
  reads 0 (Fill only).
  Hook: `text=0|1|2`, **default 2** (README). Fixed on the way: the face UVs were mirrored and put a vertex up
  (cube faces showed the tile mirrored and turned 45°); now unmirrored, with the edge facing world-down flat at the
  bottom.
  Tests: `tests/solid_render.cmake` text0 (fill .15 / .4: exact Fill floor, the bars on the face) and text1 (the
  frame on the face); `reforce_render_tests` (tile lines + mip chain, the sources, cost, 0 render allocations with
  Text=0).
  Cost at 1080p: WaveShape tile 0.055 ms GPU (4096 segments + mips), frame tile 0.058 ms.
  Peter (2026-11-15): "generally looks good"; prefers `Text=0` to `Text=1`; 0.5 rad/s spin is great; wants the
  solid to move around the screen in 3D while spinning (done in 5L.4's scene files).
- [x] 5L.4 Scene files, `SolidRun`, `SYNC_*`, settings, Themes (2026-11-15):
  - `SceneType::Solid` (`reforce.Solid.package`), `Layer::Solid` (a spawn layer like Particles:
    `engine::isSpawnLayer`; commands switch / next / kill / life / interval:solid, web API, kill fades over .Quick).
  - `scene::SolidInstance`: `Mesh` literal (never compiled), `Text` / `Pers` / `Flag` / `PDur` at load, per frame
    B vars then `X Y Z Scal RotX RotY RotZ Fill Shad Edge Pen EPen`; `engine::SolidRun` (lifetime, clamping).
    Visualizer: `startSolid` / `killSolids`, a target slot per live solid (all taken: the oldest makes way), drawn
    with the test hook's composite. Defaults: Text 0, Pen 1, Flag 0.
  - `SYNC_ON` / `SYNC_BPM` / `SYNC_BEAT` / `SYNC_LOCK` from the beat clock (now stepped first in each sim step), native
    in WaveShapes, Particles, Sprites and Solids.
  - Content: `content/` (committed, laid out like a scene root; `reforce.ContentDir`, `--content DIR|none`, default
    the source tree's) with seven solids (Icosahedron Drift, Cube Orbit, Dodecahedron Beat, Sphere Bounce, Torus
    Tumble, Prism Flyby, Octahedron Mirror; all moving in X / Y / Z) and the "Solids" theme. Themes may set
    `Solid.Slideshow.Interval.Duration`; content themes and categories merge into G-Force's. `reforce.Solid.Dir`
    adds a folder (`Extra/...`). Settings `Solid.*` like `Sprite.*` (interval 0, Duration 40, fade 4), never
    imported from G-Force.
  - Tests: tests/test_engine_solids.cpp (compile tiers, SYNC in every type, SolidRun, commands, settings, themes,
    every content solid clean and finite, Dodecahedron Beat on whole beats); solid_render.cmake phase 4 (a scene
    file = the hook bit for bit, a moving Text=0 solid deterministic, a theme starts solids); web library test.
- [x] 5L.5 Control panel Solids card, `--stress` spawns, perf report (2026-11-15). Peter: "the solids theme looks
  sweet".
  - Solids card (web UI extra cards): Interval / Duration sliders (saved as `Solid.Slideshow.Interval.Duration`,
    `Solid.Duration`), Start next, Clear (`kill:solid`), alive list, countdown, library with favourites.
  - `--stress` starts a solid every other round (so all three slots fill and the oldest makes way) and kills them
    every sixth. New perf cause tag `solid-start` (was counted as sprite-start); `TagCount` is now an int constant
    (fixes four old -Wsign-compare warnings in Perf.cpp).
  - Perf (Release, 1920x1080, 60 Hz sim, `--stress --perf-report 30`):
    with solids (14 starts, up to 3 live): cpu p50/p99/max 1.20 / 7.60 / 10.35 ms, gpu 2.50 / 4.20 / 4.94 ms,
    0 hitches; `--content none` (no solids): cpu 1.30 / 4.50 / 6.79, gpu 2.30 / 4.10 / 6.23, 0 hitches.
    GPU cost of three solids is ~0.2 ms; 13 frames of 8-12 ms CPU, about one per start (not traced further;
    well inside the 16.7 ms budget).

- [x] 5L.6 Rubik's cube solid (Peter, 2026-11-22): `Mesh="rubik"` with `Moves` / `Turn` / `Seed`, sticker colours
  picked per ColorMap (engine/StickerColors), `ColorKey = 0` for solids (opaque in the Syphon alpha and over the
  backdrop); content's Rubik Cube: scrambled in 2 beats, solved one turn per beat over the next 30, on every 8-bar
  line. Tests: test_engine_rubik.cpp, test_render_solids.cpp (rubik, ColorKey). docs/3d-design.md "Rubik's cube".
- [x] 5L.7 15 puzzle solid (Peter, 2026-11-22): `Mesh="fifteen"`, `Shuf`; content's Fifteen Puzzle (later replaced by Fifteen Glass, 5L.8): the tiles leap
  into a scramble on the 8-bar line, then 31 slides one per beat land it solved on the next. Row colours from the
  puzzle colours, seven-segment numbers. Tests: test_engine_fifteen.cpp, test_render_solids.cpp (fifteen).
  docs/3d-design.md "15 puzzle".
- [x] 5L.8 Glass 15 puzzle (Peter, 2026-11-22): `Glas = 1` with `Fros` / `Tint` / `Bend`; clear 3D glass buttons
  (engine::makeGlassButton, MSAA) over the colorized frame, refracting a half-size blur of it, one light tint,
  etched white numbers on a dark halo. Content's Fifteen Glass replaces Fifteen Puzzle. docs/3d-design.md "Glass".

### 5M. Syphon stream flicker  (Peter, 2026-11-15)  `[-]` dropped (Peter could no longer find a flicker frame)
Reported: black flicker (or tearing) in an OBS -> YouTube recording of reforce's Syphon output (stream of
2026-11-12 20:26-22:12). Found nothing on reforce's side:
- `reforce-syphon-monitor` (tests/syphon_monitor.mm; Metal or OBS-like `--gl` reads): 0 black / transparent / dipped
  frames from live reforce (20 s, 2 min stress, 2 min GL).
- `reforce-syphon-tear-test` (tests/syphon_tear_test.mm; alternating black / white frames, flipped and blit
  publish, 1080p and 4K, 30-240 Hz, in-process and cross-process): 0 torn in ~40k GL reads.
- reforce's log for the stream: steady 60 fps. OBS log: 1080p30, CBR 5000 kbps, 858 dropped frames, a reconnect
  around 20:56.
If it comes back: record locally in OBS at high quality first; for streaming try 9-12 Mbps and 1080p60; send the
clip or timestamp to line up with reforce's log.

### 5N. Organics: procedural growth  (Peter, 2026-11-17)  `[~]`
A new spawn layer for procedural algorithms that grow, evolve and die; trees (space colonisation) first, drawn as
tapered lines through the WaveShape line pass. Design: docs/organics-design.md.

Peter's decisions: an Organics layer (not a Trees layer), trees first. Physarum and reaction–diffusion are the
likely next kinds.

- [x] 5N.1 `engine::TreeGrowth` + unit tests (tests/test_engine_organics.cpp): space colonisation on uniform grids,
  trunks grow along their root's direction (tropism bends branches only: added to a trunk every step it curled the
  Willow's trunk right round), pipe-model tips, never grows onto an existing node, bounded by MaxNodes / maxTrunk.
- [x] 5N.2 `SceneType::Organic` (`reforce.Organic.package`), `scene::OrganicInstance` (C vars run per point / root
  before PX/PY and RtX/RtY/RtA so both coordinates share one random angle), `engine::OrganicRun`: growth in sim
  time (at most 8 iterations per step), the newest generation drawn at its fraction, sway as a travelling wave
  weighted by thinness, buds, wither (ungrow by generation over the fade-out), one path per tree.
- [x] 5N.3 `Layer::Organic` (kLayerCount 8): Director (catalog from content/, spawn timer, theme interval
  `Organic.Slideshow.Interval.Duration`, kill, alive), Visualizer (`startOrganic` / `killOrganics` /
  `stepOrganics`, after the particles' segments; MaxLive replaces the oldest), settings `Organic.*`,
  `reforce.Organic.MaxLive` / `.MaxNodes`, perf tag `organic-start`, web API library + favourites.
- [x] 5N.4 Control panel Organics card, seven content configs (Oak, Willow, Roots, Lightning, Neurons, Kelp
  Forest, Radial Bloom), Organics theme, docs (scene-format §5.8, README, design doc). Cost: three growing at
  once peak at 6,412 segments, CPU p99 0.27 ms. **Peter to try.**

### 5O. Rhythm: phrases, pendulums, kick zoom, drops, more procedural kinds  (Peter, 2026-11-17)  `[~]`
Peter: "do 1, then 3, then 11" (phrase awareness, pendulum wave, kick zoom), "expose these in the control panel",
then "continue and prototype some other ideas in an organized way that I can check out later". Design:
docs/rhythm-design.md. Everything is in the control panel's Rhythm section (saved to settings.json) or the Organics
card. Peter away while built: **all of it waits for Peter to try**.

- [x] 5O.1 Phrase clock (sync/PhraseClock): `SYNC_BAR`, `SYNC_PHRASE` in every scene type; auto-align from loudness
  changes, "Phrase starts now", +-1 bar, bars per phrase; quantize "phrase" for slideshow / manual switches and new
  `reforce.Quantize.Spawn`; `beatset:KEY:VALUE` command (web `{"action":"beatset"}`); Rhythm panel with a phrase strip.
  Open: real-music accuracy (compare with rekordbox's phrase analysis).
- [x] 5O.2 Pendulum wave (Organics kind `pendulums`: row, side, ring views); content Pendulum Wave, (Side), Ring.
- [x] 5O.3 Kick zoom (flow pass; `reforce.Kick.Zoom` / `.Rotate` / `.DecayMs`; default off; bit-identical at 0).
- [x] 5O.4 Six more Organics kinds (engine/OrganicKinds): phyllotaxis, harmonograph, automaton, life, lsystem,
  attractor; ten content configs.
- [x] 5O.5 Drop detector (sync/DropDetector): `SYNC_BUILD`, `SYNC_DROP`, phrase votes, "On a drop: change scenes"
  (`reforce.Drop.Switch`, off). Tuned on one track only.
- [x] Peter (2026-11-18): keep the current defaults (kick zoom off, slideshow changes on the bar, drop switch off);
  no more drop tuning for now.
- [x] 5O.6 GPU kinds (render/FieldPipeline, Peter 2026-11-18 "build these"): `ripples` (wave equation, a drop on the
  beat) and `julia` (Julia sets); content Kick Ripples, Rain on a Pond, Ripple Ring, Julia Drift, Julia Pulse, Julia
  Bands. Drawn into their own texture and composited like solids.
- [x] Peter (2026-11-22, "a mandelbrot somehow cool ... zooming in"): GPU kind `mandel` (engine/MandelDive): a
  phrase-long dive per target (ten Misiurewicz points), a doubling every 2 beats, a rush back out on the line, bands
  on the beat, kick surges, drops pull out; float32 shallow, perturbation deep (double-float kept as an option);
  content Mandelbrot Dive, Mandelbrot Abyss (Fractals). docs/rhythm-design.md "GPU kinds".
- [x] 5O.7 Organics grouped by theme (Peter 2026-11-18): categories Trees, Pendulums, Spirals, Curves, Cells, Water,
  Fractals (content/Categories/Organic) with a theme each ("Organics: Trees", ...); the Organics card lists them
  under group headings, filters by group and plays a group (switches to its theme).
- [x] 5O.8 Deck starts (Peter 2026-11-18: "take advantage of how I've structured my DJ library"): every song starts
  on a phrase line (hot cue 1 and others), so the last start of the song brought in sets the phrase lines until the
  next one. sync/DeckStarts (pure) + sync/DeckFeed (rkbx_link's new "reforce" UDP output: per-deck position, grid
  beat, tempo; DDJ-1000 MIDI: channel faders, crossfader + assign, hot cue pads). Peter's choices: a hot cue mid-song
  restarts the phrase, loops and beat jumps do not; a deck is "in" once its fader is past a third; faders over MIDI
  (not rekordbox memory); faders not moved yet count as down. Toggle "Deck starts" (`reforce.Phrase.DeckStarts`,
  on). Tried live 2026-11-18: hot cues first landed 1-3 beats off the bar (Link re-phasing after the jump); fixed by
  placing a start 0.4 s later, snapped to the nearest Link bar line.
- Later: tempo-scaled time.

### 5P. Dancers: 3D cartoon characters dancing to the beat  (Peter, 2026-11-18)  `[~]`
Peter: "animated 3D dancing cartoonish character avatars ... a wide variety of dance moves ... dance to the beat ...
vaguely like Wii characters", later friends' faces on them. Decisions: full colour on top of the visuals,
procedural characters and moves (no mocap), dancers along the bottom each doing their own moves (canon an option),
an optional disco floor only while a dancer is live; "different characters could have different dance styles, and
within each style a lot of variety"; "the rest of the visuals act as the lighting". Design: docs/dancers-design.md.
- [x] 5P.1 engine/DancerRig (frames, two-bone IK, chibi body from capsule parts: 10 hair styles, 5 outfits, 4 hats)
  and engine/DanceMoves (81 moves: 29 footwork, 31 arm patterns, 21 signature; 9 styles with 60-155 combinations
  each; choreographer on bar / phrase lines, blends, drops -> jump, energy from breakdowns; canon = lagged copy).
  tests/test_engine_dancers.cpp.
- [x] 5P.2 render/DancerPipeline: after colorize, lit by a mipmapped copy of the frame (normal-directed light, rim,
  floor reflection), drawn face (eyes, blinks, brows, mouth, blush), outline, disco floor with blob shadows, 4x MSAA,
  composite. 0.55 ms GPU for 5 dancers + floor at 1080p; 0 render allocations (tests/test_render_dancers.cpp).
- [x] 5P.3 `Layer::Dancer` (kLayerCount 9), `SceneType::Dancer` (literal keys), Director, Visualizer, commands
  (`switch:dancer:NAME`, `next:dancer`, `kill:dancer`, `life:`, `interval:`), settings `Dancer.*` and
  `reforce.Dancer.*`, theme key, web API, `--dancer-test STYLES`.
- [x] 5P.4 Control panel Dancers card (floor, moves mode, lighting, size, outline, who dances what), 12 characters
  (every style), Dance Party theme, docs.
- [x] 5P.6 Social mode (partners, call & response, face to face, cypher) and milling around the floor; floor fades
  just behind the dancers (committed 0b58b0c).
- [x] 5P.7 Walking steps (planted feet, a step on each 8th while travelling), 25 moves made continuous, two-beat
  crossfades centred on the bar line, inertial blending between sources (docs/dancers-design.md).
- [x] 5P.8 Humanising (`reforce.Dancer.Human`, 0..2, default 1; Human slider on the Dancers card): a seeded
  personality per dancer (timing lean, size of arms / hips / knees / head, left-right asymmetry, slouch, head tilt,
  weight on one leg, stance) plus slow drifts (timing, effort, hand wander, head looking around, breathing) and
  follow-through on the hands and head. Human 0 is exactly the old motion (test).
- [x] 5P.9 Dancers don't walk through each other: time-to-collision steering (both keep right head-on; the one
  walking does most of the avoiding) plus a hard 0.38 minimum distance as a backstop (engine::avoidEachOther /
  keepApart). Dancers card row no longer overflows the narrow card.
- [x] 5P.10 More variety: generated characters dressed for their style (`Random <Style>` files), proportions,
  prints, fabrics (shiny, sequins, velvet, neon), sunglasses (7 shapes, mirrored), glow sticks, faces (eye shapes,
  lashes, brows, mouths, lipstick, freckles, beards); the 12 named characters gain a touch each.
- [x] 5P.11 Floor look: `mirror` (opaque, reflects the visuals as a wall behind the floor; default) or `seethrough`
  (the original) — `reforce.Dancer.FloorLook`, Floor selector on the Dancers card.
- [x] 5P.12 Max dancers slider on the Dancers card (1-12, live; `reforce.Dancer.MaxLive`); floor shadows for up
  to 16 dancers; crowd test at 12.
- [x] 5P.13 Mingle (Social = mingle, engine::Mingle): no crew-wide formations; each dancer has their own activity
  and clock (dancing, showing off, wandering, a break at the edge, walking over to someone or a group, watching),
  a seeded personality (sociable, restless, showy), pairs and groups of 3-4 that form and break up one at a time,
  glances at others. docs/dancers-design.md "Mingle".
- [ ] 5P.5 Peter tries it; tune looks / moves.
- [ ] Later: friends' faces (`Face=` image on the face), props, solos and spotlights, a trail silhouette into the
  feedback, walk-ons.

### 5Q. Arcade organics: game-inspired visuals on the beat  (Peter, 2026-11-22)  `[~]`
Peter: "develop a few video-game inspired visuals in parallel" (Space Invaders, Q*bert, Tron), "inputs to the flow
field", "beat-synchronized". Decided 2026-11-22: drawn into the feedback only for now (the game "stirring" the warp is
a later option); light cycles top-down; the pyramid as line art; the invaders never shoot back and the player never
dies. Our own art and names (the repo is public). Design: docs/arcade-design.md.
- [x] 5Q.0 Groundwork: Organic kinds `invaders`, `hopper`, `cycles` (engine/arcade/, one file each), filled shapes on
  the OrganicCanvas (`fillPolygon`, `rect`, `bitmap`, `newPath`), tests/arcade_harness.hpp.
- [x] 5Q.1 `invaders` (Invaders.cpp, content, tests): Invader March, Invader Swarm (Invader / Rock / Brick Drift removed 2026-11-22, Peter)
- [x] 5Q.2 `hopper` (Hopper.cpp, content, tests): Pyramid Hop, Pyramid Cascade, Pyramid Mini
- [x] 5Q.3 `cycles` (Cycles.cpp, content, tests): Trail Racers, Trail Duel, Trail Swarm
- [ ] 5Q.4 Arcade group + theme, usage.md, Peter tries it.
- [x] 5Q.5 `rocks` (Rocks.cpp: Asteroids-inspired; Peter 2026-11-22 "start asteroids-style and breakout-style"): Rock Field, Rock Storm
- [x] 5Q.6 `bricks` (Bricks.cpp: Breakout-inspired): Brick Wall, Brick Storm
- [x] 5Q.7 One at a time (Peter 2026-11-22: "at most one of each of movie sprites, organics, and solids"; then "slideshows only have 1 of the three active at a time": reforce.Slideshow.OneFeature, 1):
  `reforce.Solid.MaxLive` and `reforce.Organic.MaxLive` default 1 (were 3), new `reforce.MovieSprite.MaxLive` (1);
  MaxLive counts pieces at full strength, a new one fades the oldest out under it (one spare render target for the
  crossfade), a due slideshow start waits until the live one starts fading out. `PBar` (solids, organics): the hold in
  bars, ending on a bar line or, for whole phrases, a phrase line (engine::holdForBars).
- [x] 5Q.8 `chase` (Chase.cpp; Peter 2026-11-22: "something more inspired by the [light-cycle] movies. A 3D view
  following a motorcycle dynamically"): the cycles game (CyclesSim) seen in perspective from a chase camera, drawn as
  3D line art into the feedback.
- [-] 5Q.9 `maze`: built, then dropped (Peter 2026-11-22: no round critter with a wedge mouth, which is the famous
  character; a gulping blob did not convince him either).
- [x] 5Q.10 Objects (Peter 2026-11-22: "unify them into one layer for me but keep separate rendering paths
  underneath", name "Objects", the full merge): Layer::Object replaces Layer::Solid and Layer::Organic (kLayerCount 8);
  one catalog (solids, then organics; Director::entryType says which), one card, one timer, Object.* settings and theme
  keys (old Solid.* / Organic.* read as fallbacks), reforce.Object.MaxLive counting both, groups in
  content/Categories/Object (plus Solids, Puzzles), themes "Objects" / "Objects: <group>", commands and favourites
  accept solid / organic as object.
- [x] 5Q.11 Trees (Peter 2026-11-22): Bush, Fern, Roots, Willow removed; Lightning strikes 4 times a bar apart from new
  points on the top edge (Strk, SGap); Oak sways over 2 beats about a fixed base (SwyB); Radial Bloom pushes out and
  rings on every beat (Vib, VibF); Neurons fire action potentials along their dendrites on the beat, cascading from
  neuron to neuron, all at once on a drop (Puls, PAll, PlsB, PlsE). docs/organics-design.md "Trees on the beat".
- [x] 5Q.12 Chase bikes: three designs (Bike = Arc, Blade, Orb), wheels glowing rings rolling at their real speed,
  more glow; Arc for every config (Peter picked it; the comparison configs and a headlight beam were removed).
- [ ] Later: the games stir the warp (a low-res displacement added to the flow pass: explosions push, landings ripple,
  cycles leave a wake).

### Fidelity choices: match G-Force vs do it right  (Peter, 2026-11-11)  `[ ]` Peter to decide defaults
Past decisions that copy a G-Force quirk. "Option" = can already switch it; "Proposed" = what the "done right" option
would be. **Peter decided 2026-11-12: "Done right" for all of these, plus polar/aligned/oklab blends, set in his settings.json** (decision log). **Look card (2026-11-11):** F1, F2, F4, F5 and line AA switch
live in the web UI (A/B toggle, presets "G-Force exact" / "Done right", per-key save/reset; README "Look").
| # | Decision (current default) | Kind | Option today | Proposed "done right" |
|---|---|---|---|---|
| F1 | Flow resampling = integer bilinear, 1/16-px weights, truncate (#24) | quality | `reforce.FlowRounding` (nearest/floor/point/fixed16/fixed256); **live in the web UI Look card** (`look:flowrounding:V`) | float bilinear, round-to-nearest (`nearest`); trails come out slightly brighter/longer. **Default stays fixed16; compare later (L5, low priority).** |
| F2 | 8-bit intensity buffer | quality | **`reforce.Intensity` = gforce \| hq** (settings.json, 2026-11-11; `--intensity` overrides); **live in the Look card** (trail copied into the new buffer, no blank frame) | `hq` = 16-bit float: no banding or 8-bit "sticking"; trail length may need re-tuning. **Default stays gforce; compare later (L4, low priority).** |
| F3 | Decay = 1/255 per *frame* | quality | none (fine at fixed 60 Hz sim) | decay per second; only matters if the sim rate ever changes |
| F4 | Thin lines snap to one row; thin dots are 1-px boxes (#32) | quality | **`reforce.ThinLines` = smooth \| gforce** (default smooth = reforce's AA round dots/sub-pixel lines, now at G-Force's energy; gforce = G-Force's exact rule, used by the parity suite); **live in the Look card** | smooth (current default); Peter to pick |
| F5 | FlowField expressions in double precision | quality | **`reforce.FlowPrecision` = double \| float32** (round 3). Nebula's gap was the `Aspc=1` grid (#36), not precision: float32 moves Nebula 0.58 → 0.55 mad and is a wash over all 352 fields (mean 0.372 both); **Look card, from the next field build** | keep double (current default) |
| F6 | Line width = LWdt·s(W, H), s = 0.733 + 8.67e-4·short + 5.10e-4·long px on **G-Force's frame**, which is half size (pixels doubled) above ~5.2 MP, e.g. Retina full screen 3456x2234 → 2·s(1728, 1117) = 5.17 px/LWdt (#38, re-measured at 18 sizes 2026-11-11; the −68 was that half-size frame, not window chrome) | quality | **`reforce.LineScale` = gforce \| frame** (settings.json; Look panel "Line width scaling", 2026-11-12) | `frame` = the same law on reforce's real render size (continuous, 4.43 px/LWdt at 3456x2234: ~14 % thinner lines than G-Force's full screen). Default gforce (= the look Peter knows from full screen); Peter to pick |
| F7 | `%` is integer remainder (#25) | semantics | compiler option only (`ModuloMode::Fmod`) | keep G-Force (scenes were tuned against it); maybe expose a key |
| F8 | ColorMap morph = RGB lerp, FlowField morph = linear source lerp | quality | `reforce.Blend.*` (oklab, polar, …) | already selectable per layer |
| F9 | Adaptive fft frequency axis (Later L1, not built) | audio | n/a | a fixed log axis is steadier and makes MID/TREBLE meaningful; Peter to choose before L1 is built |
| F10 | `mag()` 7-point smoothing | audio | none | keep (cheap, harmless); could expose |
| F11 | Out-of-frame flow sources → black; EPen-only sprites a half-way lighten; overwrite line blending | semantics | sprites: **`reforce.Sprite.Mix` / `reforce.MovieSprite.Mix`** (5K, default 0 = G-Force) | keep G-Force (part of how scenes look) |
| F13 | Sprite image alpha ignored (#42): transparent pixels show their stored colour | quality | **`reforce.SpriteAlpha` = black \| ignore** (default black = transparent counts as black, not drawn) | `black` (Peter, 2026-11-13) |
| F12 | Sim at 60 Hz while Peter's G-Force really runs ~55.6 | already "right" | `Graphics.TargetFrameRate` | done |

### Later (Peter, 2026-11-11: saved for later)
- [ ] L1 Model G-Force's adaptive `fft()` frequency axis (probe findings, `docs/audio-probe-results.md` §9). Needs one more
  short G-Force audio session (design in §9) before a fit can ship.
- [ ] L2 30-minute `--stress` runs at 1080p and 4K (5D.3 acceptance).
- [ ] L3 Reduce ~570 MB RSS at 4K (6 pooled RG32F field textures ≈ 400 MB in ScenePipeline flow code: fewer, half-float, or
  smaller pool).
- [x] 5G CPU comparison vs G-Force: first pass done 2026-11-12 (docs/perf-comparison.md); extras optional.
- [x] L6 Movie review: first pass done 2026-11-12 (copy workflow: `collect --copy` / `--prune`).
- [ ] L4 (low priority, Peter 2026-11-11) Compare F2 `reforce.Intensity` gforce vs hq by eye on slow/zooming FlowFields
  (e.g. "Deep Zoom") and long-trail scenes; check whether hq needs decay re-tuning to keep the same trail length.
  **Tooling ready:** web UI Look card, A/B button or `b` key flips "G-Force exact" ↔ "Done right" live on the running
  scene; "Save as default" writes the pick to settings.json.
- [ ] L5 (low priority, to consider) Compare F1 `reforce.FlowRounding` fixed16 vs nearest side by side (brightness and
  trail length vs G-Force-exact); decide whether "done right" should become the default. **Tooling ready:** Look card
  (FlowRounding buttons, or A/B with only FlowRounding differing between the slots; needs `Intensity = gforce`).
Round 3 done (2026-11-11): Nebula fixed by the Aspc=1 grid (#36) → FlowFields 349/352 (only the 3 rnd-load configs fail);
thin lines/dots rule derived (#32) as `reforce.ThinLines = gforce` (default smooth) → probe parity 166/186, 0 unexpected.
Offline (2026-11-11): #30/#31 were a strip-decoder artefact (B0 frames vs columns, 5B.1) → probe parity **181/186**, 0 unexpected.

### Order
5A first (small, immediately useful) ∥ 5C.1–5C.3 probe kit. Then G-Force probe sessions (screen unlocked):
audio + strip charts + corpus sweeps. Then 5C.4 analyzer fit ∥ 5B fixes, then parity. 5D (smoothness) starts once 5A lands (both touch app/render). 5E
follows 5D's instrumentation, so its cost is measured with the same tools.

---

## Decision log
| Date | Decision |
|---|---|
| 2026-11-11 | Project started. |
| 2026-11-11 | G-Force content is copied to git-ignored `scenes/` via `scripts/sync-scenes.sh` and never committed (EULA). |
| 2026-11-11 | Personal sprites saved to a folder in `~/Pictures`; the sync script copies them into `scenes/`. (Replaced the same day: next line.) |
| 2026-11-11 | **Personal media home = `~/reforce/images` + `~/reforce/movies`** (Peter). Read in place: `reforce.ExternalSpritesDir = ~/reforce` (recursive, so images and movies are sprites) and `reforce.VideoDir = ~/reforce/movies`; set by install-app.sh when missing. The old personal sprite folder moved there; sync-scenes.sh no longer copies personal files into `scenes/`. Uncurated clips stay in a separate folder until reviewed (tools/media_review). |
| 2026-11-11 | Stack: C++20 + Metal (metal-cpp), macOS only, Syphon output, fixed-size offscreen render. Rejected: wgpu-native, OpenGL, browser. |
| 2026-11-11 | The CPU interpreter is the source of truth for expressions. Generating MSL is an optional optimization (FlowFields are static). |
| 2026-11-11 | Independent per-layer transitions. FlowField: blend mappings (polar option). WaveShape: match, resample, align. ColorMap: OKLab option. |
| 2026-11-11 | Blend mode `gforce` (match G-Force via probes) is the default; alternatives are selectable per layer. Main use is the automatic slideshow plus manual switching per layer. |
| 2026-11-11 | Control through a built-in web UI (cpp-httplib, REST + SSE, plain HTML/JS, localhost by default, LAN opt-in with a token). |
| 2026-11-11 | Undocumented behavior is settled by black-box probes. No decompiling (EULA §2). |
| 2026-11-11 | Audio (tentative): CoreAudio input with a chosen device (default: Peter's G-Force input), plus file playback for repeatable tests. |
| 2026-11-11 | Settings: reforce has its own `~/Library/Application Support/reforce/settings.json`, snapshotted once from G-Force (Peter's choice: all groups, snapshot). Built-in defaults = measured G-Force defaults. |
| 2026-11-11 | Simulation runs at a fixed 60 Hz tick (G-Force decay and B-var counters are per frame); present and Syphon run at display rate. |
| 2026-11-11 | Next order: M5 → (M5b web UI ∥ M6 particles/sprites/themes) → M7 side-by-side pixel calibration vs G-Force → cleanup. |
| 2026-11-11 | Phase 5 decisions: Dock = Peter drags; audio probing via a Loopback `reforce probe` device; add MID/TREBLE/BEAT(+tempo) extensions; default sim rate stays 60. |
| 2026-11-11 | Video (5F): all three — G-Force-style movie sprites, a full-colour video layer, live camera/Syphon inputs — sharing one VideoSource abstraction. |
| 2026-11-12 | Peter's defaults (in his settings.json; built-in defaults stay G-Force so parity tests are unchanged): blends FlowField polar, WaveShape aligned, ColorMap oklab; Look "Done right": ThinLines smooth, Intensity hq, FlowRounding nearest, FlowPrecision double, AA on, LineScale frame. |
| 2026-11-12 | Skip list `reforce.Exclude` (Peter): the three sprites with their own .txt (Space/Moon, Space/Astronaut, G-Force Logo) are never used; run 25 (#39) showed G-Force's Moon blacks out the whole frame. Stays excluded regardless of #40 (Peter, 2026-11-12). |
| 2026-11-12 | Ableton Link (5H): GPL library accepted; `LINK_*` variables plus `BEAT`/`TEMPO` following Link by default; slideshow on bars, manual switches on beats. |
| 2026-11-12 | Sprite layering (5B.10, probe #40): built-in default `gforce` (EPen display-only, Pen feeds back); `melt` keeps reforce's old look, `trails` combines the overlay with a soft feed (Peter's idea of a third option). Peter picks his own default. |
| 2026-11-13 | Sprite blend flags (probe #42, runs 28-31): the measured formulas, keying threshold, oldest-on-top order and Rec. 601 apply to `gforce`/`trails`; `melt` keeps the earlier guesses. G-Force ignoring image alpha became an option (`reforce.SpriteAlpha`, default `black`, F13) since it is a quality quirk. #41's "Pen 0 erases" withdrawn (no erase over a non-black buffer). |
| 2026-11-13 | `reforce.SpriteAlpha` default stays `black` (Peter). |
| 2026-11-13 | Probe pass (runs 32-39): sprite keying = ⌊255·x⌋ ≥ 10; particle END_TIME = START + 5 + (fade + duration)/2 with dt = age/life (#43, scene semantics, so matched); a new EPen-only sprite replaces running EPen-only sprites with a linear 1.3 s fade-out, Pen sprites never (#44; reforce used "covers the frame"). |
| 2026-11-13 | Sprite replacement (#44) across kinds is an option: `reforce.SpriteReplace` = own (default, Peter: images only replace images, movies only movies) \| any (G-Force). |
| 2026-11-13 | Panel slideshow sliders save to settings.json; movie sprites get their own timer and shuffle, separate from image sprites (Peter). |
| 2026-11-13 | Ableton Link follow mode (5H.6): reforce joins a Link session only after another app is running, so it takes rekordbox's tempo instead of setting it (`reforce.Link.Follow`, default 1). |
| 2026-11-13 | FlowField Unions (probe #45, runs 41-43): the Union's ALFA blend persists after its transition until the next FlowField transition (G-Force); #18's opposite conclusion was a decay artefact. reforce now keeps the baked blend. Scene semantics, so no option. |
| 2026-11-14 | Dance clip beats come from motion only (clips are silent): offline detector + RIFE x4 interpolation; playback position derived from a beat clock (Link or audio phase), not a rate (Peter). |
| 2026-11-15 | Sprite visibility (5K, Peter): a Mix per sprite card (0 = G-Force, 1 = the sprite takes over its rectangle; display draws only), movie levels (global Gamma, per-movie Black/White/Gamma), and full-colour movie sprites as an option (`reforce.MovieSprite.Colour = over`). All default to G-Force. |
| 2026-11-11 | Probes are installed into user-owned subfolders of G-Force packages (one-time sudo) and triggered by the `Key 5.py` hotkey script. Screenshots come from G-Force's own `TakeScreenshot()`. |

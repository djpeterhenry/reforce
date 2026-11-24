# Rhythm: phrases, drops, kick zoom (PLAN 5O)

Beat-related structure on top of the beat clock (sync/BeatClock.hpp: Link when a session runs, else the audio phase
lock). Peter's request (2026-11-17): "1, then 3, then 11" from the list of procedural beat ideas: phrase awareness,
the pendulum wave, kick zoom; then more prototypes.

Everything here is in the control panel's **Rhythm** section and saved to settings.json from there.

## Phrases (5O.1, sync/PhraseClock)

Dance music is built in 8 / 16 / 32-bar phrases; big changes land on phrase lines. The phrase clock places the beat
clock's count in phrases of `reforce.Phrase.Bars` (16) bars and exposes `SYNC_BAR` and `SYNC_PHRASE` to every scene
type.

Where does a phrase start? Link gives bar lines but not phrase lines (its timeline is arbitrary beyond the quantum),
and the audio clock gives neither. So:

0. **Deck starts** (5O.8, below): with rkbx_link and the DJ controller, the start of the song just brought in.
   Auto-align never moves it; the buttons do, until the next song.

1. **Auto-align** (`reforce.Phrase.AutoAlign`, on): after every beat, the boundary two beats back is scored by how
   much the loudness (mean of LEVEL and BASS) of the 2 beats after it differs from the 4 beats before
   (|log ratio| >= .25). Scores go into a histogram over the phrase's beats that forgets over ~4 phrases. Its peak,
   once clear (>= 1 and >= 1.5 x the next best that is not a neighbor), is the phrase start; with Link only bar
   lines are candidates. An existing origin moves only for a peak 1.25 x stronger. Drops (below) vote too.
2. **Phrase starts now** (panel button, `beatset:phrasenow`): snaps to the nearest bar line (Link) or beat (audio)
   and votes strongly, so auto keeps it unless the music keeps disagreeing. **‹ bar / bar ›** nudge it.
3. Otherwise the clock's own zero (Link's bar 1).

**Quantize** (`reforce.Link.Quantize`, `.QuantizeManual`, new `reforce.Quantize.Spawn`): off | beat | bar | phrase.
Beat and bar wait for Link's lines (as before); phrase waits for the phrase clock's line with any beat clock source
and never waits without one. Spawn quantize makes a due slideshow particle / sprite / solid / organic wait for the
next line (default off).

Tests (tests/test_sync_phrase.cpp): a synthetic track whose sections change every 16 bars at beat 24 is found within
5 phrases (Link and audio clocks), 8-bar phrases on the same track, manual start / nudges / wrap, auto overriding a
wrong manual start, a source switch resets. On a real house track (Lose Control, Jacques Lu Cont edit) auto settled
after ~2 minutes; **no ground truth yet**: rekordbox's own phrase analysis (ANLZ .EXT PSSI) would make a proper
evaluation.

## Deck starts (5O.8, sync/DeckStarts, sync/DeckFeed)

Peter 2026-11-18: "better than an audio signal would be to take advantage of how I've structured my DJ library. I
start almost every song (hotcue 1 as well as others) at the beginning of at least an 8 bar phrase." So the start
point of the song he brings in is a phrase line, until the next song. "Sometimes I'll start a song with the fader
fully down off-phrase and then later restart it on phrase of the last song": a start counts only once the deck is
heard.

**Inputs.** rkbx_link (a patched build for rekordbox 7.2.19; the module is not in rkbx_link yet) gained a `reforce` output
module: one UDP datagram per update (120 Hz) to 127.0.0.1:4470 with every deck's position (s), running beat on the
track's beatgrid and tempo (`rkbx 1 <t> <master>` + `d <deck> <pos> <grid beat> <bpm> <original bpm>`). The faders
come from the DJ controller's own MIDI, which CoreMIDI shares with rekordbox. Which messages are the channel faders,
crossfader, assign switches and hot cue pads comes from a controller map, a data file in `content/Controllers/`
(format: src/sync/ControllerMap.hpp; user maps in `~/Library/Application Support/reforce/Controllers/` win). The
DDJ-1000 map's numbers are from rekordbox's DDJ-1000.midi.csv: channel faders CC 0x13/0x33 on channels 1-4,
crossfader CC 0x1F/0x3F on channel 7, assign A/THRU/B notes 0x16/0x1D/0x18, hot cue pads notes 0x00-0x07 on channels
8/10/12/14 (decks 1-4).

**Per deck** (Peter's choices):
- A **start** is the deck beginning to play from standing still (Play, Cue, a hot cue while stopped): its start
  point is the grid beat where the playback run began, rounded. rekordbox moves the position in audio buffers
  (~12 ms), so "stopped" is 0.12 s without movement and "playing" is 0.15 s advancing at the deck's rate; a jog
  nudge is neither.
- A **jump while playing** is a start only when a hot cue pad on that deck was pressed within 0.5 s (either order);
  loops and beat jumps are not, and the phrase keeps going through them.
- **Elapsed beats** since the start are counted as play time x tempo, not from the position, so loops and jumps after
  the start do not change them.
- **Audible**: channel fader >= 1/3 (`reforce.Phrase.FaderUp`), and the crossfader not at the far end from the deck's
  assign side. A fader not seen yet counts as down (the DDJ sends a fader only when it moves; Peter 2026-11-18), so
  right after reforce starts a deck counts only once its fader has moved.
- A start **commits** (sets the phrase lines) when made on an audible deck, or when the deck becomes audible while
  still playing from it; at most once. A start with the fader down replaced by another before the fader comes up is
  never used. The phrase clock's origin = beat clock now - elapsed, snapped to the nearest bar line with Link (songs
  start on a downbeat), else a beat (`how` = "deck"). It is placed 0.4 s after the start: a hot cue moves the master
  deck's bar phase and rkbx_link then moves Link's timeline by whole beats, which arrives after the jump (first live
  test, 2026-11-18: hammered hot cues landed 1 or 3 beats off the bar).

Panel: **Deck starts** toggle (`reforce.Phrase.DeckStarts`, on); status "deck N start · phrase P"; a line with each
deck (▶ / ■, up / down, ★ the start in use, ✓ used before, (start) waiting for its fader) and whether rkbx_link and
the MIDI source are there. Log: `phrase: deck N start|hot cue|fader, X beats ago -> phrase line at beat B`.

Tests (tests/test_sync_deckstarts.cpp): a simulated rekordbox (120 Hz frames, buffer-quantized positions): start on
an audible deck, a deck already playing sets nothing, fader down then up, the off-phrase start replaced by the
restart, loops / beat jumps / hot cues (pad before and after the jump, a stale pad), jog nudge, pitch, a fader first
seen down, the crossfader, no beatgrid; datagram and MIDI parsing; the phrase clock keeping a deck origin against
auto. Smoke test against the real DDJ-1000 and a UDP datagram: connected, start committed.

## Pendulum wave (5O.2, Organics kind `pendulums`)

Pendulum k of `Num` swings `Osc + k` times per cycle of `Cyc` bars, so they drift into waves and chaos and line up
exactly on every cycle line; on the phrase clock's bars a 16-bar cycle lines up on the phrase lines. Three views: from
above (a row of bobs and the "snake" through them), from the side (strings), a ring (swinging radially: flowers and
stars). Content: Pendulum Wave, Pendulum Wave (Side), Pendulum Ring.

## Kick zoom (5O.3, render/ScenePipeline flow pass)

On every beat the FlowField's source coordinates are scaled by (1 - zoom) and turned about the center (aspect-correct),
with a pulse exp(-age / decay): trails surge outward (and turn) with the kick. The pulse comes from the beat clock when
it is confident (exactly on the grid, with the beat offset), else from the detected onsets (BEAT). Settings
`reforce.Kick.Zoom` (0 = off, the default; .01 = 1 % per frame at the peak), `reforce.Kick.Rotate` (degrees per frame),
`reforce.Kick.DecayMs` (120). At 0 the flow pass runs the old code path, bit for bit (parity tests unchanged).

## Drops (5O.5, sync/DropDetector)

- **Breakdown**: the per-beat bass (BASS) below .65 x its usual level (a slow average over full beats) for 4 beats.
- **Build**: 0..1 over 32 beats of breakdown (`SYNC_BUILD`).
- **Drop**: after at least 8 beats of breakdown, the bass averaged over half a beat comes back 60 % of the way from
  the breakdown's level to the usual one. Fires at once (no waiting for the beat to end), then rests 8 beats.
  `SYNC_DROP` = 1, falling to 1/e over a bar. The phrase clock gets a vote at the drop's bar line.
- **On a drop: change scenes** (`reforce.Drop.Switch`, off): FlowField, WaveShape and ColorMap cut to their next configs
  over 0.4 s.

Tuning: BASS is the whole mix's low band, kick included, and fairly smooth. On Lose Control the demucs stems show a
bass-only gap at 66-74 s that BASS barely sees (the kick keeps going) and a drum breakdown at 134-144 s at ~55 % of the
usual level; the detector finds that one and drops at 143.3 s as the drums come back. The first version (40 % / per-step
bass) found nothing; a one-beat average fired inside the breakdown. Tests: tests/test_sync_drop.cpp (steady music and
short dips never drop, the build rises, the drop within a beat, silence never drops, 120 BPM without a clock).

## More procedural kinds (5O.4, engine/OrganicKinds)

Small "bodies" drawing through an OrganicCanvas (tree units, the organic's transform, one path); scene keys in
docs/scene-format.md §5.8.

| Kind | Content | Beat use |
|---|---|---|
| `phyllotaxis` | Spiral Galaxy | one seed per 16th (Rate per beat); Spiral Galaxy sweeps the angle once per phrase |
| `harmonograph` | Harmonograph, Harmonograph Rose | frequencies in cycles per bar, so the figure closes on the bar |
| `automaton` | Rule 30 | one row per 8th, scrolling up |
| `life` | Game of Life | one generation per beat, a fresh soup every 16 bars (phrase lines) |
| `lsystem` | (none: Fern and Bush removed 2026-11-22; the kind stays) | one generation per 1-2 bars, each traced in over its first bar |
| `attractor` | Clifford Attractor | parameters glide over 24-40 bars |
| `dragon` | Dragon Fold, Dragon Rounded | a fold every 2 beats, landing on the line; unfold, hold and refold every 8 bars (Peter 2026-11-23; Dragon Pinwheel, four dragons tail to tail, removed the same day: `Num` stays) |

## Dragon (5O.9, engine/Dragon)

Peter 2026-11-23: "a 'dragon' fractal ... the pattern you get with iterative copies and rotations of 90 degree
angles". The Heighway (paper-folding) dragon: generation n + 1 is generation n plus a copy of it turned 90 degrees
about its end point, so its turns are the regular paper-folding sequence and generation n has 2^n unit segments on
the square lattice (it touches itself only at corners, never along an edge).

- **The fold.** The copy starts folded back onto the curve, like a strip of paper folded in half, and swings open
  about the end point over the last `Swng` (half) of a fold interval, accelerating (angle x s^2) and snapping flat
  at exactly `Angl` (90) degrees **on the fold line** (a beat line with `Fold` 1). Then the next copy waits folded
  until its own swing. The newest copy flashes as it lands (`Flsh`, fading over a beat), or (`Rise`, Dragon Fold; Peter 2026-11-23)
  swings out dim and brightens smoothly once landed, full just as the next fold starts to swing. Dragon Fold (Peter
  2026-11-23, "settle"): one even level for the whole curve (`Shd` 0) and `Setl` .25: a segment's brightness only
  depends on how long ago it landed: a ghost at 1 - `Rise` of the level while it swings, rising past the level to a
  glow (full; the level is 1 / (1 + `Setl`)) just as the next fold starts to swing, then settling back to the level
  over the next fold interval. So at most two halves differ from the level: the newest rising, the one before it
  settling. (Before: the generation shading was relative to the current generation, so every old segment dimmed a
  little with every fold, under the rise; Peter found it weird.) How much the glow shows depends on the ColorMap. Dragon Rounded (Peter 2026-11-23): the same even level (`Shd` 0, `Flsh` 0)
  with only the running band (`Run` .45) over it.
- **Always phrase-aligned** (Peter 2026-11-23: "anything that moves where we are in a dragon phrase should update the
  dragon"). The cycle is a pure function of the phrase-aligned bar clock, so a deck start, a phrase re-alignment, a
  hot cue moving Link's timeline or the lock arriving puts the dragon at the right place in its cycle on the next
  frame. The end follows too: an organic's PBar keeps ending on a phrase line of the live clock
  (`OrganicRun::followBars`; bars left counted as the clock runs, so tempo changes move it, the end snapped every
  frame to the nearest line), and a dragon fading out from a cycle line stays one still segment (`OrganicStep::ending`)
  instead of showing the next cycle's first fold.
- **The cycle** (`Cyc` bars, 4; on the phrase-aligned bar clock, so cycles start on phrase lines): fold k lands on
  beat k x `Fold` of the cycle, up to `Gens` (12: 4096 segments) or as many as fit before the refold; it holds full
  grown; over the last `Back` beats (2) every generation folds back up (smoothly, all the way, newest first) and the
  cycle line itself shows one segment. The content: 8-bar cycles at a fold every 2 beats (Peter 2026-11-23), so Dragon Fold unfolds 12
  generations over 24 beats, holds 4 and refolds over the last 4; two cycles a phrase.
  Each cycle is mirrored and/or turned half way round (seeded), so consecutive dragons differ.
- **The view.** Each generation's chord (start to end) is turned 45 degrees from the last and sqrt 2 longer; with
  `Spin` 0 (default) the view turns with it, so every generation sits the same way, as large as fits (the angle
  that fills a 16:9 frame best, found once at the start: the chord about 11 degrees off horizontal for one dragon).
  So on screen a fold opens like a book: the old half turns one way as the copy swings the other. `Spin` 1 keeps the
  view of the full-grown curve: the old half stands still and only the copy moves, but the young generations sit
  at 45-degree steps and fill less of the frame. The scale moves geometrically from one generation's fit to the
  next's through the swing, but never so far that the swinging copy leaves the frame (its box is larger
  mid-swing): a slight breath on each fold.
- **Intensity.** By the generation a segment was born in (`Shd`): the newest copy brightest, the first segment
  dimmest, so the nested halves of the fold read in the ColorMap. Lines thin as segments shrink on screen (`Thin`
  LWdt per pixel of segment, at most `Wdth`), so the folds stay apart at 4096 segments.
- **Variants.** `Num` 2 (the twindragon), 3 or 4 dragons tail to tail about one point (four tile the plane around
  it), each at its own intensity (`Sprd`; neighbors at least a third of it apart), all folding together (a
  pinwheel), or each `Stag` fold intervals after the last (on the 16ths with `Stag` .25: busier, tried and not used
  in the content). `Rnd` rounds the corners (a quadratic curve through each corner, `Rnd` x the segment back from
  it), so the curve never touches itself and reads as one winding line. `Angl` other than 90 folds at that angle
  (120 makes overlapping lace; not the terdragon, which is a different construction). `Run`: a bright band runs
  along the curve once per bar (the rest dims by up to half to make room). Its place (Peter 2026-11-23) is measured
  along a length that grows smoothly with the unfolding (2^G segments, G the continuous generations), so it never
  jumps when a swinging copy appears or a refold takes one away and slides from the old curve onto the opening copy;
  and it runs from three widths before the start (just after the bar line) to three widths past the end (just before
  the next), so there is one band a bar, never split across the two ends.
- **Drops and breakdowns.** A drop (`SYNC_DROP` rising past .5) collapses it to one segment and bursts every
  generation open within `Brst` beats (1), fastest at first, everything flashing with the drop; it then stays open
  until the refold. In a breakdown (`SYNC_BUILD`) the flap trembles as it swings (up to 9 degrees x build, six times
  a beat), still landing exactly on the line.
- **Cost.** The points are made once for the last generation (with a fixed angle every generation is a prefix of the
  next), so a frame turns only the moving half and fits the view; the schedule is a pure function of the clock, so a
  jump of any size costs one frame. At most 16,384 segments (Num x 2^Gens), 2 per segment more with `Rnd`. Release,
  per step: 14 generations (16,384 segments) 0.18 ms; four dragons at 12 0.18 ms; four rounded at 12 (32,764) 0.33
  ms; offline at 1920 x 1080 the whole frame's GPU time stays about 2 ms at most.

Tests: tests/test_engine_dragon.cpp (the turns against the paper-folding sequence, 2^n unit lattice segments, the
end at (1 - i)^n, prefixes, no edge twice; every fold landing on its line, the swing, hold and refold; the copy at
90 degrees just before the line; the frame fit; determinism and a 4e9-beat jump; drops; variants, junk keys, huge
or broken clocks; cost).

## GPU kinds (5O.6, render/FieldPipeline)

Peter 2026-11-18: "build these". Not line segments: each live one draws into its own frame-sized RG8 texture
(premultiplied luminance, alpha) that the sprite pass composites like a solid, so it gets Pen, `Flag`, the fade
envelope and trails. Slots = `reforce.Object.MaxLive` + 1 (the spare for a crossfade), allocated at startup.

- **ripples**: the wave equation on a 270-row grid (R32Float, three fields ping-ponged), substeps at <= .5 cells
  each (stable), damping exp(-dt / Decy), absorbing (a sponge) or reflecting edges. A drop is a Gaussian bump added as
  a displacement (added in one substep, taken back from the next, so it has no velocity; the first version pushed and
  the water kept rising). Drops land on the beat lines (`Rate` per beat), at `DX`, `DY` or at random; `Rain` adds
  small ones. Drawn: the crests plus a slope highlight from the top left, minus a floor (.15) so the old rings'
  chatter stays dark. Content: Ripples Kick (a drop per beat), Ripples Pond (a big drop per bar, rain thickening
  through a breakdown, reflecting walls), Ripples Ring (drops on the 8ths stepping round a ring every 2 bars).
- **julia**: one pass per frame, smooth escape-time shading; the set is dark. Content: Julia Drift (c runs once per
  phrase along the outside of the Mandelbrot set's main cardioid: always connected, so never dust), Julia Pulse
  (San Marco, swelling on every beat), Julia Bands (escape-count bands flowing in once per beat).

- **mandel** (Peter 2026-11-22: "does a mandelbrot somehow cool ... zooming in is the usual cool thing"): an endless
  Mandelbrot zoom on the beat clock (engine/MandelDive). Each dive (`Dive` bars, 16 = a phrase) heads for a target on
  the set's edge in a seeded order, the magnification doubling every `Dbl` beats (2: about 2^30 by the end of a
  phrase), easing in over the first beat and softly capped at `Deep`; over the last `Out` beats (4) it rushes back
  out to the whole set, so the phrase line itself is seamless and the next dive starts from the same overview. The
  view center slides from the overview's center to the target as it zooms (T + (H - T) 2^(-1.5 depth)). Beats: the
  escape-count bands flow inward one band per beat; started mid-dive (by hand; from the slideshow the configs' `SBar`
  waits for a dive line) it starts from the whole set and catches up at `Ent` doublings a beat; every beat line surges the zoom in by `Kick` doublings, relaxing
  over the beat; a drop (SYNC_DROP) pulls out by `Drop` doublings with an 80 ms attack, then follows the drop's decay
  over the bar and the dive resumes; the content turns the view slowly over the phrase (`Rot`).
  Endings (Peter 2026-11-22: no more zooms into the "cross points"; minis "sound awesome if you can check what the
  compute is"; more variety): 14 targets of four kinds (MandelKind), which take turns, never the same kind twice in a
  row; each kind's targets in a seeded order. The sequence is shared by the whole process, so the next Mandelbrot
  object carries on with the next dive (another kind) instead of starting over.
  - Spiral (5): seahorse-valley Misiurewicz points with 2, 3, 4 and 5 arms (M(28,2), M(20,3), M(20,4), M(20,5)) and a
    swirl in the period-5 bulb's valley (M(12,4)); spirals all the way down.
  - Lace (2): elephant-valley Misiurewicz points M(13,3), M(12,2).
  - Mini (4): a mini-Mandelbrot filling the frame like the whole set at the start, turned upright over the last 10
    doublings: on the west antenna (period 13, 2^20, spiky antenna crown), in elephant valley (p19, 2^19, lace
    around it), by the snowflake branch (p20, 2^19), south (p49, 2^31.5, a ring of fine teeth).
  - Decor (3): the ring of Julia-like decorations a dive passes on its way to a deep mini, ending about half way down
    (in log) centered on the mini: a swirl ring (p112 mini, 2^23), a spiral ring (p49, 2^27), a star (p32, 2^24).
  Misiurewicz points (the boundary passes through each and looks the same at every depth) by Newton in 50 digits
  from random seeds per valley, exact (k, p) checked; the branch points (top M(4,1), west antenna M(3,1), period-3
  bulb M(10,2), south dendrite M(7,1)) dropped. Minis: the period a small box near a Misiurewicz point first
  surrounds 0, the nucleus by Newton (f^p(0) = 0), the size by the estimate 1 / (b l^2); distorted ones (elephant
  p15, double spiral p54) dropped, and the seahorse p936 at 2^27 (45 x 936 iterations a pixel: 236 ms).
  Iterations follow the target: on the way a fit of the 99.5th percentile escape count over the dive's frames
  (itA + itB depth, numpy) x `Itk` (1.5), at least `Iter`; landing on a mini they ramp to 45 x its period (the
  99th percentile around a framed mini: about 14 % of the frame is inside, the outside's median 6 x period), on a
  decoration 10 x. The inside of a mini: the shader maps each pixel into the mini's own coordinates
  (w = (c - nucleus) / size) and skips the main cardioid and period-2 bulb (shrunk 10 %), about 85 % of the inside,
  with no false insides against the CPU (render test, every mini, upright and turned); 33-40 % less GPU time
  (`Aff` 0 turns it off). Tried and not worth it: derivative / periodicity interior tests (catch an inside pixel only
  after ~37-53 periods), series approximation (the deltas stop being linear within one period).
  Precision: float32 pixelates past ~2^17 (at 2^30 the whole frame is one float: one flat color). Two deep paths,
  both checked against a CPU double reference (the render test at 2^30, by hand at 2^33, 2^38, 2^44: > 99.9 % of
  pixels shaded the same, except double-float at 2^44, 97 %):
  double-float (float2 hi + lo, two-sum / fma two-product, ~48 bits, its own library compiled without fast math),
  and perturbation (the view center's orbit in double on the CPU each frame, each pixel iterating only its float
  difference e' = 2 Z e + e^2 + dc, rebasing to the reference's start when |z| < |e|: no glitches, one reference).
  The default (`Prec` 0) is float32 to 2^12, perturbation beyond; both stay clean to ~2^40 (the double center is
  the limit; df64 starts to drift at 2^44, perturbation holds). Shading comes from the distance estimate in pixels,
  so it looks the same at every depth. Content (Peter's 2026-11-22 review): one config, Mandelbrot Dive: one dive of `Dive` bars (32; PBar="Dive", so
  that one number sets the length), paced (Dbl 0) to 2^32 just before 4 bars out; from the slideshow it starts on a
  phrase line (SBar 16). (Mandelbrot Abyss, a slower deeper variant, was folded into it.)
  GPU cost at 1920x1080 (M4 Pro, headless render, whole frame, held at the ending; budget 10 ms): spirals 1.6-8.0 ms
  (the double spiral the most), lace 1.1-1.3, minis 2.4 (p13) / 4.0 (p19) / 3.6 (p20) / 9.0 (p49) ms (without the
  affine test 4.0 / 6.0 / 5.8 / 13.3), decorations 8.9 (p112 ring) / 2.6 / 1.7 ms. Not kept: minis p24 4.5, p32
  5.7, p35 6.0 (look like p20), p112 27.8 ms (Res .75 14.7, .5 7.2). First measured (Peter 2026-11-22): float32
  2.5 ms on the double spiral (wrong past 2^17), double-float 21 ms, perturbation 10 ms.

GPU cost at 1920x1080: about 1 ms a frame for one live ripple or Julia (Apple silicon, measured in the headless
render). Tests: tests/test_render_fields.cpp (a ring's speed, stability, decay, reset; the Julia set dark and its
edge lit; the Mandelbrot dark inside and shaded like a CPU reference at depth 0 and 2^30 in double-float and
perturbation, float32 flat there), tests/test_engine_organics.cpp (drops on the beat lines, DX / DY placement, Julia
keys), tests/test_engine_mandel.cpp (depth on the beat clock, the rush out and the reset on the dive line, kicks,
drops, the seeded target order, keys, junk).

## Groups (5O.7)

Peter 2026-11-18: organics grouped by theme, browsable and selectable by theme. Each group is a category
(content/Categories/Object) with a theme of its own, `Objects: <group>` (CategoryExpr "+<group>", an organic every
~30 s). The library API gives the organic layer `groups` [{name, theme}]; the Objects card lists the configs under
group headings, its dropdown shows one group, and **▶ Play <group>** sets that theme (pressed again: back to
Organics). The general library dropdowns stay theme-free.

## Not done

Tempo-scaled time for scenes written "at 120 BPM".

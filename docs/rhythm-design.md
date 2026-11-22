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
   once clear (>= 1 and >= 1.5 x the next best that is not a neighbour), is the phrase start; with Link only bar
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

Tests (tests/test_sync_deckstarts.cpp): a simulated rekordbox (120 Hz frames, buffer-quantised positions): start on
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

On every beat the FlowField's source coordinates are scaled by (1 - zoom) and turned about the centre (aspect-correct),
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
| `phyllotaxis` | Sunflower, Spiral Galaxy | one seed per 16th (Rate per beat); Spiral Galaxy sweeps the angle once per phrase |
| `harmonograph` | Harmonograph, Harmonograph Rose | frequencies in cycles per bar, so the figure closes on the bar |
| `automaton` | Rule 30, Sierpinski | one row per 8th / beat, scrolling up |
| `life` | Game of Life | one generation per beat, a fresh soup every 16 bars (phrase lines) |
| `lsystem` | Fern, Bush | one generation per 1-2 bars, each traced in over its first bar |
| `attractor` | Clifford Attractor | parameters glide over 24-40 bars |

## GPU kinds (5O.6, render/FieldPipeline)

Peter 2026-11-18: "build these". Not line segments: each live one draws into its own frame-sized RG8 texture
(premultiplied luminance, alpha) that the sprite pass composites like a solid, so it gets Pen, `Flag`, the fade
envelope and trails. Slots = `reforce.Organic.MaxLive`, allocated at startup.

- **ripples**: the wave equation on a 270-row grid (R32Float, three fields ping-ponged), substeps at <= .5 cells
  each (stable), damping exp(-dt / Decy), absorbing (a sponge) or reflecting edges. A drop is a Gaussian bump added as
  a displacement (added in one substep, taken back from the next, so it has no velocity; the first version pushed and
  the water kept rising). Drops land on the beat lines (`Rate` per beat), at `DX`, `DY` or at random; `Rain` adds
  small ones. Drawn: the crests plus a slope highlight from the top left, minus a floor (.15) so the old rings'
  chatter stays dark. Content: Kick Ripples (a drop per beat), Rain on a Pond (a big drop per bar, rain thickening
  through a breakdown, reflecting walls), Ripple Ring (drops on the 8ths stepping round a ring every 2 bars).
- **julia**: one pass per frame, smooth escape-time shading; the set is dark. Content: Julia Drift (c runs once per
  phrase along the outside of the Mandelbrot set's main cardioid: always connected, so never dust), Julia Pulse
  (San Marco, swelling on every beat), Julia Bands (escape-count bands flowing in once per beat).

GPU cost at 1920x1080: about 1 ms a frame for one live (Apple silicon, measured in the headless render). Tests:
tests/test_render_fields.cpp (a ring's speed, stability, decay, reset; the set dark and its edge lit),
tests/test_engine_organics.cpp (drops on the beat lines, DX / DY placement, Julia keys).

## Groups (5O.7)

Peter 2026-11-18: organics grouped by theme, browsable and selectable by theme. Each group is a category
(content/Categories/Organic) with a theme of its own, `Organics: <group>` (CategoryExpr "+<group>", an organic every
~30 s). The library API gives the organic layer `groups` [{name, theme}]; the Organics card lists the configs under
group headings, its dropdown shows one group, and **▶ Play <group>** sets that theme (pressed again: back to
Organics). The general library dropdowns stay theme-free.

## Not done

Tempo-scaled time for scenes written "at 120 BPM".

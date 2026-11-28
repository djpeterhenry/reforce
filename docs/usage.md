# Using reforce

The full reference for running reforce: controls, settings, the control panel features and how each part behaves.
Start with the [README](../README.md) for building and setup; the scene file format is in
[scene-format.md](scene-format.md).

## Controls and command line

### Keys

| Key | Action |
|---|---|
| `Cmd+Ctrl+F` | Toggle native macOS full screen |
| Double-click the picture | Enter full screen (in full screen a double-click does nothing) |
| `Esc` | Leave full screen |
| `Cmd+K` | Open the web control panel in the browser |
| `Cmd+Q` | Quit |
| `Cmd+M` | Minimize |
| `[` / `]` | Image sprite Mix −/+ 0.1 (Shift: movie and live sprites; this session only) |

### Command-line options (for debugging and smoke tests)

```
reforce [--frame-size WxH] [--quit-after SEC] [--minimize-after SEC] [--fullscreen-after SEC]
```

`--frame-size` sets the offscreen render size (default 1920x1080). Every 5 s the app prints a
stats line to stderr: fps, how many frames came from the display link and how many from the
fallback timer, how many were presented to the window, and whether Syphon clients are connected.

Smoothness checks (Phase 5D): `--stress` switches every layer every ~2 s (random blend modes,
transitions interrupted mid-way, particles and sprites); `--perf-report SEC` runs SEC seconds
after a 3 s warm-up, quits and prints a frame-time report on stdout (CPU/GPU/interval
p50/p99/max, a histogram, hitches by kind and by cause). Hitches (a frame over 1.5x its budget,
or a sim frame shown out of cadence) are also logged as they happen, and the web UI shows p99,
the hitch count and a small histogram. Example:

```sh
build/reforce.app/Contents/MacOS/reforce --no-audio --render-size 3840x2160 --stress --perf-report 180
```

**Objects sweep** (`scripts/perf-sweep.py`, Peter 2026-11-23): every Object (solid or organic) on its own, offline
and in the background (a running reforce is untouched), 20 s each at 1080p (reforce's render size) by default,
after a visuals-only baseline,
then all of them in a row with crossfades. It reads the objects from `content/Categories/Object/`, so a new object
added to a group is swept with no change to the script, and a listed scene that no longer starts fails. Per run: GPU
and CPU ms per frame (average, 99th percentile, worst) against the 60 fps budget: `OVER` (a 99th percentile or the
worst frame over 16.7 ms; exit status 1), `WARN` (a 99th percentile over half of it), `ok`. Report as JSON plus the
logs in `build/perf-sweep/`. Run it on a quiet machine after adding objects:

```sh
scripts/perf-sweep.py                                   # everything, 1080p
scripts/perf-sweep.py --size 3840x2160                  # a stress run at 4K
scripts/perf-sweep.py --size 1920x1080 --only Rain      # a quick look at some
```

## Install (launch from the Dock)

```sh
scripts/install-app.sh        # Release build -> ~/Applications/reforce.app (--open: show it in Finder)
```

Then drag `reforce.app` to the Dock (the script never edits Dock preferences). Run the script
again after pulling changes; it is idempotent and replaces only an existing copy whose bundle id
is `com.reforce.app`. A running copy is never quit (it may be feeding OBS live): the build is staged
and a background waiter installs it as soon as reforce quits (`build-release/install-pending.log`;
`--now` waits in the foreground instead).
`~/Applications` must be writable: if some installer created it owned by root, the script
installs to `/Applications` instead and says so (`sudo chown "$USER" ~/Applications` once fixes
that), or pass `--dest DIR`.

Scenes: with no settings, reforce reads an installed G-Force's scene folder directly
(`/Library/Application Support/SoundSpectrum/G-Force`, read only). A local copy made by `scripts/sync-scenes.sh` (into
the git-ignored `scenes/`; only needed for the G-Force comparison tools) wins: the installer then writes
`reforce.ScenesDir` = this repo's `scenes/` into the settings file. `--scenes` overrides both.

Your own images and movies: if `~/reforce` exists, the installer points `reforce.ExternalSpritesDir` at it (images and
movies in any subfolder become sprites; a folder holding a file named `.reforce-ignore` is skipped with everything in
it) and `reforce.VideoDir` at `~/reforce/movies` (the video picker).

**First launch: microphone prompt.** macOS asks whether reforce may use the microphone; any audio
input, including a Loopback device, needs this. The window comes up right away and stays silent
until you answer; after "Allow" the input from `Audio.InputSource` opens. If you denied it, enable
reforce under System Settings → Privacy & Security → Microphone.

The app is ad-hoc signed (no Apple developer certificate). By default an ad-hoc signature's
*designated requirement* is the binary's hash, which changes with every build, and macOS keys
privacy permissions on it, so you would be asked again after every reinstall. The build therefore
signs with the explicit requirement `designated => identifier "com.reforce.app"`, which stays
the same across builds. macOS should then keep the microphone permission across reinstalls. This
is not guaranteed for ad-hoc code. If it asks again after an update, allow it again, or run
`tccutil reset Microphone com.reforce.app` to clear a stale entry. A self-signed code-signing
certificate in your keychain is the robust fix if it becomes a nuisance.

| What | Where |
|---|---|
| App | `~/Applications/reforce.app` (bundle id `com.reforce.app`, version = `git describe`) |
| Settings | `~/Library/Application Support/reforce/settings.json` (flat JSON; see `src/engine/Prefs.hpp`) |
| Log | `~/Library/Logs/reforce/reforce.log` (rotated at 4 MB, 3 old files kept; also printed to the terminal when run from one) |
| Window frame, screen, full-screen state | `defaults read com.reforce.app` (restored at launch) |

Behavior for Dock launches:

- **One instance.** A second launch brings the running one to the front and exits.
  `--allow-multiple` turns this off for debugging. `--background` runs a development / test
  instance that never disturbs a live one: it never activates, its window opens behind the others
  and its position is not saved, it publishes its own Syphon server (`reforce-dev-PID`), and it
  has no log file, Link, audio input, deck feed or control panel unless asked (`--ui-port N`).
- **Window.** Size, position and screen are restored. reforce always opens windowed, even if you quit
  while in full screen (Cmd+Ctrl+F enters it). Only an explicit `"reforce.Window.StartFullScreen": 1` in settings.json
  starts in full screen.
- **Start Like This** (View menu; Peter 2026-11-23). Set reforce up once the way you play: on the display you want, full
  screen or not, the control panel opened (⌘K). Then View › Start Like This saves that to settings.json, and every
  launch (from the Dock too) opens on that display, goes full screen there if it was, and opens the control panel in
  the browser if you had it open. Keys: `reforce.Window.Screen` (the display's name as macOS shows it, or
  `external` for the first display that is not built in; empty = where the window was last),
  `reforce.Window.StartFullScreen`, `reforce.UI.OpenOnLaunch`. If the display is not connected, reforce opens in a
  window where it was (never full screen on a display you did not pick). View › Don't Start Like This turns all three
  off.
- **Trail buffer.** `"reforce.Intensity": "hq"` in settings.json uses a 16-bit float trail buffer instead of
  G-Force's 8-bit one (`"gforce"`, the default): no banding, no "stuck" trails in slow flows, but trails can look a bit
  different. `--intensity gforce|hq` on the command line overrides it. `reforce.FlowRounding` applies only to `gforce`.
- **FlowField precision.** `"reforce.FlowPrecision": "float32"` evaluates FlowField expressions in single precision
  (a G-Force hypothesis the corpus sweep could not confirm); the default `"double"` is what reforce has always done.
- **Lines and dots.** `"reforce.ThinLines": "smooth"` (the default) draws antialiased round dots and lines at
  sub-pixel positions, with thin ones (under ~2 px) as bright overall as G-Force's. `"gforce"` copies how G-Force
  draws them: points snap to whole pixels, a thin horizontal line is one pixel row, and a thin dot is a 1-px-wide
  vertical bar (probe finding #32). Only applies with antialiasing on and the default `--line-aa path`.
- **Quit.** Use Cmd+Q, the Dock menu's Quit, or `osascript -e 'tell application "reforce" to quit'`.
  All of them shut down cleanly: they finish the GPU work, stop Syphon and the web UI, and close
  the audio device.
- **Menus.**
  - *reforce*: About, Quit.
  - *View*: Full Screen (⌃⌘F), Open Control Panel… (⌘K) opens the web UI, with its token in
    `--ui-lan` mode; Start Like This / Don't Start Like This (above).
  - *Scene*: Next WaveShape / ColorMap / FlowField, Hold / Release All, Start Particle, Start
    Sprite, Look A/B. These send the same commands as the window hotkeys.
  - *Audio*: Default Input (the input chosen in System Settings → Sound, named in brackets), Everything the
    Mac Plays, every input device (✓ on the current one), and No Audio.
    A choice switches the input live and saves it as `Audio.InputSource`.

**Everything the Mac Plays** (macOS 14.2 or later) listens to all the Mac's sound, from any app and whatever
output device it plays to, with no Loopback or BlackHole device. It uses a Core Audio process tap
(`src/audio/SystemAudioTap.*`), and what you hear does not change. The
setting is `Audio.InputSource = "system"` (`--audio-device system` on the command line). The first time,
macOS asks whether reforce may record system audio (System Settings → Privacy & Security → Screen & System
Audio Recording, "System Audio Recording Only"); this is separate from the microphone permission, which
this input does not need. Without it the input runs but stays silent. If it cannot start,
reforce stays silent rather than switching to a microphone.

The icon is original artwork drawn by [tools/icon/make_icon.m](../tools/icon/make_icon.m), a
CoreGraphics rendering of a video-feedback swirl. `scripts/make-icon.sh` regenerates
`resources/AppIcon.icns`.

## Look: A/B rendering quality live

The web control panel's **Look** card switches the rendering-quality choices on the running scene, at the next
frame, without a restart (PLAN.md "Fidelity choices"). Each choice shows which value matches G-Force and which is
reforce's / the precise one, with a one-line note on what changes on screen.

| Choice | Setting | G-Force | reforce / precise | Applies |
|---|---|---|---|---|
| Thin lines & dots | `reforce.ThinLines` | `gforce` (snapped pixel rows, 1-px dot boxes) | `smooth` (AA round dots, sub-pixel lines) | next frame |
| Trail buffer | `reforce.Intensity` | `gforce` (8-bit) | `hq` (16-bit float) | next frame; the trail is copied into the new buffer, so nothing blanks |
| Flow resampling | `reforce.FlowRounding` | `fixed16` | `nearest` (also `floor`, `point`, `fixed256` under "other…"; no effect with `hq`) | next frame |
| FlowField math | `reforce.FlowPrecision` | (float32 is a guess) | `double` | next FlowField build ("Rebuild field now" does a quick switch to the current field) |
| Line antialiasing | `WaveShape.AntiAlias` | on | on | next frame |
| Line edges in color | `reforce.LineEdges` | `gforce` (edges are in-between intensities: on a ColorMap that isn't a ramp they fringe in its in-between colors) | `color` (on screen a fresh line edge mixes the line's and the background's colors; flow and trails unchanged; needs antialiasing) | next frame |
| Supersampling | `reforce.Supersample` | `1x` | `2x` (everything rendered at twice the width and height, colored, then averaged down in linear light: clean edges everywhere, trails too; about 5 ms GPU at 1080p vs about 1) | next start (Save as default, then restart) |
| Line width scaling | `reforce.LineScale` | `gforce` (width law on G-Force's frame, half size above ~5.2 MP, e.g. Retina full screen) | `frame` (law on reforce's render size: ~14 % thinner lines in Retina full screen, same below ~5.2 MP) | next frame |
| Sprites and the flow | `reforce.SpriteLayering` | `gforce` "Overlay" (probe #40: EPen-only sprites are a display-only half-way lighten, no trails, nothing wiped; Pen stamps non-black pixels into the flow) | `melt` (reforce's original: sprites stamped into the flow, they stream with it and wipe the trails under them) or `trails` "Overlay + trails" (the overlay plus a soft lighten feed of the image into the flow at `reforce.SpriteTrails`, default 0.35) | next frame |

- **A/B**: the **A/B** button (or the `b` key on the page or in the reforce window, or Scene › Look A/B) flips every choice between slot A ("G-Force exact" to
  start with) and slot B ("Done right": smooth, hq, nearest, double, AA on, frame line scale). While a slot is active, changes go into
  that slot, so you can tune both sides and flip back and forth. "G-Force exact" / "Done right" set all choices at once.
- **Typos and old names are reported**: at startup the log (and the control panel's log) lists any setting in
  settings.json that reforce doesn't read ("settings: unknown setting(s) ignored: ...") and any Look value that isn't
  one of its choices, with the value used instead.
- **Session-only** until saved: "Save as default" (per choice, or "Save all") merges the key into settings.json;
  "Reset" goes back to the saved value. A choice that differs from settings.json is outlined, with the saved value shown.
- The same commands work from the command line and `POST /api/command`: `look:intensity:hq`, `look:thinlines:gforce`,
  `look:aa:off`, `look:ab`, `look:preset:right`, `look:reset[:KEY]` (e.g. `--command 2.5=look:ab` offline), or JSON
  `{"action":"look","key":"intensity","value":"hq"}`, `{"action":"look","op":"ab"|"preset"|"reset"|"save"}`.
  Saving only works on a running app with a settings file (the web server writes it; `--no-settings` answers 501).
  `/api/state` (and the SSE stream) carries `look`: current, saved, both slots, the active slot and which keys differ.
- A trail-buffer switch allocates three textures once (no per-frame allocation); the other format's pipelines are
  compiled on a worker at startup, so the switch itself takes well under a millisecond.

## Output: window + Syphon (for OBS)

Every frame is rendered into a fixed-size offscreen texture (default 1920×1080, BGRA8,
backed by an IOSurface). That texture is then:

1. **Scaled to the window** (aspect-fit, letterboxed on black) into the window's `CAMetalLayer`
   drawable, and
2. **Published through `SyphonMetalServer`** under the server name **`reforce`**. In OBS 33,
   add a *Syphon Client* source and select the `reforce` server. Syphon draws the frame into its
   own shared IOSurface on the GPU (flipped to the OpenGL row order Syphon clients expect), and OBS reads that surface without a CPU copy.

Frames are driven by the view's `CADisplayLink` (display refresh rate; 120 Hz on ProMotion).
A display link stops firing while its window is minimized or on another Space. When that
happens, a fallback timer keeps frames coming at about 60 fps. The window is not presented to,
but Syphon keeps publishing. App Nap is disabled with an `NSProcessInfo` activity, so the
timer is not throttled in the background.

To check Syphon without OBS, run this while reforce is running:

```sh
build/reforce-syphon-probe            # lists Syphon servers, receives frames from "reforce" for 1 s
build/reforce-syphon-probe --alpha    # ... and prints the frame's alpha (shares and a coarse map)
```

**Syphon alpha** (on by default; `reforce.Syphon.Alpha = 0` or *Syphon alpha* on the Video card turns it off): the Syphon output carries
transparency in its alpha channel, so OBS can put a camera behind reforce without a Color Key filter. The colors
(and the window) are unchanged. The visuals are keyed exactly like OBS's Color Key on black (similarity 80,
smoothness 50 by default: `reforce.Syphon.KeySimilarity` / `KeySmoothness`, in OBS's units). The dancers and their
disco floor stay opaque, and so do movie sprites whose `.reforce.txt` has `ColorKey = 0` (with Full color off,
the movie's whole frame; the trails it leaves are keyed like the visuals). In the
Movie sprites card's library, those show a **no key** tag; hovering a movie shows **▶ no key** (or **▶ key** on
the tag), which plays it the other way and remembers that in its `.reforce.txt`. In OBS,
tick *Allow Transparency* on the Syphon Client source and remove the Color Key filter.
Details: [docs/syphon-alpha.md](syphon-alpha.md).

**Sync test flash** (top bar **Flash**, or `--command T=videoset:beatflash:on`; not saved, off on the next launch):
the whole output, window and Syphon, goes opaque white for the first quarter of each beat and black otherwise, with
the Beat offset applied (steady dark gray while the beat clock is not locked). It overrides the video layer, the
Syphon alpha and *window shows the visuals only* while on. Use it to check what stream viewers get: play a track
with sharp beats, record a few seconds in OBS with the stream's sources, and compare each flash's first white frame
with the beat in the recorded audio. Visuals early on the stream: a *Render Delay* filter on OBS's Syphon Client
source; visuals late: the audio source's *Sync Offset*. The local Beat offset stays as it is.

## Video (movie sprites, color video layer, cameras, Syphon input)

One video-source layer (`src/video/`, architecture §5c) feeds two places:

- **Movie sprites** (G-Force compatible). Movie files (`.mov .mp4 .m4v .mpg .mpeg .avi .qt .3gp`) in the
  Sprite package play as sprites, exactly like images: same sibling `.txt` params / `_default_sprite_params.txt`,
  flags and cover fit, Pen/EPen (EPen-only = 0.5 × luminance), fades and lifetime, `~` inverts, parenthesized
  names are script-only, Themes and the slideshow include them. The frame shown is the one at the sprite's age
  turned into luminance like an image. Movies play **once** by default: the sprite's lifetime becomes the movie's
  length (longer or shorter than `Sprite.Duration`; a PDur in the movie's params only shortens it), so the whole
  movie plays and the sprite fades out as it ends (`reforce.Video.Sprite.Loop = 1` loops them for the whole lifetime instead). An extra folder of sprites
  (images and movies) joins the catalog with `reforce.ExternalSpritesDir` (like G-Force's ExternalSprites.Path).
- **Color video layer**: a full-color source composited after the ColorMap, over or under the visualizer,
  with blend `normal | add | screen | multiply`, opacity, fit `cover | contain | stretch`, an optional luma key
  (dark pixels transparent) or chroma key (key color, tolerance, softness), and a crossfade between sources
  (also from/to nothing when starting/stopping). The window and, by default, the Syphon output show the
  composite; `videoset:syphon:off` (or `reforce.Video.Layer.Syphon = 0`) sends the plain visualizer to Syphon
  while the window keeps the composite.
- **Sources**: movie files (hardware decode of H.264 / HEVC / ProRes through AVAssetReader, zero-copy into Metal),
  **cameras** (the macOS camera prompt appears the first time a camera is opened, never at startup; the Video
  card and menu show "waiting for permission" / "access denied" with a link to System Settings → Privacy &
  Security → Camera; an app bundle built before the camera usage description was added refuses to ask:
  reinstall it) and **Syphon servers** (our own `reforce` server is refused to avoid a feedback loop, unless
  `reforce.Syphon.AllowFeedback = 1`). Any source can feed the color layer or a "live sprite".
- **Robustness**: a source that fails after it started (decode error, camera unplugged) fades out with the error
  in the Video card; a Syphon input whose server quits shows nothing (not a frozen frame) and reconnects when
  the server returns. Frames drawn below 0.6× their size (a 4K movie as a small sprite, a 4K layer at 1080p) are
  sampled through a half-size box filter plus mipmaps (`src/render/VideoMips`), ~0.25 ms GPU per new 4K frame.

Controls: the web UI's **Video layer** section (collapsed by default; source picker listing movie files, cameras and
Syphon servers; play on the layer; stop; all layer settings), the **Movie sprites** card (movie files from its
library; no camera picker there since 2026-11-22: the camera goes under the visuals as the Camera backdrop; a camera or
Syphon server can still be a live sprite by command, `video:sprite:SOURCE`), the **Video** menu, and commands (`--command T=CMD`, `POST
/api/command` with `{"command": ...}`):

```
video:layer:SOURCE      crossfade the color layer to SOURCE      SOURCE = file:PATH | camera[:ID] | syphon:NAME
video:sprite:SOURCE     start SOURCE as a movie / live sprite              | a name from the video library
videostop:layer         fade the layer out          videostop:sprite   fade all video sprites out
videoset:KEY:VALUE      opacity 0..1, blend, fit, order over|under, key none|luma|chroma, keycolor #rrggbb,
                        tolerance, softness, threshold (luma), crossfade SECONDS, loop on|off, rate, offset
                        (rate/offset/loop apply to the next file), syphon on|off
```

Settings: `reforce.VideoDir` (movie folder for the picker), `reforce.ExternalSpritesDir`,
`reforce.Video.Layer.Source` (played at startup; `--video SOURCE` on the command line),
`reforce.Video.Layer.<Key>` defaults for every `videoset` key (e.g. `reforce.Video.Layer.Blend = "screen"`),
`reforce.Video.Sprite.Loop`, `reforce.Syphon.AllowFeedback`.

**Play once vs loop.** Movies play through once by default, as sprites and on the color layer (the layer fades
out over its crossfade time as the movie ends; `reforce.Video.Layer.Loop = 1` or the control panel's "Loop layer movies" checkbox loops; the checkbox also changes
the movie playing on the layer now). **Loop until stopped** (control panel, Video card; command `videohold`): the movie
sprite playing now loops, stays (no ageing out, no new slideshow sprites meanwhile) until "Stop video sprites"; this
playback only, nothing is saved. Per movie: a
`<stem>.reforce.txt` beside the movie (`Loop = 1` or `0`, `Rate = 0.5`, `Start = 12`, `End = 1:05.5`; seconds or
m:ss; `Black` / `White` / `Gamma` levels for movie sprites, see "Sprite visibility"; engine/MediaOptions.hpp) wins
over the settings. Only Start..End plays: a loop jumps back to Start, play-once
ends (and fades out) at End. The movie review writes it: **Loop** button (key `l`), Start/End fields and "here"
buttons (keys `[` `]`) that take the preview's current position; its search box filters by name (key `/`). and `review.py collect --copy`
carries it along into `~/reforce/movies` and keeps it in sync. (A plain `<stem>.txt` is G-Force's sprite params file
and replaces the package defaults, so reforce's options use their own name.) A loop lasts as long as the sprite
(fade + `Sprite.Duration` hold + fade) or, on the color layer, until you stop it. Alternatively, list movies in `reforce.Video.Loop` (names separated by `;`, matched like the skip
list, e.g. `reforce --write-setting "reforce.Video.Loop=dance_static; koyaanisqatsi_plane"`). `--syphon-name NAME` renames our Syphon server.

Test videos are generated, never committed: `build/reforce-gen-videos --suite build/videos` writes our own
synthetic patterns (1080p and 2160p, H.264 / HEVC / ProRes 422), and `build/reforce-gen-videos --bench FILE...`
measures decode cost. Try them with `--set reforce.VideoDir=$PWD/build/videos`.

## Sprite visibility (Mix, movie levels, full-color movie sprites)

G-Force draws a default sprite (EPen only) as a half-way lighten, out = F + ½·max(0, x − F), and the ColorMap's
MapFunction (default `i^2.1`) then pushes that into the dark end of the palette, so sprites are often hard to see.
The **Mix** sliders on the Image sprites and Movie sprites cards (and the settings below) make them stand out:

- With p = Mix, e = the sprite's fade envelope and c = its coverage (the image alpha with `reforce.SpriteAlpha =
  black`, else 1), the field under the sprite is first dimmed, F' = F·(1 − p·e·c), then the sprite's own blend runs
  on F' with the lighten fraction raised from ½ to ½ + ½p. Keyed copies (Pen sprites, ThresholdCopy) keep their key.
- **0 = G-Force** (byte-identical output, the default). **1 = full takeover**: a default sprite shows exactly itself
  in its rectangle (at full envelope), so a full-screen movie hides the visualizer.
- Only the displayed frame gets the Mix, so trails stay G-Force (`reforce.SpriteLayering = gforce / trails`). A Pen
  sprite with no EPen draw gets a display copy of its stamp that carries the Mix. With `melt` layering the Mix goes
  on the feedback draw instead (its EPen-only ½ factor becomes ½ + ½p).
- Applies at once to the sprites on screen and to every new one.

| Setting | Default | |
|---|---|---|
| `reforce.Sprite.Mix` | 0 | image sprites, 0..1 (panel slider, saved on release) |
| `reforce.MovieSprite.Mix` | 0 | movie and live video sprites, 0..1 (panel slider, saved on release) |

Commands: `spritemix:sprite:W`, `spritemix:movie:W` (W = 0..1; `+0.1` / `-0.1` nudge the current value, not saved);
JSON `{"action":"spritemix","target":"sprite"|"movie","value":W}`. Keys `[` / `]` nudge the image sprite Mix by
0.1, Shift (`{` / `}`) the movie one (session only).

**Movie levels.** Movie and live sprites can be brightened before they become intensity:
l = clamp((l − Black) / (White − Black))^Gamma on the frame's Rec. 601 luminance (before a `~` name inverts it).

| Where | Key | Default | |
|---|---|---|---|
| settings.json | `reforce.MovieSprite.Gamma` | 1 | every movie / live sprite; below 1 brightens (0.5 lifts a mid gray from 0.50 to 0.71) |
| `<stem>.reforce.txt` | `Black` | 0 | luminance shown as black (0..1) |
| `<stem>.reforce.txt` | `White` | 1 | luminance shown as white (0..1, above Black; otherwise both are ignored) |
| `<stem>.reforce.txt` | `Gamma` | 1 | the movie's own gamma, **multiplied** by `reforce.MovieSprite.Gamma` (so the global one stays a taste knob on top of per-movie fixes) |

The movie review (`tools/media_review/review.py`) has small Black / White / Gamma fields per movie; they write these
keys (keeping Loop, Start/End and the beat keys) into the original's `.reforce.txt` and its copy in
`~/reforce/movies`. An interpolated copy (`<stem>.rife4x.mov`) reads its original's options, so the levels apply to
it too. The sprite's start line in the log shows levels that are not the defaults.

**Full-color movie sprites** (option, `reforce.MovieSprite.Color = over`; default `off` = G-Force). Movie and live
sprites are then not drawn as luminance into the intensity buffer: their full-color frames are composited after
the ColorMap, at the sprite's rectangle, with a normal alpha-over on top of the colorized visualizer (and on top of
the color video layer). Alpha = fade envelope × (½ + ½ × `reforce.MovieSprite.Mix`): half transparent at Mix 0,
opaque at Mix 1. The movie's levels apply per channel and a `~` name inverts the colors; Pen/EPen and the blend flags
are not used. They are part of the picture in both the window and Syphon (unlike the color layer, which
`videoset:syphon:off` can keep out of Syphon). Control panel: **Full color** on the Movie sprites card (saved);
command `videoset:spritecolor:over|off`.

## Objects: 3D solids and organics (one layer)

Since 2026-11-22 (Peter) the 3D solids and the organics are one layer, **Objects**: one control panel card, one
library browsed by group (Trees, Pendulums, Curves, Cells, Overlay, Fractals, Arcade, Solids, Puzzles;
`content/Categories/Object/`), one slideshow timer, one duration and one "at a time" count. Underneath each scene file
is still a solid (`reforce.Solid.package`, drawn by the 3D solid pipeline, can sit on top with `Over` / `Glas` /
EPen) or an organic (`reforce.Organic.package`, drawn into the feedback); the two sections below describe them.

- **Control panel:** the **Objects** card: `Interval`, `Duration`, Start next / Clear, the alive list, the library
  with group headings, ★ favorites, and **▶ Play <group>** (switches to that group's theme, e.g. `Objects: Arcade`;
  press again to go back to `Objects`). Not in the default slideshow (Interval 0) unless a theme or the card sets it.
- **♩ (beat-synced):** the Object moves, grows or plays on the beat clock: by its kind (arcade games, the
  Mandelbrot dive, seeds and cell automata; pendulums and Julia sets unless `Phas` sets their phase; trees with
  beat features such as `s`, `SwyB`, `Vib`, `Puls`; model scenes with `MBar` or `Move="cuts"`) or because its
  expressions read `BEAT`, `TEMPO`, `LINK_*` or `SYNC_*` (engine/ObjectBeat). Its hold in bars (`PBar`) doesn't count.
- **Commands:** `switch:object:NAME`, `next:object`, `kill:object`, `interval:object:SEC`, `life:object:SEC`.
- **Start lines:** an object with `SBar=N` starts from the slideshow only on a line every N bars of the phrase clock
  (Lightning: 8, so its 8 strikes fill an 8-bar phrase); started by hand it starts at once. Without a beat clock it
  never waits.
- **One at a time:** `reforce.Object.MaxLive` (1) objects at full strength: a new one, solid or organic, fades the
  oldest out under it, and a slideshow start waits until the live one begins its fade-out, so a long one (`PBar`) is
  never cut short. With `reforce.Slideshow.OneFeature` (1) objects also take turns with movie sprites.
- **Settings:** `Object.Slideshow.Interval.Duration` (0 = none), `Object.Duration` (40 s), `Object.Transition.Duration`
  (fade, 4 s) and `.Quick`, `reforce.Object.MaxLive` (1). Themes: `Object.Slideshow.Interval.Duration`,
  `Object.CategoryExpr`.

## 3D solids (PLAN 5L)

Polyhedra, a sphere, a torus and a prism drawn into the intensity buffer through the sprite pass, so they trail and
take the ColorMap like everything else (design: docs/3d-design.md). 4x MSAA edges, depth, back-face culling, and
three face textures (`Text`):

- `0`: the current WaveShape, drawn again into a square tile (512x512, mipmapped) that every face wears. The tile
  shows the WaveShape's own square, -1..1 in its coordinates, so a circle stays round. The main WaveShape still
  draws in the frame as usual.
- `1`: the previous frame (its centered square), a recursive, Droste-like look.
- `2`: flat.

**Scene files** (`reforce.Solid.package`, keys in docs/scene-format.md §5.7) move and spin the solid with
expressions, like sprites: `X`, `Y`, `Z` (3D position, +z towards you), `Scal`, `RotX/Y/Z`, `Fill`, `Shad`, `Edge`,
`Pen`, `EPen`, and the beat clock as `SYNC_ON`, `SYNC_BPM`, `SYNC_BEAT`, `SYNC_LOCK` (Link, else the audio phase
lock; also in WaveShapes, Particles and Sprites). Ours are committed in `content/Packages/reforce.Solid.package/`:

| Solid | What it does |
|---|---|
| Icosahedron Drift | spins at .5 rad/s, drifts slowly in X, Y and Z |
| Dodecahedron Beat | snaps a fifth of a turn on each beat while the beat clock is locked |
| (Beat Clock) | debug, never in a slideshow: a white cube stepping through the bar (with Link the leftmost is the bar line), flashing on each beat; small and dim in the center when the clock is not locked. Use it to set the Beat offset |
| Sphere Bounce | bounces once per beat (about 105 BPM without a lock) |
| Prism Flyby | swoops in from deep space, skims past the camera left to right and back right to left with barrel rolls, then dives at you and shoots past; repeats every 20 s |
| Octahedron Mirror | `Text=1`: the frame on its faces |
| Fifteen Glass | a 15 puzzle solved on every 8-bar line: on the line the tiles leap into a scramble, then slide back one tile per beat (31 slides), the last landing on the next 8-bar line. Clear 3D glass buttons over the visuals, which show through, bent by the buttons' shape; etched white numbers |
| Knight's Tour | a chess knight visits all 64 squares, one hop per beat, so the tour fills the 16-bar phrase and its 64th hop lands back on its first square on the phrase line, leaving a web of lines; then the web unravels and the next tour begins. Board from three quarters, slowly orbiting; a drop lights up the web, a build makes the knight float |
| Knight's Web | the same with half-turn symmetric tours (a symmetric web), the board as grid lines only, seen from higher up |
| Rain Downpour | rain on glass over the visuals: the frame is a window pane, the visuals a little behind it. Drops land on the beat (a cluster on every bar line, a drizzle on the 16ths), sit as beads that show the visuals upside down and small (each is a little lens), merge and, once big, run down with stick-slip motion, leaving droplets and a clear streak through the condensation fog; on phrase lines the big drops let go together. Downpour: twice the rain, thicker fog |
| Rain Floor | rain on a glass floor seen from above, the visuals below: drops strike puddles on the beat, rings spread, cross and interfere (a damped wave equation with capillary dispersion) and bend the visuals; beads on the dry glass between the puddles; the puddles show (a bright edge, a dark contact line, deeper inside) and their ripples bounce off the edges |
| Rain Pool | the same floor all under water (Wet 1): every drop strikes the water and throws a crown; the rings spread, cross and interfere over the whole frame, no beads |
| Bubbles Drift | soap bubbles over the visuals: every bar a wand at a new random spot blows a puff of bubbles in a random direction; they drift every way on slow air currents and a wandering breeze, wobble, bounce off each other or stick in clusters with a flat shared wall. Rainbow interference bands swirl and drain down each film; the middle stays clear, the rims mirror the visuals. Pops land on the beat (planned a beat ahead: the film drains black from the top, then tears and the rim runs round in a few frames, throwing a faint spray). A build blows more in and they tremble, a breakdown makes them slow and big, a drop pops many at once and sends a burst |
| Bubbles Burst | a busy stream of small bubbles rising from the bottom edge; every kick pops a few, so the screen fizzes in time; a drop pops most of them and sends a burst racing up |
| Shatter Drop | a pane of glass in front of the visuals (docs/3d-design.md "Shatter"): four beats after the phrase line one heavy stone hits it and a dense spider web of cracks grows on every beat, glinting and bending the visuals along each crack, exploding on every phrase line (and on a drop): the shards fly out towards you, spinning, so their refraction and highlights flash; a fresh pane slides down |
| Shatter Hail | five impacts on bar lines, every 8 bars: five small webs grow until they meet along wandering seams, then the pane explodes into many small shards |
| Rubik Cube | solved on every 8-bar line: scrambles itself in two beats (30 turns), then undoes one turn per beat so the last lands on the next 8-bar line. Sticker colors are picked from the current ColorMap; `ColorKey=0` keeps the whole cube opaque over the camera |
| Tilt Maze | a wooden tilt maze (11 x 11, one maze per 16-bar phrase, turning slowly) with a steel ball that rolls for real: the board tilts on the beats, the ball clacks into the walls on the beats and drops into the goal hole exactly on the next 8-bar line; then the maze sinks, a new one rises and the ball pops up out of the start. Board, walls and ball take three ColorMap colors; the board is half see-through |
| Soma Cube | the seven Soma pieces build the 3 x 3 x 3 cube on the beat: one flies in every two beats and locks on the beat, the last on the 8-bar line; the cube stands for two bars, bursts into a ring of hovering pieces and builds again; no two pieces ever pass through each other (a different solution each round). Seven piece colors picked from the current ColorMap |
| Jigsaw | a jigsaw of the live frame, solved on every 8-bar line: on beat 4 the finished picture breaks apart into 32 pieces that scatter, turned, over the empty table (the frame frosted and dimmed); they fly home corners and border first, each snapping in on a beat (two per beat at first), the last on the next 8-bar line. Every piece keeps showing the visuals from its home place. A drop pops the picture apart; a build makes the pile tremble |
| Jigsaw Mini | the same with 12 big pieces growing from a corner, over a gray table, breaking apart on beat 8 |
| Hanoi Tower | a glass Tower of Hanoi (6 discs) solved on the beat: one disc a beat lifts, swings over and lands on the beat, 63 moves, so the tower stands complete on every phrase line; then it springs open and shut on the first beat and the next solve carries it on from that peg. The glass refracts the visuals behind it (the rims are lenses, the pegs rod lenses), frosted, with strong lenses, every part the same glass tinted with one ColorMap color; the whole tower floats and turns slowly; a build makes the stacks float apart |
| Sierpinski Apex | an endless zoom onto the tip of a Sierpinski pyramid from outside: every 2 bars twice as close and a third of the way round, which is exactly where it started, so it loops on the bar line with no seam (docs/3d-design.md "Fractal zoom"). Surges after each bar line, glows on the beats, rushes in at a drop, slows in a build |
| Sierpinski Vortex | the same kind of endless zoom, spiralling into the gaps of a Sierpinski tetrahedron: every 4 bars four times as close and a third of a turn |
| Menger Tunnels | an endless flight down the tunnels of a Menger sponge: every 4 bars nine times as close |

**Starting them:** as Objects (above): the **Objects** card, `switch:object:NAME`, or the `Objects: Solids` /
`Objects: Puzzles` themes, which start one every ~30 s (the rain, bubble and shatter scenes are in the Overlay group, `Objects: Overlay`; the three
fractal zooms in the Fractals group, `Objects: Fractals`).

```sh
curl -s -X POST -H 'Content-Type: application/json' localhost:8411/api/command -d '{"layer":"object","action":"switch","config":"Icosahedron Drift"}'
build/reforce.app/Contents/MacOS/reforce --theme "Objects: Solids"
```

Solid-only settings: `reforce.Solid.Msaa` (4 | 1), `reforce.Solid.TileSize` (512),
`reforce.Solid.Dir` (an extra folder of solid scene files, ids `Extra/...`), `reforce.ContentDir` (reforce's own
content root; default the source tree's `content/`; `--content DIR|none`).

### 3D models (your own)

A model Object draws a textured 3D model you supply, in real time, from a moving camera (docs/model-design.md). reforce
ships no models: put a `.glb` and one or more scene files in the model folder, `~/Library/Application
Support/reforce/Models` (`reforce.Model.Dir`). Model scenes join the Objects list and slideshow as `Model/<name>`; a
scene whose model file is missing is left out (see the log). Keep models out of the repo: most downloadable models
carry licenses that don't mix with the GPL.

1. **From Blender**, convert once (applies modifiers, simplifies materials, caps textures at 2048 px):
   ```sh
   blender -b MyModel.blend -P tools/model_prep/prep.py -- ~/Library/Application\ Support/reforce/Models/my-model.glb \
       [--map my-model.json] [--drop "Interior*"]
   ```
   Models with renderer-specific materials (dozens of nodes) need a small `--map` file naming each material's color,
   normal and glow images; the script's report says what it did with every material, and prints the `Front` line for
   the scene file. A `.glb` from elsewhere can go in the folder as is.
2. **A scene file** next to it, e.g. `My Model Orbit.txt`:
   ```
   Vers=100
   Mesh="model"  File="my-model.glb"  Front="+z"
   Look="0"      // 0 = into the visuals (palette, trails); 1 = its own colors over them
   Move="orbit"  MBar="16"
   ```
   Camera: `Move` picks a preset (`dolly`, `pass`, `orbit`, `under`, `flyby`, `cuts`; their keyframes are in
   `content/Camera/`), or write your own path as keyframes in the scene: `K1="t  eyeX eyeY eyeZ  aimX aimY aimZ
   [fov [roll]]"`, `K2=...`, with t from 0 to 1 over the move, plus `Ease`, or `Step="1"` for beat cuts. Copy a
   preset's lines to start from it; put your own presets in the model folder's `Camera/`. All keys: scene-format §5.7.
3. Start it like any Object: the **Objects** card (the **Models** group lists every model scene, whatever it is
   called; **▶ Play Models** switches to the `Objects: Models` theme, a model every ~30 s), `switch:object:Model/My
   Model Orbit`, or the slideshow.

Models are loaded on a background thread (the ones the scenes name are preloaded at startup), so starting one never
stalls a frame; a scene started before its model is ready waits for it. Settings: `reforce.Model.Dir` (`none` = no model folder; `--no-settings` runs use none unless `--set` gives one),
`reforce.Model.MaxTexture` (2048), `reforce.Model.Cache` (2 models kept loaded). Cost at 1080p: about 0.7 ms GPU for a
500k-triangle model, under 1 ms in color with bloom.

**Test hook:** `--solid-test SPEC` spawns a hard-coded solid from the first frame on (repeatable, up to MaxLive).
`SPEC = MESH[,key=value...]`; keys:

- `x y z scale rotx roty rotz` (sprite coordinates, radians), `spinx spiny spinz` (rad/s)
- `text` (0 | 1 | 2; **the hook's default is 2, flat**, while scene files default to 0)
- `fill shade edge edgewidth`, `lightx lighty lightz`
- `pen epen` (< 0 = none; default Pen 1, no EPen), `flag` (sprite blend flags), `opacity`
- `pers` (0 = orthographic)

```sh
build/reforce.app/Contents/MacOS/reforce --no-audio --solid-test "icosahedron,text=0,spiny=0.5"
```

## Organics: procedural growth (PLAN 5N)

Things that grow, live and die, drawn as tapered lines into the intensity buffer, so they trail and take the
ColorMap like the WaveShape (design: docs/organics-design.md). The first kind is **trees**, grown by space
colonisation: a cloud of attraction points (the crown's shape) pulls branches out of a trunk, and branches split
where they are pulled two ways. Growth happens over time (it can surge on the beat), the finished tree sways, buds can
flash on the tips, and at the end it withers: it ungrows back into its root.

**Scene files** (`reforce.Organic.package`, keys in docs/scene-format.md §5.8): `Pts`, `PX`, `PY` shape the cloud,
`Rts`, `RtX`, `RtY`, `RtA` plant the roots, `Seg`, `Infl`, `Kill`, `Jitr` set the growth, `Wdth`, `Tip` the
thickness, and per frame `Grow`, `TrpX`, `TrpY` (tropism), `Sway`, `X`, `Y`, `Scal`, `Rot`, `Pen`, `Bud`. Ours are
committed in `content/Packages/reforce.Organic.package/`:

| Organic | What it does |
|---|---|
| Oak | a trunk from the bottom into a wide crown, growth surging on the beat, leafing out as it grows (leaves unfurling over a bar, fluttering on the beat, falling at the end), swaying over and back every 4 beats about its base |
| Lightning | a strike on every bar line for 8 bars, each from a new point on the top edge: forks down in half a beat, flashes, flickers away |
| Neurons | five cells grow dendrites into each other; on every beat one fires, a pulse running out to flash the tips on the next beat, cascading to the neuron it touches; every bar line a lead neuron starts a new wave with a stronger pulse, every phrase line they all fire together and the network rewires (a third of the side branches wither back, new ones grow elsewhere); a drop fires them all hardest. Two phrases long |
| Kelp Forest | seven stalks rise from the sea floor and wave in the current |
| Radial Bloom | branches out in every direction from the center, slowly turning, shivering outwards on every beat (harder on the bar line and a drop); buds on the bar line |
| Pendulum Wave / (Side) / Ring | a pendulum wave (from above, from the side, on a ring) that lines up again on every phrase line |
| Pendulum Double | chaotic double pendulums released together on the phrase line, each a hair's breadth apart: they swing as one for the first bars, then fan out around bar 4 into a bloom of arms and tip traces, pushed along on every beat, gathered back over the last bar and released again on the next phrase line; a drop throws them out harder |
| Spiral Galaxy | phyllotaxis: a seed per 16th note at the golden angle; the galaxy sweeps its angle once per phrase |
| Harmonograph, Harmonograph Rose | Lissajous figures in cycles per bar, closing on the bar |
| Rule 30 | a 1D cellular automaton, a row per 8th, scrolling up |
| Game of Life | a generation per beat, a fresh soup every phrase |
| Clifford Attractor | a strange attractor whose parameters glide over 24-40 bars |
| Julia Drift, Julia Pulse, Julia Bands | Julia sets (GPU): morphing once per phrase, swelling on the beat, bands flowing on the beat |
| Dragon Fold, Dragon Rounded | the paper-folding dragon: every 2 beats the whole curve gets a copy that swings open 90 degrees about its end and snaps flat on the line, up to 4096 segments, then folds back up by the 8-bar line; rounded corners with a band running along it; a drop bursts it open |
| Mandelbrot Dive | a Mandelbrot zoom (GPU): one slow dive of `Dive` bars (32; its length, the zoom is paced to fill it) toward one of 14 endings of four kinds taking turns (seahorse spirals with 2-5 arms, elephant lace, a mini-Mandelbrot standing upright, the ring of decorations around a mini), up to 2^32 deep, then 4 bars rushing back out; from the slideshow on a phrase line, by hand at once (catching up from the whole set); bands flow in on the beat, kicks push the zoom, a drop pulls it out |

Kinds other than trees and their keys: docs/scene-format.md §5.8 and docs/rhythm-design.md.

**Groups and starting them:** as Objects (above): the Objects card's groups, `switch:object:NAME`, the **Objects**
theme (`content/Themes/Objects.txt`, an object every ~30 s) or a group's theme (`Objects: Trees`, ...).

Organic-only setting: `reforce.Organic.MaxNodes` (6000 per tree).

## Dancers: 3D cartoon characters (PLAN 5P)

Chibi cartoon characters dance along the bottom of the frame in full color, over the visuals and **lit by them**
(design: docs/dancers-design.md); guests can also send a message, shown for 30 s in a speech bubble over their dancer. Each dancer has a **dance style**, and the style comes first: the look
is generated to go with it. The styles are the dance genres of real recorded dancers (AIST++, CC BY 4.0): **house**,
**pop** (popping), **lock** (locking), **break** (breaking, upright), **disco** (waacking), **stomp** (krump),
**hip-hop** (party and LA hip-hop), **jazz** (street jazz) and **ballet** (ballet jazz). A dancer dances only its style's moves: 4-bar
basic steps and whole 16-bar routines (about half the time), on the music's phrase grid. They follow the music:
calmer moves in a breakdown, a high-energy move on the drop. (The earlier hand-made moves are still there, unused:
`reforce.Dancer.Mocap` 0 brings them back.) An optional **disco floor** lights up on the beat in
the visuals' colors while anyone is dancing.

**Starting them:** the control panel's **Dancers** card (Start next, Clear = everyone waves goodbye, the library),
commands (`switch:dancer:NAME`, `next:dancer`, `kill:dancer`), or the **Dance Party** theme (a dancer every ~15 s).
Up to **Max dancers** (`reforce.Dancer.MaxLive`, 5; 1-32, on the Dancers card) generated dancers on stage; another
sends the oldest off, and lowering it sends the oldest off waving. Stream guests don't count against it (the stage
holds 32 in all; past that characters make way first).

**Who dances:** a `Random <Style>` entry per style makes someone new each time (no named characters since
2026-11-21): their body (stocky, lanky, a bigger or smaller cartoon head), a color scheme in the style's palette, the
style's wardrobe (outfits, hats, prints, shiny / sequin / velvet / neon fabrics), often sunglasses (round, aviator,
visor, heart, star, cat-eye, square; mirrored lenses reflect the visuals), glow sticks for house dancers in rave gear,
and a face (eye shapes, lashes, brows, mouths, lipstick, freckles, beards). Stream guests dance the style they chose. Every look key is in docs/scene-format.md §5.9.

They **mingle** on the floor rather than standing in a line: everyone is on their own clock (dancing, showing off,
wandering, taking a break at the edge, walking over to someone), pairs (partners dancing as mirror images when they dance
the same style, call and response, face to face) and groups of 3-4 in a horseshoe form when someone goes over and break up one person at a
time, and they glance at each other.

**Card settings** (saved to settings.json): Disco floor (`reforce.Dancer.Floor`), New dancer: one button per style brings on someone new in that style (there is no library list: Past guests brings back guests). `reforce.Dancer.RandomStyles` (settings.json only: `all`, `none` or e.g. `disco,stomp`) still limits which styles Start next and the slideshow pick; the old names robot / rave / retro / chill read as pop / house / lock / jazz, Floor = reflects the visuals (an
opaque glossy floor mirroring the visuals as if they were a wall behind it) / see-through (`reforce.Dancer.FloorLook`,
`mirror` | `seethrough`), the dancers always mingle (`reforce.Dancer.Social` / `Sync` are not read since 2026-11-21; the crew-wide
formations and crew sync modes were removed 2026-11-27), Lit by visuals (`reforce.Dancer.Lighting`, .75), Size (`reforce.Dancer.Height`, .24 of
the frame), Outline (`reforce.Dancer.Outline`, 2 px), Human (`reforce.Dancer.Human`, 1: each dancer's own
timing, size of moves and posture; 0 = machine-exact), Hair motion (`reforce.Dancer.Hair`, 1: hair physics, hanging hair bends,
trails and keeps out of the body, short and big hair bounces a little; 0 = the classic swing; 2 = looser; `beatset dancehair`; long hair bends too, as three
columns that swing out on spins and are pushed by the arms, the curls riding on them: `reforce.Dancer.LongHair` in
settings.json, 1, no panel control (Peter, 2026-11-27); 0 = rigid, the tube round the head turning whole as before, to
compare; `beatset dancelonghair on|off`), Mocap moves (tries the moves recorded from real dancers, from AIST++,
one by one: `beatset dancetry ID|off`; the dancers use only these: `reforce.Dancer.Mocap` in settings.json, 1, is the
share of routines that are mocap moves, 0 = the hand-made ones; no control on the card; break's floor moves are
in the list as their own entry, **break floor** (low work on the hands, knees and hips); break dancers mix them in by
themselves, as ordinary break moves: `reforce.Dancer.FloorMoves` in settings.json, 0..4, default 1 = as often as a
standing break move, 0 = never; a floor move waits for room (never gives up): neighbors step back, the dancer stays put,
and if there's no room yet it starts on the next 4- or 8-bar point; docs/dancers-design.md "Break floor work", "Floor
moves held still, room to go down"), **Bust a move** on each dancer in the list (from their next 8-bar point two
whole break routines with floor work, 32 bars, whatever their style, then their own moves again; the ones they were
dancing with make room and watch; `beatset dancebust ID|NAME|#N`; stream guests have the same button on the guest
page), name tags: always over stream guests, never over generated dancers (no control; `reforce.Dancer.NameTags` is no
longer read), Stream guests (off by default; set up a guest service, [guests.md](guests.md):
viewers who signed up on your guest page and tap "I'm here" join the floor for `reforce.Guests.Minutes`, 30; every guest's latest dancer is kept in `Guests/` next to settings.json, and the card's **Past guests** list brings one back by hand to dance until sent off or Clear (`beatset guestcall ID|NAME`; `reforce --import-guests FILE.json` adds guests from the service's database);
Hide dancers (beatset `dancehide`, not saved) fades the stage out while it keeps going;
docs/dancers-design.md). Also `Dancer.Slideshow.Interval.Duration` (0 = none),
`Dancer.Duration` (90 s), `reforce.Dancer.MaxLive`, `reforce.Dancer.Msaa`.

**Characters** are scene files in `content/Packages/reforce.Dancer.package/` (a `Random <Style>` file per style, no named characters; keys in
docs/scene-format.md §5.9): colors, `HairStyle`, `Outfit`, `Hat`, `Build`, `Tall`, `Style`, `Moves` (favorite
moves), `Snap`. Test hook: `--dancer-test pop,hiphop,disco` (generated characters, the same each run, from the first frame; `pop@afro` forces a
hairstyle, to look at one; `pop@bob@cap` a hairstyle and a hat); `--name-tags` puts a name tag over every dancer, generated
ones too (normally only guests have one), for trying things.

## Rhythm: phrases, kick zoom, drops (PLAN 5O)

The control panel's **Rhythm** section (design: docs/rhythm-design.md):

- **Phrase strip**: the bars of the current phrase (`reforce.Phrase.Bars`, 16). Phrase lines are found by themselves
  from loudness changes (**Auto-align**), or tap **Phrase starts now** on the first beat of a phrase (‹ bar / bar ›
  nudge). Scenes read `SYNC_BAR` and `SYNC_PHRASE`.
- **Deck starts** (with rkbx_link and a DJ controller that has a map in `content/Controllers/`, e.g. the DDJ-1000):
  each song you bring in sets the phrase lines: its last start
  (Play, Cue, a hot cue) counts once its channel fader is past a third (`reforce.Phrase.DeckStarts`,
  `.FaderUp` .33, `.RkbxPort` 4470; `.MidiDevice`: only MIDI sources whose name contains this, default any source a
  map matches, `"none"` for no MIDI). Your own maps go in `~/Library/Application Support/reforce/Controllers/`
  (format: src/sync/ControllerMap.hpp, or copy `content/Controllers/Pioneer DDJ-1000.txt`). Loops and beat jumps keep the phrase going. The line
  under the buttons shows each deck (▶ / ■, fader up / down, ★ = the start in use).
- **Slideshow changes / Manual switches / New particles, sprites, solids, organics on**: off, beat, bar or phrase
  (`reforce.Link.Quantize`, `.QuantizeManual`, `reforce.Quantize.Spawn`).
- **Kick zoom**: the FlowField pushes out (and turns) for a moment on every beat (`reforce.Kick.Zoom`, `.Rotate`,
  `.DecayMs`; 0 = off).
- **Drops**: breakdown / build / drop status; scenes read `SYNC_BUILD` and `SYNC_DROP`; **On a drop: change scenes**
  cuts FlowField, WaveShape and ColorMap (`reforce.Drop.Switch`).

Commands: `beatset:phrasenow`, `beatset:quantize:phrase`, `beatset:kickzoom:0.01`, ... (web: `{"action":"beatset",
"key":K,"value":V}`).

## Slideshow timing in the control panel

Every Interval, Transition and Duration slider in the control panel is **saved to settings.json** when you let go,
so it survives a restart (a failed save says so; the change still applies).

- FlowField, WaveShape, ColorMap: `Interval` and `Transition` (`<Type>.Slideshow.Interval.Duration`,
  `<Type>.Transition.Duration`).
- Particles: `Interval` (seconds from one slideshow start to the next, ±10 %; **0 = off**) and `Duration` (seconds
  at full strength, `Particle.Duration`; a config's own PDur wins).
- **Image sprites** card: `Interval`, `Duration` and `Mix` (`Sprite.Slideshow.Interval.Duration`, `Sprite.Duration`,
  `reforce.Sprite.Mix`), Start next / Clear for images only, and a library of the images.
- **One feature at a time** (`reforce.Slideshow.OneFeature`, 1): the slideshow starts a solid, an organic or a movie
  sprite only while none of the three is at full strength; when the one showing begins its fade-out, whichever timer
  has been due longest starts (so they crossfade, never pile up). Manual starts are never held back. 0 = each on its
  own timer.
- **Movie sprites** card: movie sprites have their own shuffle and timer: `Interval`
  (`reforce.MovieSprite.Slideshow.Interval.Duration`, default = the sprite interval; 0 = no movies from the
  slideshow), `Mix` (`reforce.MovieSprite.Mix`), Start next / Clear for movies (and live video sprites), Loop until stopped, and a library of the
  movies. Movies that play once last their own length. One movie (or live video) sprite at a time
  (`reforce.MovieSprite.MaxLive`, 1; 0 = no limit): a new one fades the old one out under it, even one looping until
  stopped, and a due slideshow movie waits until the playing one fades out. Command text: `interval:sprite:SEC`, `life:sprite:SEC`,
  `movieinterval:sprite:SEC`, `nextmovie:sprite`, `kill:sprite:images|movies`.
- **Objects** card (3D solids and organics): `Interval` (`Object.Slideshow.Interval.Duration`, default 0 = none; a
  theme that sets its own, like the Objects themes, wins while it is on) and `Duration` (`Object.Duration`, 40 s),
  Start next / Clear, the alive list and the library by group. Command text: `interval:object:SEC`, `life:object:SEC`.
- **`reforce.SpriteReplace`** = `own` (default) | `any`. A new EPen-only sprite fades out the running EPen-only
  sprites (probe #44). With `own` only those of its kind: images replace images, movies replace movies. `any` is
  G-Force: any of them.

The **FlowField Union** card's **Clear** (command `kill:union`) cancels a pending Union and, while a Union shapes
the field (during its transition, or the blend G-Force keeps afterwards, probe #45), morphs to the plain current
FlowField over the quick transition. The Union timer then starts a fresh interval.

The libraries on every card (FlowField, WaveShape, ColorMap and the extra layers) start collapsed; click
**Library** to open one.

## Favorites (top-level buttons in the control panel)

Click the ☆ in front of any scene in the control panel's lists (FlowFields, WaveShapes, ColorMaps, Unions,
Particles, Sprites) to make it a favorite; it gets a button in the ★ bar at the top of the panel. A sprite button
starts that sprite (quick fade-in), the others switch their layer to it. Click ★ again to remove it. A favorite whose
scene is gone (removed or renamed) shows grayed out and dashed in the bar, with a ✕ that removes it. Each button
shows its type as a small emoji (🌀 FlowField, 〰️ WaveShape, 🎨 ColorMap, 🔀 Union, ✨ Particles, 🖼️ image sprite, 🎬
movie sprite, 🧊 Object, 💃 Dancer; the full name is in its tooltip), and the bar stays at the top under the top bar
while you scroll (on a phone it is one line that scrolls sideways). Saved in
settings.json as `reforce.Favorites` (`layer:id; layer:id`, e.g. `sprite:External/images/  dj peter henry`).
The Sprites list also shows the extra sprite folder (`reforce.ExternalSpritesDir`, ids `External/...`).

## Skip list (scenes reforce never uses)

`reforce.SpriteAlpha` (settings.json, next sprite start): `black` (default) treats a sprite image's transparent pixels
as black, so they are not drawn; `ignore` matches G-Force, which ignores the alpha channel (probe #42) and shows
whatever color is stored under transparent pixels.

`reforce.Exclude` in settings.json lists scenes reforce ignores completely: not in the slideshow, the control
panel's lists, the video picker or manual switches. The files stay where they are, so a scene sync doesn't undo it.
Entries are separated by `;`, each `Type:name` or just `name` (any type); name is the id (`Space/Moon`) or the file
stem; types WaveShape, Particle, FlowField, Union, ColorMap, Sprite. Example (Peter's):

    reforce --write-setting "reforce.Exclude=Sprite:Space/Moon; Sprite:Space/Astronaut; Sprite:G-Force Logo"

### Hidden image sprites (Review images)

The control panel's Image sprites card has **Review images…**: a grid of every image sprite, by folder. Click an
image (or Space on it) to hide it; click again to bring it back. **Hide all** / **Show all** work per folder, and
▶ (or P) shows one on screen now. A hidden sprite never comes up: not in the slideshow, the Library list, the
favorites bar or a manual start, and one on screen fades out when you hide it. Changes apply at once and are
saved to settings.json as `reforce.Sprite.Hidden` (exact ids such as `Abstract/Rose` or
`External/images/3-a4`, separated by `;`). Sprites in `reforce.Exclude` are not in the grid at all.

## Ableton Link (beat sync with rekordbox)

reforce joins an Ableton Link session on the local network automatically. In rekordbox, switch to Performance
mode and turn **LINK** on; other Link apps (Ableton Live, Traktor, …) work the same way. The control panel's header
shows **LINK** with the session tempo, peer count and four dots for the position in the bar (the red one is the bar line).
macOS asks once to allow reforce on the local network.

With a session:
- Scenes get `LINK_ON`, `LINK_BPM`, `LINK_BEAT` and `LINK_PHASE` (docs/scene-format.md §3.5), and `BEAT`/`TEMPO`
  follow the DJ's beat grid instead of the audio estimate.
- Slideshow changes start on a bar line; manual switches (keys, control panel) wait for the next beat.
- Transitions can be measured in beats.

| Setting | Default | |
|---|---|---|
| `reforce.Link.Enabled` | 1 | join Link sessions (`--no-link` for one run) |
| `reforce.Link.Follow` | 1 | join only once another Link app (rekordbox) is running, so reforce takes its tempo. In Link the longer-running session wins, so with 0 a reforce started first sets rekordbox to 120 BPM |
| `reforce.Link.OffsetMs` | 0 | sample the session this much ahead, to make up for display/projector latency (+ = visuals earlier) |
| `reforce.Link.Quantum` | 4 | beats per bar |
| `reforce.Link.DriveBeat` | 1 | `BEAT`/`TEMPO` follow Link while a session runs |
| `reforce.Link.Quantize` | `bar` | slideshow changes wait for the next `bar` / `beat` line, or `off` |
| `reforce.Link.QuantizeManual` | `beat` | manual switches wait for the next `bar` / `beat` line, or `off` |
| `reforce.Link.TransitionBeats` | 0 | > 0: transitions (not quick ones) last this many beats; 0 = the usual seconds |

### Beat-synced movies (dance clips)

A movie whose `.reforce.txt` has `BPM` / `BeatStart` / `BeatsPerBar` plays locked to the beat, as a sprite or on the
color video layer. Those keys are written
by `tools/dance_sync` (see its README), which finds the tempo from the dancers' motion.

**The beat clock:**
- With a Link session, the clock follows Link, bar lines included.
- Without one, it follows the audio: the analyzer's tempo, plus a beat phase.
  - The phase comes from a filter that weighs each hit by how beat-like it sounds and by the pattern of the last
    ~12 s folded onto one beat (kick, hats, off-beat bass, sidechain dips). Both were learnt from hand-gridded DJ
    tracks (`tools/beat_eval`), so a strong off-beat bass or pumping track no longer pulls it half a beat off.
  - The clock only reports confident when that evidence agrees; with none it falls back to the strongest onsets,
    and equally strong on- and off-beat hits give no lock, rather than a guess.
  - How it works and how it is measured: [docs/beat-detection.md](beat-detection.md).
  - The audio tempo is always folded into 80–160 BPM (a half-time 62 reads 124).
- **Steady tempo** (`reforce.BeatClock.SteadyTempo`, on): made for house and other music with a fixed tempo.
  - Once found, the tempo only changes when the music clearly says so: drift and small changes (±4 %, pitch fader)
    are followed, a different tempo must win for ~8 s (124 → 128 read in ~6 s, 124 → 100 in ~12 s).
  - Breakdowns: when the kick goes (30-120 Hz hits fall below 30 % of the track's recent level) the tempo is frozen
    and the beat clock just keeps counting, for as long as the breakdown lasts; only 3 s of silence ends it. Tempo
    changes need the kick. While the kick plays, the clock learns the exact tempo from the kicks, so it stays on
    the beat through long breakdowns (tested: 90 s, within 0.002 beat) and the drop lands on it.
  - A kick coming back off the old grid (an unmatched mix) is re-acquired after 8 kicks.
  - Off (`0`, restart): the quick-reacting tracker from before (jumps in breakdowns, drops after 4 s).

**Playback:**
- The movie plays at `clock BPM / movie BPM`. It picks ½×, 1× or 2× of the movie's beats per music beat, whichever
  puts the rate nearest 1, and only syncs within 0.5–2×.
- It starts in phase, from a downbeat: the first one at or after `Start`.
- Tempo changes and phase corrections show as small speed changes, never jumps.
- The played section is cut to a whole number of bars, so loops keep the phase and jump back on "the 1".
- A movie that plays once lasts as long as it takes at the synced rate (slower = longer), and still fades out
  as it ends.
- Without a confident clock, the movie plays at its normal speed.

**Interpolated copies:** a frame-interpolated copy beside a movie (`<stem>.rife4x.mov`, from
`tools/dance_sync/interpolate.py`) plays in its place, so slow rates stay smooth. Copies are never listed as movies of
their own.

**Control panel:** the Movie sprites card has a **Beat sync** toggle and shows the clock (`Link 128.0`,
`audio 99.9`, `(not locked)`).

| Setting | Default | |
|---|---|---|
| `reforce.Video.BeatSync` | 1 | movies with a `BPM` play beat-synced, sprites and color layer (panel toggle, saved) |
| `reforce.BeatClock.Source` | `auto` | `auto` (Link, else audio) / `link` / `audio` / `off` |
| `reforce.BeatClock.AudioOffsetMs` | 0 | audio onset detection latency (+ = the audio clock's beats earlier) |
| `reforce.BeatClock.OffsetMs` | 0 | beats this much earlier (−200..500 ms), for audio / display latency: Link sampled ahead, the audio clock's count shifted. Top bar **Beat ‹ ›** nudge (5 ms, Shift 1 ms, double-click 0), saved |
| `reforce.BeatClock.SteadyTempo` | 1 | audio tempo and beat lock assume a steady tempo (breakdowns hold it); read at startup |

`build/reforce-link-peer --bpm 124` is a minimal Link peer for testing without rekordbox. The Link library is
GPL-2.0-or-later (fine for this private project; distributing reforce builds would make reforce GPL too).

## Audio analysis

The analyzer (`src/audio/Analyzer.*`) is fitted to measurements of G-Force 5.9.7 (Phase 5C): `mag()` is the raw
sample, `fft()` has 255 broad, adaptively normalized, log-spaced bins from 120 Hz to 5.5 kHz, `LEVEL` is ~1.3 × the
mean of `fft()`, `BASS` = 4 × `LEVEL`, and `ENERGY` follows `LEVEL` slowly. The prefs `Audio.FFT.Smooth`,
`Audio.FFT.FreqLo` / `FreqHi`, `Audio.FFT.NumBins.vis`, `Audio.PCM.NumBins`, `Audio.Response.Scale` and
`Audio.FFT.Params` (`Envelopes`, `InitialVolume`, `PersistCoef`) are honored. Measurements and the model:
[docs/audio-probe-results.md](audio-probe-results.md); G-Force vs reforce scores:
[docs/audio-parity.md](audio-parity.md) (`python3 -I tools/parity/audio_compare.py --all`, also run by ctest as
`reforce_audio_parity` when the local G-Force captures exist).

**reforce extensions.** Scenes can also use `MID` (250 Hz–2 kHz) and `TREBLE` (2–16 kHz), both normalized to
their own recent peak like a meter (~0..2), `BEAT` (a 0..1 pulse at each detected beat, decaying in ~0.1 s) and
`TEMPO` (BPM, 0 when unsure). G-Force has none of these, so G-Force scenes are unaffected. The web UI header shows
small BASS / MID / TREBLE / BEAT meters and the BPM. Details: [docs/scene-format.md](scene-format.md) §3.5.


## G-Force themes

Themes: reforce's own (`content/Themes/`) are listed first, then G-Force's seven (the scene root's `Themes/`) as
"g-force: NAME". A G-Force theme's bare title still works anywhere a theme is named (`Theme.StartupTheme`,
`--theme`, the web API), so settings saved before the prefix and G-Force's own prefs keep working.

## Dependencies

All dependencies are fetched by CMake `FetchContent` at configure time and pinned (see
[cmake/Dependencies.cmake](../cmake/Dependencies.cmake)):

| Dependency | License | Pin |
|---|---|---|
| [metal-cpp](https://developer.apple.com/metal/cpp/) (macOS 26 / iOS 26 release) | Apache-2.0 | `metal-cpp_26.zip` from developer.apple.com, verified by SHA-256 |
| [Syphon-Framework](https://github.com/Syphon/Syphon-Framework) | BSD-2-Clause | commit `f4761677` (2026-09-21; no release tag has the Metal server) |
| [doctest](https://github.com/doctest/doctest) (tests only) | MIT | v2.5.3, commit `2d0a9359` |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) (control panel server; pinned in cmake/ReforceWeb.cmake) | MIT | v0.65.0, commit `7e5d0683` |
| [Ableton Link](https://github.com/Ableton/link) (+ bundled asio-standalone) | GPL-2.0-or-later (Ableton also sells a proprietary license) | Link-4.1.1, commit `5ba907ae` |

Apple publishes metal-cpp only as a zip on its website, which could change at the same URL.
If it does, the SHA-256 check fails loudly instead of silently building against different
headers.

**Syphon without Xcode.** Syphon's own build needs `xcodebuild` and the offline Metal
compiler. [third_party/syphon/CMakeLists.txt](../third_party/syphon/CMakeLists.txt) builds its
sources as a static library instead (Objective-C, ARC). It makes one source change: upstream
`SyphonServerRendererMetal.m` loads a precompiled `default.metallib` from the framework
bundle, so [third_party/syphon/SyphonServerRendererMetal.m](../third_party/syphon/SyphonServerRendererMetal.m)
replaces it and compiles the upstream `SyphonMetalShaders.metal` at runtime. CMake embeds that
source at build time.

Offline builds: set `-DFETCHCONTENT_SOURCE_DIR_METAL_CPP=…`, `-DFETCHCONTENT_SOURCE_DIR_SYPHON=…`
and `-DFETCHCONTENT_SOURCE_DIR_DOCTEST=…` to point at local copies.

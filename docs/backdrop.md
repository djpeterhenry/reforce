# Camera backdrop

A camera (or a Syphon server, or a movie) drawn **under everything** in reforce. It shows through exactly where the
[Syphon alpha](syphon-alpha.md) would let OBS show a layer behind reforce, so it replaces the old OBS setup of a
webcam source under a transparent reforce source. [Body tracking](body.md) follows the person on it.

Web UI: **Camera backdrop** card (above Video layer).

## What it does

After the visuals are colorized (with the alpha tracked; ColorKey = 0 movies included) and before the dancers and
now-playing text, one in-place pass mixes the backdrop in; the dancers, their floor and shadows and the text then draw
over it as usual:

```
a   = opaque + (1 - opaque) * key(colour)     # exactly the Syphon alpha (OBS Color Key on black)
out = colour * a + backdrop * opacity * fade * (1 - a)
alpha = 1
```

- The key uses the same `reforce.Syphon.KeySimilarity` / `KeySmoothness` (also on the backdrop card). ColorKey = 0
  movies stay solid, and the dancers, their floor and now-playing text are drawn on top, as they were in OBS.
- **While the backdrop is on, the frame is opaque.** The window and Syphon both show the composite, and the Syphon
  alpha pass is skipped. In OBS: hide the camera source (Allow Transparency no longer matters).
- The mix happens on the stored (sRGB-encoded) values, straight alpha, like OBS's default compositing.
- Turning it on, off, or switching sources fades over 0.4 s. A new source fades in once the old one has faded out and
  its first frame is in. While a source is set but has no frame yet (opening, waiting for camera permission), the
  see-through parts are black.

## Window shows the visuals only (room lights)

`reforce.Window.VisualsOnly = 1` (View menu: Window Shows Visuals Only (for Lights); web control panel: the
"Visuals only" button in the top bar, lit when on): the reforce window shows the visuals alone, with the colour movie layer and movie sprites, but no
backdrop, dancers, name tags, bubbles or now-playing text. Syphon (OBS) is unchanged and still gets everything. It is
for screen mirroring (Nanoleaf Desktop on the second monitor): the room lights follow the visuals, as they did with
G-Force, not the camera or the dancers. `Visualizer::colorize(..., visualsCopy)` copies the frame right after the
visuals pass into the window's own frame (one blit); when the video layer is kept out of Syphon, the window's own pass
stops there instead (`overlays` false). Command: `videoset windowvisuals on|off`.

Counts as visuals (in the window): scenes (wave shapes, particles, organics, flow fields, feedback), 3D solids, image
sprites, movie sprites (luminance and full colour), the colour movie layer (movies, or a camera / Syphon source played
on it), live sprites (a camera as a sprite too), and body tracking drawn into the visuals (hand trails, burn).
Left out: the backdrop with body Front and Show, the dancers and their floor, name tags, bubbles, now-playing text.

## Settings (settings.json)

| Key | Default | |
|---|---|---|
| `reforce.Backdrop.Source` | `""` (off) | `camera:Logitech BRIO` (cameras by name: a USB camera's unique id changes with the port), `syphon:...`, or a movie. Opened again at startup. |
| `reforce.Backdrop.Mirror` | `0` | flipped left-right |
| `reforce.Backdrop.Fit` | `cover` | `cover` / `contain` / `stretch` |
| `reforce.Backdrop.Opacity` | `1` | "Brightness" on the card: fades the backdrop towards black |

Commands (`/api/command`, `{"action":"videoset","key":...,"value":...}`): `backdrop` (a source or `off`),
`backdropmirror`, `backdropfit`, `backdropopacity`.

Cameras and Syphon are refused in deterministic runs (`--deterministic`).

## Where it happens

- `ScenePipeline::encodeBackdrop` / `backdrop_fs`: the pass. It reads the frame in place (framebuffer fetch).
- `Visualizer::setBackdrop`, `stepBackdrop` (fade, source swap), `encodeBackdrop` (newest frame, fit, mips).
- `Visualizer::colorize(..., backdrop)`: the visuals with trackAlpha, the backdrop, then the dancers and text (no
  alpha tracking: the frame is already opaque). `App::frame` asks for it on the Syphon frame and (when the video layer
  is kept out of Syphon) on the window's own frame.
- Tests: `tests/test_video.cpp` (the mix, no-video black, mirror), `test_web_api.cpp`, `test_engine_m6.cpp`.

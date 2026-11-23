# Room lights (Nanoleaf)

reforce drives Nanoleaf panels directly over the local network, in place of the Nanoleaf Desktop app's screen
mirroring. The panels follow **the visuals only**: scenes, solids, sprites, the color movie layer and movie sprites,
body trails and burn. Never the camera backdrop, dancers, name tags, bubbles or now-playing text. That holds whatever
the window shows (see "Window shows the visuals only" in backdrop.md for the screen-mirroring route).

Turn it on: View > Room Lights (Nanoleaf), or the **Lights** button in the control panel's top bar ("Lights …" while
connecting, "Lights ⚠" when something is wrong: the reason is in its tooltip). Quit the Nanoleaf Desktop app first:
both would fight over the panels.

## How it works

- `render/LightsCapture`: at most 30 times a second, the frame right after the visuals pass (before the backdrop,
  dancers and text; the pass with the color video layer) is copied into a mipmapped texture, and one mip level about
  120 pixels wide is read back into a shared buffer: no render-thread wait, no per-frame allocation.
- `lights/LightMap`: the panels' layout (Nanoleaf: x right, y up) is stretched over the frame; each panel takes the
  area under it (a disc about a panel across). Bright pixels decide the color (weight 0.002 + level²); the panel's
  level is the square root of the area's mean level (thin bright lines still light it), times Brightness, at least
  Floor; saturation is scaled around the color's gray. Smoothing rises 6x faster than it falls.
- reforce quits cleanly on SIGTERM / SIGINT / SIGHUP too (platform/mac/MacApplication), so a `kill` also puts the
  panels back; only a force quit (SIGKILL) or a crash leaves them on the last colors.
- `lights/LightsOutput`: its own thread. Reads the state and layout (`GET /api/v1/<token>/`), keeps on/off and the
  effect, turns the panels on and switches them to external control v2 (`PUT /effects`, `extControl`, `v2`), then
  sends a UDP packet (port 60222) 20 times a second with each panel's color and a 100 ms fade. Every 10 s it checks
  the panels still answer; when not, it reconnects every 5 s. Off or quit: the saved effect is selected again (when
  it is a saved one: "*Dynamic*" and "*Solid*" cannot be) and on/off put back.

## Settings (settings.json)

| Key | Default | |
|---|---|---|
| `reforce.Lights.Enabled` | `0` | drive the panels |
| `reforce.Lights.Brightness` | `1` | 0..2 |
| `reforce.Lights.Saturation` | `1.3` | 0..3, 1 = as on screen |
| `reforce.Lights.Floor` | `0` | 0..0.5: never darker than this |
| `reforce.Lights.Smoothing` | `0.3` | 0..2 s: how slowly a panel falls back |
| `reforce.Lights.Pulse` | `0` | 0..1: flash on each beat, fading until the next (settings only, see below) |
| `reforce.Lights.PulseTimingMs` | `40` | -100..300: the flash this much early |

Commands: `videoset lights on|off`, `lightsbrightness`, `lightssaturation`, `lightsfloor`, `lightssmoothing`,
`lightspulse`, `lightspulsetiming`, `lightspair start|cancel`.

## Control panel

The **Room lights** card: status, the panels as they hang in the colors last sent (the layout's own shapes: a
triangle points up at 0°, down at 60°), Turn on/off, **Pair…** (finds the panels: the paired host, else Bonjour
`_nanoleafapi._tcp`; then asks for a token every 2 s for a minute while the power button is held), and sliders for
Brightness, Saturation, Floor and Smoothing.

## Beat pulse (off; not in the control panel)

With Pulse > 0 and a confident beat clock (Link, or the audio locked), every beat gets an instant full-level packet
sent exactly on the beat (the clock's count extrapolated, less PulseTimingMs), and the packets between fade the
level towards 1 - Pulse. reforce's side is regular (tests: one flash every 500 ms at 120 BPM, within 5 %), but on
Peter's Light Panels it still looked irregular (2026-11-21): the panels' Wi-Fi delivers packets unevenly (ping 4-64
ms, standard deviation 18 ms, against 3-6 ms to the router), so a flash lands anywhere in ~30 ms. Kept for a wired
or steadier setup; the colors themselves don't suffer, as they change smoothly.

The device and its token: `nanoleaf.json` next to settings.json (`{"host", "port", "auth_token"}`; private). Pairing:
hold the controller's power button 5-7 s until its lights flash, then `POST /api/v1/new` within 30 s
(`lights::pairNanoleaf`).

Offline renders never drive the lights; a `--background` test instance only with `--lights` (and its own
nanoleaf.json next to its `--settings`).

Tests: `tests/test_lights.cpp` (mapping, packet, parsing, settings, a fake Nanoleaf over HTTP + UDP on localhost).

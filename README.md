# reforce

A real-time music visualizer for macOS. reforce is a from-scratch engine that runs
[G-Force](https://www.soundspectrum.com/g-force/) scene files (WaveShapes, FlowFields, ColorMaps, Particles, Sprites)
and adds its own layers on top: 3D solids, organics (procedural growth), dancing 3D characters driven by motion
capture, video and camera input, and beat sync with DJ software. It renders with Metal into a window and publishes
the picture over [Syphon](https://syphon.github.io/) for OBS and other video apps. A web control panel runs the show.

reforce is an independent project, not affiliated with or endorsed by SoundSpectrum. It contains no G-Force code or
content: it reads the scene files of a G-Force you have installed yourself (see below).

## Requirements

- macOS 14+ (Apple Silicon or Intel with Metal)
- Apple Command Line Tools (Apple clang with C++20). **Xcode is not required**: shaders are compiled at runtime.
- CMake ≥ 3.25
- Network access on the first configure (CMake fetches pinned dependencies)

## Build and run

```sh
cmake -S . -B build                 # fetches pinned deps into build/_deps
cmake --build build -j
ctest --test-dir build              # unit tests (doctest)
open build/reforce.app
```

To install it to `~/Applications` so it launches from the Dock: `scripts/install-app.sh`. Press `Cmd+K` in the app to
open the control panel in your browser. The full user guide (keys, settings, every feature) is
[docs/usage.md](docs/usage.md).

## Using your G-Force scenes

G-Force is commercial software from SoundSpectrum. To use its scenes with reforce:

1. Buy and install G-Force from [soundspectrum.com](https://www.soundspectrum.com/g-force/).
2. Run reforce. It finds the scenes in `/Library/Application Support/SoundSpectrum/G-Force` by itself and reads them
   in place. It never writes there and never changes G-Force's preferences.

To use a different folder, pass `--scenes DIR` or set `reforce.ScenesDir` in
`~/Library/Application Support/reforce/settings.json`. `reforce --import-gforce-prefs` copies your G-Force preferences
(slideshow timing, audio response and so on) into reforce's settings once, reading G-Force's file without changing it.

**Without G-Force**, reforce still runs: the base layer is a single built-in WaveShape, FlowField and ColorMap, with
reforce's own solids, organics, dancers and themes from [content/](content/) on top. Original reforce WaveShapes,
FlowFields and ColorMaps for `content/` are very welcome contributions.

Never commit G-Force's files to this repository: they are SoundSpectrum's. The git-ignored `scenes/` folder
(`scripts/sync-scenes.sh`) holds a local copy for the comparison tools only.

## Optional inputs and outputs

Everything here is optional. reforce runs without any of it, and without a camera.

| What | How it connects | Setup |
|---|---|---|
| Audio | Any macOS input device (a Loopback / BlackHole device for system audio) | Audio menu; macOS asks for microphone access once |
| OBS and video apps | Syphon server `reforce`, with transparency | [docs/usage.md](docs/usage.md#output-window--syphon-for-obs) |
| Cameras, movies, Syphon input | Colour video layer, movie sprites, a camera backdrop, body tracking | macOS asks for camera access the first time one is opened |
| Ableton Link | Tempo and bar sync with rekordbox, Ableton Live, Traktor | Automatic on the local network |
| rekordbox decks | Track positions and titles from a "reforce" output module for [rkbx_link](https://github.com/grufkork/rkbx_link) (UDP port 4470; the module is not in rkbx_link yet), plus a DJ controller's faders over MIDI, mapped by a text file in [content/Controllers](content/Controllers/) (Pioneer DDJ-1000 included) | [docs/rhythm-design.md](docs/rhythm-design.md) |
| Stream guests | Viewers check in on your own guest page and dance on stage with a name tag | Off by default; [docs/guests.md](docs/guests.md) |
| Nanoleaf Light Panels | Each panel follows its part of the picture | Pair from the control panel; [docs/lights.md](docs/lights.md) |

## Documentation

- [docs/usage.md](docs/usage.md): the user guide
- [docs/scene-format.md](docs/scene-format.md): the scene file format, including reforce's additions
- [docs/architecture.md](docs/architecture.md): how the engine is put together
- Design notes for each part: [3D solids](docs/3d-design.md), [organics](docs/organics-design.md),
  [dancers](docs/dancers-design.md), [rhythm](docs/rhythm-design.md), [beat detection](docs/beat-detection.md),
  [camera backdrop](docs/backdrop.md), [body tracking](docs/body.md), [lights](docs/lights.md),
  [stream guests](docs/guests.md)

## Repository layout

```
src/app/         main loop, Director (scene scheduling), Visualizer (the frame), command line
src/audio/       audio input, analysis (G-Force-compatible), beat tracking
src/expr/        the scene expression language: parser, compiler, VM
src/scene/       scene files: parsing and evaluation
src/engine/      layers and their state: particles, sprites, solids, organics, dancers, themes, settings
src/render/      Metal pipelines (metal-cpp), shaders compiled at runtime
src/video/       movie files, cameras, Syphon input (AVFoundation)
src/sync/        Ableton Link, phrase clock, rekordbox deck feed and DJ controller MIDI
src/lights/      Nanoleaf output
src/web/         control panel: HTTP server, JSON API, the UI (embedded in the app)
src/platform/    Cocoa window, display link, Syphon output
content/         reforce's own scene files, themes and dance moves (bundled into the app)
tests/           doctest unit tests
docs/            user guide, format spec, design notes
scripts/         install, icon, G-Force scene sync, publish (maintainer: private repo -> public)
tools/, probes/  development tools; see below
```

Some of the repository is the maintainer's own workshop, kept in the open:

- [PLAN.md](PLAN.md) is the development journal: phases, decisions and what is next.
- `probes/`, `tools/parity`, `tools/sweep` and `tools/perfcompare` compare reforce with a running G-Force. They need
  a licensed G-Force and write test scenes into its folder while they run; you don't need them to build or
  contribute.
- `tools/story` and `tools/media_review` are personal side tools (a story page about the project, a movie review
  page); `tools/dancer_web` builds the dancers for the browser (WebAssembly). `tools/aist_import` rebuilds `content/Moves/aist.moves` from the AIST++ dataset.

## Contributing

Pull requests are welcome: new scenes and themes in `content/`, mappings for other DJ controllers, other light
outputs, fixes and features. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

reforce is licensed under the [GNU General Public License v3.0](LICENSE) or (at your option) any later version.

Third-party parts keep their own licenses:

| Part | License |
|---|---|
| Dance moves in `content/Moves/aist.moves`, from the [AIST++](https://google.github.io/aistplusplus_dataset/) annotations (Li et al., Google Research, 2021), retargeted and cut ([content/README.md](content/README.md)) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| [Ableton Link](https://github.com/Ableton/link) (+ bundled asio-standalone) | GPL-2.0-or-later |
| [Syphon-Framework](https://github.com/Syphon/Syphon-Framework) (built by [third_party/syphon](third_party/syphon/)) | BSD-2-Clause |
| [metal-cpp](https://developer.apple.com/metal/cpp/) | Apache-2.0 |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | MIT |
| [doctest](https://github.com/doctest/doctest) (tests only) | MIT |

All dependencies are fetched and pinned by CMake; see [cmake/Dependencies.cmake](cmake/Dependencies.cmake) and
[docs/usage.md](docs/usage.md#dependencies). G-Force is a product of SoundSpectrum, Inc.; its name is used here only
to say which scene format reforce reads.

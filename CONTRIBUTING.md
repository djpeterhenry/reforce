# Contributing to reforce

Thanks for wanting to help. Pull requests of every size are welcome, from one new scene file to a new feature.

## Good places to start

- **Scenes and themes.** reforce's own scene files live in [content/](content/): solids in
  `Packages/reforce.Solid.package/`, organics in `Packages/reforce.Organic.package/`, dancers in
  `Packages/reforce.Dancer.package/`, themes in `Themes/`. They are plain text; the format is in
  [docs/scene-format.md](docs/scene-format.md). Original WaveShapes, FlowFields and ColorMaps are especially welcome:
  without G-Force installed, reforce has only one built-in scene of each.
- **DJ controllers.** A map for your controller is one text file in `content/Controllers/`: copy
  `Pioneer DDJ-1000.txt`, change `Match` to part of your controller's MIDI name and the numbers to its faders,
  crossfader and pads (its MIDI documentation, or rekordbox's `.midi.csv` for it). The format is described in
  `src/sync/ControllerMap.hpp`. Put it in `~/Library/Application Support/reforce/Controllers/` to try it without a
  rebuild.
- **Light outputs** other than Nanoleaf (`src/lights/`).
- **Bugs.** An issue with steps to reproduce, your macOS version and Mac model, and the log
  (`~/Library/Logs/reforce/reforce.log`) helps a lot.

For larger changes, open an issue first to talk the idea through.

## Build, test, preview

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build
```

GitHub Actions runs the same build and tests on every pull request (`.github/workflows/ci.yml`). Its GPU is virtual,
so time budgets in the tests (`tests/PerfBudget.hpp`) are reported there but not enforced.

A development build reads `content/` straight from your checkout, so a scene file edit shows on the next start
without a rebuild. To try a scene, start it from the control panel (`Cmd+K`), or render it offline to PNGs:

```sh
build/reforce.app/Contents/MacOS/reforce --no-settings --no-audio --deterministic \
    --render-frames 300 --save-every 30 --out /tmp/frames --command "0=switch:object:Icosahedron Drift"
```

(`switch:organic:NAME`, `switch:dancer:NAME` and so on for the other layers; `--audio-file song.wav` drives it with
music.) `--background` runs a second, windowed copy that never takes focus, if you also have reforce running.

## Ground rules

- **No G-Force material.** Don't commit G-Force's scene files, images, docs or anything copied or adapted from them:
  they belong to SoundSpectrum. reforce treats G-Force only as a black box (run it, look at the output); don't
  decompile or disassemble it. Scene files you write yourself are fine, of course.
- **Only content you have the right to share.** Images, movies, motion data and so on need a license that allows
  redistribution under the GPL; say where they come from in the pull request and in `content/README.md`.
- **G-Force compatibility stays the default.** When reforce draws a G-Force scene, it aims to look like G-Force. If you
  want it to look better in a way G-Force doesn't, add it as an option rather than changing the default.
- **Optional things stay optional.** reforce must run without a camera, controller, lights, network or G-Force.
- **Code.** C++20, following the style of the code around your change. The build uses `-Wall -Wextra`: please add no
  new warnings. Pure logic gets doctest tests in `tests/`.
- **Visual changes:** a screenshot or short clip in the pull request helps review a lot.

## Sign your commits (DCO)

reforce uses the [Developer Certificate of Origin](https://developercertificate.org/): by signing off a commit you
state that you wrote it, or otherwise have the right to submit it under the project's license. Add the sign-off with
`git commit -s`, which appends:

```
Signed-off-by: Your Name <you@example.com>
```

## License

reforce is GPL-3.0-or-later ([LICENSE](LICENSE)). By contributing, you agree that your contribution is licensed the
same way.

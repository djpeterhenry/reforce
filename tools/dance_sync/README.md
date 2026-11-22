# dance_sync

Tools for playing dance clips in time with the music (PLAN.md 5J). The clips are silent, so the beat comes from
the dancers' motion.

```sh
tools/dance_sync/run dance_beats.py "~/reforce/movies/ dance_luma_1.mov" --preview   # analyse, render, open
tools/dance_sync/run dance_beats.py ~/reforce/movies                                 # table for a folder
tools/dance_sync/run dance_beats.py CLIP --bpm 100 --downbeat 2 --preview            # fix octave / bar
tools/dance_sync/run dance_beats.py CLIP --write                                     # save to <stem>.reforce.txt
tools/dance_sync/run selftest.py                                                     # synthetic check
```

`run` creates `.venv` (numpy, scipy, opencv-python) on first use. Needs ffmpeg.

**Signals** (`--signal`, default `auto`):
- **bounce**: the body's up-and-down motion, measured in three bands relative to the body's own height:
  upper body 20%, hips 40%, legs 40%.
  - A beat is the bottom of a bounce. If the "on" point is the top instead, use `--offset-beats 0.5`.
  - Silhouette clips use the figure as the body mask. A silhouette is a solid white or black figure on a plain
    background, either way round.
  - Other clips use the moving region as an approximate mask.
  - The `bounce` mix is 70% bounce plus 30% hits, and `bounce-only` is bounce alone.
- **hits**: any movement stopping or reversing.
- **auto** fits both `bounce` and `hits` and keeps whichever is more confident. The JSON records the choice
  (`auto:bounce`) and the other result.
  - On a silhouette, bounce is measured on the real figure, so it stays unless hits is more confident by over 0.2
    (luma_5's stray hits fit led by only 0.10; silhouette's real one leads by 0.39).
  - Smooth, swaying dancers (luma) go to bounce.
  - Sharp, hit-driven ones (silhouette, static) go to hits.

**Frame timing is cleaned up first, because cadence artefacts look like rhythm.**
- Repeated frames are skipped. The luma clips are 15 fps padded to 23.976; shadow_dance is 25 fps converted to 29.97.
- Real frames are evenly re-spaced locally: a sliding window that never crosses a cut or a long hold.
  - Timing that slips between stretches is kept.
  - Genuine held frames are left alone, such as animation drawn on twos or stop-motion.
- A repeating uneven pattern inside the content is detected and corrected, up to 8 frames long. The luma clips came
  from 25 fps by keeping 3 of every 5 frames, which gives gaps of 2, 2, 1.
  - The pattern must look the same in every quarter of the clip.
  - Motion that happens to repeat every few frames doesn't count.
- The JSON records `duplicate_frames`, `content_fps` and `cadence`.

**Clips with several tempos:** a medley, or speed ramps, won't fit one BPM. Analyse one stretch with
`--start` / `--end` to check, then set Start/End to play that stretch.

**The preview** (`~/Library/Caches/reforce/dance_sync/<stem>.beats.mp4`, opened in QuickTime):
- The border flashes on each beat, red on the downbeat, and the dots at top left count the bar.
- A click plays on every beat, accented on the downbeat. Listen while watching the dancer.
- The bottom strip scrolls past the playhead and shows:
  - the bounce signal (orange)
  - the hits signal (green)
  - the motion peaks matched to beats (white triangles)
  - the fitted grid (lines, red = downbeat).

**Reading the table:**
- `conf` mixes three things:
  - `hits`: the share of beats that have a motion hit near them
  - `jitter`: the spread of those hits around the grid
  - how clearly the chosen tempo beats the alternatives.
- `alternatives` lists other tempo peaks. When the clip looks wrong at one tempo, try a listed one with `--bpm`.
  Real dancers often give a 2:1 or 3:2 choice (e.g. 100 vs 150).
- Bar position can't be seen in motion. `--downbeat N` makes beat N of the grid the "1".

The analysis JSON (envelope, peaks, grid, per-window phase drift) is cached next to the preview.

## Interpolation (`interpolate.py`)

```sh
tools/dance_sync/run interpolate.py --install                                    # fetch RIFE (437 MB, checksummed)
tools/dance_sync/run interpolate.py "~/reforce/movies/ dance_luma_2.mov" --compare
```

**What it does:**
1. Restores the clip's real frame timing:
   - drops repeated frames
   - has RIFE recreate the frames a 2-2-1 cadence dropped (luma: back to 25 fps).
2. Interpolates by `--factor` (default 4, so the luma clips become 100 fps).

**Output:** `<stem>.rife4x.mov` beside the original, HEVC 10-bit at high quality (VideoToolbox quality 85, about 10x smaller than ProRes LT and visually identical).
- It starts at the same moment and lasts the same time as the original, so BPM / BeatStart carry over.
- reforce plays it in place of the original, and never lists it as a movie of its own.

**`--compare`** adds `<stem>.compare.mp4`: original | restored, side by side at half speed.

**Speed and size:** about 2 minutes and 200 MB per 80 s luma clip on an M4 Pro.

**RIFE:** rife-ncnn-vulkan 20221029 with the rife-v4.6 model, in `bin/` (git-ignored).

## Seamless loops (VJ loops)

Clips whose last frame flows into the first get special treatment:
- **Tempo from the length:** when the clip lasts a whole number of bars at the detected tempo, the tempo is set from
  the length exactly (N beats in the clip's frame time), so the loop stays in sync forever. The JSON records this in
  `loop`.
  - Checked against Beeple's labelled loops: harvest_128bpm came out at 128.1 and popbottles_110bpm at 110.3.
  - Others landed on round numbers: ignite_running_man 96.000, space_crystals 150.000, spinning_red_earth 120.000.
- **Tempo in the file name:** `_128bpm` narrows the search.
- **Interpolation across the seam:** interpolate.py also fills the jump from the last frame back to the first, so the
  seam plays as smoothly as the rest.
- **reforce:** a section within 2% of a whole number of bars plays whole instead of losing nearly a bar.

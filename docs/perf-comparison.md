# G-Force vs reforce: CPU, GPU and memory (PLAN 5G)

Measured 2026-11-12 on an Apple M4 Pro (14 cores), macOS 26, with `tools/perfcompare/perfcompare.py`
(no sudo). One app at a time, machine otherwise idle.

**Conditions (the same for both apps):**
- Each app ran its normal slideshow with the user's own settings.
- Windowed and in front. G-Force kept its usual 990×658 pt window, which renders about 1980×1316 px on Retina; reforce
  used `--render-size 1980x1316`.
- Both read the same audio: kick, noise bands and a sweep, looped silently through the "reforce probe" Loopback device.
- Each run was 120 s, sampled after a 20 s warm-up.
- G-Force 5.9.7. reforce was the installed build bda2f19.

## Results

| Run | CPU (100 % = 1 core) | CPU p95 (1 s samples) | GPU busy (whole GPU) | Memory (RSS) | Threads | Frame rate |
|---|---|---|---|---|---|---|
| G-Force run 1 | 119 % | 131 % | 2 % | 228 MB | 33 | ~55.6 fps (measured earlier) |
| G-Force run 2 | 123 % | 129 % | 2 % | 228 MB | 34 | ~55.6 fps |
| reforce run 1 | 25 % | 39 % | 15 % | 240 MB (max 263) | 40 | 61 fps |
| reforce run 2 | 54 % | 80 % | 14 % | 268 MB (max 320) | 40 | 61 fps |
| reforce run 3 | 13 % | 19 % | 14 % | 255 MB (max 276) | 40 | 61 fps |
| reforce trial runs (2) | 16 %, 25 % | 25 %, 87 % | 19 % | 254–260 MB | 40 | |

The idle GPU baseline was 0–2 %. G-Force Toolbar added 0.1 % CPU, included in the G-Force rows.

**Summary:**
- **CPU:** G-Force uses about 1.2 cores, steadily, whatever the scene. reforce uses 0.13–0.54 cores, usually about
  0.2, depending on which scenes the slideshow picks. That is roughly **1/5 of G-Force's CPU** for a typical
  slideshow, and **under 1/2 at its heaviest**. Per frame, G-Force spends about 22 ms of CPU and reforce about 2–9 ms.
- **GPU:** G-Force draws almost everything on the CPU and barely uses the GPU (~2 %). reforce moves the drawing to
  the GPU (~15 % busy at this size). This is the trade that frees the CPU.
- **Memory:** about the same, ~230 MB for G-Force and ~240–270 MB for reforce.
- **Frame rate:** reforce ran at 60 fps while G-Force managed ~55.6, so reforce draws ~8 % more frames for its
  lower CPU.

The spread between reforce's runs comes from scene choice: WaveShape expression evaluation dominates its CPU. One
heavy WaveShape morph took the 1-s peak to 80–87 % (tracked in PLAN, WaveShape performance).

## Not measured (would need more)

- **Power in watts and per-process GPU time** need `sudo powermetrics --samplers cpu_power,gpu_power,tasks`, which
  Peter would have to run. Activity Monitor's Energy and GPU columns give a rough check without sudo.
- **Full screen and 4K** were not measured. Full screen would toggle G-Force's remembered window rectangle
  (memory: gforce-must-stay-unaffected). Note that G-Force draws frames above ~5.2 MP at half size (#38), so its full
  screen costs less than its pixel count suggests.
- **Fixed scenes** (one heavy WaveShape, a sprite scene) were not measured; only the normal slideshow was.

## Rerun

```
python3 -I tools/perfcompare/perfcompare.py gforce  --seconds 120
python3 -I tools/perfcompare/perfcompare.py reforce --seconds 120 [extra reforce args, e.g. --eval-threads 0]
```

The script writes results to `build/perfcompare/results.jsonl`. It needs `build/reforce-audio-play` and the
Loopback device "reforce probe".

For G-Force, the script:
- backs up `~/Library/Preferences/SoundSpectrum` first, and restores and diffs it afterwards;
- changes only the audio input and the startup UI for the run;
- installs nothing, and never resizes or full-screens the window.

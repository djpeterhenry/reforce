# G-Force audio response: probe results (Phase 5C.4)

What G-Force 5.9.7 does with audio, measured black-box from screenshots, and the model reforce's analyzer was
fitted to. Numbers only (tables instead of plots). The G-Force vs reforce comparison with scores is in
[audio-parity.md](audio-parity.md).

## 1. Method

- **Signals** (`tools/audio_signals/gen_signals.py`, 44.1 kHz mono, schedules in `probes/audio/*.json`): a 2 kHz
  sync click at 0.5 s, content from 2.0 s. `tone_steps` (1 kHz, −40/−30/−20/−10/−6/0 dBFS peak, 2 s each),
  `sweep` (20 Hz → 20 kHz log sweep over 20 s, −6 dBFS), `noise_bands` (flat noise, −15 dBFS RMS: 40–150 Hz,
  400–2000 Hz, 4–12 kHz, 3 s each, 1 s gaps), `kick120` (150→55 Hz kick, −3 dBFS, 120 BPM, 32 hits),
  `attack_release` (pink noise −12 dBFS RMS, 2 s on / 2.5 s off, ×3), `silence`.
- **Route:** played to the Loopback device `reforce probe` (no monitoring), G-Force's `Audio.InputSource`.
- **Probes** (`tools/parity/audio_layout.py`): a strip chart (a FlowField scrolls the top of the screen left by
  1 px per frame; a WaveShape draws one dot per signal per frame for `LEVEL`, `BASS`, `ENERGY`, `fft(.05)`,
  `fft(.2)`, `fft(.5)`, `fft(.8)`, `mag(.5)` and a time base `wrap(t/8)`) plus `fft(s)` / `mag(s)` snapshot curves;
  and a full-screen spectrum snapshot probe.
- **Captures:** runs 7–14 on 2026-11-11 (`probes/captures/20261111-1705…1710`, local only). G-Force ran with
  Peter's prefs: defaults except **`Audio.FFT.Smooth = 2`** (default 1) (`docs/gforce-defaults.md`).
- **Decoding** (`tools/parity/audio_measure.py`): per column, the dot centroid in each band → value. The rails
  (u = −0.1 and 1.1) are now used to calibrate each band's y map: G-Force draws dots 0.5–1.5 px lower than
  nominal, which read as −0.01 (`LEVEL`) to −0.04 (`BASS`) in silence before calibration and read ~0 after.
  Resolution: 1 px = 0.016 (`LEVEL`), 0.033 (`BASS`, `ENERGY`, `fft`, `mag`) on the strip, 0.005 on the
  spectrum probe.
- **Frame rate:** G-Force drew 51–55 fps (fitted per shot from the time band; median ~53). A dot fades by ~2/255
  per frame there, so each shot holds ~120 frames (2.3 s); shots every 3 s leave ~0.7 s gaps in the series.
- **Latency:** shot audio time = timeline time − 2.0 s (player delay) − **0.23 ± 0.03 s** (player start-up, device
  and G-Force input latency; from the sync click in all six strip runs). The reforce render reproduces this
  offset so snapshots compare the same audio moment.
- **Sync click onset (run 11):** the detector took its baseline from the first 0.3 s of *columns*; the first
  run-11 shot also showed ~50 older value-less columns (pixels of the previous frame state), so the baseline was
  empty, `LEVEL`'s −0.010 uncalibrated silence offset exceeded the 0.01 margin and the onset was reported at
  −0.319 s. Fixed: the baseline now starts at the first column with values (and calibration removes the offset);
  run 11's click decodes at 2.716 s like the other runs (2.70–2.75 s).

## 2. `mag()`

| tone (1 kHz) | expected RMS of the raw sample | G-Force RMS of `mag(.5)` | ratio |
|---|---|---|---|
| −40 dBFS | 0.007 | 0.013 | (below resolution) |
| −30 dBFS | 0.022 | 0.016 | (below resolution) |
| −20 dBFS | 0.071 | 0.031 | 0.44 |
| −10 dBFS | 0.224 | 0.238 | 1.06 |
| −6 dBFS | 0.354 | 0.421 | 1.19 |
| 0 dBFS | 0.707 | 0.762 | 1.08 |

- **Scale:** the raw sample, gain ~1 (a −6 dBFS tone's dots span ±0.48–0.50, 0 dBFS ±0.95), **no auto-gain**
  (reforce had an auto-gain that boosted quiet input up to 4×; now off by default, kept as an option).
- **No inter-frame smoothing visible** at `Audio.PCM.Smooth = 0.008`: blending with the previous frame (reforce's
  old reading of the pref) would lower the RMS of a random-phase sample to ~0.88×; G-Force reads ≥ 1×.
- **Window:** 255 consecutive samples at 44.1 kHz (zero crossings of the sweep in the `mag(s)` snapshot match
  the sweep frequency within ~5 %), oldest first.
- **Lowpass: a 7-point moving average** (follow-up runs 15/16, §9.3). It explains the 4–12 kHz noise (0.12×) and
  the sines above ~5 kHz; reforce now does the same (`AnalyzerConfig::pcmBoxcar = 7`).
- **Still not reproduced:** 400–2000 Hz noise reads 0.095 (0.53×; the moving average passes that band at ≥ 0.85×),
  and loud 1 kHz tones read 1.08–1.19× (−6 / 0 dBFS). The sign of `mag()` was not determined (no asymmetric test
  signal).

## 3. `fft()` frequency axis

`SSEngine.py` (G-Force's Python library) calls `Interpreter.SetFFTParams(120, 5500, 8, .02, .07, False)`: lowest
bin 120 Hz, highest 5500 Hz, 8 periods per analysis window clamped to 20–70 ms, **log** spacing
(`Script Command Reference.py`). `Audio.FFT.FreqLo/FreqHi = 0` (auto) keeps that range. The stationary
measurements agree:

| signal | expected bins (120–5500 Hz log, 255 bins) | G-Force half-max range | G-Force peak |
|---|---|---|---|
| 1 kHz tone, 0 dBFS (after 12 s) | 141 | bins 75–232 (375 Hz–3.8 kHz) | bin 141 |
| noise 400–2000 Hz | 80–188 | 47–211 (245 Hz–2.9 kHz) | bins 143–184 (1.0–1.9 kHz) |
| noise 4–12 kHz (in range: 4–5.5 kHz) | 234–254 | 198–254 (2.4–5.5 kHz) | bins 207–232 (2.7–3.9 kHz) |
| noise 40–150 Hz (in range: 120–150 Hz) | 0–15 | 0–145 (spreads over 2.5 s) | bins 0–14 at first |

Bins are **broad**: a pure tone lights about −1.4 … +1.9 octaves at half maximum at 0 dBFS, with smooth flanks (no
sidelobe ripple), and the curve is clamped at **0.92–0.95** (spectrum probe: maximum 0.92 in every saturated
shot).

### 3.1 Not stationary: tones drift, the sweep runs ahead

| `tone_steps` shot | audio t | level | `fft(s)` centroid (bin) | half-max range (bins) |
|---|---|---|---|---|
| 2 | 4.9 s | −30 dBFS | 27 | 13–42 |
| 3 | 7.9 s | −20 dBFS | 64 | 28–98 |
| 4 | 10.9 s | −6 dBFS | 122 | 58–186 |
| 5 | 13.9 s | 0 dBFS | 151 | 75–232 |

A steady 1 kHz tone first appears at the **low** end of the axis and its bump moves up and widens over ~10 s
(and with level) until it is centered on its true bin. At each level step `fft(.05)` flashes (0.24–0.27 for a
frame or two).

| `sweep` (snapshot probe) audio t | sweep frequency | G-Force peak bin | that bin's frequency |
|---|---|---|---|
| 10.0 s | 319 Hz | 55 | 275 Hz |
| 10.5 s | 379 Hz | 94 | 494 Hz |
| 11.0 s | 451 Hz | 138 | 959 Hz |
| 11.5 s | 536 Hz | 182 | 1.86 kHz |
| 12.0 s | 637 Hz | 223 | 3.45 kHz |
| 12.5 s | 757 Hz | 254 | 5.5 kHz |
| 13.0–22 s | 0.9–20 kHz | — | dark (max ≤ 0.15) |

Strip run (run 8) agrees: `fft(.05)` peaks at 9.04 s (227 Hz; bin center 145 Hz), `fft(.2)` at 9.82 s (298 Hz;
258 Hz), `fft(.5)` at 10.71 s (405 Hz; 812 Hz), `fft(.8)` at 11.65 s (560 Hz; 2.56 kHz). `LEVEL` falls to 0 at
11.9 s (605 Hz) and stays 0 for the remaining 10 s of the sweep, although `mag(s)` still shows the full 0.5
sine. Below ~200 Hz (2–9 s) the curve is lit almost everywhere (`LEVEL` 0.38). So during a sweep the bump
runs ~3.5× faster in log frequency than the tone and then G-Force stops responding to a moving tone above
~0.8 kHz. **Explained by the follow-up runs (§9): the fft() frequency axis is not fixed but adapts to the
content.** reforce's model keeps a fixed axis; the parity report marks these checks as known gaps.

## 4. Normalization and dynamics

`LEVEL` and the curve are adaptively normalized: weak bands grow over seconds, a sudden broadband onset
overshoots and settles, silence resets quickly.

| situation | G-Force |
|---|---|
| pink noise −12 dBFS RMS after 2 s silence (`attack_release`, each of 3 bursts) | `LEVEL` 0.50–0.58 for ~0.25 s, then down to 0.28–0.33 within ~0.6 s; steady 0.32 / 0.36 / 0.36 (bursts 1–3) |
| pink noise stops | `LEVEL` and every `fft` bin to 0 within 1–2 frames (~40 ms) |
| 400–2000 Hz noise starts | `LEVEL` 0.50 at once, 0.35–0.45 after 1 s; out-of-band `fft(.8)` (2.6 kHz) grows 0.06 → 0.65 over 3 s |
| 40–150 Hz noise (mostly below 120 Hz) | `LEVEL` grows 0.07 → 0.25 over 2.5 s |
| 4–12 kHz noise (mostly above 5.5 kHz) | `LEVEL` 0.015 → 0.048; `fft(.8)` grows 0.13 → 0.8 over 1.4 s |
| kick 120 BPM (energy mostly below 120 Hz) | per-hit `LEVEL` peak grows 0.12 (hit 1) → 0.24 (hits 2–5) → 0.31–0.38 (6–11) → 0.34–0.43 (12–29); fall to 10 % in 0.24–0.33 s; onset +0.00…+0.10 s after the kick |
| silence (run 12, 10 s) | every signal 0 (calibrated decode: \|value\| ≤ 0.002 for `LEVEL`) |

| 1 kHz tone step | `LEVEL` steady mean | notes |
|---|---|---|
| −40 dBFS | 0.006 | essentially off |
| −30 dBFS | 0.147 | rises 0.13 → 0.15 during the step |
| −20 dBFS | 0.291 | spike 0.32 at the step, then 0.25 → 0.33 |
| −10 dBFS | 0.519 | 0.48 → 0.54 |
| −6 dBFS | 0.607 | spike 0.69 at the step, settles to 0.61 |
| 0 dBFS | 0.629 | spike 0.75, settles to 0.61 (curve saturated) |

Values sometimes hold for 4–6 frames and then jump (e.g. kick 3.00–3.10 s), consistent with audio arriving in
~90 ms buffers; reforce analyzes every frame and does not copy this.

## 5. `LEVEL`, `BASS`, `ENERGY`

- **`LEVEL` = c × mean of `fft()`:** at every strip shot the snapshot curve's mean and `LEVEL` agree up to a
  factor 1.0–1.3 (e.g. 0.123 / 0.147, 0.295 / 0.360, 0.515 / 0.592, 0.338 / 0.377, 0.318 / 0.377, 0.065 / 0.081).
  Also in the sweep: `LEVEL` dies exactly when the curve does.
- **`BASS` = 4.0 × `LEVEL`:** least squares over 2119 unsaturated frames of runs 7–11: `BASS` = 3.93 `LEVEL` +
  0.03 (RMS 0.041, ~1.2 px); through the origin 4.03 (RMS 0.046). It holds for any spectrum (also when only
  the top bins are lit during the sweep), so in 5.9.7 `BASS` is not a low-band measure ("Fixed BASS config var
  not receiving the proper audio level", version history).
- **`ENERGY`:** rises slowly while `LEVEL` is up and decays slowly after: pink bursts 0 → 0.62 / 0.21 → 0.69 /
  0.27 → 0.72 in 2 s; after a burst it decays with τ ≈ 1.9–2.0 s; tone steps saturate it at 0.95 (≈ 1.0, the top
  of its range); kicks hold it at 0.50–0.57. A plain integrator of `LEVEL` cannot give both the kick (low mean,
  high peaks) and the noise values; a follower of recent `LEVEL` peaks can (§6).

## 6. reforce model (fitted)

Fitted by Nelder–Mead on a NumPy re-implementation of the model, simulated at G-Force's 53 fps on the same WAVs,
against the decoded G-Force series (objective: RMS of `LEVEL` on runs 7, 9–12 ×2, of `fft(.05/.2/.5/.8)` on runs
9–11, of the snapshot curves on runs 7, 9–11, 14 ×2; `ENERGY` separately). All constants are in seconds, so the
60 Hz reforce sim behaves the same. Defaults in `AnalyzerConfig` (`src/audio/Analyzer.hpp`).

| stage | model | fitted / G-Force constants |
|---|---|---|
| spectrum | Hann 2048-point FFT power each frame; vis bin j (center 120 Hz × (5500/120)^(j/254)) = sqrt(Σ_k w_jk P_k) × (f_j / 1 kHz)^tilt, w = 1/(1 + \|log2(f_k/f_j)/bw\|^shape) | bw 0.428 oct, shape 2.20, tilt −1.19 |
| normalization | 16 groups (`Envelopes`), group mean amplitude x → reference R: attack 0.040 s, release 7.4 s, R ≥ refMin, R₀ = refInitial / `InitialVolume`; v = gain × a / R (R interpolated between group centers) | gain 0.528, refMin 0.00122, refInitial 0.517 |
| smoothing | per bin: instant attack, release τ = `Audio.FFT.Smooth`/60 s × `PersistCoef` | Smooth 1 (default), 2 in the captures |
| output | `fft` = min(v, 0.93) × `Audio.Response.Scale` | |
| `LEVEL` | min(1, 1.343 × mean(`fft`)) | |
| `BASS` | min(4, 4.0 × `LEVEL`) | |
| `ENERGY` | p = peak-hold of `LEVEL` (decay 0.205 s); follow 2.96 × p with attack 2.52 s / release 1.43 s; clamp 0..1 | |
| `mag` | sample × 1 through a 7-point moving average (§9.3), 255 newest samples; optional auto-gain / inter-frame smoothing (off) | |

Unused `Audio.FFT.Params` keys (`FloorRaiseMax`, `Short*`, `Long*`, `ExciterImpulse`, `PersistLowThresh`) are
still parsed and kept in the config; reforce's earlier guess at their meaning (short/long envelopes, floor
raise, exciter) did not fit the measurements better than the simpler model above. `Audio.FFT.NumBins.in`
(128) has no effect.

Fit quality (NumPy model, RMS over each run's frames): `LEVEL` 0.04 (tone steps), 0.07 (noise bands), 0.08
(kick), 0.07 (pink bursts), 0.015 (silence); `ENERGY` 0.06–0.09; snapshot curves 0.08–0.15 (noise, kick), 0.28
(tone steps: the drift of §3.1).

**Parity** (`tools/parity/audio_compare.py`, [audio-parity.md](audio-parity.md); 306 checks over runs 7–14):
the previous analyzer passed 33.9 % of all checks (53.6 % of those outside the gaps below); the fitted one
passes 71.6 % of all checks and 100 % of those outside the documented gaps (87 checks, listed per run in the
report). Registered in ctest as `reforce_audio_parity` (`--strict`; skipped without the local captures).

## 7. Not reproduced (known gaps)

1. The drift of a steady tone's bump over ~10 s, and the level-step flashes (§3.1). **Cause found (§9.1): the
   adaptive fft() axis.** Not modeled in reforce yet (§9.2).
2. The sweep: the bump running ahead of the tone and G-Force going dark above ~0.8 kHz (runs 8 and 13). **Same
   cause (§9.1).**
3. `mag()`: sines above ~5 kHz and high noise — **fixed** (7-point moving average, §9.3); still open: 400–2000 Hz
   noise (0.53×) and loud tones (1.08–1.19×).
4. Out-of-band leakage that slowly lights bins far from the content (e.g. `fft(.8)` during 400–2000 Hz noise
   reaches 0.65 in G-Force, 0.15 in reforce), and the 4–12 kHz band's low `LEVEL` (G-Force 0.03, reforce 0.17).
   Both follow from the adaptive axis: the axis tops out near 11 kHz but contracts toward the content within
   seconds, so content above it is unseen (4–12 kHz noise) and a narrow axis spreads one band over many bins.
5. The pink bursts read slightly higher in reforce and creep up from burst to burst (0.39 / 0.45 / 0.51 vs
   0.32 / 0.36 / 0.36): the slow 7.4 s release that reproduces the kick's growth also carries over between
   bursts.
6. ~90 ms buffer holds (§4).

## 8. reforce extensions (5C.5): `MID`, `TREBLE`, `BEAT`, `TEMPO`

Not in G-Force (no G-Force scene uses them, so no parity impact). Definitions: `docs/scene-format.md` §3.5;
code: `src/audio/BeatTracker.*` and the band meters in `Analyzer.cpp`. On `probes/audio/kick120.wav` (60 Hz
frames, deterministic):

| measure | result |
|---|---|
| onsets detected | 33 = the sync click + all 32 kicks, no extra |
| onset time vs kick start | mean +10 ms, max 20 ms (peak picking on 10 ms hops, one hop of look-ahead) |
| `BEAT` | ~0.9 in the frame after each kick, < 0.05 just before the next |
| `TEMPO` | 120.0 BPM from 3.25 s (after the click + 3 kicks; needs 4 onsets) to the end; 0 for silence and steady tones |
| other tempi (unit tests) | 90 and 140 BPM kick tracks: within 3 %; `TEMPO` returns to 0 ~4 s after the music stops |
| `MID` / `TREBLE` | kick: `TREBLE` ≤ 0.03; 800 Hz tone: `MID` ~1, `TREBLE` < 0.2; 6 kHz tone the other way round |

## 9. Follow-up runs (2026-11-11 evening): the adaptive fft() axis, `mag()` lowpass

Runs 15 (strip) and 16 (spectrum, every 0.5 s): `tone_freqs.wav` — sines at 125/250/500/1k/2k/4k Hz, −12 dBFS, 6 s
each with 2 s gaps, then 1 kHz for 20 s, then a falling log sweep 20 kHz → 20 Hz (−6 dBFS). Run 23 (spectrum):
`tone_pairs.wav` — two or three simultaneous sines (−18 dBFS each): 300+2400 Hz, 200+400+3200 Hz, 1k → 1k+4k → 4k,
250+1000 Hz. Captures `probes/captures/20261111-203921`, `-204133`, `-205240` (local). In run 15 G-Force's input
came up ~0.45 s late and missed the sync click; `audio_measure.py` now detects that (the "onset" is followed by
sustained signal) and aligns on the content start instead.

### 9.1 The fft() axis follows the content

Two simultaneous tones fix both ends of a log axis per snapshot (bin j = 254 log(f/lo) / log(hi/lo)):

| run 23, audio t | tones (Hz) | bins | axis lo – hi (Hz) | width |
|---|---|---|---|---|
| 2.0 s (after 2 s silence) | 300 + 2400 | 13.6 / 156 | 246 – 10 040 | 5.35 oct |
| 4.0 s | 300 + 2400 | 18.8 / 196.5 | 241 – 4 704 | 4.29 oct |
| 8.0 s | 300 + 2400 | 22.3 / 226 | 239 – 3 194 | 3.74 oct |
| 11.5 s (settled) | 300 + 2400 | 22.6 / 230.1 | 239 – 3 050 | 3.67 oct |
| 14.0 s (after 2 s silence) | 400 + 3200 | 38.9 / 197.5 | 240 – 6 712 | 4.80 oct |
| 19.5 s | 400 + 3200 | 46.4 / 233.5 | 239 – 4 019 | 4.07 oct |
| 23.5 s | 400 + 3200 | 29.0 / 234.5 | 298 – 3 898 | 3.71 oct |
| 35.5 s (4 kHz added at 32 s) | 1000 + 4000 | 44.9 / 250.1 | 738 – 4 107 | 2.48 oct |
| 37.5 s | 1000 + 4000 | 31.4 / 231 | 804 – 4 693 | 2.55 oct |
| 48.5 s → 51.5 s | 250 + 1000 | 0 → 5.9 / 109 → 171 | 250 → 238 / 6 249 → 1 997 | 4.6 → 3.1 oct |

- **The upper edge contracts toward the highest tone** (settles ~1.22–1.27× above it: 2400 → 3050, 3200 → 3900)
  as an exponential in log frequency with τ ≈ 2 s, and **relaxes back up toward ~11 kHz in silence** with about the
  same τ (3050 Hz at the end of the 300+2400 chord → 6712 Hz 2 s later). It also expands toward new content above it
  (4 kHz entered at the top 3.5 s after it started).
- **The lower edge has a floor at ~240 Hz** (= 2 × the documented 120 Hz, as the ~11 kHz ceiling is 2 × 5.5 kHz)
  and rises toward the lowest tone only slowly and after a few seconds (400 Hz: from 240 Hz after 6 s; 1 kHz alone
  for 6 s: to ~740 Hz). A 200 Hz tone below the floor is not shown.
- Single tones therefore climb the axis as the upper edge comes down: 500 Hz bin 75 → 208 in 6 s, 1 kHz 141 → 211,
  2 kHz 175 → 205; with the lower edge rising (after a 4 kHz tone) a 1 kHz tone first falls (55 → 26) and then
  climbs (→ 168 by 20 s). The bump also widens as the axis narrows (1 kHz: 35 → 180 bins at half maximum), which
  is what lifted `LEVEL` to ~0.6 in the 12 s `tone_steps` run.
- **Sweeps:** a rising sweep starts below the 240 Hz floor, the upper edge contracts onto it, the tone then
  crosses a ~1.6-octave axis (≈ 150 bins per octave instead of 46: the "3.5× too fast") and leaves it at the top
  faster than the edge can follow (dark above ~0.8 kHz). The falling sweep mirrors it: enters at the top near
  6 kHz, runs ~1.45× fast and leaves at the bottom near 450 Hz (`mag()` still shows it).
- `LEVEL` (the curve mean) inherits all this: steady −12 dBFS tones read 0.03–0.2 for the first seconds and up to
  0.63 once the axis has closed around them; content above the axis contributes nothing (4–12 kHz noise: 0.03).

### 9.2 Status in reforce

Not implemented: a first-order edge model (lo floor 240 Hz, hi ceiling 11 kHz, targets lowest/highest tone ÷/× a
margin, asymmetric time constants, a hold before the lower edge rises) fitted to the bump positions of runs 13, 16
and 23 reproduces the chords to ~2 bins but single tones and the rising sweep only to 20–100 bins (exploratory
fit, not committed). The edges must be driven by something other than the plain tone extent
(how the lower edge picks its moment is the open question), and the normalization stage was fitted with a fixed
axis, so both would need refitting together. Next probe if this is pursued: chords whose lowest tone steps up and
down while the highest stays fixed (and vice versa), plus band-limited noise of varying width.

### 9.3 `mag()` = 7-point moving average

The `mag(s)` snapshot shows the waveform at full sample rate (1 kHz: ~44 points per period) but attenuated at high
frequencies, with a short ramp at the oldest end of every frame's curve (zero history):

| sine | G-Force amplitude / input | 7-point moving average |
|---|---|---|
| 1 kHz | 0.96 | 0.96 |
| 2 kHz | 0.84 | 0.84 |
| 4 kHz | 0.50 (strip RMS 0.36×) | 0.46 |
| ~6.3 kHz (= 44.1 kHz / 7) | ~0.1 (falling sweep) | 0 (first null) |
| 8–20 kHz | ≤ 0.1 | ≤ 0.2 (side lobes) |

reforce: `AnalyzerConfig::pcmBoxcar = 7` (causal, zero history at the window's oldest end). Audio parity: all
eight `mag(.5)` tone checks of run 15 pass (e.g. 4 kHz: G-Force 0.063, reforce 0.075), and the
4–12 kHz noise now matches (0.021 / 0.022; was 0.18).

### 9.4 Parity

`docs/audio-parity.md`: 100 % of the checks outside documented gaps pass (276; 0 fail). Runs 7–14 alone: 221 pass
(was 219), 85 known gaps (was 87). The new runs add 55 passing checks (`mag`, the run 15 attack/release timings)
and 315 known-gap checks, nearly all `fft()`/`LEVEL`/`BASS`/`ENERGY` values that depend on the adaptive axis.

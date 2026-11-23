# reforce — Beat detection

The audio tempo (TEMPO) and beat clock (SYNC_*) when there is no Ableton Link, and how they are measured against
Peter's hand-gridded DJ library. Plan item: PLAN.md 5J.12. Status 2026-11-16: kick-grid tempo, installed.

## 1. Goals and constraints

- Peter wants **tempo and beats**. Bar lines / downbeats are not a priority (said 2026-11-16).
- Music: mostly house / tech house / big room, played in DJ sets (rekordbox, DDJ-1000, audio into reforce via a
  Loopback device). Steady tempo, breakdowns, beatmatched blends, sometimes pitch rides.
- **Never modify the music library.** The library's `database.json` (a djtools Songs folder: `--songs DIR`, or
  `REFORCE_SONGS`) is only read; audio is opened read-only (ExtAudioFile); everything written goes to
  `build/beat-eval/` (beat_eval.py refuses an `--out` inside the library). djtools is the maintainer's own library
  tool (reference only).
- numpy is only on `python3.11`: the training scripts need it; `beat_eval.py` runs on any python3.
- After a finished change: tests (`ctest --test-dir build -j4`), then `scripts/install-app.sh`, no need to ask.
- Live latency: `reforce.BeatClock.AudioOffsetMs` = 10 (the detector's own delay); Peter's Beat offset nudge
  (top bar, `reforce.BeatClock.OffsetMs`) was ~45 ms before the one-step fix, ~52 ms expected after it.

## 2. How it works

All in `src/audio/BeatTracker.{hpp,cpp}` (audio thread, per 10 ms hop of a 1024-point FFT) and
`src/sync/BeatClock.{hpp,cpp}` (per sim frame). `Analyzer` copies the tracker's outputs into `AudioFrame`;
`Visualizer::stepBeatClock` feeds them to the clock. `tools/beat_eval/beat_eval.cpp` does the same offline.

1. **Onsets.** Log-compressed spectral flux (low + mid bands), adaptive threshold → BEAT pulse. Per hop, 9 band
   features (30-60-120-250-500-1k-2k-4k-8k-16k Hz flux, each vs its 4 s mean).
2. **On-beat probability ("beatness")** of each onset: logistic regression on the onset hop's bands + the next
   hop's (+ the bands half a beat earlier once a tempo exists: `halfW*`). Trained on the grids
   (`train_beatness.py`). Held-out AUC 0.805.
3. **Tempo.** Autocorrelation of onset strength × the hop's on-beat probability (`acfBeatness`). Candidates are the
   peaks of a comb score: 1-beat + 2-beat + 2 × 4-beat repeats (`comb2`, `comb4`, `combPeaks`; k-beat peaks searched
   ±k/2 hops), refined from the 4-beat peak (`refine4`), log-Gaussian prior around 120, folded into 80-160 BPM.
4. **Steady mode** (`reforce.BeatClock.SteadyTempo`, on): first lock replaceable for 20 s; afterwards local drift
   within ±4 % is followed (10 s smoothing; fast after a 0.5 % move for 2 s), a different tempo needs 8 s of evidence;
   the kick gate (30-120 Hz level vs its recent peak) freezes the tempo in breakdowns; silence drops it after 3 s.
   **Kick grid** (`kg*`): on-beat onsets (probability >= 0.5) while a kick plays are numbered by beat and a line
   t = a + P n is fitted over the last 5 s; when 60 / P is > 0.3 % from TEMPO, TEMPO follows it (1 s) and the
   autocorrelation's drift following waits 8 s. ~2 s behind a nudge instead of ~8-10 s. No kick, no update.
5. **Beat fold** ("which half is the beat"): band flux + band level folded onto 16 bins of the beat (on the tracker's
   own phase `phi`, which runs at TEMPO), 12 s decay. A linear score per rotation (`foldW`, `train_fold.py`) gives a
   softmax over where the beat is. Right half 92.7 % held out; when confident, 98.4 %.
6. **Phase filter** (the research's HMM idea, phase only): 64 states on phi's circle; random walk; each hop's
   activation (strength × on-beat probability) and each fold score update it. While TEMPO moves (a pitch ride) it
   loosens (`pfDiffusionMoving`) and ignores the smeared fold (`pfFoldGainMoving` 0). Optional tempo dimension
   (`pfTempoRows`, default 1 = off; see §5.1): rows of tempi whose beats drift along phi at their own rate.
7. **Clock.** Steers its count to the filter's peak while the filter is confident (`useFilter`, `filterOn/Off`,
   `filterOffsetMs` 5); otherwise the older path: onset phase average (gated, beatness-weighted), off-beat rescue
   from the fold (`useFold`) or from onsets. Freewheels at the held tempo with no kick (breakdowns).

## 3. Measuring

**Data.** djtools `database.json`: per song a clip with Ableton warp markers (the grid) and the audio path;
`ts_list` = play timestamps (`--recent N` = Peter's N most recently played). 10,560 gridded songs.
- MP3 grids sit ~25 ms late against Apple's decoder (encoder padding): corrected (`--mp3-offset-ms 25`).
- **Untrusted grids are skipped** (`--all-grids` keeps them): never-warped clips (record BPM missing, or the
  markers' tempo ≠ the record's BPM), names with unwarped / no warp / acapella. ~3.5 % of the library.
- Split: held-out = `sha1(id)[0] % 5 == 0`. Tune on train (`--split train`), check held-out (`--split test`).
  The recent 2,000 mix both (they are what Peter plays).

**Tools** (build: `cmake --build build --target reforce-beat-eval`):

| Command | What |
|---|---|
| `tools/beat_eval/quick.sh songs NAME [--set k=v]` | one line: recent 2,000 + 600 held-out (~2.5 min) |
| `tools/beat_eval/quick.sh mixes NAME [--set k=v]` | one line: 400 simulated DJ mixes (~1 min) |
| `python3 -I tools/beat_eval/beat_eval.py --recent 2000 --split all` | full summary + worst songs |
| `tools/beat_eval/quick.sh nudges NAME [--set k=v]` | one line: 400 mixes with a quick ±1-3 BPM tempo nudge (~1 min) |
| `... --mixes 400` | DJ mixes: beatmatched, 16-bar blend with bass swap, every other one rides the pitch (`--nudge`: quick tempo nudges instead, 10-80 s after the blend; `--nudge-at 30`: the first version's fixed place) |
| `build/reforce-beat-eval --trace OUT.csv ONE.tsv` | per-0.5 s trace of one song (`--mix` for a pair) |
| `--set beat.NAME=V` / `--set clock.NAME=V` | parameter overrides (list: `setParam` in beat_eval.cpp) |
| `... --mixes 400 --nudge --oracle-tempo` | B's true tempo replaces TEMPO: the ceiling for better tempo tracking |

Mix traces (`--mix --trace`): u, part, truth / TEMPO / clock BPM, locked, phase error, filter confidence, kick,
kick-grid BPM, the phase filter's own error. Mix JSON lines also count `clock_bpm_ok` (the clock's tempo within 2 %).

Scores: TEMPO right = within 2 %; "on the beat" = clock confident, its tempo within 2 % and phase within 0.1 beat;
"off-beat" = half a beat out. Throughput ~6,000× realtime. Per-song JSON lines land in `build/beat-eval/*.jsonl`.

**Retraining** (only when the features or the detector feeding them change):
- On-beat model: `beat_eval.py --sample 3000 --split train --dump-onsets --name X`, then
  `python3.11 -I tools/beat_eval/train_beatness.py build/beat-eval/X.onsets.csv --kick-only`; paste the printed
  arrays into `BeatConfig`.
- Fold: `beat_eval.py --sample 1500 --split train --dump-folds --name X` (~0.8 GB), then
  `python3.11 -I tools/beat_eval/train_fold.py build/beat-eval/X`; paste `foldW`. Delete the dump afterwards.

## 4. Where it stands (2026-11-16, trusted grids)

| | TEMPO right | On the beat | Off-beat |
|---|---|---|---|
| Recent 2,000 songs | 96.5 % | 92.8 % | 2.2 % |
| Held-out 600 | 94.7 % | 89.7 % | 2.9 % |

Mixes (400): A's last minute 88.6 % on the beat; blend halves 95.9 / 98.3 %; B's first 30 s 99.1 %, then 95.6 %;
during a pitch ride 79.4 %; around a quick 1-3 BPM tempo nudge 77.6 % (off-beat 5.7 %; 74.9 % before the kick grid)
— see §5.1. Start of the project (2026-11-15, held-out, all grids): TEMPO 77.5 %, on the beat 42.2 %.

By genre tag (recent, wrong TEMPO): TECH_HOUSE 1.8 %, BIG_ROOM 1.4 %, ACTUAL_HOUSE 0.6 %; "90" 8.9 %.

## 5. Open issues and next ideas

1. **Quick tempo nudges — top priority.** Peter often changes the master tempo by 1-3 BPM over < 10 s (both decks
   together; 2026-11-16). Simulated (`quick.sh nudges`, 400 mixes: ±1-3 BPM over 3-10 s, 10-80 s after the blend):
   on the beat 77.6 %, off-beat 5.7 % from the glide to 40 s after (steady playing: ~95-98 %). Back on the beat for
   good after the glide: median 3.8 s, 75 % within 22 s.
   - **Tempo is the lever.** With B's true tempo fed in (`--oracle-tempo`) the same nudges are 96.0 % on the beat,
     breakdowns included: phi, the fold and the filter all stay right when TEMPO does. With the old tracker TEMPO
     followed a nudge ~8-10 s late (8 s autocorrelation window, then steady smoothing), the beat slid 0.2-0.5 beat
     meanwhile and the filter re-locked, often on the off-beat, 15-30 s after the glide.
   - The kick grid (§2.4) catches about 2 in 3 nudges where a kick plays; those score 96.5 %, as the oracle. Missed:
     sections where the on-beat model rates the kicks low (< 0.5; e.g. MK & Dom Dolla - Rhyme Dust, Bob Sinclar -
     Adore after bar 32), so the grid has nothing to fit. Taking weaker onsets near the grid's beats catches more
     (80.2 %) but its noisier tempo costs steady playing (B 30-90 s 97.6 -> 96.4 %).
   - About 1 in 5 simulated nudges fall in a stretch without a kick: by design TEMPO holds there (the
     autocorrelation is noise in breakdowns, confidence < 0.12), so the beat slides until the kick is back. These are
     ~45 % on the beat. Only faster re-acquisition after the breakdown can help.
   - Optional, off: the phase filter's tempo dimension (`pfTempoRows` 7, opened while the autocorrelation reads a
     move): nudges 77.6 -> 79.7 %, rides 79.4 -> 81.1 %, songs -0.2 % (recent and held out). It lets the filter
     follow any steady drift, so in songs with live drift or off-beat accents it sometimes slides onto the off-beat.
     Peter (2026-11-16): leave it off.
   - Ideas: a better on-beat model for the grid's input (the kicks it misses are on the grid); the grid's beats as a
     phase observation for the filter; re-acquire faster when the kick returns after a breakdown.
   Long pitch rides back to a track's own tempo (32 s, up to 6 %) are rarer for Peter: 79 % on the beat.
2. **Remaining 2/3 locks** (~1 % recent): dotted / triplet riffs over a four-on-the-floor kick (Ben Pearce - What I
   Might Do, Yotto - Odd One Out, FISHER's Jamming rework), where 1.5 / 3 / 6-beat repeats are as strong as
   1 / 2 / 4. A half-beat comb term traded them 1:1 for "no tempo". Ideas: a learnt tempo-candidate scorer (features
   of each candidate's comb + fold sharpness at that tempo), trained on the grids like the fold.
3. **No tempo** (~1 %): intros / ambient; a multi-onset-function committee (research idea) is untested.
4. **Non-dance songs** (the "90" tag, pop / rock): live drums, tempo drift; lower priority for Peter.
5. **Breakbeats** whose strongest hits sit on the grid's off-beat (Bicep - Apricots): the fold helps; a small causal
   network on ~40-80 bands trained on the library is the research's route if it matters.
6. Live check: Peter should re-check the Beat offset against the `(Beat Clock)` solid after these changes.

Research summary (2026-11-15): BTrack, aubio, qm-dsp are GPL; madmom's models non-commercial; Beat This! MIT but
offline; BeatNet CC-BY-4.0 but trained on non-electronic music. None used; ideas only.

## 6. Log

Moved from PLAN.md 5J.12; newest last.

- Setup (2026-11-15), `tools/beat_eval/`:
  `beat_eval.py` reads djtools' Songs/database.json (read-only; 10,560 songs, Ableton warp markers = beat grids;
  8,015 are anchor + tempo, 1/32 beat apart), `reforce-beat-eval` runs BeatTracker + BeatClock on the audio
  (ExtAudioFile, read-only; ~2000x realtime on 12 threads) and scores every 0.25 s: TEMPO right / 0 / octave / 2/3
  / other; clock locked, on the beat (|phase| < 0.1), off-beat; phase in ms; `--trace` per song. Train / held-out
  split by sha1(id) (1 in 5 held out).
  - Found: the clock put every beat one sim step early (back-dating used the previous step's count). Fixed; the
    detector's own 10 ms delay is now the default `reforce.BeatClock.AudioOffsetMs` (synthetic grid: 0.06 ms).
    Live, beats land ~7 ms later than before: Peter's Beat offset 45 -> ~52 ms.
  - Found: real kicks score 0.1-0.25 kick-band share (synthetic: 0.4), so off-beat locks (hats, bass) never escaped.
    New onset kick-ness (30-120 Hz flux vs recent peak; BeatTracker::lastOnsetKickness), default in the clock.
  - Held-out (599 songs, 55.8 h): TEMPO right 77.5 % (2/3: 5.9 %, other 12.7 %, 0: 3.8 %); clock on the beat
    42.2 -> 45.4 %, off-beat 9.9 %. Old non-steady behavior on train: TEMPO 68.5 %, on the beat 32.2 %.
  - Scoring: MP3 grids sit ~29 ms later than Apple's decoding (encoder padding; AIFF/FLAC/WAV ~0), corrected by
    25 ms (`--mp3-offset-ms`); "aligned" = on the beat after the song's own constant offset (the Beat offset's job).
  - Kick-band phase (BeatTracker::kickPhase, flux^2 on a TEMPO-rate circle): too spread (+-0.15 beat) to steer the
    clock (on the beat 51.5 -> 31-35 %), but rarely half a beat off, so it drives an off-beat rescue: our beat vs the
    kick phase > 0.35 beat apart for 4 s while a kick plays -> the count slides onto the kicks (default on).
  - Wrong TEMPO is mostly 3:4 confusion (x2/3: ~35 % of the wrong time, x4/3: ~14 %; breaks / garage / hip-hop
    refixes), locked at the start and kept by steady tempo. Bar check (BeatConfig::comb4 = 2): a candidate's score
    adds the autocorrelation at 4x its lag (a bar). Band weights did not help.
  - Held-out (599 songs): TEMPO right 77.5 -> 79.8 %, x2/3 5.9 -> 4.0 %; clock on the beat 48.9 -> 51.3 %,
    off-beat 10.1 -> 9.2 %; songs under 30 % on the beat 38 -> 33 %.
  - Round 2 (same day; tuning songs, then held-out):
    - First lock: for 20 s after a tempo is first found it can still be replaced quickly (two agreeing updates),
      with steady holding (firstLockSeconds). x2/3 3.7 -> 2.8 %.
    - Steady drift tracking: small wobbles (< 1 %) smoothed over 20 s; consistent larger moves (3 s the same way)
      over 2 s, so a new track's tempo still comes in (124 -> 128 in 7.5 s).
    - Tempo refined from the autocorrelation peak 4 beats later (4x resolution): TEMPO within 0.3 % 73 -> 80 %.
    - Clock trim capped at 0.1 % (larger trims made the clock's tempo worse on real music).
    - Learnt on-beat probability of each onset ("beatness", tools/beat_eval/train_beatness.py): logistic
      regression on 9 band-flux features, 1.43 M onsets / 1,392 tuning songs, held-out AUC 0.70 (kick-ness 0.55).
      Weights: kick punch 120-250 Hz and 1-4 kHz click up, 250 Hz-1 kHz and 4-8 kHz (hats) down. The clock weighs
      onsets by it (kickShare 0.4). Off-beat basslines share the kick band, which kick-ness could not separate.
    - Off-beat rescue only on a concentrated kick phase (>= 0.4); kick gate 0.5 s (a breakdown's first notes no
      longer pull the phase).
    - Tried, no gain: 8-beat bar check / longer windows, band weights, clock slew / memory / gate / lock settings,
      steering the clock by the kick phase directly, blending beatness with kick-ness.
    - The on-beat model also sees the hop after the onset (a kick's body decays, a bass note sustains): 18
      features, held-out AUC 0.696 -> 0.745.
  - Held-out (599 songs) vs round 1 (`21ac7c9`): TEMPO right 79.8 -> 83.2 %, within 0.3 % 77 %; clock on the beat
    51.3 -> 63.0 %, aligned 56.8 -> 71.5 %, off-beat 9.2 -> 6.6 %; songs on the beat >= 80 % of the time 24 -> 47 %.
  - Round 3: the tempo autocorrelation runs on the onset strength x each hop's on-beat probability (acfBeatness =
    1; squared / cubed were worse). Held-out: TEMPO right 83.2 -> 86.3 %, within 0.3 % 77.1 -> 82.4 %; on the
    beat 63.0 -> 66.5 %, aligned 71.5 -> 75.4 %.
  - "Grid drift" check (drift_beats / drift_rms in beat_eval): Peter confirmed the top 5 flagged grids are correct,
    and beatness-weighted onsets sit on his grid all song long, so it measures the clock wandering, not grids.
    Kept as a clock diagnostic only; no songs are excluded.
  - Round 4: the off-beat rescue now uses an ungated average of where on-beat-weighted onsets land (beatness^2,
    16 s memory; > 1/4 beat away for 2 s -> slide), not the kick phase (off-beat basslines share the kick band, and
    the 8-in-a-row escape was reset by every off-beat kick-like hit). Held-out: on the beat 66.5 -> 68.7 %,
    off-beat 6.4 -> 4.7 %, songs >= 80 % on the beat 49 -> 53 %.
  - Round 5: on-beat model refitted on 3,000 tuning songs gathered with the round-4 detector, kick sections only
    (`train_beatness.py --kick-only`): small gain. Tried, worse: a 3:4 check by onset phase concentration.
  - beat_eval: `--recent N` (Peter's N most recently played songs, djtools ts_list), scores split by kick playing
    vs not (breakdowns / intros / outros). Throughput ~6000x realtime (2,000 songs = 197 h in 1:41).
  - Peter's 2,000 most recently played songs: TEMPO right 92.6 %, within 0.3 % 90.3 %; clock on the beat 78.2 %,
    aligned 83.8 %, off-beat 4.4 %; songs >= 80 % on the beat 65 %, < 30 % 12 %. With a kick (91 % of the time):
    TEMPO 93.3 %, on the beat 79.0 %; without: 86.2 %, 70.2 %. Held-out songs among them (398): 91.8 %, 77.3 %.
  - Round 6 (the 2/3 locks): traces showed the real beat's 1-beat repeat is often weak and not a local peak, while
    its bar (4-beat) repeat is strong (0.6-0.8); a 3:4 impostor is the reverse. The bar check never saw the real
    tempo as a candidate. Now candidates are the peaks of the combined score (1 + 2-beat + 2 x 4-beat repeats,
    normalized), `combPeaks`. Recent 2,000: TEMPO right 92.6 -> 95.6 %, on the beat 78.2 -> 80.0 %, x2/3 2.3 ->
    1.1 %; held-out: 86.3 -> 91.9 %, 68.8 -> 73.2 %, x2/3 3.3 -> 0.8 %. Tried, worse: a narrower tempo prior.
  - Still open: breakbeat tracks where the strongest kick-like hits sit on the grid's off-beat (Bicep - Apricots
    original: 31 % of on-beat-weighted hits at 0.5 vs 26 % at 0): the features cannot tell the downbeat.
  - Round 7: of 113 recent songs locked off-beat > 30 % of the time, 77 had the on-beat model itself fooled (off-beat
    bass / organ / pumping house). With a tempo, the onset model now also sees the bands half a beat before
    (a beat has hats there, an off-beat the kick): held-out AUC 0.757 -> 0.805; clock threshold 0.7. Recent 2,000:
    on the beat 80.0 -> 83.4 %, off-beat 5.1 -> 4.1 %; held-out 73.2 -> 74.3 % (off-beat 5.1 -> 6.3 %).
  - Research (subagent, 2026-11-15): BTrack / qm-dsp / madmom / BeatNet / Beat This!. Ranked ideas: an online
    bar-pointer HMM (tempo x phase [x bar position], forward filter, madmom-style observation) replacing the PLL,
    rescue and steady logic; a phase-folded "which half is the beat" classifier on band energy + flux profiles over
    8-16 beats (sidechain dips, off-beat bass, hats); a small causal TCN / GRU on ~40-80 bands trained on the
    library (breakbeats); rhythmic-pattern observation models; a multi-onset-function committee for "no tempo";
    joint bar / downbeat tracking. Licenses: BTrack, aubio, qm-dsp GPL; madmom models non-commercial; Beat This!
    MIT (offline); BeatNet CC-BY-4.0.
  - Round 8 (research idea 2): the beat fold. The tracker folds band flux + band level (9 + 9) into 16 bins per beat
    at TEMPO (decay 12 s); a linear score of the normalized profile at each of the 16 rotations, softmax, trained as
    a conditional logit on the grids (`train_fold.py`, `--dump-folds`): right half 92.7 % held out (kick-band flux
    peak 88.2 %), confident (>= 0.8) 79 % of the time and then right 98.4 %. The clock's rescue uses it first
    (confidence >= 0.6; slides when > 0.1 beat away, kick playing). Recent 2,000: on the beat 83.4 -> 88.6 %,
    off-beat 4.1 -> 2.6 %; held-out 74.3 -> 81.2 %, off-beat 6.3 -> 3.7 %. Slides 0.5 -> 1.2 per minute. In
    breakdowns too: no gain (left off).
  - Round 9 (research idea 1, phase part): a forward filter over the beat's place on the tracker's phi circle (64
    states; random walk 0.01 beat/sqrt(s); per hop while a kick plays, the hop's activation = strength x on-beat
    probability vs its 4 s mean sharpens the states near its phi; each fold score multiplies in fold softmax^0.05).
    The clock steers to its peak while the probability within 0.1 beat is >= 0.5 (0.3 to keep), onsets otherwise.
    Recent 2,000: on the beat 88.6 -> 91.9 % (96 % of the time TEMPO is right), off-beat 2.6 -> 2.3 %; held-out
    81.2 -> 85.8 %, off-beat 3.7 -> 3.2 %; 90th-percentile first lock 34 -> 29 s. Insensitive to its parameters
    (width, floor, thresholds: +-0.1 %). Without the fold term: 82.3 %. Not done: a tempo dimension (madmom's bar
    pointer); TEMPO errors are now the main loss (held out 8 %).
  - Tempo-miss diagnosis (2026-11-16): a third of the held-out misses were untrusted grids: never-warped clips
    (Ableton's default markers: record BPM missing, or the markers' tempo != the record's, e.g. Nirvana 125 vs 116),
    "unwarped" / "NO WARP" clips, acapellas. beat_eval.py now skips them (--all-grids to keep; 3.5 % of the library).
    Of the rest, house / tech house / big room are 0.4-1.8 % wrong; the tag "90" 9 %. Bug: the comb looked for the
    2 / 4 / 8-beat peaks +-1 hop around k x the integer lag, but that is up to k/2 hops off (129 BPM = 46.5 hops:
    the bar peak at 186 was missed from 184 and 2/3 won; Act Of God 0 -> 100 %). Now +-1 / 2 / 4. A half-beat
    (subdivision) comb term trades 2/3 for "no tempo" 1:1 (combHalf, off). Trusted grids: recent 2,000 TEMPO 96.5 %,
    on the beat 92.9 %; held out 94.7 %, 89.6 %. Remaining 2/3 (~1 %): sections with dotted / triplet riffs over the
    kick (Ben Pearce, Yotto, FISHER's Jamming), where 1.5 / 3 / 6 beats are as periodic as 1 / 2 / 4.
  - DJ mix simulation (`beat_eval.py --mixes N`, 2026-11-16): pairs of recent songs within 6 %, B resampled to A's
    tempo on A's 16-beat line, 16-bar blend with a bass swap, every other one riding the pitch back to B's tempo
    over 32 s. 400 mixes: blends are fine (on the beat: A's last minute 89 %, blend halves 96 / 98 %, B's first
    30 s 99 %, then 96 %). Pitch rides were the weak spot: 62.5 % on the beat (steady mode follows a glide ~10 s
    late, and the filter, stiffened by the fold, held the old phase). Now, while TEMPO moves, the filter loosens
    (diffusion 0.04, fold weight 0) and steady smoothing is 10 s / fast after 0.5 % for 2 s: rides 79.3 %, songs
    unchanged. Tried and dropped: a phi-rate trim from the filter peak's drift (ran away: the filter stayed
    confident while a wrong trim slid the beat); a lead detector (activation vs peak) to loosen it (songs -1.5).
    Remaining in rides: a glide that starts during a kick-less part is frozen by the kick gate (by design).
  - Ideas not done: separating songs whose two-marker grids drift (live drums); a richer onset model (more context
    than one hop); per-song learning of the beat pattern.
- Quick tempo nudges (2026-11-16). Traced bad nudges: TEMPO ~9 s late, the beat slides, the filter (stiff, confident)
  holds the old phase and re-locks half a beat out. Oracle (`--oracle-tempo`, true tempo): 96 % on the beat, so
  tempo tracking is the lever. Kick-grid tempo (§2.4): nudges 74.9 -> 77.6 %, songs / blends / rides unchanged
  (recent 92.8 -> 92.7 %, held out 89.7 %). The simulation's nudge sat 30 s after the blend, about bar 32 of B where
  house tracks change section; now 10-80 s at random (baseline 74.9 % vs 75.5 % at the old place). Phase filter
  tempo dimension (`pfTempoRows`, off by default): see §5.1. Tried, not kept: steady TEMPO following the filter's
  tempo (rows nearly flat, noisy: hurt blends); tempo jumps between rows; turning the fold off while the filter's
  tempo disagrees (+1 nudges, -0.5 steady); the clock running at the filter's tempo; drift following at
  autocorrelation confidence 0.12 (+1.6 nudges, songs -1.2 %), also only when the filter agrees (no gain); tempo
  diffusion in breakdowns (no gain); kick-band flux peaks as the grid's input (too noisy: steady -1 to -4 %); a 6 s
  tempo window (+1 nudges, held out -0.7 %); faster steady smoothing (+0.5).

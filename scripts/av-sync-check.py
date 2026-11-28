#!/usr/bin/env python3.11
"""Measure audio/video sync from a video of reforce's sync test flash (top bar Flash, docs/usage.md).

    scripts/av-sync-check.py OBS_RECORDING.mov                          what stream viewers get
    scripts/av-sync-check.py --local PHONE.MOV --reference OBS_RECORDING.mov   what you see and hear in the room

The flash is white for the first quarter of each beat. From the video this finds the flash grid (tempo and phase, to
about a millisecond from the many beats), then folds the audio's onsets over one beat relative to the flash. The answer
is where the kick lands: + = after the flash (visuals early), - = before (visuals late).

OBS recording (line audio): the kick is the strongest 30-250 Hz plus 2-6 kHz onset. Fix: Render Delay on OBS's Syphon Client source
(visuals early) or the audio source's Sync Offset (visuals late).

Phone video of the screen from where you listen (normal speed, not slo-mo): the room smears the bass, so the kick is
found by matching the phone's sound pattern (120 Hz and up) against an OBS recording of the same track (--reference),
whose kick is known. Only the flashing pixels (the screen) count, and the phone's partly-lit frames (rolling shutter)
time each flash onset to a fraction of a frame. Fix: change the Beat offset by the answer (+ = lower it, - = raise it),
then OBS's Render Delay by the same change. Needs ffmpeg, numpy, scipy.
"""
import argparse
import subprocess
import sys

import numpy as np
import scipy.signal as sg

W, H, SR, RES = 64, 36, 44100, 0.002  # analysis frame size, audio rate, profile bin (s)


def ffmpeg(args):
    return subprocess.run(["ffmpeg", "-v", "error", *args], check=True, capture_output=True).stdout


def flash_grid(path):
    """(period s, time of the flash onset at beat 0 s, description)."""
    # Real frame times (phones record a variable frame rate; OBS drops frames), one per decoded frame.
    pts = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                          "frame=best_effort_timestamp_time", "-of", "csv=p=0", path],
                         check=True, capture_output=True, text=True).stdout.split()
    pts = np.array([float(x.strip(",")) for x in pts if x.strip(",") not in ("", "N/A")])
    v = np.frombuffer(ffmpeg(["-i", path, "-fps_mode", "passthrough", "-vf", f"scale={W}:{H},format=gray",
                              "-f", "rawvideo", "-"]), np.uint8).reshape(-1, W * H).astype(float)
    n = min(len(v), len(pts))
    v, pts = v[:n], pts[:n]
    # The flashing pixels (all of a screen grab; the screen in a phone video), as 0..1 of the way from dark to white.
    swing = np.percentile(v, 95, axis=0) - np.percentile(v, 5, axis=0)
    if swing.max() < 20:
        sys.exit(f"{path}: no flashing found (the picture hardly changes)")
    m = v[:, swing > 0.5 * swing.max()].mean(1)
    dark, bright = np.percentile(m, 5), np.percentile(m, 95)
    fr = np.clip((m - dark) / (bright - dark), 0, 1)
    camera = np.mean((fr > 0.05) & (fr < 0.95)) > 0.02  # partly lit frames: a camera, not a screen grab
    f = float(np.median(np.diff(pts)))
    rises = []
    for i in np.where((fr[1:] > 0.5) & (fr[:-1] <= 0.5))[0] + 1:
        j = i - 1
        if camera:
            # A camera frame integrates [t, t + f): a partly lit frame before the first white one lit up (1 - r) in.
            rises.append(pts[j] + (1 - fr[j]) * f if fr[j] > 0.05 else pts[i])
        else:
            rises.append((pts[j] + pts[i]) / 2)  # a screen grab samples instants: somewhere between the two
    rises = np.array(rises)
    if len(rises) < 8:
        sys.exit(f"{path}: fewer than 8 flashes found: was Flash on and the beat clock locked?")
    gap = np.median(np.diff(rises))
    k = np.concatenate([[0], np.cumsum(np.round(np.diff(rises) / gap))])  # beat numbers, counted gap by gap
    if np.diff(k).max() > 1:
        print(f"{path}: warning: missed flashes (skipped beats or a tempo change)")
    p, t0 = np.polyfit(k, rises, 1)
    sd = np.std(rises - (t0 + k * p))
    desc = (f"{len(rises)} flashes at {1 / f:.3g} fps ({'camera' if camera else 'screen grab'}), {60 / p:.2f} BPM, "
            f"onset +/- {sd / np.sqrt(len(rises)) * 1e3:.1f} ms")
    return p, t0, desc


def onset_profile(path, p, t0, lo_hz, hi_hz):
    """Onset strength over one beat, bin 0 = the flash onset."""
    a = np.frombuffer(ffmpeg(["-i", path, "-map", "0:a:0", "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"]), np.float32)
    x = sg.sosfiltfilt(sg.butter(4, lo_hz / (SR / 2), "high", output="sos"), a)
    if hi_hz:
        x = sg.sosfiltfilt(sg.butter(4, hi_hz / (SR / 2), output="sos"), x)
    env = sg.sosfiltfilt(sg.butter(2, 80 / (SR / 2), output="sos"), np.abs(x))
    on = np.diff(env, prepend=env[0]).clip(0)
    ph = ((np.arange(len(a)) / SR - t0) / p) % 1.0
    h, _ = np.histogram(ph, int(round(p / RES)), (0, 1), weights=on)
    return h / h.max()


def signed_ms(i, n, p):  # profile bin -> ms in (-P/2, P/2]
    d = i / n * p * 1e3
    return d - p * 1e3 if d > p * 500 else d


def kick_ms(path, p, t0):
    # Bass alone can tie with a bass note just after the kick: the kick's 2-6 kHz click settles it.
    h = onset_profile(path, p, t0, 30, 250) + onset_profile(path, p, t0, 2000, 6000)
    return signed_ms(int(np.argmax(h)), len(h), p)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("video")
    ap.add_argument("--local", action="store_true", help="the video is a phone filming the screen in the room")
    ap.add_argument("--reference", help="an OBS recording of the same track (needed with --local)")
    args = ap.parse_args()
    if args.local and not args.reference:
        sys.exit("--local needs --reference OBS_RECORDING (room bass is too smeared to find the kick on its own)")

    p, t0, desc = flash_grid(args.video)
    print(f"video: {desc}")
    if not args.reference:
        kick = kick_ms(args.video, p, t0)
    else:
        rp, rt0, rdesc = flash_grid(args.reference)
        rkick = kick_ms(args.reference, rp, rt0)
        print(f"reference: {rdesc}; its kick {rkick:+.0f} ms from the flash")
        # The same music in both: the shift that best lines up the two sound patterns (summed over the bands).
        n = min(int(round(p / RES)), int(round(rp / RES)))
        total = np.zeros(n)
        for lo_hz, hi_hz in ((120, 400), (400, 2000), (2000, 6000), (6000, 0)):
            a = onset_profile(args.video, p, t0, lo_hz, hi_hz)[:n]
            b = onset_profile(args.reference, rp, rt0, lo_hz, hi_hz)[:n]
            a, b = a - a.mean(), b - b.mean()
            c = np.array([np.dot(np.roll(b, s), a) for s in range(n)]) / (np.linalg.norm(a) * np.linalg.norm(b))
            print(f"  {lo_hz}-{hi_hz or ''} Hz: {signed_ms(int(np.argmax(c)), n, p):+.0f} ms vs the reference "
                  f"(match {c.max():.2f})")
            total += c
        best = int(np.argmax(total))
        runner = max(total[i] for i in range(n) if min(abs(i - best), n - abs(i - best)) * RES > 0.03)
        shift = signed_ms(best, n, p)
        print(f"  all: {shift:+.0f} ms (score {total[best]:.2f}, next best {runner:.2f})")
        kick = rkick + shift
        if kick > p * 500:
            kick -= p * 1e3
        elif kick <= -p * 500:
            kick += p * 1e3
    print()
    when = f"{abs(kick):.0f} ms {'AFTER' if kick > 0 else 'BEFORE'} the flash"
    if args.local:
        print(f"In the room the kick comes {when}: the visuals are {abs(kick):.0f} ms {'early' if kick > 0 else 'late'}.\n"
              f"Fix: {'lower' if kick > 0 else 'raise'} the Beat offset by {abs(kick):.0f} ms, and change OBS's "
              f"Render Delay by the same amount ({'less' if kick > 0 else 'more'}).")
    elif kick > 0:
        print(f"The kick comes {when}: viewers see the visuals {kick:.0f} ms early.\n"
              f"Fix: OBS Syphon Client source > Filters > Render Delay ~{kick:.0f} ms.")
    else:
        print(f"The kick comes {when}: viewers see the visuals {-kick:.0f} ms late.\n"
              f"Fix: OBS audio source > Advanced Audio Properties > Sync Offset +{-kick:.0f} ms.")
    print("(Within one beat the answer is ambiguous by a whole beat: check it is the plausible one.)")


if __name__ == "__main__":
    main()

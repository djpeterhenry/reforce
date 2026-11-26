# Lyrics: word-timed karaoke over the output

For songs that have lyrics files, reforce shows the words as karaoke over the output (window and Syphon): two
centered lines, the line being sung filling from gray to white word by word, the next line waiting in the other row.
It follows the deck you hear in rekordbox, so cue jumps, loops, beat jumps and the pitch fader stay in sync.

It needs the rekordbox deck feed (rkbx_link's "reforce" output, see [rhythm-design.md](rhythm-design.md)) and a
lyrics file per song. Without either, nothing shows. Off by default: Rhythm card, Lyrics, Show lyrics.

## Where the timing comes from

Lyrics files are timed in seconds of the audio file rekordbox plays. rkbx_link reports each deck's position in that
same file, so one is looked up in the other with no tempo math: rekordbox plays the file unwarped (a beatgrid only
marks where the beats are), and the pitch fader only changes how fast the position runs.

One catch: MP3 files with a LAME gapless header. rekordbox (and Ableton) ignore the header and play the encoder delay
and the header frame as silence, where ffmpeg and most libraries trim both, so a tool that decodes with ffmpeg sees
such a file start 51 ms (2257 samples at 44.1 kHz) early. Measured against Ableton warp markers on about 300 songs:
gapless MP3s 52 ms off, plain MP3s and WAV / AIFF in line. djtools decodes MP3s with that silence put back before
timing them; other tools should do the same (ffprobe's `start_time` + one frame, 1152 samples).

Between rkbx_link's readings (about 120 a second) each deck has a small clock (`sync::DeckClock`): anchored where the
position last changed (rekordbox moves it in audio-buffer steps), run on at the deck's rate (tempo / original tempo),
eased toward each new reading; a reading far off is a jump (cue, loop, scratch) and is taken as it is; a position
standing still for 0.15 s is a stop. The words are shown for the position reached the beat offset
(`reforce.BeatClock.OffsetMs`) from now, as the beat clock is, minus the lyrics' own nudge.

Which deck: the loudest playing deck with lyrics, by the controller's channel faders and crossfader (as now playing);
a louder one takes over after leading for a second. Without controller MIDI, rekordbox's master deck.

## Lyrics files

A folder (`reforce.Lyrics.Folder`, default `~/Music/PioneerDJ/LocalSamples/Lyrics`) holds one JSON file per song and an
`index.json`. reforce looks at `index.json` every second and reads the song files again whenever it changes, so a
tool that rewrites the index after every save gets its edits shown live.

`index.json` names each song's file by the artist and title rekordbox shows for the track (the title before any run of
spaces rkbx_link cuts off; case and repeated spaces do not matter):

```json
{"version": 1, "songs": [
  {"artist": "Night & Harbor", "title": "Silver Lakes (Acapella)", "file": "Night & Harbor - Silver Lakes (Acapella).lyrics.json",
   "reviewed": true}
]}
```

A song file (other fields are ignored):

```json
{
  "artist": "Night & Harbor",
  "title": "Silver Lakes (Acapella)",
  "offset": 0.0,
  "reviewed": true,
  "lines": [
    {"t0": 2.30, "t1": 5.16, "text": "Slow boats over silver lakes", "words": [
      {"t0": 2.30, "t1": 2.67, "text": "Slow"}, {"t0": 2.79, "t1": 3.33, "text": "boats"},
      {"t0": 3.39, "t1": 3.59, "text": "over"}, {"t0": 3.67, "t1": 4.54, "text": "silver"},
      {"t0": 4.68, "t1": 5.16, "text": "lakes"}]}
  ]
}
```

- `t0` / `t1`: seconds into the audio file; `offset` (s) is added to all of them (+ = later).
- A line shows its `words`, one space apart; a line without words is skipped.
- `reviewed`: with `reforce.Lyrics.ReviewedOnly`, only these show.

The files above are the interface: any tool that writes them works. The author makes them with their own DJ library
tools ([djtools](https://github.com/djpeterhenry/djtools): `lyrics.py` looks the lyrics up on LRCLIB, separates the vocals with Demucs, finds the
song's structure with Whisper and times every word with forced alignment; `lyrics_server.py` is a page to review and
correct them while listening). Lyrics are copyrighted: keep lyrics files local, never in this repository.

## On screen

Line *i* of a song sits in row *i* mod 2. It appears 4 s before its first word (once the line two before it, which
shares its row, is done), and goes 2 s after its last word, or as soon as the line two after it needs the row. Lines
fade in and out over a quarter second; a new song fades the old one out first. Long lines get smaller type to fit
(never cut). The adaptive shadow of the now-playing text (`reforce.NowPlaying.Shadow`) also keeps the lyrics readable
on bright pictures.

Like the now-playing text, lyrics are drawn over the finished frame: never in the room lights' picture.

## Settings

Rhythm card, Lyrics; saved to settings.json:

| Setting | Key | Default |
| --- | --- | --- |
| Show lyrics | `reforce.Lyrics.Show` | off |
| Reviewed only | `reforce.Lyrics.ReviewedOnly` | off |
| Position: bottom, center, top | `reforce.Lyrics.Position` | bottom |
| Size, % of 64 px type at 1080 p | `reforce.Lyrics.Size` | 100 |
| Words later by (ms) | `reforce.Lyrics.OffsetMs` | 0 |
| Folder | `reforce.Lyrics.Folder` (settings.json only) | `~/Music/PioneerDJ/LocalSamples/Lyrics` |

The panel's status line says which deck and line are showing, and how many songs the folder has.

## Trying it without rekordbox

A `--background` instance listens for a test feed with `--rkbx-port PORT` (no controller MIDI, so the live copy keeps
the controller). Send it datagrams in rkbx_link's format (`rkbx 1 <t> <master>`, then `d <deck> <position> <beat>
<bpm> <original bpm>` and `t <deck> <title>\t<artist>\t<tags>` lines) and watch its Syphon server.

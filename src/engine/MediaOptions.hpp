#pragma once
// Per-movie options (reforce-only): "<stem>.reforce.txt" beside a movie, e.g.
//   " dance_static.mov"  ->  " dance_static.reforce.txt"
// with G-Force-style "Key = value" lines, // comments, optional quotes:
//   Loop  = 1     play on repeat (0 = play once), overrides reforce.Video.Sprite.Loop / Video.Layer.Loop
//   Rate  = 0.5   playback rate (>= 0)
//   Start = 12    start this many seconds in (a loop jumps back here)
//   End   = 40    stop (play once) or loop back (Loop = 1) at this many seconds; ignored unless > Start
//   BPM         = 90.04  the dancers' tempo (tools/dance_sync): beat-synced playback (MovieBeatSync.hpp)
//   BeatStart   = 0.49   seconds of a downbeat in the movie
//   BeatsPerBar = 4
//   Black = 0.05   luminance levels of a movie sprite (PLAN 5K): l = ((l - Black) / (White - Black))^Gamma, clamped;
//   White = 0.9    Black and White 0..1 (defaults 0 and 1; ignored unless White > Black), Gamma > 0 (default 1;
//   Gamma = 0.7    below 1 brightens), multiplied by the global reforce.MovieSprite.Gamma
//   ColorKey = 0   with reforce.Syphon.Alpha: the movie sprite is opaque in the Syphon alpha (not keyed out
//                  where it is dark, like the dancers). Default 1 (keyed like the visuals). Needs full-color movie
//                  sprites (reforce.MovieSprite.Color = over): otherwise the movie is part of the visuals.
// Times are seconds into the movie; "m:ss" (e.g. 1:05.5) works too.
// A plain "<stem>.txt" stays G-Force's sprite params file (it replaces the
// package defaults), so reforce options live in their own file. Written by
// tools/media_review (Loop toggle); unknown keys are ignored.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace reforce::engine {

struct MediaOptions {
    std::optional<bool> loop;
    std::optional<double> rate;
    std::optional<double> start;
    std::optional<double> end;
    std::optional<double> bpm;          // > 0
    std::optional<double> beatStart;    // >= 0
    std::optional<int> beatsPerBar;     // 1..32
    std::optional<double> black;        // 0..1, below White
    std::optional<double> white;        // 0..1, above Black
    std::optional<double> gamma;        // > 0 (up to 10)
    std::optional<bool> colorKey;       // ColorKey (also ColorKey)
};

std::filesystem::path mediaOptionsPath(const std::filesystem::path& movie);
MediaOptions parseMediaOptions(std::string_view text);
// Missing file: all unset.
MediaOptions loadMediaOptions(const std::filesystem::path& movie);

// Frame-interpolated copies (tools/dance_sync/interpolate.py): "<stem>.rife<N>x.<ext>" beside a movie, same start
// and length, played in its place and never listed as a movie of their own (isSpriteVideoFile).
bool isInterpolatedMovieName(const std::filesystem::path& p);
// The interpolated copy of `movie` (highest N when there are several), else `movie` itself.
std::filesystem::path interpolatedMovie(const std::filesystem::path& movie);

}  // namespace reforce::engine

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>

#include "engine/MediaOptions.hpp"
#include "engine/MovieBeatSync.hpp"
#include "engine/Sprites.hpp"

using namespace reforce::engine;
namespace fs = std::filesystem;

namespace {

BeatSample link(double bpm, double beat) {
    BeatSample s;
    s.confident = true;
    s.source = 1;
    s.bpm = bpm;
    s.beat = beat;
    s.bars = true;
    return s;
}

// Movie position in movie beats for media time u.
double moviePos(const ClipBeats& c, double u) { return (u - c.beatStart) * c.bpm / 60.0; }

}  // namespace

TEST_CASE("media options: beat keys") {
    const MediaOptions o = parseMediaOptions("BPM = 90.04  // x\nBeatStart = 0.49\nBeatsPerBar = 3\n");
    REQUIRE(o.bpm);
    CHECK(*o.bpm == doctest::Approx(90.04));
    REQUIRE(o.beatStart);
    CHECK(*o.beatStart == doctest::Approx(0.49));
    CHECK(o.beatsPerBar == std::optional<int>(3));
    CHECK(!parseMediaOptions("BPM = -5\nBeatsPerBar = 0").bpm);
    CHECK(!parseMediaOptions("BeatsPerBar = 0").beatsPerBar);
}

TEST_CASE("media options: ColorKey (Syphon alpha)") {
    CHECK(!parseMediaOptions("BPM = 90").colorKey);
    CHECK(parseMediaOptions("ColorKey = 0").colorKey == std::optional<bool>(false));
    CHECK(parseMediaOptions("colourkey = off // opaque").colorKey == std::optional<bool>(false));
    CHECK(parseMediaOptions("ColorKey = 1").colorKey == std::optional<bool>(true));
    CHECK(!parseMediaOptions("ColorKey = maybe").colorKey);
    CHECK(parseMediaOptions("Loop = yes").loop == std::optional<bool>(true));
    CHECK(parseMediaOptions("Loop = 0").loop == std::optional<bool>(false));
}

TEST_CASE("interpolated movies: found beside the original, hidden from lists, share its options") {
    const fs::path dir = fs::temp_directory_path() / "reforce_test_rife";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path movie = dir / " dance.mov";
    std::ofstream(movie) << "x";
    CHECK(interpolatedMovie(movie) == movie);
    std::ofstream(dir / " dance.rife2x.mov") << "x";
    std::ofstream(dir / " dance.rife4x.mov") << "x";
    std::ofstream(dir / " dance_2.rife8x.mov") << "x";  // another movie's
    std::ofstream(dir / " dance.rife4x.reforce.txt") << "x";  // not a movie
    CHECK(interpolatedMovie(movie) == dir / " dance.rife4x.mov");
    CHECK(isInterpolatedMovieName(dir / " dance.rife4x.mov"));
    CHECK(!isInterpolatedMovieName(movie));
    CHECK(!isInterpolatedMovieName(dir / "rife.mov"));
    CHECK(!isSpriteVideoFile(dir / " dance.rife4x.mov"));
    CHECK(isSpriteVideoFile(movie));
    CHECK(mediaOptionsPath(dir / " dance.rife4x.mov") == dir / " dance.reforce.txt");
    fs::remove_all(dir);
}

TEST_CASE("movie beat sync: rate follows the clock, phase locks to the bar") {
    ClipBeats clip;
    clip.bpm = 90;
    clip.beatStart = 0.37;
    MovieBeatSync sync(clip, 0.0, 1.0);
    const double dt = 1.0 / 60, bpm = 99;
    double beat = 13.3;  // arbitrary session position
    double u = 0, lastU = -1;
    bool forward = true;
    for (int i = 0; i < 60 * 20; ++i) {
        beat += dt * bpm / 60;
        u = sync.step(dt, link(bpm, beat), true);
        if (u < lastU) forward = false;
        lastU = u;
    }
    CHECK(forward);
    CHECK(sync.synced());
    CHECK(sync.ratio() == 1.0);
    CHECK(sync.rate() == doctest::Approx(99.0 / 90.0).epsilon(0.01));
    // the movie's bar position matches the music's (beats 0, 4, 8 ... are downbeats)
    const double err = std::fmod(moviePos(clip, u) - beat, 4.0);
    CHECK(std::min(std::abs(err), 4 - std::abs(err)) < 0.02);
}

TEST_CASE("movie beat sync: picks the octave nearest rate 1 and stays put near the boundary") {
    ClipBeats clip;
    clip.bpm = 90;
    MovieBeatSync sync(clip, 0.0, 1.0);
    double beat = 0;
    const double dt = 1.0 / 60;
    for (int i = 0; i < 120; ++i) sync.step(dt, link(174, beat += dt * 174 / 60), true);
    CHECK(sync.ratio() == 0.5);  // 174 BPM: play half a movie beat per music beat (rate 0.97)
    CHECK(sync.rate() == doctest::Approx(0.97).epsilon(0.02));
    // 128 BPM: r=1 gives 1.42, r=0.5 gives 0.71: both ~0.35 from rate 1 in log terms -> keeps 0.5
    for (int i = 0; i < 120; ++i) sync.step(dt, link(128, beat += dt * 128 / 60), true);
    CHECK(sync.ratio() == 0.5);
    for (int i = 0; i < 120; ++i) sync.step(dt, link(95, beat += dt * 95 / 60), true);
    CHECK(sync.ratio() == 1.0);
}

TEST_CASE("movie beat sync: phase corrections are gentle and never run backwards") {
    ClipBeats clip;
    clip.bpm = 120;
    MovieBeatSync sync(clip, 0.0, 1.0);
    const double dt = 1.0 / 60;
    double beat = 0;
    for (int i = 0; i < 60; ++i) sync.step(dt, link(120, beat += dt * 2), true);
    // the session jumps half a beat (a Link re-sync): the movie slews over, not jumps
    beat += 0.5;
    double maxRate = 0, minRate = 10;
    for (int i = 0; i < 60 * 6; ++i) {
        sync.step(dt, link(120, beat += dt * 2), true);
        maxRate = std::max(maxRate, sync.rate());
        minRate = std::min(minRate, sync.rate());
    }
    CHECK(maxRate <= 1.0 * 1.26);
    CHECK(minRate >= 1.0 * 0.74);
    CHECK(std::abs(sync.phaseError()) < 0.02);
}

TEST_CASE("movie beat sync: without a clock it plays at its own rate, continuously") {
    ClipBeats clip;
    clip.bpm = 100;
    MovieBeatSync sync(clip, 5.0, 1.0);
    BeatSample none;  // not confident
    double u = 0;
    for (int i = 0; i < 60; ++i) u = sync.step(1.0 / 60, none, true);
    CHECK(!sync.synced());
    CHECK(u == doctest::Approx(6.0));
    // disabled: same, even with a clock
    u = sync.step(0.5, link(100, 3.0), false);
    CHECK(u == doctest::Approx(6.5));
    // a movie without a BPM never syncs
    MovieBeatSync noBpm(ClipBeats{}, 0.0, 1.0);
    noBpm.step(0.1, link(120, 1.0), true);
    CHECK(!noBpm.synced());
}

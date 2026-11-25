#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

#include <unistd.h>

#include "sync/Lyrics.hpp"

using namespace reforce::sync;

namespace {

const char* kSidecar = R"J({
 "version": 1,
 "song": "Night & Harbor - Silver Lakes (Acapella).als",
 "artist": "Night & Harbor",
 "title": "Silver Lakes (Acapella)",
 "offset": 0.05,
 "reviewed": true,
 "source": {"lyrics": "lrclib:1", "separated": false},
 "lines": [
  {"t0": 2.3, "t1": 5.16, "text": "Slow boats over silver lakes", "note": "x",
   "words": [{"t0": 2.3, "t1": 2.67, "text": "Slow", "score": 0.19}, {"t0": 2.79, "t1": 3.33, "text": "boats"},
             {"t0": 3.39, "t1": 3.59, "text": "over"}, {"t0": 3.67, "t1": 4.54, "text": "silver"},
             {"t0": 4.68, "t1": 5.16, "text": "lakes"}]},
  {"t0": 5.19, "t1": 7.7, "text": "Under the moon", "words": [{"t0": 5.19, "t1": 5.6, "text": "Under"},
             {"t0": 5.7, "t1": 6.0, "text": "the"}, {"t0": 6.1, "t1": 7.7, "text": "moon"}]},
  {"t0": 1.0, "t1": 1.0, "text": "empty", "words": []},
  {"t0": 9.78, "t1": 12.0, "text": "Paint rain ’n café 😀", "words": [{"t0": 9.78, "t1": 10.5, "text": "Paint"},
             {"t0": 10.6, "t1": 11.0, "text": "rain"}, {"t0": 11.1, "t1": 11.4, "text": "’n café"},
             {"t0": 11.5, "t1": 12.0, "text": "😀"}]}
 ]
})J";

LyricsSong song(std::initializer_list<std::pair<double, double>> lines) {
    LyricsSong s;
    for (auto [a, b] : lines) {
        LyricLine l;
        l.words.push_back({a, (a + b) / 2, "one"});
        l.words.push_back({(a + b) / 2, b, "two"});
        l.t0 = a;
        l.t1 = b;
        s.lines.push_back(l);
    }
    return s;
}

DeckFrame at(double pos, double bpm = 124, double orig = 124) {
    DeckFrame f;
    f.position = pos;
    f.gridBeat = 0;
    f.bpm = bpm;
    f.originalBpm = orig;
    return f;
}

}  // namespace

TEST_CASE("lyrics: sidecar") {
    std::string err;
    auto s = parseLyricsSidecar(kSidecar, &err);
    REQUIRE_MESSAGE(s, err);
    CHECK(s->artist == "Night & Harbor");
    CHECK(s->title == "Silver Lakes (Acapella)");
    CHECK(s->offset == doctest::Approx(0.05));
    CHECK(s->reviewed);
    REQUIRE(s->lines.size() == 3);  // the line without words is dropped
    CHECK(s->lines[0].text() == "Slow boats over silver lakes");
    CHECK(s->lines[0].t0 == doctest::Approx(2.3));
    CHECK(s->lines[0].t1 == doctest::Approx(5.16));
    // \u escapes (a surrogate pair too) become UTF-8; a space inside a word would break the layout's word count.
    CHECK(s->lines[2].words[2].text == "\xE2\x80\x99n_caf\xC3\xA9");
    CHECK(s->lines[2].words[3].text == "\xF0\x9F\x98\x80");
    CHECK_FALSE(parseLyricsSidecar("{\"lines\": [", &err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(parseLyricsSidecar("[]"));
}

TEST_CASE("lyrics: index and keys") {
    auto idx = parseLyricsIndex(R"J({"version": 1, "songs": [
        {"artist": "Paper Moon", "title": "Tides (Lantern Remix)", "file": "Paper Moon - Tides (Lantern Remix).lyrics.json", "reviewed": false},
        {"artist": "X", "title": "Y", "file": "../etc/passwd"}, {"artist": "X", "title": "Z", "file": "a/b.json"}]})J");
    REQUIRE(idx);
    REQUIRE(idx->size() == 1);
    CHECK((*idx)[0].file == "Paper Moon - Tides (Lantern Remix).lyrics.json");
    CHECK(lyricsKey(" Night  &  Harbor", "Silver Lakes (Acapella) ") == lyricsKey("night & harbor", "SILVER LAKES (ACAPELLA)"));
    CHECK(lyricsKey("A", "B") != lyricsKey("A B", ""));
}

TEST_CASE("lyrics: deck clock follows the position between frames") {
    DeckClock c;
    CHECK_FALSE(c.known());
    c.frame(0.0, at(10.0));
    CHECK(c.known());
    CHECK_FALSE(c.playing());
    CHECK(c.position(0.5) == doctest::Approx(10.0));
    // Playing at +4 % (pitched): readings every 1/120 s, moving in 512-sample buffers (11.6 ms).
    const double rate = 128.96 / 124.0, buf = 512.0 / 44100.0;
    double worst = 0;
    for (int k = 1; k < 1200; ++k) {
        const double now = k / 120.0;
        const double truth = 10.0 + rate * now;
        const double reported = 10.0 + std::floor(rate * now / buf) * buf;
        c.frame(now, at(reported, 128.96, 124));
        if (k > 60) worst = std::max(worst, std::abs(c.position(now + 0.004) - (truth + rate * 0.004)));
    }
    CHECK(c.playing());
    CHECK(c.rate() == doctest::Approx(rate));
    CHECK(worst < 0.02);
    // A cue jump snaps.
    c.frame(10.0, at(42.0, 128.96, 124));
    CHECK(c.position(10.0) == doctest::Approx(42.0));
    // Unchanged for longer than stopSeconds: stopped where it last was.
    for (int k = 1; k <= 30; ++k) c.frame(10.0 + k / 120.0, at(42.0, 128.96, 124));
    CHECK_FALSE(c.playing());
    CHECK(c.position(11.0) == doctest::Approx(42.0));
    // Nothing loaded: forgotten.
    c.frame(11.0, at(NAN));
    CHECK_FALSE(c.known());
}

TEST_CASE("lyrics: the loudest playing deck with lyrics, switching after it leads a while") {
    KaraokePicker p({0.1, 1.0});
    bool has[4] = {true, true, false, true};
    double level[4] = {1, 0, 1, 0};
    bool playing[4] = {true, true, true, false};
    CHECK(p.update(0, has, level, playing, -1, true) == 0);
    level[1] = 1;
    CHECK(p.update(1, has, level, playing, -1, true) == 0);  // a tie keeps deck 1
    level[0] = 0.5;
    CHECK(p.update(2, has, level, playing, -1, true) == 0);
    CHECK(p.update(2.9, has, level, playing, -1, true) == 0);
    CHECK(p.update(3.0, has, level, playing, -1, true) == 1);
    level[1] = 0;                                              // faded out: back to what is heard, at once
    CHECK(p.update(3.1, has, level, playing, -1, true) == 0);
    level[0] = 0.05;
    CHECK(p.update(3.2, has, level, playing, -1, true) == -1);  // deck 3 is loud but has no lyrics
    // No controller: rekordbox's master deck.
    KaraokePicker q;
    CHECK(q.update(0, has, level, playing, 1, false) == 1);
    CHECK(q.update(0, has, level, playing, 2, false) == -1);
}

TEST_CASE("lyrics: two rows, a line waits in its row while the one before is sung") {
    KaraokeConfig cfg;  // preview 4, hold 2, linger .3, fade .25
    const LyricsSong s = song({{10, 12}, {12.2, 14}, {14.2, 16}, {40, 42}});
    auto shown = [&](double t) {
        std::vector<int> v;
        for (const KaraokeShown& k : karaokeLines(s, t, cfg))
            if (k.opacity > 0.99) v.push_back(k.line);
        return v;
    };
    CHECK(shown(5).empty());
    CHECK(shown(6.5) == std::vector<int>{0});        // 4 s ahead
    CHECK(shown(9) == std::vector<int>{0, 1});       // the next line waits below
    CHECK(shown(12.7) == std::vector<int>{1, 2});    // line 2 took line 0's row once line 0 was done (+ linger)
    CHECK(shown(17.0) == std::vector<int>{2});       // line 1 gone 2 s after its end; line 2 in its hold
    CHECK(shown(20).empty());                        // a long gap: nothing
    CHECK(shown(36.5) == std::vector<int>{3});
    for (const KaraokeShown& k : karaokeLines(s, 12.7, cfg)) CHECK(k.row == k.line % 2);
    // Fades: in over `fade` after appearing.
    auto op = [&](double t, int line) {
        for (const KaraokeShown& k : karaokeLines(s, t, cfg))
            if (k.line == line) return k.opacity;
        return 0.0;
    };
    CHECK(op(6.0, 0) == doctest::Approx(0.0));
    CHECK(op(6.125, 0) == doctest::Approx(0.5));
    CHECK(op(12.3, 2) == doctest::Approx(0.0).epsilon(0.01));
    CHECK(op(12.425, 2) == doctest::Approx(0.5));
    CHECK(op(12.425, 0) == doctest::Approx(0.5));    // line 0 fades out as line 2 fades in (a crossfade)
}

TEST_CASE("lyrics: fill across each word by its timing") {
    LyricLine l;
    l.words = {{1.0, 2.0, "aa"}, {2.5, 3.0, "bb"}};
    const std::vector<float> spans = {0, 40, 55, 100};
    CHECK(karaokeFillX(l, 0.5, spans) == doctest::Approx(0));
    CHECK(karaokeFillX(l, 1.5, spans) == doctest::Approx(20));
    CHECK(karaokeFillX(l, 2.2, spans) == doctest::Approx(40));   // between words: the sung word's end
    CHECK(karaokeFillX(l, 2.75, spans) == doctest::Approx(77.5));
    CHECK(karaokeFillX(l, 9, spans) == doctest::Approx(100));
    CHECK(karaokeFillX(l, 9, {}) == doctest::Approx(0));          // not laid out yet
}

TEST_CASE("lyrics: library reads the folder and follows edits") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("reforce-lyrics-test-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    LyricsLibrary lib(dir.string(), false);
    lib.scan();
    CHECK(lib.find("Night & Harbor", "Silver Lakes (Acapella)") == nullptr);
    CHECK(lib.status().find("no lyrics yet") != std::string::npos);
    {
        std::ofstream(dir / "Lakes.lyrics.json") << kSidecar;
        std::ofstream(dir / "index.json") << R"J({"version": 1, "songs": [{"artist": "Night & Harbor",
            "title": "Silver Lakes (Acapella)", "file": "Lakes.lyrics.json", "reviewed": true},
            {"artist": "Gone", "title": "Missing", "file": "Missing.lyrics.json"}]})J";
    }
    const auto v0 = lib.version();
    lib.scan();
    CHECK(lib.version() > v0);
    auto s = lib.find("night & harbor", "silver lakes (acapella)");
    REQUIRE(s);
    CHECK(s->lines.size() == 3);
    CHECK(lib.status().find("1 song in") != std::string::npos);
    CHECK(lib.status().find("1 unreadable") != std::string::npos);
    // An edit: the sidecar and then the index are rewritten (djtools' save); the new text shows.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::string edited = kSidecar;
        edited.replace(edited.find("\"Slow\""), 6, "\"Fast\"");
        std::ofstream(dir / "Lakes.lyrics.json") << edited;
        std::ofstream(dir / "index.json") << R"J({"version": 1, "songs": [{"artist": "Night & Harbor",
            "title": "Silver Lakes (Acapella)", "file": "Lakes.lyrics.json"}]})J";
    }
    lib.scan();
    s = lib.find("Night & Harbor", "Silver Lakes (Acapella)");
    REQUIRE(s);
    CHECK(s->lines[0].words[0].text == "Fast");
    fs::remove_all(dir);
}

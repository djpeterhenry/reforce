#pragma once
// reforce extension: word-timed lyrics as karaoke over the output, for the songs that have them.
//
// The lyrics come from djtools (lyrics.py makes them, lyrics_server.py reviews and corrects them): one JSON sidecar
// per song in a folder (reforce.Lyrics.Folder, by default ~/Music/PioneerDJ/LocalSamples/Lyrics), and an index.json
// there that names each song's sidecar by the artist and title the rekordbox export gives the track, which is what
// rkbx_link reports (sync/DeckFeed's "t" lines).
//   index.json     {"version": 1, "songs": [{"artist", "title", "file", "reviewed"}, ...]}
//   <song>.lyrics.json  {"artist", "title", "offset", "reviewed", "lines": [{"t0", "t1", "text",
//                        "words": [{"t0", "t1", "text"}, ...]}, ...], ...}
// Times are seconds in the audio file rekordbox plays, which is the position rkbx_link sends for a deck (rekordbox
// plays the file unwarped); "offset" (s) is added to all of them.
//
// Pieces, all pure except LyricsLibrary:
//   parseLyricsSidecar / parseLyricsIndex   the two files.
//   DeckClock      one deck's position in its track between rkbx_link frames: anchored where the position last
//                  changed (rekordbox moves it in audio-buffer steps), carried on at the deck's rate (tempo /
//                  original tempo), eased toward each new reading; a jump (cue, loop, scratch) snaps; unchanged for
//                  `stopSeconds`: stopped.
//   KaraokePicker  which deck's lyrics to show: the loudest playing deck that has lyrics (DeckStarts::level), a new
//                  one once it has led for `switchSeconds`; without controller MIDI, rekordbox's master deck.
//   karaokeLines   which lines are on screen at a song time and how opaque: two rows, line i in row i % 2, so a
//                  line waits in its row while the one before it is sung (classic two-line karaoke). A line appears
//                  `preview` s before its first word (not before the line two back, which shares its row, is done),
//                  and goes `hold` s after its last word, or when the line two on needs the row.
//   karaokeFillX   how far a line is sung, as x in its laid-out text: across each word by its own timing.
//   LyricsLibrary  the folder, read on a worker thread: index.json checked every second, sidecars (re)read when the
//                  index changes (djtools rewrites it on every save), so edits in lyrics_server show up live.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "sync/DeckStarts.hpp"

namespace reforce::sync {

struct LyricWord {
    double t0 = 0, t1 = 0;
    std::string text;
};

struct LyricLine {
    double t0 = 0, t1 = 0;
    std::vector<LyricWord> words;
    std::string text() const;   // the words, one space apart (how it is laid out, so word i is the i-th word)
};

struct LyricsSong {
    std::string artist, title;
    double offset = 0;          // s, added to every time
    bool reviewed = false;
    std::vector<LyricLine> lines;   // in time order; each with at least one word
};

struct LyricsIndexEntry {
    std::string artist, title, file;
    bool reviewed = false;
};

std::optional<LyricsSong> parseLyricsSidecar(std::string_view json, std::string* error = nullptr);
std::optional<std::vector<LyricsIndexEntry>> parseLyricsIndex(std::string_view json, std::string* error = nullptr);
// How a song is looked up: lowercase, spaces collapsed, trimmed, "artist\ttitle".
std::string lyricsKey(const std::string& artist, const std::string& title);

class DeckClock {
public:
    double stopSeconds = 0.15;   // position unchanged this long: stopped
    double jumpSeconds = 0.08;   // a reading this far off the estimate: a jump (snap)
    double ease = 0.15;          // share of the error taken from each in-line reading

    // `now`: local receive time (DeckFeed::now()); a non-finite position (nothing loaded) resets.
    void frame(double now, const DeckFrame& f);
    void reset() { *this = DeckClock{}; }
    bool known() const { return have_; }
    bool playing() const { return have_ && rate_ != 0; }
    double rate() const { return rate_; }
    double position(double now) const { return have_ ? anchorPos_ + rate_ * (now - anchorNow_) : 0.0; }

private:
    bool have_ = false;
    double anchorPos_ = 0, anchorNow_ = 0, rate_ = 0;
    double lastPos_ = 0, lastChange_ = 0;
};

struct KaraokePickerConfig {
    double minLevel = 0.1;        // a deck this loud (DeckStarts::level) counts as heard
    double switchSeconds = 1.0;   // a louder deck with lyrics takes over after leading this long
};

class KaraokePicker {
public:
    static constexpr int kDecks = DeckStarts::kDecks;
    explicit KaraokePicker(KaraokePickerConfig cfg = {}) : cfg_(cfg) {}
    // hasLyrics / level / playing per deck; masterDeck (-1 unknown) leads when `fadersSeen` is false.
    int update(double now, const bool hasLyrics[kDecks], const double level[kDecks], const bool playing[kDecks],
               int masterDeck, bool fadersSeen);
    int deck() const { return deck_; }

private:
    KaraokePickerConfig cfg_;
    int deck_ = -1;
    int leader_ = -1;
    double leaderSince_ = 0;
};

struct KaraokeConfig {
    double preview = 4.0;   // s a line shows (unsung) before its first word
    double hold = 2.0;      // s a line stays after its last word when nothing needs its row
    double linger = 0.3;    // s a sung line stays at least, before the line two on may take its row
    double fade = 0.25;     // s of fade in / out
};

struct KaraokeShown {
    int line = 0;           // index into LyricsSong::lines
    int row = 0;            // 0 = upper, 1 = lower
    double opacity = 1;
};

// The lines on screen at song time `t` (seconds in the file, after the song's offset is taken off), oldest first.
std::vector<KaraokeShown> karaokeLines(const LyricsSong& song, double t, const KaraokeConfig& cfg = {});
// x (px from the text box's left) up to which `line` is sung at `t`; `spans` = each word's [x0, x1] as laid out
// (2 per word). 0 before its first word, the last word's end after it, the sung word's end between words.
double karaokeFillX(const LyricLine& line, double t, const std::vector<float>& spans);

class LyricsLibrary {
public:
    // `worker`: look every second on a thread of its own (tests call scan() instead). An empty folder: never finds
    // anything.
    explicit LyricsLibrary(std::string folder, bool worker = true);
    ~LyricsLibrary();
    LyricsLibrary(const LyricsLibrary&) = delete;
    LyricsLibrary& operator=(const LyricsLibrary&) = delete;

    // Any thread: the song for a deck's track (nullptr = none).
    std::shared_ptr<const LyricsSong> find(const std::string& artist, const std::string& title) const;
    std::uint64_t version() const { return version_.load(); }   // bumps when anything was (re)loaded
    std::string status() const;                                 // "12 songs in <folder>" / what went wrong
    const std::string& folder() const { return folder_; }

    // One look at the folder (the worker calls it every second; tests call it directly).
    void scan();

private:
    void loop();

    std::string folder_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> version_{0};
    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<const LyricsSong>> songs_;   // guarded by mu_
    std::string status_;                                               // guarded by mu_
    // Worker-only state.
    std::int64_t indexStamp_ = -3;   // -3 never looked, -2 look again, -1 no index.json
    std::map<std::string, std::pair<std::int64_t, std::shared_ptr<const LyricsSong>>> files_;
    std::thread thread_;
};

}  // namespace reforce::sync

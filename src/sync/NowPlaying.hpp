#pragma once
// reforce extension: the "now playing" text (artist, title, year, and a vocal laid over it), as djtools'
// history_server overlay shows it, but straight from the decks instead of rekordbox's history.
//
// Input, per deck: what is loaded (rkbx_link's track lines: title, artist and the tags djtools appends to the
// title after a run of spaces, e.g. "[#good] [#vocal] [#y03]") and how loud it is in the mix (DeckStarts::level:
// playing x channel fader x crossfader).
//   main   the loudest deck not tagged [#vocal]; a new one takes over once it has led for `switchSeconds`
//          (ties keep the current one). Without controller MIDI (no fader seen) the rekordbox master deck leads.
//          Nothing audible for `hideSeconds`: hidden.
//   vocal  the loudest audible deck tagged [#vocal], once audible for `vocalSeconds`; gone `vocalHoldSeconds`
//          after it is no longer audible.
// Titles are cleaned as history_server does: "(Original ...)" / "(Extended ...)" / "(als)" dropped, a trailing single digit
// or "fixed" dropped ("Lost Signal 2", not "Tay Do 22"); the vocal line reads "<title before any '('> (Vocal)". The year comes from the [#yNN] tag:
// 20NN up to this year, else 19NN.
//
// Pure and single-threaded (the render thread); times are seconds on one clock.

#include <array>
#include <cstdint>
#include <string>

namespace reforce::sync {

// What is loaded on a deck, from rkbx_link's "t" lines.
struct DeckTrack {
    std::string title, artist, tags;
    bool operator==(const DeckTrack& o) const { return title == o.title && artist == o.artist && tags == o.tags; }
    bool operator!=(const DeckTrack& o) const { return !(*this == o); }
};

bool tagsVocal(const std::string& tags);                      // has [#vocal]
int tagsYear(const std::string& tags, int currentYear);       // [#yNN] -> 19NN / 20NN; 0 = none
std::string cleanTitle(const std::string& title);             // history_server's process_title
std::string vocalTitle(const std::string& cleanedTitle);      // "Say My Name (Vocal)"

struct NowPlayingConfig {
    double switchSeconds = 3;      // reforce.NowPlaying.SwitchSeconds: a new loudest deck leads this long first
    double hideSeconds = 15;       // nothing audible this long: hide
    double vocalSeconds = 1;       // a vocal deck audible this long: show its line
    double vocalHoldSeconds = 3;   // ... and keep it this long after it goes quiet
};

struct NowPlayingDeck {
    DeckTrack track;
    double level = 0;              // 0..1 (DeckStarts::level)
};

struct NowPlayingText {
    bool show = false;
    int deck = -1;
    std::string artist, title, year;
    bool vocal = false;
    int vocalDeck = -1;
    std::string vocalArtist, vocalTitle;
    bool operator==(const NowPlayingText& o) const {
        return show == o.show && deck == o.deck && artist == o.artist && title == o.title && year == o.year &&
               vocal == o.vocal && vocalDeck == o.vocalDeck && vocalArtist == o.vocalArtist &&
               vocalTitle == o.vocalTitle;
    }
};

class NowPlaying {
public:
    static constexpr int kDecks = 4;
    explicit NowPlaying(NowPlayingConfig cfg = {}, int currentYear = 0);
    void setConfig(const NowPlayingConfig& cfg) { cfg_ = cfg; }
    const NowPlayingConfig& config() const { return cfg_; }

    // `masterDeck`: rekordbox's master (0..3, -1 unknown), used when `fadersSeen` is false.
    void update(double now, const std::array<NowPlayingDeck, kDecks>& decks, int masterDeck, bool fadersSeen);
    const NowPlayingText& text() const { return text_; }
    std::uint64_t version() const { return version_; }   // bumps when text() changes

private:
    NowPlayingConfig cfg_;
    int year_;
    NowPlayingText text_;
    std::uint64_t version_ = 0;
    int shown_ = -1;
    DeckTrack shownTrack_;
    int leader_ = -1;
    double leaderSince_ = 0;
    double lastAudible_ = -1e9;
    int vocal_ = -1;
    DeckTrack vocalTrack_;
    int vocalCand_ = -1;
    double vocalCandSince_ = 0;
    double vocalLastAudible_ = -1e9;
};

}  // namespace reforce::sync

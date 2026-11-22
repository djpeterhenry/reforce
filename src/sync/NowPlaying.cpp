#include "sync/NowPlaying.hpp"

#include <cctype>
#include <cmath>
#include <ctime>
#include <regex>

namespace reforce::sync {

namespace {
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
int thisYear() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    return tm.tm_year + 1900;
}
}  // namespace

bool tagsVocal(const std::string& tags) { return tags.find("[#vocal]") != std::string::npos; }

int tagsYear(const std::string& tags, int currentYear) {
    static const std::regex re(R"(\[#y(\d\d)\])");
    std::smatch m;
    if (!std::regex_search(tags, m, re)) return 0;
    const int yy = std::stoi(m[1].str());
    const int century = currentYear / 100 * 100;
    return century + yy <= currentYear ? century + yy : century - 100 + yy;
}

std::string cleanTitle(const std::string& title) {
    static const std::regex mix(R"(\s*\((?:original|extended)\b[^)]*\))", std::regex::icase);
    static const std::regex tail(R"(\s+(?:fixed|\d)$)", std::regex::icase);
    std::string t = trim(std::regex_replace(trim(title), mix, ""));
    return trim(std::regex_replace(t, tail, ""));
}

std::string vocalTitle(const std::string& cleanedTitle) {
    std::string base = trim(cleanedTitle.substr(0, cleanedTitle.find('(')));
    return (base.empty() ? cleanedTitle : base) + " (Vocal)";
}

NowPlaying::NowPlaying(NowPlayingConfig cfg, int currentYear)
    : cfg_(cfg), year_(currentYear > 0 ? currentYear : thisYear()) {}

void NowPlaying::update(double now, const std::array<NowPlayingDeck, kDecks>& decks, int masterDeck, bool fadersSeen) {
    auto loaded = [&](int i) { return !decks[std::size_t(i)].track.title.empty(); };
    auto vocal = [&](int i) { return tagsVocal(decks[std::size_t(i)].track.tags); };
    // Loudness for picking; without controller MIDI only the rekordbox master counts.
    auto level = [&](int i) {
        if (fadersSeen) return decks[std::size_t(i)].level;
        return i == masterDeck ? 1.0 : 0.0;
    };

    // ---- main
    // The shown deck no longer holds the track that was shown (a new one loaded): it has to lead again.
    if (shown_ >= 0 && decks[std::size_t(shown_)].track != shownTrack_) {
        if (leader_ == shown_) leader_ = -1;
        shown_ = -1;
    }
    int best = -1;
    double bestLevel = 0;
    for (int i = 0; i < kDecks; ++i) {
        if (!loaded(i) || vocal(i)) continue;
        const double l = level(i);
        if (l <= 0) continue;
        if (l > bestLevel + 1e-9 || (std::abs(l - bestLevel) <= 1e-9 && i == shown_)) {
            best = i;
            bestLevel = l;
        }
    }
    if (best >= 0) lastAudible_ = now;
    // The shown deck still as loud as the best keeps its place.
    if (best >= 0 && shown_ >= 0 && level(shown_) >= bestLevel - 1e-9) best = shown_;
    if (best != leader_) {
        leader_ = best;
        leaderSince_ = now;
    }
    if (leader_ >= 0 && leader_ != shown_ && now - leaderSince_ >= cfg_.switchSeconds) {
        shown_ = leader_;
        shownTrack_ = decks[std::size_t(shown_)].track;
    }
    if (shown_ >= 0 && now - lastAudible_ >= cfg_.hideSeconds) shown_ = -1;

    // ---- vocal
    if (vocal_ >= 0 && decks[std::size_t(vocal_)].track != vocalTrack_) vocal_ = -1;
    int vbest = -1;
    double vlevel = 0;
    for (int i = 0; i < kDecks; ++i) {
        if (!loaded(i) || !vocal(i)) continue;
        const double l = fadersSeen ? decks[std::size_t(i)].level : (i == masterDeck ? 1.0 : 0.0);
        if (l > vlevel + 1e-9 || (l > 0 && std::abs(l - vlevel) <= 1e-9 && i == vocal_)) {
            vbest = i;
            vlevel = l;
        }
    }
    if (vbest >= 0 && vbest == vocal_) vocalLastAudible_ = now;
    if (vbest != vocalCand_) {
        vocalCand_ = vbest;
        vocalCandSince_ = now;
    }
    if (vocalCand_ >= 0 && vocalCand_ != vocal_ && now - vocalCandSince_ >= cfg_.vocalSeconds) {
        vocal_ = vocalCand_;
        vocalTrack_ = decks[std::size_t(vocal_)].track;
        vocalLastAudible_ = now;
    }
    if (vocal_ >= 0 && now - vocalLastAudible_ >= cfg_.vocalHoldSeconds) vocal_ = -1;

    // ---- text
    NowPlayingText t;
    if (shown_ >= 0) {
        t.show = true;
        t.deck = shown_;
        t.artist = shownTrack_.artist;
        t.title = cleanTitle(shownTrack_.title);
        const int y = tagsYear(shownTrack_.tags, year_);
        if (y > 0) t.year = std::to_string(y);
    }
    if (vocal_ >= 0) {
        t.vocal = true;
        t.vocalDeck = vocal_;
        t.vocalArtist = vocalTrack_.artist;
        t.vocalTitle = vocalTitle(cleanTitle(vocalTrack_.title));
    }
    if (!(t == text_)) {
        text_ = t;
        ++version_;
    }
}

}  // namespace reforce::sync

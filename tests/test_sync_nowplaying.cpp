#include <doctest/doctest.h>

#include "sync/NowPlaying.hpp"

using namespace reforce::sync;

TEST_CASE("now playing: titles and tags") {
    CHECK(cleanTitle("Trapped (Original Mix)") == "Trapped");
    CHECK(cleanTitle("Midnight Runner (Extended Mix)") == "Midnight Runner");
    CHECK(cleanTitle("Neon Lights (DJ Tool Mix)") == "Neon Lights (DJ Tool Mix)");
    CHECK(cleanTitle("Sunset (The Original)") == "Sunset (The Original)");
    CHECK(cleanTitle("Lost Signal fixed") == "Lost Signal");
    CHECK(cleanTitle("Tay Do 22") == "Tay Do 22");
    CHECK(cleanTitle("Warehouse Dub 2") == "Warehouse Dub");
    CHECK(cleanTitle("Stay") == "Stay");
    CHECK(cleanTitle("Night Drive (Dub Mix) (als)") == "Night Drive (Dub Mix)");
    CHECK(cleanTitle("Deep Blue (Acapella) (ALS) 2") == "Deep Blue (Acapella)");
    CHECK(cleanTitle("Falsetto (Also Mix)") == "Falsetto (Also Mix)");
    CHECK(vocalTitle("Say My Name") == "Say My Name (Vocal)");
    CHECK(vocalTitle("Say My Name (Acapella)") == "Say My Name (Vocal)");
    CHECK(tagsVocal("[#good] [#vocal] [#y03]"));
    CHECK_FALSE(tagsVocal("[#good] [#no_lyrics]"));
    CHECK(tagsYear("[#good] [#y03]", 2026) == 2003);
    CHECK(tagsYear("[#y26]", 2026) == 2026);
    CHECK(tagsYear("[#y85] [#p2]", 2026) == 1985);
    CHECK(tagsYear("[#good]", 2026) == 0);
}

namespace {
std::array<NowPlayingDeck, 4> decks(double l0, double l1, double l2 = 0, double l3 = 0) {
    std::array<NowPlayingDeck, 4> d;
    d[0].track = {"Trapped (Original Mix)", "Kolter", "[#good] [#y19]"};
    d[1].track = {"Tay Do 22", "Bermuda Triangle", "[#y03]"};
    d[2].track = {"Say My Name", "Luna Vox", "[#vocal] [#y22]"};
    d[3].track = {"Planet (Original Mix)", "Four Tet", ""};
    d[0].level = l0;
    d[1].level = l1;
    d[2].level = l2;
    d[3].level = l3;
    return d;
}
}  // namespace

TEST_CASE("now playing: the loudest deck takes over after leading a while") {
    NowPlaying np({}, 2026);
    np.update(0, decks(1, 0), -1, true);
    CHECK_FALSE(np.text().show);
    np.update(2.9, decks(1, 0), -1, true);
    CHECK_FALSE(np.text().show);
    np.update(3.0, decks(1, 0), -1, true);
    REQUIRE(np.text().show);
    CHECK(np.text().deck == 0);
    CHECK(np.text().artist == "Kolter");
    CHECK(np.text().title == "Trapped");
    CHECK(np.text().year == "2019");
    // Deck 2 comes in: equal faders keep deck 1; once it is louder it leads, and shows 3 s later.
    np.update(4, decks(1, 1), -1, true);
    np.update(10, decks(1, 1), -1, true);
    CHECK(np.text().deck == 0);
    np.update(11, decks(0.5, 1), -1, true);
    np.update(13.9, decks(0.5, 1), -1, true);
    CHECK(np.text().deck == 0);
    np.update(14, decks(0.2, 1), -1, true);
    CHECK(np.text().deck == 1);
    CHECK(np.text().title == "Tay Do 22");
    CHECK(np.text().year == "2003");
    // A brief swing back does not flip it.
    np.update(15, decks(1, 0.5), -1, true);
    np.update(16, decks(0, 1), -1, true);
    CHECK(np.text().deck == 1);
}

TEST_CASE("now playing: vocals get their own line, never the main one") {
    NowPlaying np({}, 2026);
    np.update(0, decks(1, 0), -1, true);
    np.update(3, decks(1, 0), -1, true);
    np.update(4, decks(1, 0, 1), -1, true);
    CHECK_FALSE(np.text().vocal);
    np.update(5, decks(1, 0, 1), -1, true);
    REQUIRE(np.text().vocal);
    CHECK(np.text().deck == 0);
    CHECK(np.text().vocalArtist == "Luna Vox");
    CHECK(np.text().vocalTitle == "Say My Name (Vocal)");
    np.update(6, decks(1, 0, 0), -1, true);
    np.update(7.9, decks(1, 0, 0), -1, true);
    CHECK(np.text().vocal);
    np.update(8, decks(1, 0, 0), -1, true);
    CHECK_FALSE(np.text().vocal);
    // Only a vocal playing: no main text.
    NowPlaying v({}, 2026);
    v.update(0, decks(0, 0, 1), -1, true);
    v.update(10, decks(0, 0, 1), -1, true);
    CHECK_FALSE(v.text().show);
    CHECK(v.text().vocal);
}

TEST_CASE("now playing: hides after silence, new track on the shown deck, no MIDI uses master") {
    NowPlaying np({}, 2026);
    np.update(0, decks(1, 0), -1, true);
    np.update(3, decks(1, 0), -1, true);
    REQUIRE(np.text().show);
    np.update(17.9, decks(0, 0), -1, true);
    CHECK(np.text().show);
    np.update(18, decks(0, 0), -1, true);
    CHECK_FALSE(np.text().show);

    NowPlaying m({}, 2026);
    m.update(0, decks(0, 0), 3, false);
    m.update(3, decks(0, 0), 3, false);
    REQUIRE(m.text().show);
    CHECK(m.text().deck == 3);
    CHECK(m.text().title == "Planet");
    CHECK(m.text().year.empty());
    // A new track loaded on the shown deck must lead again before it shows.
    auto d = decks(0, 0);
    d[3].track = {"Other", "Someone", ""};
    m.update(4, d, 3, false);
    CHECK_FALSE(m.text().show);
    m.update(7, d, 3, false);
    CHECK(m.text().title == "Other");
}

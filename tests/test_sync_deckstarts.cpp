// Deck starts (PLAN 5O.8): phrase lines from track starts on rekordbox decks (rkbx_link frames) and the DJ
// controller's faders / hot cue pads (MIDI).
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "sync/DeckFeed.hpp"
#include "sync/DeckStarts.hpp"
#include "sync/PhraseClock.hpp"

using namespace reforce::sync;

namespace {

// rekordbox as rkbx_link sees it: frames at 120 Hz; a playing deck's position moves in audio buffers (512 samples),
// so some frames repeat the last position. Tracks at `bpm` with the grid's beat 0 at 0 s.
struct Rekordbox {
    struct Deck {
        bool playing = false;
        double pos = 0;   // true position, s
        double shown = 0; // position as read (buffer-quantised)
        double bpm = 128, origBpm = 128;
    };
    Deck decks[4];
    double t = 0;
    double buffer = 512.0 / 44100.0;
    double nextBuffer = 0;
    DeckStarts starts;

    double beatOf(const Deck& d) const { return d.shown * d.origBpm / 60.0; }
    void run(double seconds) {
        const double dt = 1.0 / 120.0;
        const int n = int(std::lround(seconds / dt));
        for (int k = 0; k < n; ++k) {
            t += dt;
            const bool tick = t >= nextBuffer;
            if (tick) nextBuffer += buffer * std::ceil((t - nextBuffer) / buffer + 1e-9);
            for (int i = 0; i < 4; ++i) {
                Deck& d = decks[i];
                if (d.playing) d.pos += dt * d.bpm / d.origBpm;
                if (tick || !d.playing) d.shown = d.pos;
                DeckFrame f;
                f.deck = i;
                f.position = d.shown;
                f.gridBeat = beatOf(d);
                f.bpm = d.bpm;
                f.originalBpm = d.origBpm;
                starts.frame(t, t, f);
            }
        }
    }
    // Play from a cue on beat `beat` (stopped first, so the deck is known to be stopped).
    void cueAndPlay(int i, double beat, double settle = 0.5) {
        decks[i].playing = false;
        decks[i].pos = decks[i].shown = beat * 60.0 / decks[i].origBpm;
        run(settle);
        decks[i].playing = true;
    }
    void jumpTo(int i, double beat) { decks[i].pos = decks[i].shown = beat * 60.0 / decks[i].origBpm; }
    double beatsPerSecond(int i) const { return decks[i].bpm / 60.0; }
};

}  // namespace

TEST_CASE("deck starts: a start on an audible deck is a phrase line") {
    Rekordbox rb;
    rb.starts.fader(0, 0, 0.6);
    rb.run(1.0);
    rb.cueAndPlay(0, 64);
    rb.run(2.0);
    const auto c = rb.starts.take();
    REQUIRE(c.size() == 1);
    CHECK(c[0].deck == 0);
    CHECK(c[0].why == "start");
    // elapsed as of the commit (confirmed ~0.15 s in) plus the time since then = 2 s of beats.
    const double now = c[0].elapsed + (rb.t - c[0].at) * c[0].bpm / 60.0;
    CHECK(now == doctest::Approx(2.0 * rb.beatsPerSecond(0)).epsilon(0.03));
    CHECK(rb.starts.deck(0).playing);
    CHECK(rb.starts.lastCommitDeck() == 0);
}

TEST_CASE("deck starts: a deck already playing when we first see it sets nothing") {
    Rekordbox rb;
    rb.decks[1].playing = true;
    rb.decks[1].pos = 40;
    rb.run(3.0);
    CHECK(rb.starts.take().empty());
    CHECK(rb.starts.deck(1).playing);
    CHECK_FALSE(rb.starts.deck(1).hasStart);
}

TEST_CASE("deck starts: fader down, then up later") {
    Rekordbox rb;
    rb.starts.fader(0, 1, 0.0);
    rb.run(0.5);
    rb.cueAndPlay(1, 32);
    rb.run(8.0);
    CHECK(rb.starts.take().empty());
    CHECK(rb.starts.deck(1).hasStart);
    CHECK_FALSE(rb.starts.deck(1).committed);
    rb.starts.fader(rb.t, 1, 0.2);  // below a third: still out
    CHECK(rb.starts.take().empty());
    rb.starts.fader(rb.t, 1, 0.5);
    const auto c = rb.starts.take();
    REQUIRE(c.size() == 1);
    CHECK(c[0].why == "fader");
    CHECK(c[0].elapsed == doctest::Approx(8.0 * rb.beatsPerSecond(1)).epsilon(0.03));
    // Down and up again: the same start, not again.
    rb.starts.fader(rb.t, 1, 0.0);
    rb.starts.fader(rb.t, 1, 1.0);
    CHECK(rb.starts.take().empty());
}

TEST_CASE("deck starts: an off-phrase start with the fader down is replaced by the restart") {
    Rekordbox rb;
    rb.starts.fader(0, 2, 0.0);
    rb.cueAndPlay(2, 17);  // off phrase
    rb.run(5.0);
    rb.cueAndPlay(2, 64);  // Cue (stops), then Play from the phrase
    rb.run(4.0);
    CHECK(rb.starts.take().empty());
    rb.starts.fader(rb.t, 2, 0.8);
    const auto c = rb.starts.take();
    REQUIRE(c.size() == 1);
    CHECK(c[0].elapsed == doctest::Approx(4.0 * rb.beatsPerSecond(2)).epsilon(0.03));
}

TEST_CASE("deck starts: loops and beat jumps keep the phrase; a hot cue restarts it") {
    Rekordbox rb;
    rb.starts.fader(0, 0, 1.0);
    rb.cueAndPlay(0, 0);
    rb.run(1.0);
    REQUIRE(rb.starts.take().size() == 1);
    // A 4-beat loop: jump back every 4 beats, 4 times.
    for (int k = 0; k < 4; ++k) {
        const double b = rb.beatOf(rb.decks[0]);
        rb.run(4.0 * 60.0 / 128.0);
        rb.jumpTo(0, b);
    }
    rb.run(1.0);
    // A 16-beat jump forward.
    rb.jumpTo(0, rb.beatOf(rb.decks[0]) + 16);
    rb.run(1.0);
    CHECK(rb.starts.take().empty());
    const double played = 1.0 + 4 * 4.0 * 60.0 / 128.0 + 1.0 + 1.0;
    CHECK(rb.starts.deck(0).elapsed == doctest::Approx(played * rb.beatsPerSecond(0)).epsilon(0.02));
    // Hot cue pad, then rekordbox jumps.
    rb.starts.hotCue(rb.t, 0);
    rb.run(0.02);
    rb.jumpTo(0, 128);
    rb.run(1.0);
    auto c = rb.starts.take();
    REQUIRE(c.size() == 1);
    CHECK(c[0].why == "hot cue");
    CHECK(rb.starts.deck(0).elapsed == doctest::Approx(1.0 * rb.beatsPerSecond(0)).epsilon(0.05));
    // The pad's MIDI after the jump still counts.
    rb.jumpTo(0, 256);
    rb.run(0.05);
    rb.starts.hotCue(rb.t, 0);
    c = rb.starts.take();
    REQUIRE(c.size() == 1);
    CHECK(c[0].why == "hot cue");
    // A pad press long before a jump does not.
    rb.starts.hotCue(rb.t, 0);
    rb.run(2.0);
    rb.jumpTo(0, 300);
    rb.run(1.0);
    CHECK(rb.starts.take().empty());
}

TEST_CASE("deck starts: a jog nudge while stopped is not a start") {
    Rekordbox rb;
    rb.cueAndPlay(3, 10, 0.5);
    rb.decks[3].playing = false;  // (cueAndPlay set playing; undo before any frame)
    rb.run(0.5);
    rb.decks[3].playing = true;
    rb.run(0.06);
    rb.decks[3].playing = false;
    rb.run(0.5);
    CHECK(rb.starts.take().empty());
    CHECK_FALSE(rb.starts.deck(3).playing);
}

TEST_CASE("deck starts: tempo changes and pitch") {
    Rekordbox rb;
    rb.starts.fader(0, 0, 1.0);
    rb.decks[0].bpm = 132;  // pitched up from 128
    rb.cueAndPlay(0, 64);
    rb.run(10.0);
    const auto c = rb.starts.take();
    REQUIRE(c.size() == 1);
    CHECK(rb.starts.deck(0).elapsed == doctest::Approx(10.0 * 132 / 60.0).epsilon(0.01));
}

TEST_CASE("deck starts: a fader that has not moved yet counts as down") {
    Rekordbox rb;
    rb.cueAndPlay(1, 0);
    rb.run(2.0);
    CHECK(rb.starts.take().empty());
    CHECK_FALSE(rb.starts.deck(1).audible);
    rb.starts.fader(rb.t, 1, 0.01);  // moved, still down
    rb.run(1.0);
    CHECK(rb.starts.take().empty());
    rb.starts.fader(rb.t, 1, 0.9);
    const auto c = rb.starts.take();
    REQUIRE(c.size() == 1);
    CHECK(c[0].why == "fader");
}

TEST_CASE("deck starts: the crossfader cuts a deck assigned to the other side") {
    Rekordbox rb;
    rb.starts.fader(0, 1, 1.0);
    rb.starts.assign(0, 1, -1);      // deck 2 on side A
    rb.starts.crossfader(0, 1.0);    // all the way to B
    rb.cueAndPlay(1, 0);
    rb.run(2.0);
    CHECK(rb.starts.take().empty());
    rb.starts.crossfader(rb.t, 0.5);
    const auto c = rb.starts.take();
    REQUIRE(c.size() == 1);
    CHECK(c[0].deck == 1);
}

TEST_CASE("deck starts: no beatgrid, no starts") {
    DeckStarts s;
    for (int k = 0; k < 240; ++k) {
        DeckFrame f;
        f.position = k / 120.0;
        f.gridBeat = std::nan("");
        f.bpm = f.originalBpm = 128;
        s.frame(k / 120.0, k / 120.0, f);
    }
    CHECK(s.take().empty());
    CHECK_FALSE(s.deck(0).seen);
}

TEST_CASE("deck feed: rkbx datagrams") {
    std::vector<DeckFeedEvent> ev;
    CHECK(parseRkbxDatagram("rkbx 1 12.5000 1\nd 0 30.0000 64.0000 128.000 128.000\nd 1 1.0000 nan 0.000 120.000\n",
                            7.0, ev));
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].deck == 0);
    CHECK(ev[0].senderT == 12.5);
    CHECK(ev[0].now == 7.0);
    CHECK(ev[0].frame.position == 30.0);
    CHECK(ev[0].frame.gridBeat == 64.0);
    CHECK(std::isnan(ev[1].frame.gridBeat));
    ev.clear();
    CHECK_FALSE(parseRkbxDatagram("hello", 0, ev));
    CHECK_FALSE(parseRkbxDatagram("rkbx 2 1 0\n", 0, ev));
}

TEST_CASE("deck feed: rkbx track lines") {
    int deck = -1;
    DeckTrack t;
    CHECK(parseRkbxTrackLine("t 1 Tay Do 22\tBermuda Triangle", deck, t));
    CHECK(deck == 1);
    CHECK(t.title == "Tay Do 22");
    CHECK(t.artist == "Bermuda Triangle");
    CHECK(t.tags.empty());
    CHECK(parseRkbxTrackLine("t 0 Say My Name\tLuna Vox\t[#vocal] [#y22]", deck, t));
    CHECK(t.artist == "Luna Vox");
    CHECK(t.tags == "[#vocal] [#y22]");
    CHECK(parseRkbxTrackLine("t 3 \t", deck, t));
    CHECK(deck == 3);
    CHECK(t.title.empty());
    CHECK(t.artist.empty());
    CHECK_FALSE(parseRkbxTrackLine("t 4 x\ty", deck, t));
    CHECK_FALSE(parseRkbxTrackLine("d 0 1 2 3 4", deck, t));
    std::vector<DeckFeedEvent> ev;  // frames still parse with track lines in between
    CHECK(parseRkbxDatagram("rkbx 1 1 0\nt 0 A\tB\nd 0 1 2 3 4\n", 0, ev));
    CHECK(ev.size() == 1);
}

TEST_CASE("deck feed: DDJ-1000 MIDI (content/Controllers map)") {
    const auto maps = loadControllerMaps({std::filesystem::path(REFORCE_SOURCE_DIR) / "content" / "Controllers"});
    const ControllerMap* map = findControllerMap(maps, "PIONEER DDJ-1000");
    REQUIRE(map != nullptr);
    CHECK(map->name == "Pioneer DDJ-1000");
    CHECK(findControllerMap(maps, "IAC Driver Bus 1") == nullptr);
    auto parseDdjMidi = [&](unsigned st, unsigned d1, unsigned d2, double now, MidiHiRes& h, DeckFeedEvent& ev) {
        return parseControllerMidi(*map, st, d1, d2, now, h, ev);
    };
    MidiHiRes hr;
    DeckFeedEvent e;
    // Channel fader deck 2: MSB then LSB.
    REQUIRE(parseDdjMidi(0xB1, 0x13, 64, 0, hr, e));
    CHECK(e.kind == DeckFeedEvent::Kind::Fader);
    CHECK(e.deck == 1);
    CHECK(e.value == doctest::Approx(64 / 127.0));
    REQUIRE(parseDdjMidi(0xB1, 0x33, 0, 0, hr, e));
    CHECK(e.value == doctest::Approx(64 * 128 / 16383.0));
    // Crossfader.
    REQUIRE(parseDdjMidi(0xB6, 0x1F, 127, 0, hr, e));
    CHECK(e.kind == DeckFeedEvent::Kind::Crossfader);
    // Assign: deck 3 to B.
    REQUIRE(parseDdjMidi(0x92, 0x18, 127, 0, hr, e));
    CHECK(e.kind == DeckFeedEvent::Kind::Assign);
    CHECK(e.deck == 2);
    CHECK(e.value == 1);
    // Hot cue pads: channel 8 / 10 / 12 / 14 = decks 1-4; released (velocity 0) ignored.
    REQUIRE(parseDdjMidi(0x97, 0x00, 127, 0, hr, e));
    CHECK(e.kind == DeckFeedEvent::Kind::HotCue);
    CHECK(e.deck == 0);
    REQUIRE(parseDdjMidi(0x9D, 0x07, 127, 0, hr, e));
    CHECK(e.deck == 3);
    CHECK_FALSE(parseDdjMidi(0x97, 0x00, 0, 0, hr, e));
    CHECK_FALSE(parseDdjMidi(0x98, 0x00, 127, 0, hr, e));  // shift + pad (delete hot cue)
    CHECK_FALSE(parseDdjMidi(0x97, 0x20, 127, 0, hr, e));  // beat jump pad
    CHECK_FALSE(parseDdjMidi(0x90, 0x0B, 127, 0, hr, e));  // play
}

TEST_CASE("controller map: parsing") {
    ControllerMap m;
    std::string err;
    REQUIRE(parseControllerMap("// comment\nMatch = \"FLX4\"\nFader = 2, 1, 0x13 // 7-bit\nCrossfader = 7, 31\n"
                               "HotCues = 1, 8, 0, 7\n",
                               "Some FLX4", m, err));
    CHECK(m.name == "Some FLX4");
    CHECK(m.match == "FLX4");
    REQUIRE(m.faders.size() == 1);
    CHECK(m.faders[0].deck == 1);
    CHECK(m.faders[0].channel == 0);
    CHECK(m.faders[0].ccLsb == -1);
    CHECK(m.hasCrossfader);
    // 7-bit fader: the value comes from the one CC.
    MidiHiRes hr;
    DeckFeedEvent e;
    REQUIRE(parseControllerMidi(m, 0xB0, 0x13, 127, 0, hr, e));
    CHECK(e.kind == DeckFeedEvent::Kind::Fader);
    CHECK(e.deck == 1);
    CHECK(e.value == doctest::Approx(1.0));

    CHECK_FALSE(parseControllerMap("Fader = 1, 1, 0x13\n", "x", m, err));  // no Match
    CHECK_FALSE(parseControllerMap("Match = x\nFader = 5, 1, 0x13\n", "x", m, err));  // deck 5
    CHECK(err.find("line 2") != std::string::npos);
    CHECK_FALSE(parseControllerMap("Match = x\nFadr = 1, 1, 1\n", "x", m, err));  // unknown key
    CHECK_FALSE(parseControllerMap("Match = x\nFader = 1, one, 1\n", "x", m, err));
    CHECK_FALSE(parseControllerMap("Match = x\nHotCues = 1, 8, 7, 0\n", "x", m, err));  // last < first
}

TEST_CASE("phrase clock: a deck start sets the origin and auto leaves it") {
    PhraseClock pc;
    BeatClockState s;
    s.source = BeatSource::Link;
    s.confident = true;
    s.bpm = 128;
    s.bars = true;
    s.quantum = 4;
    s.beat = 1000.2;
    pc.update(s, 1.0);
    pc.startAt(1000.2 - 38.9, 2);  // started 38.9 beats ago: beat 961.3 -> the bar line at 960
    CHECK(pc.state().how == "deck");
    CHECK(pc.state().deck == 2);
    CHECK(pc.state().origin == doctest::Approx(std::fmod(960.0, 64.0)));
    // Loud changes elsewhere for many phrases: still the deck's.
    for (int k = 0; k < 64 * 8 * 60; ++k) {
        s.beat += 128.0 / 60.0 / 60.0;
        const double b = std::fmod(s.beat, 64.0);
        pc.update(s, b > 10 && b < 30 ? 1.0 : 0.1);
    }
    CHECK(pc.state().how == "deck");
    CHECK(pc.state().origin == doctest::Approx(std::fmod(960.0, 64.0)));
    // The buttons still win.
    pc.nudge(1);
    CHECK(pc.state().how == "manual");
    CHECK(pc.state().deck == -1);
}

// End to end through CoreMIDI: a virtual MIDI source named like a controller, a DeckFeed with a map for it, one fader
// move. Skipped where CoreMIDI is unavailable (some CI machines).
#include <CoreMIDI/CoreMIDI.h>
#include <chrono>
#include <thread>

TEST_CASE("deck feed: a mapped virtual MIDI source reaches DeckStarts") {
    MIDIClientRef client = 0;
    MIDIEndpointRef src = 0;
    if (MIDIClientCreate(CFSTR("reforce test"), nullptr, nullptr, &client) != noErr ||
        MIDISourceCreateWithProtocol(client, CFSTR("reforce test controller"), kMIDIProtocol_1_0, &src) != noErr) {
        MESSAGE("CoreMIDI unavailable; skipped");
        return;
    }
    ControllerMap map;
    std::string err;
    REQUIRE(parseControllerMap("Match = reforce test controller\nFader = 3, 2, 0x13\n", "test", map, err));
    {
        DeckFeed feed(47991, "", {map});
        CHECK(feed.midiSources() == "reforce test controller");
        MIDIEventList list;
        MIDIEventPacket* p = MIDIEventListInit(&list, kMIDIProtocol_1_0);
        const UInt32 word = 0x20000000u | (0xB1u << 16) | (0x13u << 8) | 100u;  // CC 0x13 = 100 on channel 2
        p = MIDIEventListAdd(&list, sizeof list, p, 0, 1, &word);
        REQUIRE(p != nullptr);
        REQUIRE(MIDIReceivedEventList(src, &list) == noErr);
        DeckStarts starts;
        for (int i = 0; i < 100 && !starts.fadersSeen(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            feed.drain(starts);
        }
        CHECK(starts.fadersSeen());
    }
    MIDIEndpointDispose(src);
    MIDIClientDispose(client);
}

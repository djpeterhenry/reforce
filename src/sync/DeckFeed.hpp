#pragma once
// reforce extension (PLAN 5O.8): the live inputs of sync/DeckStarts.
//
//   rkbx_link's "reforce" output module: UDP datagrams on 127.0.0.1:<port> (default 4470), plain text:
//       rkbx 1 <t> <master>
//       d <deck> <position s> <grid beat | nan> <bpm> <original bpm>      (one line per deck)
//       t <deck> <title>\t<artist>\t<tags>   (on a track change and about once a second; "" = unknown / empty deck;
//                                             tags: what djtools appends to the title, e.g. "[#vocal] [#y03]")
//   the DJ controller's MIDI (CoreMIDI), read alongside rekordbox: every MIDI source that a controller map
//   (sync/ControllerMap, content/Controllers/*.txt) matches by name, optionally only those whose name also contains
//   `midiDevice` (settings reforce.Phrase.MidiDevice). The map says which messages are the channel faders, the
//   crossfader, the assign switches and the hot cue pads.
//
// Both arrive on their own threads and are queued with a local time stamp; drain() hands them to a DeckStarts on
// the render thread in arrival order.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sync/ControllerMap.hpp"
#include "sync/DeckStarts.hpp"
#include "sync/NowPlaying.hpp"

namespace reforce::sync {

struct DeckFeedEvent {
    enum class Kind { Frame, Fader, Crossfader, Assign, HotCue } kind = Kind::Frame;
    double now = 0;     // local receive time (DeckFeed::now())
    double senderT = 0; // Frame: the datagram's time
    int deck = 0;
    double value = 0;   // Fader / Crossfader 0..1; Assign -1 / 0 / +1
    DeckFrame frame;
};

// Parses one rkbx datagram into Frame events (pure, for tests). False if it is not one.
bool parseRkbxDatagram(const std::string& text, double now, std::vector<DeckFeedEvent>& out);

// One "t <deck> <title>\t<artist>[\t<tags>]" line (pure, for tests). False if it is not one.
bool parseRkbxTrackLine(const std::string& line, int& deck, DeckTrack& out);
class DeckFeed {
public:
    // maps: the controller maps (loadControllerMaps); midiDevice: "" = any source a map matches.
    DeckFeed(int port, std::string midiDevice, std::vector<ControllerMap> maps);
    ~DeckFeed();
    DeckFeed(const DeckFeed&) = delete;
    DeckFeed& operator=(const DeckFeed&) = delete;

    static double now();  // seconds, monotonic
    // `frames`: also sees every deck frame (sync/Lyrics' deck clocks).
    void drain(DeckStarts& starts, const std::function<void(double now, const DeckFrame&)>& frames = {});

    // Status for the panel / log.
    bool rkbxActive() const;            // a datagram within the last second
    std::string midiSources() const;    // connected MIDI source names, comma-separated ("" = none)
    int masterDeck() const { return master_.load(); }
    DeckTrack track(int deck) const;    // deck 0..3; empty until rkbx_link reports it

    void push(const DeckFeedEvent& e);  // MIDI thread / tests
    struct Midi;                        // CoreMIDI state (DeckFeed.cpp)

private:
    void udpLoop();

    int port_;
    std::string midiDevice_;
    std::vector<ControllerMap> maps_;
    std::atomic<bool> stop_{false};
    std::atomic<double> lastDatagram_{-1e9};
    std::atomic<int> master_{-1};
    int sock_ = -1;
    std::thread udp_;
    mutable std::mutex mu_;
    std::vector<DeckFeedEvent> queue_;
    DeckTrack tracks_[DeckStarts::kDecks];  // guarded by mu_
    std::unique_ptr<Midi> midi_;
};

}  // namespace reforce::sync

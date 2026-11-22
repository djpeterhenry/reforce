#pragma once
// DJ controller MIDI maps (data files, content/Controllers/*.txt): which MIDI messages are the channel faders, the
// crossfader, the crossfader assign switches and the hot cue pads, per deck. sync/DeckFeed reads them so the phrase
// clock (sync/DeckStarts) knows which deck is audible and when a hot cue was pressed.
//
// File format: one `Key = value` per line, `//` starts a comment, values are comma-separated integers (decimal or
// 0x hex). Decks and MIDI channels count from 1, as on the controller and in its MIDI documentation.
//
//   Name = "Pioneer DDJ-1000"           display name (default: the file name)
//   Match = "DDJ-1000"                  used for MIDI sources whose name contains this (required)
//   Fader = DECK, CHANNEL, CC [, CC_LSB]          channel fader; CC_LSB for 14-bit faders (LSB after MSB)
//   Crossfader = CHANNEL, CC [, CC_LSB]
//   Assign = DECK, CHANNEL, NOTE_A, NOTE_THRU, NOTE_B   crossfader assign switch (note on = that position)
//   HotCues = DECK, CHANNEL, FIRST_NOTE, LAST_NOTE     hot cue pads (note on with velocity > 0)
//
// Every key but Name and Match may repeat (one line per deck). Faders matter most: a fader that has never moved counts
// as down (sync/DeckStarts), so a deck without a Fader line never counts as audible. Without a Crossfader line the
// crossfader cuts nothing; without Assign lines every deck is THRU; without HotCues pads are not used.

#include <filesystem>
#include <string>
#include <vector>

namespace reforce::sync {

struct DeckFeedEvent;

struct ControllerMap {
    struct Fader {
        int deck = 0;  // 0-based
        int channel = 0;  // 0-based
        int cc = 0;
        int ccLsb = -1;  // -1: 7-bit
    };
    struct Assign {
        int deck = 0, channel = 0, noteA = 0, noteThru = 0, noteB = 0;
    };
    struct HotCues {
        int deck = 0, channel = 0, first = 0, last = 0;
    };

    std::string name;
    std::string match;
    std::vector<Fader> faders;
    bool hasCrossfader = false;
    Fader crossfader;  // deck unused
    std::vector<Assign> assigns;
    std::vector<HotCues> hotCues;
};

// Parses a map file's text (pure, for tests). On failure returns false with `error` naming the line.
bool parseControllerMap(const std::string& text, const std::string& defaultName, ControllerMap& out,
                        std::string& error);

// Every *.txt map in these folders, in name order; later folders win for the same Match. Bad files are logged and
// skipped.
std::vector<ControllerMap> loadControllerMaps(const std::vector<std::filesystem::path>& dirs);

// The first map whose Match is part of a MIDI source name, or nullptr.
const ControllerMap* findControllerMap(const std::vector<ControllerMap>& maps, const std::string& sourceName);

// 14-bit controls send MSB then LSB; the MSB is kept here between messages.
struct MidiHiRes {
    int msb[16][128] = {};
};

// One MIDI 1.0 channel message (status, data1, data2) through a map into an event (pure, for tests). False if the
// map does not use the message.
bool parseControllerMidi(const ControllerMap& map, unsigned status, unsigned d1, unsigned d2, double now,
                         MidiHiRes& hr, DeckFeedEvent& out);

}  // namespace reforce::sync

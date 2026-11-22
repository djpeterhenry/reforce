#include "sync/ControllerMap.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "sync/DeckFeed.hpp"

namespace reforce::sync {

namespace {

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::string unquote(std::string s) {
    s = trim(std::move(s));
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

// Comma-separated integers (decimal or 0x hex). False on anything else.
bool parseInts(const std::string& value, std::vector<int>& out) {
    out.clear();
    std::stringstream ss(unquote(value));
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (item.empty()) return false;
        try {
            size_t used = 0;
            const int v = std::stoi(item, &used, 0);
            if (used != item.size()) return false;
            out.push_back(v);
        } catch (...) {
            return false;
        }
    }
    return !out.empty();
}

bool inRange(int v, int lo, int hi) { return v >= lo && v <= hi; }

}  // namespace

bool parseControllerMap(const std::string& text, const std::string& defaultName, ControllerMap& out,
                        std::string& error) {
    out = ControllerMap{};
    out.name = defaultName;
    std::stringstream in(text);
    std::string line;
    int lineNo = 0;
    auto fail = [&](const std::string& why) {
        error = "line " + std::to_string(lineNo) + ": " + why;
        return false;
    };
    while (std::getline(in, line)) {
        ++lineNo;
        if (const auto c = line.find("//"); c != std::string::npos) line.erase(c);
        line = trim(line);
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) return fail("expected Key = value");
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "Name") {
            out.name = unquote(value);
            continue;
        }
        if (key == "Match") {
            out.match = unquote(value);
            continue;
        }
        std::vector<int> v;
        if (!parseInts(value, v)) return fail(key + ": expected comma-separated numbers");
        auto deckOk = [](int d) { return inRange(d, 1, 4); };
        auto chOk = [](int c) { return inRange(c, 1, 16); };
        auto byteOk = [](int b) { return inRange(b, 0, 127); };
        if (key == "Fader") {
            if (v.size() < 3 || v.size() > 4) return fail("Fader = DECK, CHANNEL, CC [, CC_LSB]");
            if (!deckOk(v[0]) || !chOk(v[1]) || !byteOk(v[2]) || (v.size() == 4 && !byteOk(v[3])))
                return fail("Fader: deck 1-4, channel 1-16, CC 0-127");
            out.faders.push_back({v[0] - 1, v[1] - 1, v[2], v.size() == 4 ? v[3] : -1});
        } else if (key == "Crossfader") {
            if (v.size() < 2 || v.size() > 3) return fail("Crossfader = CHANNEL, CC [, CC_LSB]");
            if (!chOk(v[0]) || !byteOk(v[1]) || (v.size() == 3 && !byteOk(v[2])))
                return fail("Crossfader: channel 1-16, CC 0-127");
            out.hasCrossfader = true;
            out.crossfader = {0, v[0] - 1, v[1], v.size() == 3 ? v[2] : -1};
        } else if (key == "Assign") {
            if (v.size() != 5) return fail("Assign = DECK, CHANNEL, NOTE_A, NOTE_THRU, NOTE_B");
            if (!deckOk(v[0]) || !chOk(v[1]) || !byteOk(v[2]) || !byteOk(v[3]) || !byteOk(v[4]))
                return fail("Assign: deck 1-4, channel 1-16, notes 0-127");
            out.assigns.push_back({v[0] - 1, v[1] - 1, v[2], v[3], v[4]});
        } else if (key == "HotCues") {
            if (v.size() != 4) return fail("HotCues = DECK, CHANNEL, FIRST_NOTE, LAST_NOTE");
            if (!deckOk(v[0]) || !chOk(v[1]) || !byteOk(v[2]) || !byteOk(v[3]) || v[3] < v[2])
                return fail("HotCues: deck 1-4, channel 1-16, notes 0-127, first <= last");
            out.hotCues.push_back({v[0] - 1, v[1] - 1, v[2], v[3]});
        } else {
            return fail("unknown key \"" + key + "\"");
        }
    }
    if (out.match.empty()) {
        lineNo = 0;
        error = "no Match = \"part of the MIDI source name\"";
        return false;
    }
    return true;
}

std::vector<ControllerMap> loadControllerMaps(const std::vector<std::filesystem::path>& dirs) {
    namespace fs = std::filesystem;
    std::vector<ControllerMap> maps;
    for (const fs::path& dir : dirs) {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec)) continue;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".txt") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            ControllerMap m;
            std::string err;
            if (!parseControllerMap(ss.str(), f.stem().string(), m, err)) {
                std::fprintf(stderr, "[reforce] controller map %s: %s (skipped)\n", f.string().c_str(), err.c_str());
                continue;
            }
            // A later folder (the user's) replaces a map for the same controller.
            maps.erase(std::remove_if(maps.begin(), maps.end(),
                                      [&](const ControllerMap& o) { return o.match == m.match; }),
                       maps.end());
            maps.push_back(std::move(m));
        }
    }
    return maps;
}

const ControllerMap* findControllerMap(const std::vector<ControllerMap>& maps, const std::string& sourceName) {
    for (const ControllerMap& m : maps)
        if (!m.match.empty() && sourceName.find(m.match) != std::string::npos) return &m;
    return nullptr;
}

bool parseControllerMidi(const ControllerMap& map, unsigned status, unsigned d1, unsigned d2, double now,
                         MidiHiRes& hr, DeckFeedEvent& out) {
    const int type = int(status & 0xF0), ch = int(status & 0x0F);
    const int n = int(d1 & 0x7F), val = int(d2 & 0x7F);
    out = DeckFeedEvent{};
    out.now = now;
    if (type == 0xB0) {
        // A fader's value: 7-bit from the MSB, 14-bit once its LSB follows.
        auto read = [&](const ControllerMap::Fader& f, double& v) {
            if (f.channel != ch) return false;
            if (n == f.cc) {
                hr.msb[ch][n] = val;
                v = val / 127.0;
                return true;
            }
            if (f.ccLsb >= 0 && n == f.ccLsb) {
                v = (hr.msb[ch][f.cc] * 128 + val) / 16383.0;
                return true;
            }
            return false;
        };
        double v = 0;
        for (const auto& f : map.faders)
            if (read(f, v)) {
                out.kind = DeckFeedEvent::Kind::Fader;
                out.deck = f.deck;
                out.value = v;
                return true;
            }
        if (map.hasCrossfader && read(map.crossfader, v)) {
            out.kind = DeckFeedEvent::Kind::Crossfader;
            out.value = v;
            return true;
        }
        return false;
    }
    if (type == 0x90 && val > 0) {
        for (const auto& a : map.assigns)
            if (a.channel == ch && (n == a.noteA || n == a.noteThru || n == a.noteB)) {
                out.kind = DeckFeedEvent::Kind::Assign;
                out.deck = a.deck;
                out.value = n == a.noteA ? -1 : n == a.noteB ? 1 : 0;
                return true;
            }
        for (const auto& h : map.hotCues)
            if (h.channel == ch && n >= h.first && n <= h.last) {
                out.kind = DeckFeedEvent::Kind::HotCue;
                out.deck = h.deck;
                return true;
            }
    }
    return false;
}

}  // namespace reforce::sync

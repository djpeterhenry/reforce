#include "sync/DeckFeed.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace reforce::sync {

// ---------------------------------------------------------------- parsing (pure)

bool parseRkbxDatagram(const std::string& text, double now, std::vector<DeckFeedEvent>& out) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line)) return false;
    std::istringstream head(line);
    std::string magic;
    int version = 0, master = -1;
    double t = 0;
    if (!(head >> magic >> version >> t >> master) || magic != "rkbx" || version != 1 || !std::isfinite(t))
        return false;
    while (std::getline(in, line)) {
        if (line.size() < 2 || line[0] != 'd') continue;
        // strtod reads "nan" (no beatgrid) as NaN; istream does not.
        const char* p = line.c_str() + 1;
        char* end = nullptr;
        const long deck = std::strtol(p, &end, 10);
        if (end == p) continue;
        double v[4];
        bool ok = true;
        for (double& x : v) {
            p = end;
            x = std::strtod(p, &end);
            if (end == p) ok = false;
        }
        if (!ok || deck < 0 || deck >= DeckStarts::kDecks) continue;
        DeckFeedEvent e;
        e.kind = DeckFeedEvent::Kind::Frame;
        e.now = now;
        e.senderT = t;
        e.deck = int(deck);
        e.value = master;
        e.frame.deck = int(deck);
        e.frame.position = v[0];
        e.frame.gridBeat = v[1];
        e.frame.bpm = v[2];
        e.frame.originalBpm = v[3];
        out.push_back(e);
    }
    return true;
}

bool parseRkbxTrackLine(const std::string& line, int& deck, DeckTrack& out) {
    if (line.size() < 3 || line[0] != 't' || line[1] != ' ') return false;
    const char* p = line.c_str() + 2;
    char* end = nullptr;
    const long d = std::strtol(p, &end, 10);
    if (end == p || *end != ' ' || d < 0 || d >= DeckStarts::kDecks) return false;
    const std::string rest(end + 1);
    const std::size_t tab = rest.find('\t');
    out.title = rest.substr(0, tab);
    out.artist = tab == std::string::npos ? std::string() : rest.substr(tab + 1);
    const std::size_t tab2 = out.artist.find('\t');
    out.tags = tab2 == std::string::npos ? std::string() : out.artist.substr(tab2 + 1);
    if (tab2 != std::string::npos) out.artist.resize(tab2);
    deck = int(d);
    return true;
}

// ---------------------------------------------------------------- CoreMIDI

namespace {
std::string cfString(CFStringRef s) {
    if (!s) return {};
    char buf[256];
    if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return buf;
    return {};
}
std::string endpointName(MIDIEndpointRef e) {
    CFStringRef name = nullptr;
    if (MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &name) != noErr || !name) return {};
    std::string s = cfString(name);
    CFRelease(name);
    return s;
}
}  // namespace

struct DeckFeed::Midi {
    // One per MIDI source ever connected (its map and 14-bit state); handed to CoreMIDI as the source's refCon, so it
    // lives as long as the client, even after the source goes away.
    struct Source {
        // Published with release: the MIDI thread's acquire load then also sees owner and the maps written before.
        std::atomic<const ControllerMap*> map{nullptr};
        MidiHiRes hr;  // MIDI thread only
    };
    DeckFeed* owner = nullptr;
    std::string match;  // "" = any source a map matches
    const std::vector<ControllerMap>* maps = nullptr;
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    std::mutex mu;
    std::vector<std::pair<MIDIUniqueID, std::string>> connected;
    std::vector<std::pair<MIDIUniqueID, std::unique_ptr<Source>>> sources;  // guarded by mu

    void connectAll() {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<std::pair<MIDIUniqueID, std::string>> now;
        const ItemCount n = MIDIGetNumberOfSources();
        for (ItemCount i = 0; i < n; ++i) {
            const MIDIEndpointRef src = MIDIGetSource(i);
            const std::string name = endpointName(src);
            if (!match.empty() && name.find(match) == std::string::npos) continue;
            const ControllerMap* map = findControllerMap(*maps, name);
            if (!map) continue;
            MIDIUniqueID id = 0;
            MIDIObjectGetIntegerProperty(src, kMIDIPropertyUniqueID, &id);
            bool had = false;
            for (const auto& c : connected) had = had || c.first == id;
            if (!had) {
                Source* ctx = nullptr;
                for (auto& s : sources)
                    if (s.first == id) ctx = s.second.get();
                if (!ctx) ctx = sources.emplace_back(id, std::make_unique<Source>()).second.get();
                ctx->map.store(map, std::memory_order_release);
                if (MIDIPortConnectSource(port, src, ctx) != noErr) continue;
                std::fprintf(stderr, "[reforce] deck starts: MIDI from \"%s\" (%s)\n", name.c_str(), map->name.c_str());
            }
            now.emplace_back(id, name);
        }
        connected.swap(now);
    }
};

double DeckFeed::now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

DeckFeed::DeckFeed(int port, std::string midiDevice, std::vector<ControllerMap> maps)
    : port_(port), midiDevice_(std::move(midiDevice)), maps_(std::move(maps)) {
    // UDP from rkbx_link.
    sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ >= 0) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(std::uint16_t(port_));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        timeval tv{0, 250000};
        ::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (::bind(sock_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            std::fprintf(stderr, "[reforce] deck starts: cannot listen on UDP port %d (in use?)\n", port_);
            ::close(sock_);
            sock_ = -1;
        }
    }
    if (sock_ >= 0) {
        std::fprintf(stderr, "[reforce] deck starts: listening for rkbx_link on 127.0.0.1:%d\n", port_);
        udp_ = std::thread([this] { udpLoop(); });
    }

    // MIDI from the controller.
    if (midiDevice_ != "none" && !maps_.empty()) {
        midi_ = std::make_unique<Midi>();
        Midi* m = midi_.get();
        m->owner = this;
        m->match = midiDevice_;
        m->maps = &maps_;
        OSStatus st = MIDIClientCreateWithBlock(CFSTR("reforce"), &m->client, ^(const MIDINotification* n) {
            if (n->messageID == kMIDIMsgSetupChanged) m->connectAll();
        });
        if (st == noErr)
            st = MIDIInputPortCreateWithProtocol(
                m->client, CFSTR("reforce deck starts"), kMIDIProtocol_1_0, &m->port,
                ^(const MIDIEventList* list, void* refCon) {
                    auto* source = static_cast<Midi::Source*>(refCon);
                    const ControllerMap* map = source ? source->map.load(std::memory_order_acquire) : nullptr;
                    if (!map) return;
                    const double t = DeckFeed::now();
                    const MIDIEventPacket* p = &list->packet[0];
                    for (UInt32 k = 0; k < list->numPackets; ++k) {
                        for (UInt32 w = 0; w < p->wordCount; ++w) {
                            const UInt32 word = p->words[w];
                            if ((word >> 28) != 0x2) continue;  // MIDI 1.0 channel voice (one word)
                            DeckFeedEvent e;
                            if (parseControllerMidi(*map, (word >> 16) & 0xFF, (word >> 8) & 0xFF, word & 0xFF, t,
                                                    source->hr, e))
                                m->owner->push(e);
                        }
                        p = MIDIEventPacketNext(p);
                    }
                });
        if (st != noErr) {
            std::fprintf(stderr, "[reforce] deck starts: CoreMIDI unavailable (%d)\n", int(st));
            if (m->client) MIDIClientDispose(m->client);
            midi_.reset();
        } else {
            m->connectAll();
            if (midiSources().empty())
                std::fprintf(stderr, "[reforce] deck starts: no mapped MIDI controller connected yet (%zu map(s))\n",
                             maps_.size());
        }
    }
}

DeckFeed::~DeckFeed() {
    stop_ = true;
    if (udp_.joinable()) udp_.join();
    if (sock_ >= 0) ::close(sock_);
    if (midi_) {
        if (midi_->port) MIDIPortDispose(midi_->port);
        if (midi_->client) MIDIClientDispose(midi_->client);
    }
}

void DeckFeed::udpLoop() {
    char buf[2048];
    std::vector<DeckFeedEvent> evs;
    while (!stop_) {
        const ssize_t n = ::recv(sock_, buf, sizeof buf - 1, 0);
        if (n <= 0) continue;  // timeout (checks stop_) or error
        const double t = now();
        evs.clear();
        if (!parseRkbxDatagram(std::string(buf, std::size_t(n)), t, evs)) continue;
        lastDatagram_ = t;
        std::string head(buf, std::size_t(n));
        int master = -1;
        if (std::sscanf(head.c_str(), "rkbx 1 %*f %d", &master) == 1) master_ = master;
        std::lock_guard<std::mutex> lock(mu_);
        if (head.find("\nt ") != std::string::npos) {
            std::istringstream in(head);
            std::string line;
            int deck = 0;
            DeckTrack tr;
            while (std::getline(in, line))
                if (parseRkbxTrackLine(line, deck, tr) && !(tracks_[deck] == tr)) {
                    tracks_[deck] = tr;
                    std::fprintf(stderr, "[reforce] deck %d track: %s - %s%s%s\n", deck + 1,
                                 tr.artist.empty() ? "?" : tr.artist.c_str(), tr.title.empty() ? "(none)" : tr.title.c_str(),
                                 tr.tags.empty() ? "" : "  ", tr.tags.c_str());
                }
        }
        queue_.insert(queue_.end(), evs.begin(), evs.end());
        if (queue_.size() > 20000) queue_.erase(queue_.begin(), queue_.begin() + 10000);  // nobody draining
    }
}

void DeckFeed::push(const DeckFeedEvent& e) {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.push_back(e);
}

void DeckFeed::drain(DeckStarts& s, const std::function<void(double now, const DeckFrame&)>& frames) {
    std::vector<DeckFeedEvent> q;
    {
        std::lock_guard<std::mutex> lock(mu_);
        q.swap(queue_);
    }
    for (const DeckFeedEvent& e : q) {
        switch (e.kind) {
            case DeckFeedEvent::Kind::Frame:
                s.frame(e.now, e.senderT, e.frame);
                if (frames) frames(e.now, e.frame);
                break;
            case DeckFeedEvent::Kind::Fader: s.fader(e.now, e.deck, e.value); break;
            case DeckFeedEvent::Kind::Crossfader: s.crossfader(e.now, e.value); break;
            case DeckFeedEvent::Kind::Assign: s.assign(e.now, e.deck, int(e.value)); break;
            case DeckFeedEvent::Kind::HotCue: s.hotCue(e.now, e.deck); break;
        }
    }
}

DeckTrack DeckFeed::track(int deck) const {
    std::lock_guard<std::mutex> lock(mu_);
    return deck >= 0 && deck < DeckStarts::kDecks ? tracks_[deck] : DeckTrack{};
}

bool DeckFeed::rkbxActive() const { return now() - lastDatagram_.load() < 1.0; }

std::string DeckFeed::midiSources() const {
    if (!midi_) return {};
    std::lock_guard<std::mutex> lock(midi_->mu);
    std::string s;
    for (const auto& c : midi_->connected) s += (s.empty() ? "" : ", ") + c.second;
    return s;
}

}  // namespace reforce::sync

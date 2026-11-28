#pragma once
// reforce extension: stream guests (docs/dancers-design.md "Stream guests").
//
// Viewers sign up once on a guest page (a name and a dancer look) and tap "I'm here" while watching. A guest service,
// separate from reforce, keeps the check-ins; GuestFeed polls its /api/checkins every second on its own thread,
// and the Visualizer puts each new guest on the dance floor with their name tag (reforce.Guests.*). The protocol is
// in docs/guests.md.
//
// A service may also take the polls as "the stream is live" (the maintainer's page shows LIVE while reforce polled in
// the last minute).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace reforce::web {

struct GuestArrival {
    std::int64_t seq = 0;
    std::string id;         // stable per guest
    std::string name;       // the name tag
    std::string sceneText;  // a reforce.Dancer.package scene file (Vers=100, Style=..., Name=...)
    double minutes = 0;     // how long they want to dance, picked on the guest page (0: reforce.Guests.Minutes)
    bool session = false;   // picked "until the end of the stream": stays until reforce quits or Clear
};

// A message for a speech bubble over the guest's dancer (sent from the page; the service drops ones over 2 minutes old).
struct GuestMessage {
    std::int64_t seq = 0;
    std::string id;    // the guest
    std::string name;
    std::string text;  // UTF-8, control characters removed
};

// How a guest on stage wants to dance (set on the guest page; docs/guests.md "Social").
struct GuestSocial {
    std::string id;
    std::string vibe;  // "solo" | "mingle" | "crew" ("" or anything else: mingle)
    std::string with;  // the guest they want to dance with ("" = nobody in particular)
};

// A one-off request from a guest on the floor (docs/guests.md "requests"): `kind` "bust" (Bust a move: two whole
// break routines with floor work, 32 bars). Unknown kinds are kept and ignored by the stage.
struct GuestRequest {
    std::int64_t seq = 0;
    std::string id;    // the guest
    std::string kind;
};

// Where a guest's "Bust a move" stands (docs/guests.md "bust"), sent with every poll for the guests on stage that
// have one (none: left out), so the guest page can show it.
struct GuestBust {
    enum class State { Waiting, Room, On };  // for its 8-bar point / put off, making room / busting
    std::string id;
    State state = State::Waiting;
    double seconds = -1;  // until it starts (Waiting, Room) or ends (On); < 0 unknown
};
// The &bust= value: "ID:wait:12,ID:room:20,ID:on:57" (whole seconds, left out when unknown; ids percent-encoded
// like &floor=).
std::string bustParam(const std::vector<GuestBust>& busts);

// One poll of /api/checkins: new arrivals, messages and requests, and the service's newest seqs.
struct GuestPoll {
    std::vector<GuestArrival> arrivals;
    std::vector<GuestMessage> messages;
    std::vector<GuestSocial> social;  // the guests on the floor (and arriving), when the service sent "social"
    bool hasSocial = false;
    std::vector<GuestRequest> requests;  // (a service without them sends none: nothing happens)
    std::int64_t latest = -1, msgLatest = -1, reqLatest = -1;
};
std::optional<GuestPoll> parseGuestPoll(std::string_view json, std::string* error = nullptr);

// /api/checkins response -> arrivals in order (seq > after). `latest`: the service's newest seq.
// Returns nullopt (with `error`) when the text isn't the expected JSON.
std::optional<std::vector<GuestArrival>> parseCheckins(std::string_view json, std::int64_t* latest,
                                                       std::string* error = nullptr);

// HTTPS GET (platform: NSURLSession). The body on a 2xx, else nullopt with `error`. `bearer`: sent as
// "Authorization: Bearer <bearer>" when not empty.
std::optional<std::string> httpGet(const std::string& url, double timeoutSeconds, std::string* error,
                                   const std::string& bearer = "");

// A dump of the guest service's guests (a JSON list of {id, name, dancer (object or its JSON text), at: ms of the last
// check-in}) saved into `book` (web/GuestBook): past guests from before reforce kept them. Returns how many, or -1.
class GuestBook;
int importGuests(std::string_view json, GuestBook& book, std::string* error = nullptr);

class GuestFeed {
public:
    // `memoryPath`: a small file remembering the last check-in handed over for each service URL, so a restart (or
    // switching the feed off and on) never brings the same check-in twice. "" = remember only while running.
    explicit GuestFeed(std::string memoryPath = "");
    ~GuestFeed();
    GuestFeed(const GuestFeed&) = delete;
    GuestFeed& operator=(const GuestFeed&) = delete;

    // Poll `baseUrl` (no trailing /api) every `interval` seconds while on; "" or off stops polling. Check-ins from the
    // last few hours that were never handed over (guests who tapped before the stream started) come first. `key`:
    // the service's read key, sent as a bearer token ("" = none).
    void configure(bool on, const std::string& baseUrl, double interval = 1.0, const std::string& key = "");
    // The guests on stage now (their ids), sent with every poll as &floor=ID,ID (the service shows them on its page
    // and answers with their social settings). Any thread.
    void setFloor(const std::vector<std::string>& ids);
    // Their busts (bustParam), sent with every poll as &bust= (an empty one too: the service learns this reforce
    // reports them). Any thread.
    void setBusts(const std::vector<GuestBust>& busts);
    // reforce.Guests.Minutes, sent with every poll as &minutes= (the page shows it as the usual stay). Any thread.
    void setDefaultMinutes(double minutes);
    // New arrivals and messages since the last call, together (a message's guest arrives in the same batch or
    // earlier). Any thread.
    struct Batch {
        std::vector<GuestArrival> arrivals;
        std::vector<GuestMessage> messages;
        std::vector<GuestSocial> social;  // the latest social settings (when hasSocial)
        bool hasSocial = false;
        std::vector<GuestRequest> requests;
    };
    Batch take();
    // For the panel: "" before the first poll, "ok", or the last error.
    std::string status() const;
    int arrivals() const { return total_.load(); }

private:
    void run();

    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool on_ = false, stop_ = false;
    std::string url_;
    std::string key_;
    double interval_ = 1.0;
    std::string floor_;  // "ID,ID" (URL-safe)
    std::string busts_;  // bustParam (URL-safe)
    std::string minutes_ = "30";  // &minutes=
    std::uint64_t generation_ = 0;  // bumped when the URL changes: the next poll starts from what is remembered
    std::string memoryPath_;
    std::vector<GuestArrival> pending_;
    std::vector<GuestMessage> pendingMessages_;
    std::vector<GuestRequest> pendingRequests_;
    std::vector<GuestSocial> social_;
    bool socialFresh_ = false;
    std::string status_;
    std::atomic<int> total_{0};
    std::thread thread_;
};

// Who was on the floor (Peter 2026-11-27): written next to settings.json while reforce runs, so a restart brings them
// back with the time they had left. One guest per line, "ID ENDS" (ms since 1970; 0 = brought on by hand, stays until
// cleared), after a first line "saved MS".
struct FloorGuest {
    std::string id;
    std::int64_t endsMs = 0;
};
std::string floorText(std::int64_t savedMs, const std::vector<FloorGuest>& guests);
struct FloorFile {
    std::int64_t savedMs = 0;
    std::vector<FloorGuest> guests;
};
std::optional<FloorFile> parseFloorText(std::string_view text);
// Who comes back at `nowMs`, with the seconds they have left (0: until cleared). A timed guest comes back while their
// time runs (at least `minLeftMs` of it left); one brought on by hand only when the file is at most `handRecentMs`
// old (a restart, not the next night).
struct FloorReturn {
    std::string id;
    double seconds = 0;
};
std::vector<FloorReturn> floorReturns(const FloorFile& f, std::int64_t nowMs, std::int64_t minLeftMs = 5000,
                                      std::int64_t handRecentMs = 10 * 60 * 1000);

}  // namespace reforce::web

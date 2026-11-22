#pragma once
// reforce extension: stream guests (docs/dancers-design.md "Stream guests").
//
// Viewers sign up once on a guest page (a name and a dancer look) and tap "I'm here" while watching. A guest service,
// separate from reforce, keeps the check-ins; GuestFeed polls its /api/checkins every few seconds on its own thread,
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
};

// A message for a speech bubble over the guest's dancer (sent from the page; the service drops ones over 2 minutes old).
struct GuestMessage {
    std::int64_t seq = 0;
    std::string id;    // the guest
    std::string name;
    std::string text;  // UTF-8, control characters removed
};

// One poll of /api/checkins: new arrivals and messages, and the service's newest seqs.
struct GuestPoll {
    std::vector<GuestArrival> arrivals;
    std::vector<GuestMessage> messages;
    std::int64_t latest = -1, msgLatest = -1;
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
    void configure(bool on, const std::string& baseUrl, double interval = 5.0, const std::string& key = "");
    // New arrivals and messages since the last call, together (a message's guest arrives in the same batch or
    // earlier). Any thread.
    struct Batch {
        std::vector<GuestArrival> arrivals;
        std::vector<GuestMessage> messages;
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
    double interval_ = 5.0;
    std::uint64_t generation_ = 0;  // bumped when the URL changes: the next poll starts from what is remembered
    std::string memoryPath_;
    std::vector<GuestArrival> pending_;
    std::vector<GuestMessage> pendingMessages_;
    std::string status_;
    std::atomic<int> total_{0};
    std::thread thread_;
};

}  // namespace reforce::web

#include "web/Guests.hpp"

#include "web/GuestBook.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <tuple>

#include "web/Json.hpp"

namespace reforce::web {

namespace {

// A value safe inside Key="...": no quotes, backslashes or control characters.
std::string clean(std::string_view s, std::size_t max = 64) {
    std::string out;
    for (char c : s) {
        if (out.size() >= max) break;
        if (c == '"' || c == '\\' || (static_cast<unsigned char>(c) < 0x20)) continue;
        out.push_back(c);
    }
    return out;
}

std::string sceneText(const json::Value& dancer, const std::string& name) {
    std::string t = "Vers=100\nMeta=\"reforce dancer: a stream guest\"\n";
    for (const auto& [k, v] : dancer.object) {
        // Keys are plain words (the dancer file keys); the service validated the values already.
        bool word = !k.empty() && k.size() < 32;
        for (char c : k) word = word && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
        if (!word || k == "Name" || k == "Meta" || k == "Vers" || k == "Random") continue;
        std::string val;
        if (v.isString()) val = clean(v.str);
        else if (v.isNumber()) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%g", v.num);
            val = buf;
        } else if (v.isBool()) val = v.b ? "1" : "0";
        else continue;
        t += k + "=\"" + val + "\"\n";
    }
    t += "Name=\"" + clean(name, 48) + "\"\n";
    return t;
}

}  // namespace

std::optional<std::vector<GuestArrival>> parseCheckins(std::string_view text, std::int64_t* latest,
                                                       std::string* error) {
    auto p = parseGuestPoll(text, error);
    if (!p) return std::nullopt;
    if (latest && p->latest >= 0) *latest = p->latest;
    return std::move(p->arrivals);
}

std::optional<GuestPoll> parseGuestPoll(std::string_view text, std::string* error) {
    std::string err;
    const auto root = json::parse(text, &err);
    if (!root || !root->isObject()) {
        if (error) *error = err.empty() ? "not a JSON object" : err;
        return std::nullopt;
    }
    const json::Value* list = root->get("checkins");
    if (!list || !list->isArray()) {
        if (error) *error = "no checkins list";
        return std::nullopt;
    }
    GuestPoll poll;
    if (const json::Value* s = root->get("seq"); s && s->isNumber()) poll.latest = std::int64_t(s->num);
    if (const json::Value* s = root->get("msgSeq"); s && s->isNumber()) poll.msgLatest = std::int64_t(s->num);
    std::vector<GuestArrival>& out = poll.arrivals;
    for (const json::Value& c : list->array) {
        const json::Value *seq = c.get("seq"), *id = c.get("id"), *name = c.get("name"), *dancer = c.get("dancer");
        if (!seq || !seq->isNumber() || !id || !id->isString() || !name || !name->isString() || !dancer ||
            !dancer->isObject())
            continue;
        GuestArrival a;
        a.seq = std::int64_t(seq->num);
        a.id = clean(id->str);
        a.name = clean(name->str, 48);
        a.sceneText = sceneText(*dancer, a.name);
        out.push_back(std::move(a));
    }
    if (const json::Value* msgs = root->get("messages"); msgs && msgs->isArray()) {
        for (const json::Value& m : msgs->array) {
            const json::Value *seq = m.get("seq"), *id = m.get("id"), *name = m.get("name"), *txt = m.get("text");
            if (!seq || !seq->isNumber() || !id || !id->isString() || !txt || !txt->isString()) continue;
            GuestMessage g;
            g.seq = std::int64_t(seq->num);
            g.id = clean(id->str);
            g.name = name && name->isString() ? clean(name->str, 48) : std::string();
            for (char ch : txt->str) g.text.push_back(static_cast<unsigned char>(ch) < 0x20 ? ' ' : ch);
            if (g.text.size() > 1024) g.text.resize(1024);
            poll.messages.push_back(std::move(g));
        }
    }
    return poll;
}

namespace {
// The memory file: the service URL, then the last check-in seq and the last message seq handed over.
std::pair<std::int64_t, std::int64_t> recall(const std::string& path, const std::string& url) {
    if (path.empty()) return {-1, -1};
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return {-1, -1};
    char u[1024] = {};
    long long seq = -1, msg = -1;
    const int n = std::fscanf(f, "%1023s %lld %lld", u, &seq, &msg);
    std::fclose(f);
    if (n < 2 || url != u) return {-1, -1};
    return {std::int64_t(seq), n >= 3 ? std::int64_t(msg) : -1};
}
void remember(const std::string& path, const std::string& url, std::int64_t seq, std::int64_t msg) {
    if (path.empty()) return;
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "%s %lld %lld\n", url.c_str(), static_cast<long long>(seq), static_cast<long long>(msg));
    std::fclose(f);
    std::rename(tmp.c_str(), path.c_str());
}
}  // namespace

GuestFeed::GuestFeed(std::string memoryPath) : memoryPath_(std::move(memoryPath)), thread_([this] { run(); }) {}

GuestFeed::~GuestFeed() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void GuestFeed::configure(bool on, const std::string& baseUrl, double interval, const std::string& key) {
    std::string url = baseUrl;
    while (!url.empty() && url.back() == '/') url.pop_back();
    {
        std::lock_guard<std::mutex> lock(mu_);
        const bool want = on && !url.empty();
        if (want == on_ && url == url_ && interval == interval_ && key == key_) return;
        on_ = want;
        key_ = key;
        if (url != url_) ++generation_;  // a different service: its own memory (off / on keeps where it was)
        url_ = url;
        interval_ = std::max(1.0, interval);
        if (!on_) status_.clear();
    }
    cv_.notify_all();
}

GuestFeed::Batch GuestFeed::take() {
    std::lock_guard<std::mutex> lock(mu_);
    Batch out;
    out.arrivals.swap(pending_);
    out.messages.swap(pendingMessages_);
    return out;
}

std::string GuestFeed::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

void GuestFeed::run() {
    std::int64_t after = -1, msgAfter = -1;
    std::uint64_t gen = ~std::uint64_t(0);
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        cv_.wait(lock, [&] { return stop_ || on_; });
        if (stop_) return;
        if (gen != generation_) {  // first poll of this service: carry on after what was handed over before
            gen = generation_;
            std::tie(after, msgAfter) = recall(memoryPath_, url_);
        }
        const std::string url = url_ + "/api/checkins?after=" + std::to_string(after) + "&msgAfter=" + std::to_string(msgAfter);
        const double interval = interval_;
        const std::string key = key_;
        lock.unlock();
        std::string err;
        std::optional<GuestPoll> got;
        if (auto body = httpGet(url, 10.0, &err, key)) got = parseGuestPoll(*body, &err);
        if (err == "HTTP 401" || err == "HTTP 403")
            err += key.empty() ? ": the service wants a key (reforce.Guests.Key)" : ": wrong reforce.Guests.Key";
        lock.lock();
        if (stop_) return;
        if (gen == generation_) {
            if (got) {
                status_ = "ok";
                const std::int64_t before = after, msgBefore = msgAfter;
                // The service started over (its tables were cleared): start over too.
                if (got->latest >= 0 && got->latest < after) after = -1;
                if (got->msgLatest >= 0 && got->msgLatest < msgAfter) msgAfter = -1;
                for (auto& a : got->arrivals) {
                    after = std::max(after, a.seq);
                    pending_.push_back(std::move(a));
                    total_.fetch_add(1);
                }
                for (auto& m : got->messages) {
                    msgAfter = std::max(msgAfter, m.seq);
                    pendingMessages_.push_back(std::move(m));
                }
                after = std::max(after, got->latest);
                msgAfter = std::max(msgAfter, got->msgLatest);
                if (after != before || msgAfter != msgBefore) remember(memoryPath_, url_, after, msgAfter);
            } else {
                status_ = err.empty() ? "error" : err;
            }
        }
        cv_.wait_for(lock, std::chrono::duration<double>(interval), [&] { return stop_ || gen != generation_; });
        if (stop_) return;
    }
}

int importGuests(std::string_view text, GuestBook& book, std::string* error) {
    std::string err;
    const auto root = json::parse(text, &err);
    const json::Value* list = root && root->isArray() ? &*root : root && root->isObject() ? root->get("guests") : nullptr;
    if (!list || !list->isArray()) {
        if (error) *error = err.empty() ? "not a list of guests" : err;
        return -1;
    }
    int n = 0;
    for (const json::Value& g : list->array) {
        const json::Value *id = g.get("id"), *name = g.get("name"), *dancer = g.get("dancer"), *at = g.get("at");
        if (!id || !id->isString() || !name || !name->isString() || !dancer) continue;
        // The dancer as stored by the service (a JSON object, or its JSON text).
        std::optional<json::Value> parsed;
        const json::Value* d = dancer;
        if (dancer->isString()) {
            parsed = json::parse(dancer->str);
            d = parsed ? &*parsed : nullptr;
        }
        if (!d || !d->isObject()) continue;
        const std::string nm = clean(name->str, 48);
        const std::int64_t seen = at && at->isNumber() ? std::int64_t(at->num) : 0;
        const std::string e = book.save(clean(id->str), nm, sceneText(*d, nm), seen);
        if (!e.empty()) {
            if (error) *error = e;
            return -1;
        }
        ++n;
    }
    return n;
}

}  // namespace reforce::web

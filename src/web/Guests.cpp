#include "web/Guests.hpp"

#include "web/GuestBook.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    if (const json::Value* s = root->get("reqSeq"); s && s->isNumber()) poll.reqLatest = std::int64_t(s->num);
    if (const json::Value* reqs = root->get("requests"); reqs && reqs->isArray()) {
        for (const json::Value& q : reqs->array) {
            const json::Value *seq = q.get("seq"), *id = q.get("id"), *kind = q.get("kind");
            if (!seq || !seq->isNumber() || !id || !id->isString() || !kind || !kind->isString()) continue;
            poll.requests.push_back({std::int64_t(seq->num), clean(id->str), clean(kind->str, 16)});
        }
    }
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
        if (const json::Value* mins = c.get("minutes"); mins && mins->isNumber() && mins->num > 0)
            a.minutes = std::min(mins->num, 24.0 * 60.0);
        if (const json::Value* ses = c.get("session"); ses && ses->isBool()) a.session = ses->b;
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
    if (const json::Value* soc = root->get("social"); soc && soc->isArray()) {
        poll.hasSocial = true;
        for (const json::Value& e : soc->array) {
            const json::Value *id = e.get("id"), *vibe = e.get("vibe"), *with = e.get("with");
            if (!id || !id->isString()) continue;
            GuestSocial g;
            g.id = clean(id->str);
            g.vibe = vibe && vibe->isString() ? clean(vibe->str, 16) : std::string();
            g.with = with && with->isString() ? clean(with->str) : std::string();
            poll.social.push_back(std::move(g));
        }
    }
    return poll;
}

namespace {
// The memory file: the service URL, then the last check-in, message and request seqs handed over.
std::tuple<std::int64_t, std::int64_t, std::int64_t> recall(const std::string& path, const std::string& url) {
    if (path.empty()) return {-1, -1, -1};
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return {-1, -1, -1};
    char u[1024] = {};
    long long seq = -1, msg = -1, req = -1;
    const int n = std::fscanf(f, "%1023s %lld %lld %lld", u, &seq, &msg, &req);
    std::fclose(f);
    if (n < 2 || url != u) return {-1, -1, -1};
    return {std::int64_t(seq), n >= 3 ? std::int64_t(msg) : -1, n >= 4 ? std::int64_t(req) : -1};
}
void remember(const std::string& path, const std::string& url, std::int64_t seq, std::int64_t msg, std::int64_t req) {
    if (path.empty()) return;
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "%s %lld %lld %lld\n", url.c_str(), static_cast<long long>(seq), static_cast<long long>(msg),
                 static_cast<long long>(req));
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

namespace {

// Ids are the service's own (hex); anything else is percent-encoded.
void appendId(std::string& f, const std::string& id) {
    for (unsigned char c : id) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            f.push_back(char(c));
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            f += buf;
        }
    }
}

}  // namespace

void GuestFeed::setFloor(const std::vector<std::string>& ids) {
    std::string f;
    for (const auto& id : ids) {
        if (!f.empty()) f += ',';
        appendId(f, id);
    }
    std::lock_guard<std::mutex> lock(mu_);
    floor_ = std::move(f);
}

std::string bustParam(const std::vector<GuestBust>& busts) {
    std::string f;
    for (const auto& b : busts) {
        if (!f.empty()) f += ',';
        appendId(f, b.id);
        f += b.state == GuestBust::State::On ? ":on" : b.state == GuestBust::State::Room ? ":room" : ":wait";
        if (b.seconds >= 0) f += ':' + std::to_string(std::lround(b.seconds));
    }
    return f;
}

void GuestFeed::setBusts(const std::vector<GuestBust>& busts) {
    std::string f = bustParam(busts);
    std::lock_guard<std::mutex> lock(mu_);
    busts_ = std::move(f);
}

void GuestFeed::setDefaultMinutes(double minutes) {
    std::string m = std::to_string(std::lround(std::clamp(minutes, 1.0, 24.0 * 60.0)));
    std::lock_guard<std::mutex> lock(mu_);
    minutes_ = std::move(m);
}

GuestFeed::Batch GuestFeed::take() {
    std::lock_guard<std::mutex> lock(mu_);
    Batch out;
    out.arrivals.swap(pending_);
    out.messages.swap(pendingMessages_);
    out.requests.swap(pendingRequests_);
    if (socialFresh_) {
        out.social = social_;
        out.hasSocial = true;
        socialFresh_ = false;
    }
    return out;
}

std::string GuestFeed::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

void GuestFeed::run() {
    std::int64_t after = -1, msgAfter = -1, reqAfter = -1;
    std::uint64_t gen = ~std::uint64_t(0);
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        cv_.wait(lock, [&] { return stop_ || on_; });
        if (stop_) return;
        if (gen != generation_) {  // first poll of this service: carry on after what was handed over before
            gen = generation_;
            std::tie(after, msgAfter, reqAfter) = recall(memoryPath_, url_);
        }
        const std::string url = url_ + "/api/checkins?after=" + std::to_string(after) + "&msgAfter=" + std::to_string(msgAfter) +
                                "&reqAfter=" + std::to_string(reqAfter) + "&floor=" + floor_ + "&bust=" + busts_ + "&minutes=" + minutes_;
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
                const std::int64_t before = after, msgBefore = msgAfter, reqBefore = reqAfter;
                // The service started over (its tables were cleared): start over too.
                if (got->latest >= 0 && got->latest < after) after = -1;
                if (got->msgLatest >= 0 && got->msgLatest < msgAfter) msgAfter = -1;
                if (got->reqLatest >= 0 && got->reqLatest < reqAfter) reqAfter = -1;
                for (auto& a : got->arrivals) {
                    after = std::max(after, a.seq);
                    pending_.push_back(std::move(a));
                    total_.fetch_add(1);
                }
                for (auto& m : got->messages) {
                    msgAfter = std::max(msgAfter, m.seq);
                    pendingMessages_.push_back(std::move(m));
                }
                for (auto& q : got->requests) {
                    if (q.seq <= reqAfter) continue;
                    reqAfter = std::max(reqAfter, q.seq);
                    pendingRequests_.push_back(std::move(q));
                }
                if (got->hasSocial) {
                    social_ = std::move(got->social);
                    socialFresh_ = true;
                }
                after = std::max(after, got->latest);
                msgAfter = std::max(msgAfter, got->msgLatest);
                reqAfter = std::max(reqAfter, got->reqLatest);
                if (after != before || msgAfter != msgBefore || reqAfter != reqBefore)
                    remember(memoryPath_, url_, after, msgAfter, reqAfter);
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

std::string floorText(std::int64_t savedMs, const std::vector<FloorGuest>& guests) {
    std::string out = "saved " + std::to_string(savedMs) + "\n";
    for (const auto& g : guests) {
        if (g.id.empty() || g.id.find_first_of(" \n\r\t") != std::string::npos) continue;
        out += g.id + " " + std::to_string(g.endsMs) + "\n";
    }
    return out;
}

std::optional<FloorFile> parseFloorText(std::string_view text) {
    FloorFile f;
    bool head = false;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) nl = text.size();
        std::string line(text.substr(at, nl - at));
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::size_t sp = line.find(' ');
        if (sp == std::string::npos || sp == 0) return std::nullopt;
        const std::string key = line.substr(0, sp), val = line.substr(sp + 1);
        char* end = nullptr;
        const long long v = std::strtoll(val.c_str(), &end, 10);
        if (end == val.c_str() || *end != '\0' || v < 0) return std::nullopt;
        if (!head) {
            if (key != "saved") return std::nullopt;
            f.savedMs = v;
            head = true;
        } else {
            f.guests.push_back({key, v});
        }
    }
    if (!head) return std::nullopt;
    return f;
}

std::vector<FloorReturn> floorReturns(const FloorFile& f, std::int64_t nowMs, std::int64_t minLeftMs,
                                      std::int64_t handRecentMs) {
    std::vector<FloorReturn> out;
    const bool recent = nowMs >= f.savedMs && nowMs - f.savedMs <= handRecentMs;
    for (const auto& g : f.guests) {
        if (std::any_of(out.begin(), out.end(), [&](const FloorReturn& r) { return r.id == g.id; })) continue;
        if (g.endsMs == 0) {
            if (recent) out.push_back({g.id, 0.0});
        } else if (g.endsMs - nowMs >= minLeftMs) {
            out.push_back({g.id, double(g.endsMs - nowMs) / 1000.0});
        }
    }
    return out;
}

}  // namespace reforce::web

#include "lights/LightsOutput.hpp"

#include <arpa/inet.h>
#include <dns_sd.h>
#include <sys/select.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include <httplib.h>

#include "engine/Log.hpp"
#include "web/Json.hpp"

namespace reforce::lights {

namespace json = web::json;
using Clock = std::chrono::steady_clock;

namespace {
constexpr auto kSendPeriod = std::chrono::milliseconds(50);  // 20 packets a second
constexpr auto kCheckPeriod = std::chrono::seconds(10);      // are the panels still there?
constexpr auto kRetryPeriod = std::chrono::seconds(5);       // reconnect attempts
constexpr int kTransition = 1;                               // panel fade per packet, in 100 ms

std::string apiBase(const DeviceConfig& c) { return "/api/v1/" + c.token; }
}  // namespace

// ---------------------------------------------------------------- config

std::optional<DeviceConfig> loadDeviceConfig(const std::filesystem::path& path, std::string* error) {
    std::ifstream f(path);
    if (!f) {
        if (error) *error = "not paired (no " + path.filename().string() + ")";
        return std::nullopt;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string err;
    auto root = json::parse(ss.str(), &err);
    if (!root || !root->isObject()) {
        if (error) *error = path.filename().string() + ": " + (err.empty() ? "not a JSON object" : err);
        return std::nullopt;
    }
    DeviceConfig c;
    if (const auto* v = root->get("host"); v && v->isString()) c.host = v->str;
    if (const auto* v = root->get("port"); v && v->isNumber()) c.port = int(v->num);
    if (const auto* v = root->get("auth_token"); v && v->isString()) c.token = v->str;
    if (const auto* v = root->get("udpPort"); v && v->isNumber()) c.udpPort = int(v->num);
    if (c.host.empty() || c.token.empty()) {
        if (error) *error = path.filename().string() + ": needs host and auth_token";
        return std::nullopt;
    }
    return c;
}

bool saveDeviceConfig(const std::filesystem::path& path, const DeviceConfig& cfg, std::string* error) {
    json::Writer w;
    w.beginObject().field("host", cfg.host).field("port", cfg.port).field("auth_token", cfg.token);
    if (cfg.udpPort != 60222) w.field("udpPort", cfg.udpPort);
    w.endObject();
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) {
            if (error) *error = "cannot write " + tmp.string();
            return false;
        }
        f << w.str() << "\n";
    }
    std::error_code ec;
    std::filesystem::permissions(tmp, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

std::optional<DeviceState> parseDeviceState(std::string_view text, std::string* error) {
    std::string err;
    auto root = json::parse(text, &err, 64);
    if (!root || !root->isObject()) {
        if (error) *error = err.empty() ? "not a JSON object" : err;
        return std::nullopt;
    }
    DeviceState d;
    if (const auto* v = root->get("name"); v && v->isString()) d.name = v->str;
    if (const auto* v = root->get("model"); v && v->isString()) d.model = v->str;
    if (const auto* v = root->get("firmwareVersion"); v && v->isString()) d.firmware = v->str;
    if (const auto* st = root->get("state"))
        if (const auto* on = st->get("on"))
            if (const auto* v = on->get("value"); v && v->isBool()) d.on = v->b;
    if (const auto* fx = root->get("effects")) {
        if (const auto* v = fx->get("select"); v && v->isString()) d.effect = v->str;
        if (const auto* v = fx->get("effectsList"); v && v->isArray())
            for (const auto& e : v->array)
                if (e.isString()) d.effects.push_back(e.str);
    }
    const json::Value* lay = nullptr;
    if (const auto* pl = root->get("panelLayout")) lay = pl->get("layout");
    if (!lay) {
        if (error) *error = "no panelLayout.layout";
        return std::nullopt;
    }
    if (const auto* v = lay->get("sideLength"); v && v->isNumber() && v->num > 0) d.layout.sideLength = v->num;
    if (const auto* pd = lay->get("positionData"); pd && pd->isArray())
        for (const auto& p : pd->array) {
            const auto *id = p.get("panelId"), *x = p.get("x"), *y = p.get("y"), *shape = p.get("shapeType");
            if (!id || !id->isNumber() || !x || !x->isNumber() || !y || !y->isNumber()) continue;
            // Controllers and connectors (shapeType 12+ on newer panels) have no light: skip them. 0..4 and the
            // rest of the light-carrying shapes are kept.
            if (shape && shape->isNumber() && (int(shape->num) == 12 || int(shape->num) == 15)) continue;
            const auto* orient = p.get("o");
            d.layout.panels.push_back(Panel{int(id->num), x->num, y->num, orient && orient->isNumber() ? orient->num : 0.0});
        }
    if (d.layout.panels.empty()) {
        if (error) *error = "no panels in the layout";
        return std::nullopt;
    }
    return d;
}

std::optional<std::string> pairNanoleaf(const std::string& host, int port, std::string* error) {
    httplib::Client cli(host, port);
    cli.set_connection_timeout(3, 0);
    cli.set_read_timeout(3, 0);
    auto res = cli.Post("/api/v1/new");
    if (!res) {
        if (error) *error = "no answer from " + host + " (" + httplib::to_string(res.error()) + ")";
        return std::nullopt;
    }
    if (res->status == 403) {
        if (error) *error = "not in pairing mode: hold the power button 5-7 s until the lights flash, then try again";
        return std::nullopt;
    }
    if (res->status < 200 || res->status >= 300) {
        if (error) *error = "HTTP " + std::to_string(res->status);
        return std::nullopt;
    }
    auto root = json::parse(res->body);
    const json::Value* tok = root ? root->get("auth_token") : nullptr;
    if (!tok || !tok->isString() || tok->str.empty()) {
        if (error) *error = "no auth_token in the answer";
        return std::nullopt;
    }
    return tok->str;
}

namespace {
struct Discovery {
    std::string name, type, domain;
    std::uint32_t ifIndex = 0;
    bool found = false, resolved = false;
    std::string host;
    int port = 0;
};

void DNSSD_API onBrowse(DNSServiceRef, DNSServiceFlags flags, std::uint32_t ifIndex, DNSServiceErrorType err,
                        const char* name, const char* type, const char* domain, void* ctx) {
    auto* d = static_cast<Discovery*>(ctx);
    if (err != kDNSServiceErr_NoError || !(flags & kDNSServiceFlagsAdd) || d->found) return;
    d->name = name, d->type = type, d->domain = domain, d->ifIndex = ifIndex;
    d->found = true;
}

void DNSSD_API onResolve(DNSServiceRef, DNSServiceFlags, std::uint32_t, DNSServiceErrorType err, const char*,
                         const char* host, std::uint16_t portNet, std::uint16_t, const unsigned char*, void* ctx) {
    auto* d = static_cast<Discovery*>(ctx);
    if (err != kDNSServiceErr_NoError || d->resolved) return;
    d->host = host;
    if (!d->host.empty() && d->host.back() == '.') d->host.pop_back();
    d->port = ntohs(portNet);
    d->resolved = true;
}

// Runs `ref`'s callbacks until done() or the deadline.
template <class Done>
void serviceUntil(DNSServiceRef ref, Clock::time_point deadline, Done done) {
    const int fd = DNSServiceRefSockFD(ref);
    while (!done()) {
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
        if (left <= 0 || fd < 0) return;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        timeval tv{time_t(left / 1000000), suseconds_t(left % 1000000)};
        if (::select(fd + 1, &set, nullptr, nullptr, &tv) <= 0) return;
        if (DNSServiceProcessResult(ref) != kDNSServiceErr_NoError) return;
    }
}
}  // namespace

std::optional<std::pair<std::string, int>> discoverNanoleaf(double timeoutSeconds, std::string* error) {
    Discovery d;
    const auto deadline = Clock::now() + std::chrono::milliseconds(int(timeoutSeconds * 1000));
    DNSServiceRef browse = nullptr;
    if (DNSServiceBrowse(&browse, 0, 0, "_nanoleafapi._tcp", nullptr, onBrowse, &d) != kDNSServiceErr_NoError) {
        if (error) *error = "Bonjour browse failed";
        return std::nullopt;
    }
    serviceUntil(browse, deadline, [&] { return d.found; });
    DNSServiceRefDeallocate(browse);
    if (!d.found) {
        if (error) *error = "no Nanoleaf found on the network";
        return std::nullopt;
    }
    DNSServiceRef res = nullptr;
    if (DNSServiceResolve(&res, 0, d.ifIndex, d.name.c_str(), d.type.c_str(), d.domain.c_str(), onResolve, &d) ==
        kDNSServiceErr_NoError) {
        serviceUntil(res, std::max(deadline, Clock::now() + std::chrono::seconds(2)), [&] { return d.resolved; });
        DNSServiceRefDeallocate(res);
    }
    if (!d.resolved) {
        if (error) *error = "found \"" + d.name + "\" but could not resolve it";
        return std::nullopt;
    }
    return std::make_pair(d.host, d.port);
}

// ---------------------------------------------------------------- output

LightsOutput::LightsOutput(std::filesystem::path configPath) : configPath_(std::move(configPath)) {
    thread_ = std::thread([this] { run(); });
}

LightsOutput::~LightsOutput() { shutdown(); }

void LightsOutput::shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (sock_ >= 0) ::close(sock_);
    sock_ = -1;
}

void LightsOutput::setEnabled(bool on) {
    {
        std::lock_guard<std::mutex> lk(m_);
        enabled_.store(on);
    }
    cv_.notify_all();
}

void LightsOutput::setSettings(const MapSettings& s) {
    std::lock_guard<std::mutex> lk(m_);
    settings_ = s;
}

void LightsOutput::submit(const std::uint8_t* bgra, int w, int h, int rowBytes) {
    if (!bgra || w <= 0 || h <= 0 || rowBytes < w * 4) return;
    std::lock_guard<std::mutex> lk(m_);
    const std::size_t bytes = std::size_t(rowBytes) * std::size_t(h);
    if (pending_.size() != bytes) pending_.resize(bytes);  // once, at the first capture
    std::memcpy(pending_.data(), bgra, bytes);
    pw_ = w, ph_ = h, prow_ = rowBytes;
    ++pendingSeq_;
}

LightsStatus LightsOutput::status() const {
    std::lock_guard<std::mutex> lk(viewM_);
    return status_;
}

std::vector<PanelView> LightsOutput::panels() const {
    std::lock_guard<std::mutex> lk(viewM_);
    return view_;
}

void LightsOutput::setStatus(const std::string& state, const std::string& message) {
    std::lock_guard<std::mutex> lk(viewM_);
    if (status_.state != state || status_.message != message) {
        if (state == "streaming")
            rlog("[reforce] lights: streaming to %s (%d panels)\n", status_.device.c_str(), status_.panels);
        else if (state == "off" && status_.state == "streaming")
            rlog("[reforce] lights: off, panels put back\n");
        else if (!message.empty())
            rlog("[reforce] lights: %s\n", message.c_str());
    }
    status_.state = state;
    status_.message = message;
    if (state != "streaming") status_.sendHz = 0;
}

bool LightsOutput::request(const char* method, const std::string& path, const std::string& body,
                           std::string* response, double timeout) {
    httplib::Client cli(cfg_.host, cfg_.port);
    const auto secs = time_t(timeout), usecs = time_t((timeout - double(secs)) * 1e6);
    cli.set_connection_timeout(secs, usecs);
    cli.set_read_timeout(secs, usecs);
    cli.set_write_timeout(secs, usecs);
    httplib::Result res = std::strcmp(method, "GET") == 0 ? cli.Get(path) : cli.Put(path, body, "application/json");
    if (!res) {
        if (response) *response = httplib::to_string(res.error());
        return false;
    }
    if (response) *response = res->body;
    if (res->status < 200 || res->status >= 300) {
        if (response) *response = "HTTP " + std::to_string(res->status);
        return false;
    }
    return true;
}

bool LightsOutput::connect() {
    std::string err;
    auto cfg = loadDeviceConfig(configPath_, &err);
    if (!cfg) {
        setStatus("not paired", err);
        return false;
    }
    cfg_ = *cfg;
    setStatus("connecting", "connecting to " + cfg_.host);
    std::string body;
    if (!request("GET", apiBase(cfg_) + "/", "", &body, 3.0)) {
        setStatus("error", cfg_.host + ": " + body);
        return false;
    }
    auto st = parseDeviceState(body, &err);
    if (!st) {
        setStatus("error", cfg_.host + ": " + err);
        return false;
    }
    // Resolve the UDP address (IPv4: Light Panels stream over v4).
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* ai = nullptr;
    const std::string port = std::to_string(cfg_.udpPort);
    if (::getaddrinfo(cfg_.host.c_str(), port.c_str(), &hints, &ai) != 0 || !ai) {
        setStatus("error", "cannot resolve " + cfg_.host);
        return false;
    }
    addr_.assign(reinterpret_cast<const std::uint8_t*>(ai->ai_addr),
                 reinterpret_cast<const std::uint8_t*>(ai->ai_addr) + ai->ai_addrlen);
    ::freeaddrinfo(ai);
    if (sock_ < 0) {
        sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ >= 0) ::fcntl(sock_, F_SETFL, ::fcntl(sock_, F_GETFL) | O_NONBLOCK);
    }
    if (sock_ < 0) {
        setStatus("error", "no UDP socket");
        return false;
    }
    // Keep what the panels showed, once (a reconnect while streaming would find our own stream).
    if (!saved_) saved_ = *st;
    if (!request("PUT", apiBase(cfg_) + "/state", R"({"on":{"value":true}})", &body, 3.0) ||
        !request("PUT", apiBase(cfg_) + "/effects",
                 R"({"write":{"command":"display","animType":"extControl","extControlVersion":"v2"}})", &body, 3.0)) {
        setStatus("error", "external control refused: " + body);
        return false;
    }
    mapper_.setLayout(st->layout);
    ids_.clear();
    for (const Panel& p : st->layout.panels) ids_.push_back(p.id);
    {
        std::lock_guard<std::mutex> lk(viewM_);
        status_.device = st->name.empty() ? cfg_.host : st->name;
        status_.panels = int(st->layout.panels.size());
        status_.sideLength = st->layout.sideLength;
        view_.clear();
        for (std::size_t i = 0; i < st->layout.panels.size(); ++i) {
            const Panel& p = st->layout.panels[i];
            view_.push_back(PanelView{p.id, p.x, p.y, p.o, mapper_.u(i), mapper_.v(i), Rgb{}});
        }
    }
    setStatus("streaming", "");
    return true;
}

void LightsOutput::restore() {
    streaming_.store(false);
    if (!saved_) return;
    std::string body;
    const DeviceState s = *saved_;
    saved_.reset();
    // A saved effect comes back; "*Dynamic*" / "*Solid*" etc. cannot be selected, so only on/off is put back then.
    if (std::find(s.effects.begin(), s.effects.end(), s.effect) != s.effects.end())
        request("PUT", apiBase(cfg_) + "/effects", "{\"select\":" + json::quote(s.effect) + "}", &body, 2.0);
    request("PUT", apiBase(cfg_) + "/state", std::string(R"({"on":{"value":)") + (s.on ? "true" : "false") + "}}",
            &body, 2.0);
}

void LightsOutput::setBeat(double beat, double bpm, bool confident) {
    std::lock_guard<std::mutex> lk(m_);
    beat_ = beat, bpm_ = bpm, beatConfident_ = confident;
    beatAt_ = Clock::now();
}

void LightsOutput::startPairing() {
    {
        std::lock_guard<std::mutex> lk(m_);
        pairRequested_ = true;
        pairCancel_ = false;
    }
    cv_.notify_all();
}

void LightsOutput::cancelPairing() {
    {
        std::lock_guard<std::mutex> lk(m_);
        pairRequested_ = false;
        pairCancel_ = true;
    }
    cv_.notify_all();
}

void LightsOutput::pair() {
    auto stopped = [&] {
        std::lock_guard<std::mutex> lk(m_);
        return quit_ || pairCancel_;
    };
    // The paired panels' host when there is one (a re-pair after a reset), else the first one Bonjour finds.
    std::string host;
    int port = 16021;
    if (auto old = loadDeviceConfig(configPath_)) host = old->host, port = old->port;
    if (host.empty()) {
        setStatus("pairing", "looking for Nanoleaf panels on the network");
        std::string err;
        auto found = discoverNanoleaf(5.0, &err);
        if (!found) {
            setStatus("error", "pairing: " + err);
            return;
        }
        host = found->first, port = found->second;
    }
    setStatus("pairing", "hold the controller's power button 5-7 s, until its lights flash (" + host + ")");
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    std::string err;
    while (!stopped() && Clock::now() < deadline) {
        if (auto tok = pairNanoleaf(host, port, &err)) {
            DeviceConfig c;
            c.host = host, c.port = port, c.token = *tok;
            if (!saveDeviceConfig(configPath_, c, &err)) {
                setStatus("error", "pairing: " + err);
                return;
            }
            setStatus("off", "paired with " + host);
            return;
        }
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::seconds(2), [&] { return quit_ || pairCancel_; });
    }
    setStatus("off", stopped() ? "pairing canceled" : "pairing timed out (" + err + ")");
}

void LightsOutput::run() {
    bool connected = false;
    std::uint64_t usedSeq = 0;
    int w = 0, h = 0, row = 0;
    Clock::time_point nextSend = Clock::now(), nextCheck = Clock::now(), lastUpdate = Clock::now(),
                      hzStart = Clock::now();
    int sentSinceHz = 0;
    long long lastBeat = LLONG_MIN;  // the beat whose flash went out last
    std::unique_lock<std::mutex> lk(m_);
    auto putBack = [&] {
        if (!connected && !saved_) return;
        lk.unlock();
        restore();
        lk.lock();
        connected = false;
    };
    while (!quit_) {
        if (pairRequested_) {
            pairRequested_ = false;
            putBack();
            lk.unlock();
            pair();
            lk.lock();
            continue;
        }
        if (!enabled_.load()) {
            putBack();
            std::string msg;  // a pairing result stays shown
            {
                std::lock_guard<std::mutex> vl(viewM_);
                if (status_.state == "off" || status_.message.rfind("pairing", 0) == 0) msg = status_.message;
            }
            setStatus("off", msg);
            cv_.wait(lk, [&] { return quit_ || enabled_.load() || pairRequested_; });
            continue;
        }
        if (!connected) {
            lk.unlock();
            connected = connect();
            lk.lock();
            if (!connected) {
                cv_.wait_for(lk, kRetryPeriod, [&] { return quit_ || !enabled_.load() || pairRequested_; });
                continue;
            }
            streaming_.store(true);
            nextSend = nextCheck = Clock::now();
            nextCheck += kCheckPeriod;
            lastUpdate = hzStart = Clock::now();
            sentSinceHz = 0;
        }
        // The beat pulse: each flash goes out exactly on its beat (less the lead), so every beat looks the same; the
        // fade's ticks follow from there on the same 50 ms grid.
        auto pulseOn = [&](Clock::time_point t) {
            return settings_.pulse > 0 && beatConfident_ && bpm_ > 0 && t - beatAt_ < std::chrono::seconds(2);
        };
        auto beatPos = [&](Clock::time_point t) {  // the beat count showing on the panels for a packet sent at t
            return beat_ + (std::chrono::duration<double>(t - beatAt_).count() + settings_.pulseLeadMs / 1000.0) * bpm_ / 60.0;
        };
        Clock::time_point wake = nextSend, beatDue = Clock::time_point::max();
        if (pulseOn(Clock::now())) {
            const auto t0 = Clock::now();
            const double pos = beatPos(t0);
            long long next = (long long)std::floor(pos) + 1;
            if (next - 1 > lastBeat && pos - std::floor(pos) < 0.02) next -= 1;  // on it right now
            beatDue = t0 + std::chrono::duration_cast<Clock::duration>(
                               std::chrono::duration<double>((double(next) - pos) * 60.0 / bpm_));
            if (beatDue < nextSend + std::chrono::milliseconds(20)) wake = beatDue;  // no extra tick just before it
        }
        cv_.wait_until(lk, wake, [&] { return quit_ || !enabled_.load() || pairRequested_; });
        if (quit_ || !enabled_.load() || pairRequested_) continue;
        const auto now = Clock::now();
        bool beatTick = false;
        if (beatDue != Clock::time_point::max() && now + std::chrono::milliseconds(2) >= beatDue) {
            const long long b = std::llround(beatPos(now));
            if (b > lastBeat) {
                beatTick = true;
                lastBeat = b;
                nextSend = beatDue + kSendPeriod;  // the fade's ticks line up with the beat
            }
        }
        if (!beatTick) nextSend = std::max(nextSend + kSendPeriod, now);
        const bool fresh = pendingSeq_ != usedSeq;
        if (fresh) {
            std::swap(pending_, work_);
            w = pw_, h = ph_, row = prow_;
            usedSeq = pendingSeq_;
        }
        const MapSettings s = settings_;
        // The beat pulse: where the beat will be when this packet shows (its lead), from the clock's last report.
        double pulse = 1.0;
        const bool pulsing = pulseOn(now);
        if (pulsing) pulse = beatTick ? 1.0 : pulseFactor(beatPos(now), s.pulse);
        lk.unlock();
        if (fresh) {
            const double dt = std::chrono::duration<double>(now - lastUpdate).count();
            lastUpdate = now;
            mapper_.update(work_.data(), w, h, row, std::min(dt, 0.5), s, colors_);
        }
        if (!colors_.empty()) {
            // Every tick (the pulse moves even when no new capture came in), after smoothing: the flash stays sharp.
            sent_.resize(colors_.size());
            for (std::size_t i = 0; i < colors_.size(); ++i)
                sent_[i] = Rgb{std::uint8_t(std::lround(colors_[i].r * pulse)), std::uint8_t(std::lround(colors_[i].g * pulse)),
                               std::uint8_t(std::lround(colors_[i].b * pulse))};
            // The flash itself is instant; everything else fades over 100 ms.
            encodeStreamV2(ids_, sent_, beatTick ? 0 : kTransition, packet_);
            ::sendto(sock_, packet_.data(), packet_.size(), 0, reinterpret_cast<const sockaddr*>(addr_.data()),
                     socklen_t(addr_.size()));
            ++sentSinceHz;
            std::lock_guard<std::mutex> vl(viewM_);
            for (std::size_t i = 0; i < view_.size() && i < sent_.size(); ++i) view_[i].color = sent_[i];
            status_.pulsing = pulsing;
        }
        if (now - hzStart >= std::chrono::seconds(1)) {
            std::lock_guard<std::mutex> vl(viewM_);
            status_.sendHz = sentSinceHz / std::chrono::duration<double>(now - hzStart).count();
            hzStart = now;
            sentSinceHz = 0;
        }
        if (now >= nextCheck) {
            nextCheck = now + kCheckPeriod;
            std::string body;
            if (!request("GET", apiBase(cfg_) + "/state/on", "", &body, 2.0)) {
                streaming_.store(false);
                connected = false;
                setStatus("error", "lost the panels (" + body + "), reconnecting");
            }
        }
        lk.lock();
    }
    lk.unlock();
    if (connected || saved_) {
        restore();
        setStatus("off", "");
    }
}

}  // namespace reforce::lights

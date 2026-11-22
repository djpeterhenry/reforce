#include "web/WebServer.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>  // arc4random_buf

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <optional>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <thread>

#include <httplib.h>

#include "engine/SceneLibrary.hpp"
#include "web/Json.hpp"

namespace reforce::web {
namespace {

using Clock = std::chrono::steady_clock;

bool isLoopbackHost(const std::string& h) { return h == "127.0.0.1" || h == "localhost" || h == "::1"; }

// Host header minus the port; "[::1]:8411" -> "::1".
std::string hostOnly(const std::string& hostHeader) {
    if (!hostHeader.empty() && hostHeader.front() == '[') {
        const auto close = hostHeader.find(']');
        return close == std::string::npos ? hostHeader : hostHeader.substr(1, close - 1);
    }
    const auto colon = hostHeader.find(':');
    return colon == std::string::npos ? hostHeader : hostHeader.substr(0, colon);
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

const char* mimeType(const std::string& name) {
    auto ends = [&](const char* ext) {
        const std::size_t n = std::strlen(ext);
        return name.size() >= n && name.compare(name.size() - n, n, ext) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".css")) return "text/css; charset=utf-8";
    if (ends(".js")) return "text/javascript; charset=utf-8";
    if (ends(".json") || ends(".webmanifest")) return "application/json";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".png")) return "image/png";
    if (ends(".ico")) return "image/x-icon";
    return "application/octet-stream";
}

std::string lowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

void jsonError(httplib::Response& res, int status, const std::string& message) {
    json::Writer w;
    w.beginObject().field("ok", false).field("error", message).endObject();
    res.status = status;
    res.set_content(w.take(), "application/json");
}

}  // namespace

struct WebServer::Impl {
    WebServerOptions opts;
    engine::CommandQueue& commands;
    StateProvider state;
    std::shared_ptr<const Library> library;
    std::string libraryJson;
    std::function<VideoSourceList()> videoSources;
    std::function<std::string(const engine::PrefsTable&)> lookSaver;
    std::mutex favMutex;
    std::vector<Favorite> favorites;
    std::function<std::string(const std::string&)> favSaver;
    std::mutex hidMutex;
    std::vector<std::string> hidden;  // reforce.Sprite.Hidden ids
    std::function<std::string(const std::string&)> hidSaver;
    std::map<std::string, std::filesystem::path> spriteImages;  // lower-case id -> image sprite file
    httplib::Server server;
    std::thread thread;
    std::atomic<bool> stopping{false};
    std::atomic<bool> started{false};
    int port = 0;

    Impl(WebServerOptions o, engine::CommandQueue& q, StateProvider s, std::shared_ptr<const Library> lib)
        : opts(std::move(o)), commands(q), state(std::move(s)), library(std::move(lib)) {
        if (!library) library = std::make_shared<Library>();
        libraryJson = libraryToJson(*library);
        if (const LibraryLayer* ll = library->find(engine::Layer::Sprite))
            for (const auto& c : ll->configs)
                if (!c.movie && !c.path.empty()) spriteImages[lowerAscii(c.id)] = c.path;
        routes();
    }

    std::string hiddenJson() const {  // under hidMutex
        json::Writer w;
        w.beginObject().field("canSave", bool(hidSaver)).key("hidden").stringArray(hidden).endObject();
        return w.take();
    }

    bool authorized(const httplib::Request& req) const {
        if (opts.token.empty()) return true;
        std::string t = req.get_param_value("token");
        if (t.empty()) t = req.get_header_value("X-Reforce-Token");
        if (t.empty()) {
            const std::string auth = req.get_header_value("Authorization");
            if (auth.rfind("Bearer ", 0) == 0) t = auth.substr(7);
        }
        return constantTimeEquals(t, opts.token);
    }

    // Look "save" runs here (file I/O stays off the render thread); every other
    // command is passed through. Returns "" or sets *error ("501": no saver).
    std::string resolveLookSaves(const std::vector<engine::Command>& in, std::vector<engine::Command>& out,
                                 std::string* error) {
        std::optional<engine::LookControl> look;
        for (const auto& c : in) {
            if (c.type != engine::CommandType::Look) {
                out.push_back(c);
                continue;
            }
            if (!look) look.emplace(state().look);
            if (c.text != "save") {
                // Mirror the change so a save later in this request saves it.
                unsigned m = 0;
                if (c.text == "ab") look->toggleAB();
                else if (c.text == "preset") look->preset(*engine::lookPreset(c.arg));
                else if (c.text == "reset") look->resetToSaved(c.arg.empty() ? std::nullopt : engine::parseLookKey(c.arg));
                else if (auto k = engine::parseLookKey(c.text)) look->set(*k, c.arg, &m);
                out.push_back(c);
                continue;
            }
            if (!lookSaver) return *error = "501";
            const auto key = c.arg.empty() ? std::nullopt : engine::parseLookKey(c.arg);
            const std::string err = lookSaver(engine::lookSaveEntries(look->current(), key));
            if (!err.empty()) return *error = "saving the Look: " + err;
            for (const auto& k : engine::lookKeys()) {
                if (key && *key != k.key) continue;
                look->markSaved(k.key, look->current()[k.key]);
                out.push_back(engine::Command::look("saved", std::string(k.id) + "=" + look->current()[k.key]));
            }
        }
        return {};
    }

    void routes() {
        // SO_REUSEADDR only: httplib's default adds SO_REUSEPORT, which would
        // let a second reforce bind the same port silently.
        server.set_socket_options([](socket_t sock) {
            int yes = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        });
        server.set_payload_max_length(64 * 1024);
        server.set_default_headers({{"X-Content-Type-Options", "nosniff"},
                                    {"Referrer-Policy", "no-referrer"},
                                    {"Cache-Control", "no-store"}});

        server.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            // Loopback-only server: refuse foreign Host headers (DNS rebinding).
            if (isLoopbackHost(opts.host) && !isLoopbackHost(hostOnly(req.get_header_value("Host")))) {
                jsonError(res, 403, "forbidden host");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.path.rfind("/api/", 0) == 0 && !authorized(req)) {
                jsonError(res, 401, "missing or wrong token");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

        server.Get("/api/favorites", [this](const httplib::Request&, httplib::Response& res) {
            std::lock_guard<std::mutex> lock(favMutex);
            res.set_content(favoritesToJson(favorites, bool(favSaver)), "application/json");
        });
        server.Post("/api/favorites", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.get_header_value("Content-Type").rfind("application/json", 0) != 0) {
                jsonError(res, 415, "Content-Type must be application/json");
                return;
            }
            const auto v = json::parse(req.body);
            const json::Value* layer = v ? v->get("layer") : nullptr;
            const json::Value* id = v ? v->get("id") : nullptr;
            const json::Value* on = v ? v->get("on") : nullptr;
            if (!layer || !layer->isString() || !id || !id->isString() || id->str.empty() || id->str.size() > 512 ||
                !on || !on->isBool() || id->str.find_first_of(";\n") != std::string::npos) {
                jsonError(res, 400, "expected {\"layer\":L,\"id\":ID,\"on\":true|false}");
                return;
            }
            const auto parsed = parseFavorites(layer->str + ":" + id->str);
            if (parsed.empty()) {
                jsonError(res, 400, "unknown layer \"" + layer->str + "\"");
                return;
            }
            std::lock_guard<std::mutex> lock(favMutex);
            const Favorite f = parsed.front();
            const auto it = std::find(favorites.begin(), favorites.end(), f);
            if (on->b && it == favorites.end()) favorites.push_back(f);
            if (!on->b && it != favorites.end()) favorites.erase(it);
            if (favSaver) {
                if (const std::string err = favSaver(joinFavorites(favorites)); !err.empty()) {
                    jsonError(res, 500, err);
                    return;
                }
            }
            res.set_content(favoritesToJson(favorites, bool(favSaver)), "application/json");
        });
        server.Get("/api/sprites/hidden", [this](const httplib::Request&, httplib::Response& res) {
            std::lock_guard<std::mutex> lock(hidMutex);
            res.set_content(hiddenJson(), "application/json");
        });
        server.Post("/api/sprites/hidden", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.get_header_value("Content-Type").rfind("application/json", 0) != 0) {
                jsonError(res, 415, "Content-Type must be application/json");
                return;
            }
            const auto v = json::parse(req.body);
            const json::Value* ids = v ? v->get("ids") : nullptr;
            const json::Value* on = v ? v->get("on") : nullptr;
            bool good = ids && ids->isArray() && !ids->array.empty() && ids->array.size() <= 4096 && on && on->isBool();
            if (good)
                for (const auto& id : ids->array)
                    good = good && id.isString() && !id.str.empty() && id.str.size() <= 512 &&
                           id.str.find_first_of(";\n") == std::string::npos;
            if (!good) {
                jsonError(res, 400, "expected {\"ids\":[ID, ...],\"on\":true|false}");
                return;
            }
            std::lock_guard<std::mutex> lock(hidMutex);
            std::vector<std::string> next = hidden;
            for (const auto& id : ids->array) {
                const bool has = engine::idListHas(next, id.str);
                if (on->b && !has) next.push_back(id.str);
                if (!on->b && has)
                    next.erase(std::remove_if(next.begin(), next.end(),
                                              [&](const std::string& x) { return engine::idListHas({x}, id.str); }),
                               next.end());
            }
            const std::string text = engine::joinIdList(next);
            if (hidSaver)
                if (const std::string err = hidSaver(text); !err.empty()) {
                    jsonError(res, 500, err);
                    return;
                }
            hidden = std::move(next);
            commands.push(engine::Command::spriteHidden(text));
            res.set_content(hiddenJson(), "application/json");
        });
        server.Get("/api/sprites/image", [this](const httplib::Request& req, httplib::Response& res) {
            // Only files the library lists: the id is a key, never a path.
            const auto it = spriteImages.find(lowerAscii(req.get_param_value("id")));
            if (it == spriteImages.end()) {
                jsonError(res, 404, "no image sprite with that id");
                return;
            }
            std::ifstream in(it->second, std::ios::binary);
            if (!in) {
                jsonError(res, 404, "cannot read " + it->second.filename().string());
                return;
            }
            std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const std::string ext = lowerAscii(it->second.extension().string());
            const char* type = ext == ".png"                     ? "image/png"
                               : ext == ".gif"                   ? "image/gif"
                               : ext == ".bmp"                   ? "image/bmp"
                               : ext == ".tif" || ext == ".tiff" ? "image/tiff"
                                                                 : "image/jpeg";
            res.headers.erase("Cache-Control");
            res.set_header("Cache-Control", "private, max-age=600");  // the grid scrolls back and forth
            res.set_content(std::move(body), type);
        });
        server.Get("/api/library", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(libraryJson, "application/json");
        });

        server.Get("/api/video", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(videoSourcesToJson(videoSources ? videoSources() : VideoSourceList{}), "application/json");
        });

        server.Get("/api/state", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(stateToJson(state()), "application/json");
        });

        server.Get("/api/events", [this](const httplib::Request& req, httplib::Response& res) {
            struct Conn {
                bool first = true;
                bool live = true;     // send `live` events between full states
                double period = 0.1;  // poll (and live event) period, s
                std::string lastStable, lastLive;
                Clock::time_point lastSend, lastWrite;
            };
            auto conn = std::make_shared<Conn>();
            conn->period = opts.ssePollSeconds;
            // ?live=HZ: the panel's choice of live update rate (1..30); 0 = no live events (full states only).
            if (req.has_param("live")) {
                const double hz = std::atof(req.get_param_value("live").c_str());
                if (hz <= 0) conn->live = false;
                else conn->period = 1.0 / std::min(30.0, std::max(1.0, hz));
            }
            res.set_header("X-Accel-Buffering", "no");
            res.set_chunked_content_provider(
                "text/event-stream", [this, conn](std::size_t, httplib::DataSink& sink) {
                    if (stopping) return false;
                    const auto now = Clock::now();
                    auto write = [&](const std::string& s) {
                        conn->lastWrite = now;
                        return sink.write(s.data(), s.size());
                    };
                    if (conn->first && !write("retry: 2000\n\n")) return false;
                    const engine::EngineState s = state();
                    std::string stable = stateToJson(s, true);
                    const double sinceSend = std::chrono::duration<double>(now - conn->lastSend).count();
                    if (conn->first || stable != conn->lastStable || sinceSend >= opts.sseRefreshSeconds) {
                        conn->first = false;
                        conn->lastStable = std::move(stable);
                        conn->lastSend = now;
                        if (!write("data: " + stateToJson(s) + "\n\n")) return false;
                        conn->lastLive.clear();
                    } else if (std::string live = conn->live ? liveToJson(s) : std::string();
                               conn->live && live != conn->lastLive) {
                        // Between full states: the small live part (meters, Link beat, progress), when it moved.
                        if (!write("event: live\ndata: " + live + "\n\n")) return false;
                        conn->lastLive = std::move(live);
                    } else if (std::chrono::duration<double>(now - conn->lastWrite).count() >=
                               opts.sseKeepaliveSeconds) {
                        if (!write(": keepalive\n\n")) return false;
                    }
                    // Throttle: at most one check (and push) per poll period.
                    const auto until = now + std::chrono::duration<double>(conn->period);
                    while (!stopping && Clock::now() < until) {
                        if (!sink.is_writable()) return false;
                        const auto left = std::chrono::duration_cast<Clock::duration>(until - Clock::now());
                        std::this_thread::sleep_for(std::min<Clock::duration>(std::chrono::milliseconds(20), left));
                    }
                    return !stopping.load();
                });
        });

        server.Post("/api/command", [this](const httplib::Request& req, httplib::Response& res) {
            const std::string ct = req.get_header_value("Content-Type");
            if (ct.rfind("application/json", 0) != 0) {
                jsonError(res, 415, "Content-Type must be application/json");
                return;
            }
            const CommandParseResult r = parseCommandJson(req.body, library.get());
            if (!r.ok()) {
                jsonError(res, r.status, r.error);
                return;
            }
            std::vector<engine::Command> queue;
            if (std::string err; !resolveLookSaves(r.commands, queue, &err).empty()) {
                jsonError(res, err == "501" ? 501 : 500, err == "501" ? "saving the Look needs the app's settings file" : err);
                return;
            }
            // Slideshow settings from the panel are saved as they change (file I/O here,
            // not on the render thread). A failed save still applies the change.
            std::string saveError;
            if (lookSaver) {
                engine::PrefsTable save;
                for (const auto& c : queue) {
                    const engine::PrefsTable t = engine::slideshowSaveEntries(c);  // keep alive while iterating
                    for (const auto& [k, e] : t.entries()) save.set(e.key, e.value, e.quoted);
                }
                if (!save.entries().empty()) saveError = lookSaver(save);
            }
            json::Writer w;
            w.beginObject().field("ok", true);
            if (!saveError.empty()) w.field("saveError", saveError);
            w.key("queued").beginArray();
            for (const auto& c : queue) {
                w.value(engine::describe(c));
                commands.push(c);
            }
            w.endArray().endObject();
            res.set_content(w.take(), "application/json");
        });

        auto serveAsset = [](const std::string& name, httplib::Response& res) {
            const UiAsset* a = findUiAsset(name);
            if (!a) {
                res.status = 404;
                res.set_content("not found", "text/plain");
                return;
            }
            res.set_header("Content-Security-Policy",
                           "default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; "
                           "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
            res.set_content(reinterpret_cast<const char*>(a->data), a->size, mimeType(name));
        };
        server.Get("/", [serveAsset](const httplib::Request&, httplib::Response& res) {
            serveAsset("index.html", res);
        });
        server.Get(R"(/([A-Za-z0-9._-]+))", [serveAsset](const httplib::Request& req, httplib::Response& res) {
            serveAsset(req.matches[1].str(), res);
        });
    }
};

WebServer::WebServer(WebServerOptions options, engine::CommandQueue& commands, StateProvider state,
                     std::shared_ptr<const Library> library)
    : impl_(std::make_unique<Impl>(std::move(options), commands, std::move(state), std::move(library))) {}

WebServer::~WebServer() { stop(); }

void WebServer::setVideoSources(std::function<VideoSourceList()> provider) { impl_->videoSources = std::move(provider); }
void WebServer::setLookSaver(std::function<std::string(const engine::PrefsTable&)> saver) {
    impl_->lookSaver = std::move(saver);
}

void WebServer::setFavorites(const std::string& initial, std::function<std::string(const std::string&)> saver) {
    std::lock_guard<std::mutex> lock(impl_->favMutex);
    impl_->favorites = parseFavorites(initial);
    impl_->favSaver = std::move(saver);
}

void WebServer::setSpriteHidden(const std::string& initial, std::function<std::string(const std::string&)> saver) {
    std::lock_guard<std::mutex> lock(impl_->hidMutex);
    impl_->hidden = engine::parseIdList(initial);
    impl_->hidSaver = std::move(saver);
}

bool WebServer::start(std::string* error) {
    if (impl_->started) return true;
    Impl& m = *impl_;
    if (m.opts.port == 0) {
        m.port = m.server.bind_to_any_port(m.opts.host);
        if (m.port <= 0) {
            if (error) *error = "cannot bind " + m.opts.host + " (any port)";
            return false;
        }
    } else {
        if (!m.server.bind_to_port(m.opts.host, m.opts.port)) {
            if (error) *error = "cannot bind " + m.opts.host + ":" + std::to_string(m.opts.port) + " (in use?)";
            return false;
        }
        m.port = m.opts.port;
    }
    m.stopping = false;
    m.started = true;
    m.thread = std::thread([&m] {
        if (!m.server.listen_after_bind() && !m.stopping)
            std::fprintf(stderr, "[reforce] web UI server stopped unexpectedly\n");
    });
    m.server.wait_until_ready();
    return true;
}

void WebServer::stop() {
    if (!impl_ || !impl_->started) return;
    impl_->stopping = true;
    impl_->server.stop();
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->started = false;
}

bool WebServer::running() const { return impl_->started && impl_->server.is_running(); }
int WebServer::port() const { return impl_->port; }
const WebServerOptions& WebServer::options() const { return impl_->opts; }

std::string generateToken() {
    unsigned char bytes[16];
    arc4random_buf(bytes, sizeof bytes);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char b : bytes) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 15]);
    }
    return out;
}

std::vector<std::string> lanAddresses() {
    std::vector<std::string> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if ((a->ifa_flags & IFF_UP) == 0 || (a->ifa_flags & IFF_LOOPBACK) != 0) continue;
        char buf[INET_ADDRSTRLEN];
        const auto* sin = reinterpret_cast<const sockaddr_in*>(a->ifa_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf)) out.emplace_back(buf);
    }
    freeifaddrs(list);
    return out;
}

}  // namespace reforce::web

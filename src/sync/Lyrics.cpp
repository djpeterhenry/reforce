#include "sync/Lyrics.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <variant>

namespace reforce::sync {

namespace {

// ---------------------------------------------------------------- a small JSON reader (the two lyrics files)

struct Json;
using JsonArray = std::vector<Json>;
using JsonObject = std::vector<std::pair<std::string, Json>>;
struct Json {
    std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject> v;
    const Json* get(std::string_view key) const {
        if (const auto* o = std::get_if<JsonObject>(&v))
            for (const auto& [k, val] : *o)
                if (k == key) return &val;
        return nullptr;
    }
    double num(std::string_view key, double def = 0) const {
        const Json* j = get(key);
        const double* d = j ? std::get_if<double>(&j->v) : nullptr;
        return d && std::isfinite(*d) ? *d : def;
    }
    std::string str(std::string_view key) const {
        const Json* j = get(key);
        const std::string* s = j ? std::get_if<std::string>(&j->v) : nullptr;
        return s ? *s : std::string();
    }
    bool flag(std::string_view key) const {
        const Json* j = get(key);
        const bool* b = j ? std::get_if<bool>(&j->v) : nullptr;
        return b && *b;
    }
    const JsonArray* arr(std::string_view key) const {
        const Json* j = get(key);
        return j ? std::get_if<JsonArray>(&j->v) : nullptr;
    }
};

struct Reader {
    std::string_view s;
    std::size_t i = 0;
    std::string err;
    int depth = 0;

    bool fail(const char* m) {
        if (err.empty()) err = std::string(m) + " at offset " + std::to_string(i);
        return false;
    }
    void ws() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }
    bool hex4(unsigned& cp) {
        if (i + 4 > s.size()) return fail("bad \\u escape");
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= unsigned(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected string");
        ++i;
        while (i < s.size() && s[i] != '"') {
            const char c = s[i++];
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i >= s.size()) return fail("bad escape");
            const char e = s[i++];
            switch (e) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    // A surrogate pair (emoji and the like): two escapes, one code point.
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        unsigned lo = 0;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: out.push_back(e); break;  // \" \\ \/
            }
        }
        if (i >= s.size()) return fail("unterminated string");
        ++i;
        return true;
    }
    bool value(Json& out) {
        if (++depth > 64) return fail("nested too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        const char c = s[i];
        bool ok = true;
        if (c == '{') {
            ++i;
            JsonObject o;
            ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
            } else {
                for (;;) {
                    ws();
                    std::string k;
                    if (!str(k)) return false;
                    ws();
                    if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                    ++i;
                    Json v;
                    if (!value(v)) return false;
                    o.emplace_back(std::move(k), std::move(v));
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; break; }
                    return fail("expected ',' or '}'");
                }
            }
            out.v = std::move(o);
        } else if (c == '[') {
            ++i;
            JsonArray a;
            ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
            } else {
                for (;;) {
                    Json v;
                    if (!value(v)) return false;
                    a.push_back(std::move(v));
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; break; }
                    return fail("expected ',' or ']'");
                }
            }
            out.v = std::move(a);
        } else if (c == '"') {
            std::string t;
            ok = str(t);
            out.v = std::move(t);
        } else if (s.compare(i, 4, "true") == 0) {
            i += 4;
            out.v = true;
        } else if (s.compare(i, 5, "false") == 0) {
            i += 5;
            out.v = false;
        } else if (s.compare(i, 4, "null") == 0) {
            i += 4;
            out.v = nullptr;
        } else if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t j = i;
            while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '-' || s[j] == '+' ||
                                    s[j] == '.' || s[j] == 'e' || s[j] == 'E'))
                ++j;
            const std::string num(s.substr(i, j - i));
            char* end = nullptr;
            const double d = std::strtod(num.c_str(), &end);
            if (!end || *end) return fail("bad number");
            out.v = d;
            i = j;
        } else {
            return fail("unexpected character");
        }
        --depth;
        return ok;
    }
};

std::optional<Json> parse(std::string_view text, std::string* error) {
    Reader r{text, 0, {}, 0};
    Json j;
    if (!r.value(j)) {
        if (error) *error = r.err;
        return std::nullopt;
    }
    r.ws();
    if (r.i != text.size()) {
        if (error) *error = "trailing characters at offset " + std::to_string(r.i);
        return std::nullopt;
    }
    return j;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return bool(in) || in.eof();
}

// Modification time in ns (-1: no such file).
std::int64_t stamp(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return -1;
    return std::int64_t(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
}

double smoothstep(double a, double b, double x) {
    if (b <= a) return x >= b ? 1.0 : 0.0;
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

}  // namespace

// ---------------------------------------------------------------- files

std::string LyricLine::text() const {
    std::string s;
    for (const LyricWord& w : words) {
        if (!s.empty()) s.push_back(' ');
        s += w.text;
    }
    return s;
}

std::optional<LyricsSong> parseLyricsSidecar(std::string_view json, std::string* error) {
    auto j = parse(json, error);
    if (!j) return std::nullopt;
    if (!std::holds_alternative<JsonObject>(j->v)) {
        if (error) *error = "not a JSON object";
        return std::nullopt;
    }
    LyricsSong song;
    song.artist = j->str("artist");
    song.title = j->str("title");
    song.offset = j->num("offset");
    song.reviewed = j->flag("reviewed");
    if (const JsonArray* lines = j->arr("lines")) {
        for (const Json& l : *lines) {
            LyricLine line;
            if (const JsonArray* words = l.arr("words")) {
                for (const Json& w : *words) {
                    LyricWord word;
                    word.t0 = w.num("t0", NAN);
                    word.t1 = w.num("t1", NAN);
                    word.text = w.str("text");
                    // Words are laid out one space apart: a word must not hold one itself.
                    std::replace_if(word.text.begin(), word.text.end(),
                                    [](char c) { return std::isspace(static_cast<unsigned char>(c)); }, '_');
                    if (word.text.empty() || !std::isfinite(word.t0) || !std::isfinite(word.t1)) continue;
                    word.t1 = std::max(word.t1, word.t0);
                    line.words.push_back(std::move(word));
                }
            }
            if (line.words.empty()) continue;
            std::stable_sort(line.words.begin(), line.words.end(),
                             [](const LyricWord& a, const LyricWord& b) { return a.t0 < b.t0; });
            line.t0 = line.words.front().t0;
            line.t1 = 0;
            for (const LyricWord& w : line.words) line.t1 = std::max(line.t1, w.t1);
            song.lines.push_back(std::move(line));
        }
    }
    std::stable_sort(song.lines.begin(), song.lines.end(),
                     [](const LyricLine& a, const LyricLine& b) { return a.t0 < b.t0; });
    return song;
}

std::optional<std::vector<LyricsIndexEntry>> parseLyricsIndex(std::string_view json, std::string* error) {
    auto j = parse(json, error);
    if (!j) return std::nullopt;
    const JsonArray* songs = j->arr("songs");
    if (!songs) {
        if (error) *error = "no \"songs\" list";
        return std::nullopt;
    }
    std::vector<LyricsIndexEntry> out;
    for (const Json& s : *songs) {
        LyricsIndexEntry e;
        e.artist = s.str("artist");
        e.title = s.str("title");
        e.file = s.str("file");
        e.reviewed = s.flag("reviewed");
        // Only plain names inside the folder.
        if (e.file.empty() || e.file.find('/') != std::string::npos || e.file.rfind("..", 0) == 0) continue;
        out.push_back(std::move(e));
    }
    return out;
}

std::string lyricsKey(const std::string& artist, const std::string& title) {
    auto norm = [](const std::string& s) {
        std::string out;
        bool space = false;
        for (char c : s) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                space = !out.empty();
                continue;
            }
            if (space) out.push_back(' ');
            space = false;
            out.push_back(char(std::tolower(static_cast<unsigned char>(c))));
        }
        return out;
    };
    return norm(artist) + "\t" + norm(title);
}

// ---------------------------------------------------------------- DeckClock

void DeckClock::frame(double now, const DeckFrame& f) {
    if (!std::isfinite(f.position)) {
        reset();
        return;
    }
    const double rate = f.bpm > 0 && f.originalBpm > 0 ? f.bpm / f.originalBpm : 1.0;
    if (!have_) {
        have_ = true;
        anchorPos_ = lastPos_ = f.position;
        anchorNow_ = lastChange_ = now;
        rate_ = 0;  // stopped until the position moves
        return;
    }
    if (std::abs(f.position - lastPos_) > 1e-4) {
        const double est = position(now);
        const double err = f.position - est;
        // Started, or a jump (cue, loop, beat jump, scratch, a reverse): take the reading as it is.
        const bool snap = rate_ == 0 || std::abs(err) > jumpSeconds;
        anchorPos_ = snap ? f.position : est + ease * err;
        anchorNow_ = now;
        rate_ = rate;
        lastPos_ = f.position;
        lastChange_ = now;
    } else if (rate_ != 0 && now - lastChange_ > stopSeconds) {
        rate_ = 0;
        anchorPos_ = lastPos_;
        anchorNow_ = now;
    }
}

// ---------------------------------------------------------------- KaraokePicker

int KaraokePicker::update(double now, const bool hasLyrics[kDecks], const double level[kDecks],
                          const bool playing[kDecks], int masterDeck, bool fadersSeen) {
    auto heard = [&](int i) {
        if (!hasLyrics[i] || !playing[i]) return false;
        return fadersSeen ? level[i] >= cfg_.minLevel : i == masterDeck;
    };
    int best = -1;
    for (int i = 0; i < kDecks; ++i)
        if (heard(i) && (best < 0 || level[i] > level[best])) best = i;
    if (deck_ >= 0 && !heard(deck_)) deck_ = -1;
    if (deck_ < 0) {
        deck_ = best;
        leader_ = -1;
        return deck_;
    }
    // Ties keep the current deck; a louder one leads for switchSeconds first.
    if (best >= 0 && best != deck_ && level[best] > level[deck_]) {
        if (leader_ != best) {
            leader_ = best;
            leaderSince_ = now;
        }
        if (now - leaderSince_ >= cfg_.switchSeconds) {
            deck_ = best;
            leader_ = -1;
        }
    } else {
        leader_ = -1;
    }
    return deck_;
}

// ---------------------------------------------------------------- what is on screen

std::vector<KaraokeShown> karaokeLines(const LyricsSong& song, double t, const KaraokeConfig& cfg) {
    std::vector<KaraokeShown> out;
    const auto& L = song.lines;
    const int n = int(L.size());
    if (n == 0) return out;
    // appear(i): `preview` before its first word, not before line i - 2 (its row) has lingered.
    auto appear = [&](int i) {
        double a = L[std::size_t(i)].t0 - cfg.preview;
        if (i >= 2) a = std::max(a, L[std::size_t(i - 2)].t1 + cfg.linger);
        return a;
    };
    // vanish(i): `hold` after its last word, or as line i + 2 takes the row.
    auto vanish = [&](int i) {
        double v = L[std::size_t(i)].t1 + cfg.hold;
        if (i + 2 < n) v = std::min(v, appear(i + 2));
        return v;
    };
    // Lines ending before t - hold - fade are gone; start the look a little before the first that may show.
    auto first = std::lower_bound(L.begin(), L.end(), t - cfg.hold - cfg.fade - 1e-9,
                                  [](const LyricLine& l, double x) { return l.t1 < x; });
    for (int i = std::max(0, int(first - L.begin()) - 2); i < n; ++i) {
        if (L[std::size_t(i)].t0 - cfg.preview > t) break;  // this and every later line: not yet
        const double a = appear(i);
        if (a > t) continue;
        const double v = vanish(i);
        if (t >= v + cfg.fade) continue;
        const double op = smoothstep(a, a + cfg.fade, t) * (1.0 - smoothstep(v, v + cfg.fade, t));
        if (op <= 0.001) continue;
        out.push_back({i, i % 2, op});
    }
    return out;
}

double karaokeFillX(const LyricLine& line, double t, const std::vector<float>& spans) {
    const std::size_t n = std::min(line.words.size(), spans.size() / 2);
    if (n == 0) return 0;
    double x = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const LyricWord& w = line.words[k];
        const double x0 = spans[2 * k], x1 = spans[2 * k + 1];
        if (t < w.t0) break;
        if (t >= w.t1) {
            x = x1;
            continue;
        }
        const double p = w.t1 > w.t0 ? (t - w.t0) / (w.t1 - w.t0) : 1.0;
        x = x0 + (x1 - x0) * std::clamp(p, 0.0, 1.0);
        break;
    }
    return x;
}

// ---------------------------------------------------------------- LyricsLibrary

LyricsLibrary::LyricsLibrary(std::string folder, bool worker) : folder_(std::move(folder)) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_ = folder_.empty() ? "no lyrics folder set" : "looking in " + folder_;
    }
    if (worker && !folder_.empty()) thread_ = std::thread([this] { loop(); });
}

LyricsLibrary::~LyricsLibrary() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void LyricsLibrary::loop() {
    while (!stop_.load()) {
        scan();
        for (int k = 0; k < 10 && !stop_.load(); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void LyricsLibrary::scan() {
    if (folder_.empty()) return;
    const std::string indexPath = folder_ + "/index.json";
    const std::int64_t st = stamp(indexPath);
    if (st == indexStamp_) return;
    indexStamp_ = st;
    auto publish = [&](std::map<std::string, std::shared_ptr<const LyricsSong>> songs, std::string status) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            songs_ = std::move(songs);
            status_ = std::move(status);
        }
        version_.fetch_add(1);
    };
    if (st < 0) {
        files_.clear();
        publish({}, "no lyrics yet (" + indexPath + " not there)");
        return;
    }
    std::string text, err;
    if (!readFile(indexPath, text)) {
        indexStamp_ = -2;  // try again next time
        return;
    }
    auto index = parseLyricsIndex(text, &err);
    if (!index) {
        // Half written? djtools replaces it atomically, so this is a real problem; look again next time.
        indexStamp_ = -2;
        publish({}, "index.json: " + err);
        return;
    }
    std::map<std::string, std::pair<std::int64_t, std::shared_ptr<const LyricsSong>>> files;
    std::map<std::string, std::shared_ptr<const LyricsSong>> songs;
    int bad = 0;
    for (const LyricsIndexEntry& e : *index) {
        const std::string path = folder_ + "/" + e.file;
        const std::int64_t fs = stamp(path);
        std::shared_ptr<const LyricsSong> song;
        auto old = files_.find(e.file);
        if (old != files_.end() && old->second.first == fs) {
            song = old->second.second;
        } else if (fs >= 0) {
            std::string body, perr;
            if (readFile(path, body))
                if (auto parsed = parseLyricsSidecar(body, &perr))
                    song = std::make_shared<const LyricsSong>(std::move(*parsed));
        }
        if (!song) {
            ++bad;
            continue;
        }
        files[e.file] = {fs, song};
        songs[lyricsKey(e.artist, e.title)] = song;
    }
    files_ = std::move(files);
    std::string status = std::to_string(songs.size()) + (songs.size() == 1 ? " song" : " songs") + " in " + folder_;
    if (bad) status += " (" + std::to_string(bad) + " unreadable)";
    publish(std::move(songs), std::move(status));
}

std::shared_ptr<const LyricsSong> LyricsLibrary::find(const std::string& artist, const std::string& title) const {
    if (title.empty()) return nullptr;
    const std::string key = lyricsKey(artist, title);
    std::lock_guard<std::mutex> lock(mu_);
    auto it = songs_.find(key);
    return it == songs_.end() ? nullptr : it->second;
}

std::string LyricsLibrary::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

}  // namespace reforce::sync

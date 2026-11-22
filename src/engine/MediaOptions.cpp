#include "engine/MediaOptions.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace reforce::engine {

namespace {

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::optional<double> number(const std::string& v);

// Seconds, or m:ss(.s) / h:mm:ss.
std::optional<double> seconds(const std::string& v) {
    if (v.find(':') == std::string::npos) return number(v);
    double total = 0;
    std::size_t pos = 0;
    while (true) {
        const auto colon = v.find(':', pos);
        const auto part = number(v.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos));
        if (!part || *part < 0) return std::nullopt;
        total = total * 60 + *part;
        if (colon == std::string::npos) break;
        pos = colon + 1;
    }
    return total;
}

std::optional<double> number(const std::string& v) {
    if (v.empty()) return std::nullopt;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0') return std::nullopt;
    return d;
}

}  // namespace

namespace {
int rifeFactor(const std::string& stem);
}

std::filesystem::path mediaOptionsPath(const std::filesystem::path& movie) {
    std::string stem = movie.stem().string();
    if (rifeFactor(stem) > 0) stem.resize(stem.rfind('.'));  // an interpolated copy shares its original's options
    return movie.parent_path() / (stem + ".reforce.txt");
}

MediaOptions parseMediaOptions(std::string_view text) {
    MediaOptions o;
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        if (const auto c = line.find("//"); c != std::string::npos) line.resize(c);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(std::string_view(line).substr(0, eq)));
        std::string val = trim(std::string_view(line).substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = trim(val.substr(1, val.size() - 2));
        const std::string lv = lower(val);
        const bool on = lv == "1" || lv == "on" || lv == "yes" || lv == "true";
        const bool off = lv == "0" || lv == "off" || lv == "no" || lv == "false";
        if (key == "loop") {
            if (on || off) o.loop = on;
        } else if (key == "colorkey" || key == "colourkey") {
            if (on || off) o.colorKey = on;
        } else if (key == "rate") {
            if (auto d = number(val); d && *d >= 0) o.rate = *d;
        } else if (key == "start") {
            if (auto d = seconds(val); d && *d >= 0) o.start = *d;
        } else if (key == "end") {
            if (auto d = seconds(val); d && *d > 0) o.end = *d;
        } else if (key == "bpm") {
            if (auto d = number(val); d && *d > 0 && *d < 1000) o.bpm = *d;
        } else if (key == "beatstart") {
            if (auto d = seconds(val); d && *d >= 0) o.beatStart = *d;
        } else if (key == "beatsperbar") {
            if (auto d = number(val); d && *d >= 1 && *d <= 32) o.beatsPerBar = int(*d);
        } else if (key == "black") {
            if (auto d = number(val); d && *d >= 0 && *d <= 1) o.black = *d;
        } else if (key == "white") {
            if (auto d = number(val); d && *d >= 0 && *d <= 1) o.white = *d;
        } else if (key == "gamma") {
            if (auto d = number(val); d && *d > 0 && *d <= 10) o.gamma = *d;
        }
    }
    if (o.end && *o.end <= o.start.value_or(0.0)) o.end.reset();
    if (o.white.value_or(1.0) <= o.black.value_or(0.0)) {  // White must be above Black: ignore both
        o.black.reset();
        o.white.reset();
    }
    return o;
}

MediaOptions loadMediaOptions(const std::filesystem::path& movie) {
    std::ifstream f(mediaOptionsPath(movie));
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return parseMediaOptions(ss.str());
}

namespace {
// ".rife<digits>x" at the end of a stem; returns the factor (0 = not one).
int rifeFactor(const std::string& stem) {
    const std::string l = lower(stem);
    const auto dot = l.rfind(".rife");
    if (dot == std::string::npos || l.size() < dot + 7 || l.back() != 'x') return 0;
    int n = 0;
    for (std::size_t i = dot + 5; i + 1 < l.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(l[i]))) return 0;
        n = n * 10 + (l[i] - '0');
        if (n > 1000) return 0;
    }
    return n;
}
}  // namespace

bool isInterpolatedMovieName(const std::filesystem::path& p) { return rifeFactor(p.stem().string()) > 0; }

std::filesystem::path interpolatedMovie(const std::filesystem::path& movie) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string stem = movie.stem().string();
    const fs::path dir = movie.has_parent_path() ? movie.parent_path() : fs::path(".");
    fs::path best = movie;
    int bestN = 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        const std::string s = p.stem().string();
        if (s.size() <= stem.size() || s.compare(0, stem.size(), stem) != 0) continue;
        const int n = rifeFactor(s);
        // exactly "<stem>.rife<N>x"
        if (n > bestN && s.size() == stem.size() + 6 + std::to_string(n).size() && it->is_regular_file(ec)) {
            best = p;
            bestN = n;
        }
    }
    return best;
}

}  // namespace reforce::engine

// Pure parts of the video module (no Apple frameworks): source specs, media
// time, Syphon self-subscription guard, Card frame decoding.
#include <algorithm>
#include <cctype>
#include <cmath>

#include "engine/MediaOptions.hpp"
#include "engine/Sprites.hpp"
#include "video/VideoSource.hpp"

namespace reforce::video {
namespace fs = std::filesystem;

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
bool startsWithNoCase(const std::string& s, const char* prefix) {
    const std::string p = prefix;
    return s.size() >= p.size() && lower(s.substr(0, p.size())) == p;
}
}  // namespace

const char* sourceKindName(SourceKind k) {
    switch (k) {
        case SourceKind::File: return "file";
        case SourceKind::Camera: return "camera";
        case SourceKind::Syphon: return "syphon";
    }
    return "?";
}

std::optional<SourceSpec> parseSourceSpec(const std::string& textIn, std::string* error) {
    const std::string text = trim(textIn);
    SourceSpec s;
    if (lower(text) == "camera") {
        s.kind = SourceKind::Camera;
        s.id = "default";
        return s;
    }
    if (startsWithNoCase(text, "camera:")) {
        s.kind = SourceKind::Camera;
        s.id = trim(text.substr(7));
        if (s.id.empty()) s.id = "default";
        return s;
    }
    if (startsWithNoCase(text, "syphon:")) {
        s.kind = SourceKind::Syphon;
        s.id = trim(text.substr(7));
        if (s.id.empty()) {
            if (error) *error = "syphon: needs a server name";
            return std::nullopt;
        }
        return s;
    }
    s.kind = SourceKind::File;
    s.id = startsWithNoCase(text, "file:") ? trim(text.substr(5)) : text;
    if (s.id.empty()) {
        if (error) *error = "empty video source";
        return std::nullopt;
    }
    return s;
}

std::string specToString(const SourceSpec& s) { return std::string(sourceKindName(s.kind)) + ":" + s.id; }

std::string specLabel(const SourceSpec& s) {
    switch (s.kind) {
        case SourceKind::File: {
            std::string stem = fs::path(s.id).stem().string();
            if (engine::isInterpolatedMovieName(s.id)) stem.resize(stem.rfind('.'));  // "<stem>.rife4x"
            return stem;
        }
        case SourceKind::Camera: return s.id == "default" ? std::string("Camera") : "Camera " + s.id;
        case SourceKind::Syphon: return "Syphon " + s.id;
    }
    return s.id;
}

bool isVideoFile(const fs::path& p) { return engine::isSpriteVideoFile(p); }

namespace {
double segmentEnd(double offset, double duration, double end) {
    if (end > offset && (duration <= 0 || end < duration)) return end;
    return duration;
}
}  // namespace

double mediaTime(double t, double offset, double rate, double duration, bool loop, double end) {
    offset = std::max(0.0, offset);
    double m = offset + std::max(0.0, t) * std::max(0.0, rate);
    const double b = segmentEnd(offset, duration, end);
    if (b > 0) {
        if (loop && b > offset) m = offset + std::fmod(m - offset, b - offset);
        else if (loop) m = std::fmod(m, b);  // offset past the end: whole-movie wrap
        else m = std::min(m, b);
    }
    return m;
}

double sectionEnd(const SourceSpec& spec, double duration) {
    const double a = std::max(0.0, spec.offset);
    double b = segmentEnd(a, duration, spec.end);
    if (spec.loopUnit > 0 && b > a) {  // whole units, like the decoder (VideoSourceMac.mm)
        // A section within 2% of a unit of a whole number (a seamless loop of N bars, give or take a frame)
        // plays whole rather than losing nearly a full unit.
        const double units = std::floor((b - a) / spec.loopUnit + 0.02);
        if (units >= 1) b = std::min(b, a + units * spec.loopUnit);
    }
    return b;
}

double playLength(const SourceSpec& spec, double duration) {
    const double b = sectionEnd(spec, duration);
    const double a = std::max(0.0, spec.offset);
    if (!(b > a) || !(spec.rate > 0)) return 0;
    return (b - a) / spec.rate;
}

bool isSelfSyphonServer(const SyphonServerInfo& s, const std::string& ownName, const std::string& ownApp,
                        const std::string& ownUuid) {
    if (!ownUuid.empty() && s.uuid == ownUuid) return true;
    return !ownApp.empty() && lower(s.app) == lower(ownApp) && lower(s.name) == lower(ownName);
}

std::string syphonDisplayName(const SyphonServerInfo& s) {
    if (s.app.empty()) return s.name;
    if (s.name.empty()) return s.app;
    return s.app + " - " + s.name;
}

bool syphonMatches(const SyphonServerInfo& s, const std::string& wantIn) {
    const std::string want = lower(trim(wantIn));
    if (want.empty()) return false;
    return want == lower(s.uuid) || want == lower(syphonDisplayName(s)) || want == lower(s.name) ||
           (s.name.empty() && want == lower(s.app));
}

int decodeCardIndex(const std::uint8_t* bgra, int width, int height, std::size_t bpr) {
    if (!bgra || width < 8 || height < 4) return -1;
    int v = 0;
    const int y = height / 8;  // middle of the top band
    for (int k = 0; k < 8; ++k) {
        const int x = (2 * k + 1) * width / 16;  // block center
        const std::uint8_t* p = bgra + std::size_t(y) * bpr + std::size_t(x) * 4;
        const int lum = (int(p[0]) + int(p[1]) + int(p[2])) / 3;
        if (lum >= 128) v |= 1 << (7 - k);
    }
    return v;
}

}  // namespace reforce::video

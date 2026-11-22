#include "engine/VideoComposite.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace reforce::engine {

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

bool parseNumber(const std::string& s, double& out) {
    const std::string t = trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}

bool parseBool(const std::string& s, bool& out) {
    const std::string t = lower(trim(s));
    if (t == "1" || t == "on" || t == "true" || t == "yes") return out = true, true;
    if (t == "0" || t == "off" || t == "false" || t == "no") return out = false, true;
    return false;
}

float smooth(float e0, float e1, float x) {
    if (e1 <= e0) return x >= e0 ? 1.0f : 0.0f;
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float luma601(const Rgb& c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; }
std::array<float, 2> cbcr(const Rgb& c) {
    return {-0.168736f * c[0] - 0.331264f * c[1] + 0.5f * c[2], 0.5f * c[0] - 0.418688f * c[1] - 0.081312f * c[2]};
}

const char* const kKeys[] = {"opacity", "blend",    "fit",  "order", "key",    "keycolor", "tolerance",
                             "softness", "threshold", "crossfade", "loop", "rate", "offset", "syphon", nullptr};

}  // namespace

const char* videoBlendName(VideoBlend b) {
    switch (b) {
        case VideoBlend::Normal: return "normal";
        case VideoBlend::Add: return "add";
        case VideoBlend::Screen: return "screen";
        case VideoBlend::Multiply: return "multiply";
    }
    return "?";
}
const char* videoFitName(VideoFit f) {
    switch (f) {
        case VideoFit::Cover: return "cover";
        case VideoFit::Contain: return "contain";
        case VideoFit::Stretch: return "stretch";
    }
    return "?";
}
const char* videoOrderName(VideoOrder o) { return o == VideoOrder::Under ? "under" : "over"; }
const char* videoKeyName(VideoKey k) {
    switch (k) {
        case VideoKey::None: return "none";
        case VideoKey::Luma: return "luma";
        case VideoKey::Chroma: return "chroma";
    }
    return "?";
}

std::optional<VideoBlend> parseVideoBlend(const std::string& s) {
    const std::string t = lower(trim(s));
    for (VideoBlend b : {VideoBlend::Normal, VideoBlend::Add, VideoBlend::Screen, VideoBlend::Multiply})
        if (t == videoBlendName(b)) return b;
    if (t == "over" || t == "alpha") return VideoBlend::Normal;
    if (t == "additive") return VideoBlend::Add;
    return std::nullopt;
}
std::optional<VideoFit> parseVideoFit(const std::string& s) {
    const std::string t = lower(trim(s));
    for (VideoFit f : {VideoFit::Cover, VideoFit::Contain, VideoFit::Stretch})
        if (t == videoFitName(f)) return f;
    if (t == "fill") return VideoFit::Cover;
    if (t == "fit") return VideoFit::Contain;
    return std::nullopt;
}
std::optional<VideoOrder> parseVideoOrder(const std::string& s) {
    const std::string t = lower(trim(s));
    if (t == "over" || t == "above" || t == "top") return VideoOrder::Over;
    if (t == "under" || t == "below" || t == "bottom") return VideoOrder::Under;
    return std::nullopt;
}
std::optional<VideoKey> parseVideoKey(const std::string& s) {
    const std::string t = lower(trim(s));
    for (VideoKey k : {VideoKey::None, VideoKey::Luma, VideoKey::Chroma})
        if (t == videoKeyName(k)) return k;
    if (t == "off" || t.empty()) return VideoKey::None;
    if (t == "green" || t == "chroma-key") return VideoKey::Chroma;
    return std::nullopt;
}

std::optional<Rgb> parseColor(const std::string& s) {
    std::string t = trim(s);
    if (!t.empty() && t[0] == '#') t = t.substr(1);
    if (t.size() == 6 && std::all_of(t.begin(), t.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
        const long v = std::strtol(t.c_str(), nullptr, 16);
        return Rgb{float((v >> 16) & 255) / 255.0f, float((v >> 8) & 255) / 255.0f, float(v & 255) / 255.0f};
    }
    double c[3];
    std::size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        const std::size_t comma = t.find(',', start);
        if ((i < 2) != (comma != std::string::npos)) return std::nullopt;
        if (!parseNumber(t.substr(start, i < 2 ? comma - start : std::string::npos), c[i]) || c[i] < 0)
            return std::nullopt;
        start = comma + 1;
    }
    const double k = (c[0] > 1 || c[1] > 1 || c[2] > 1) ? 255.0 : 1.0;
    Rgb out;
    for (int i = 0; i < 3; ++i) out[std::size_t(i)] = float(std::clamp(c[i] / k, 0.0, 1.0));
    return out;
}

std::string colorToHex(const Rgb& c) {
    char buf[8];
    auto b = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", b(c[0]), b(c[1]), b(c[2]));
    return buf;
}

const char* const* videoSettingKeys() { return kKeys; }

std::string applyVideoSetting(VideoLayerParams& p, const std::string& keyIn, const std::string& value) {
    const std::string key = lower(trim(keyIn));
    double num = 0;
    bool flag = false;
    auto number = [&](double lo, double hi) -> bool { return parseNumber(value, num) && num >= lo && num <= hi; };
    if (key == "opacity") {
        if (!number(0, 1)) return "opacity: 0..1";
        p.opacity = float(num);
    } else if (key == "blend") {
        auto b = parseVideoBlend(value);
        if (!b) return "blend: normal | add | screen | multiply";
        p.blend = *b;
    } else if (key == "fit") {
        auto f = parseVideoFit(value);
        if (!f) return "fit: cover | contain | stretch";
        p.fit = *f;
    } else if (key == "order") {
        auto o = parseVideoOrder(value);
        if (!o) return "order: over | under";
        p.order = *o;
    } else if (key == "key") {
        auto k = parseVideoKey(value);
        if (!k) return "key: none | luma | chroma";
        p.key = *k;
    } else if (key == "keycolor" || key == "keycolour") {
        auto c = parseColor(value);
        if (!c) return "keycolor: #rrggbb or r,g,b";
        p.keyColor = *c;
    } else if (key == "tolerance") {
        if (!number(0, 1)) return "tolerance: 0..1";
        p.tolerance = float(num);
    } else if (key == "softness") {
        if (!number(0, 1)) return "softness: 0..1";
        p.softness = float(num);
    } else if (key == "threshold") {
        if (!number(0, 1)) return "threshold: 0..1";
        p.lumaThreshold = float(num);
    } else if (key == "crossfade") {
        if (!number(0, 600)) return "crossfade: 0..600 s";
        p.crossfade = num;
    } else if (key == "loop") {
        if (!parseBool(value, flag)) return "loop: on | off";
        p.loop = flag;
    } else if (key == "rate") {
        if (!number(0, 16)) return "rate: 0..16";
        p.rate = num;
    } else if (key == "offset") {
        if (!number(0, 24 * 3600)) return "offset: seconds >= 0";
        p.offset = num;
    } else if (key == "syphon") {
        if (!parseBool(value, flag)) return "syphon: on | off";
        p.toSyphon = flag;
    } else {
        return "unknown video setting \"" + keyIn + "\"";
    }
    return {};
}

UvTransform fitTransform(double videoW, double videoH, double frameW, double frameH, VideoFit fit) {
    UvTransform t;
    if (fit == VideoFit::Stretch || videoW <= 0 || videoH <= 0 || frameW <= 0 || frameH <= 0) return t;
    const double sx = frameW / videoW, sy = frameH / videoH;
    const double k = fit == VideoFit::Cover ? std::max(sx, sy) : std::min(sx, sy);
    // Fraction of the frame the scaled video spans, per axis.
    const double rw = videoW * k / frameW, rh = videoH * k / frameH;
    t.scale[0] = float(1.0 / rw);
    t.scale[1] = float(1.0 / rh);
    t.offset[0] = float(0.5 - 0.5 / rw);
    t.offset[1] = float(0.5 - 0.5 / rh);
    return t;
}

float lumaKeyAlpha(const Rgb& c, float threshold, float softness) {
    return smooth(threshold, threshold + std::max(0.0f, softness), luma601(c));
}

float chromaKeyAlpha(const Rgb& c, const Rgb& key, float tolerance, float softness) {
    const auto a = cbcr(c), k = cbcr(key);
    const float d = std::hypot(a[0] - k[0], a[1] - k[1]);
    return smooth(tolerance, tolerance + std::max(0.0f, softness), d);
}

float keyAlpha(const Rgb& c, const VideoLayerParams& p) {
    switch (p.key) {
        case VideoKey::Luma: return lumaKeyAlpha(c, p.lumaThreshold, p.softness);
        case VideoKey::Chroma: return chromaKeyAlpha(c, p.keyColor, p.tolerance, p.softness);
        default: return 1.0f;
    }
}

VideoSample crossfadeSamples(const VideoSample& a, const VideoSample& b, float w) {
    w = std::clamp(w, 0.0f, 1.0f);
    VideoSample s;
    s.a = a.a + (b.a - a.a) * w;
    for (std::size_t i = 0; i < 3; ++i) {
        const float pm = a.c[i] * a.a + (b.c[i] * b.a - a.c[i] * a.a) * w;
        s.c[i] = s.a > 1e-6f ? std::clamp(pm / s.a, 0.0f, 1.0f) : 0.0f;
    }
    return s;
}

namespace {
float blendChannel(VideoBlend b, float base, float top) {
    switch (b) {
        case VideoBlend::Add: return std::min(1.0f, base + top);
        case VideoBlend::Screen: return 1.0f - (1.0f - base) * (1.0f - top);
        case VideoBlend::Multiply: return base * top;
        default: return top;
    }
}
}  // namespace

Rgb compositeVideo(const Rgb& v, const VideoSample& s, const VideoLayerParams& p) {
    Rgb out;
    const float k = std::clamp(p.opacity, 0.0f, 1.0f) * std::clamp(s.a, 0.0f, 1.0f);
    if (p.order == VideoOrder::Over) {
        for (std::size_t i = 0; i < 3; ++i) out[i] = v[i] + (blendChannel(p.blend, v[i], s.c[i]) - v[i]) * k;
    } else {
        const float vmax = std::max({v[0], v[1], v[2]});
        for (std::size_t i = 0; i < 3; ++i) {
            const float u = s.c[i] * k;
            out[i] = p.blend == VideoBlend::Normal ? v[i] + u * (1.0f - vmax) : blendChannel(p.blend, u, v[i]);
        }
    }
    for (float& c : out) c = std::clamp(c, 0.0f, 1.0f);
    return out;
}

}  // namespace reforce::engine

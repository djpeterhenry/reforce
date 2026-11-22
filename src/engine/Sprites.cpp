#include "engine/Sprites.hpp"

#include "engine/MediaOptions.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace reforce::engine {
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

bool readText(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Triangle-wave reflection of u into [-1, 1].
double reflect(double u) {
    double m = std::fmod(u + 1.0, 4.0);
    if (m < 0) m += 4.0;
    return m < 2.0 ? m - 1.0 : 3.0 - m;
}

}  // namespace

const char* const kBuiltinSpriteParams = "X=\"0\" Y=\"0\" EPen=\"1\" Flag=7\n";

std::string describeSpriteFlags(unsigned flags) {
    static const std::pair<unsigned, const char*> names[] = {
        {sprite::AlphaBlend, "AlphaBlend"},       {sprite::AlphaMask, "AlphaMask"},
        {sprite::SelectiveBlend, "SelectiveBlend"}, {sprite::AdditiveBlend, "AdditiveBlend"},
        {sprite::ThresholdCopy, "ThresholdCopy"}, {sprite::Invert, "Invert"},
        {sprite::DontCrop, "DontCrop"},           {sprite::MaintainAspect, "MaintainAspect"},
        {sprite::EnlargeToFit, "EnlargeToFit"},   {sprite::ShrinkToFit, "ShrinkToFit"},
    };
    std::string out;
    unsigned rest = flags;
    for (const auto& [bit, name] : names) {
        if (!(flags & bit)) continue;
        if (!out.empty()) out += '|';
        out += name;
        rest &= ~bit;
    }
    if (rest) {
        if (!out.empty()) out += '|';
        out += std::to_string(rest);
    }
    return out.empty() ? std::string("0") : out;
}

SpriteBlend spriteBlendMode(unsigned flags) {
    if (flags & sprite::AlphaBlend) return SpriteBlend::AlphaBlend;
    if (flags & sprite::AlphaMask) return SpriteBlend::AlphaMask;
    if (flags & sprite::SelectiveBlend) return SpriteBlend::Selective;
    if (flags & sprite::AdditiveBlend) return SpriteBlend::Additive;
    if (flags & sprite::ThresholdCopy) return SpriteBlend::Threshold;
    return SpriteBlend::Copy;
}

const char* spriteBlendName(SpriteBlend b) {
    switch (b) {
        case SpriteBlend::Copy: return "copy";
        case SpriteBlend::Threshold: return "threshold";
        case SpriteBlend::Additive: return "additive";
        case SpriteBlend::Selective: return "selective";
        case SpriteBlend::AlphaMask: return "alpha-mask";
        case SpriteBlend::AlphaBlend: return "alpha-blend";
    }
    return "?";
}

std::optional<SpriteLayering> parseSpriteLayering(const std::string& s) {
    std::string l;
    for (char c : s) l.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    if (l == "gforce" || l == "g-force") return SpriteLayering::GForce;
    if (l == "melt") return SpriteLayering::Melt;
    if (l == "trails") return SpriteLayering::Trails;
    return std::nullopt;
}

const char* spriteLayeringName(SpriteLayering m) {
    switch (m) {
        case SpriteLayering::GForce: return "gforce";
        case SpriteLayering::Melt: return "melt";
        case SpriteLayering::Trails: return "trails";
    }
    return "?";
}

bool spriteOldestOnTop(SpriteLayering m) { return m != SpriteLayering::Melt; }

SpriteLayerPlan spriteLayerPlan(const SpriteDrawSpec& d, SpriteLayering mode, double trailsAmount, double mix) {
    SpriteLayerPlan p;
    const double m = std::isfinite(mix) ? std::clamp(mix, 0.0, 1.0) : 0.0;
    auto add = [&](SpriteOp op, SpriteBlend blend, double pen, double drawMix = 0.0) {
        p.feedback[p.feedbackCount++] = SpriteLayerDraw{op, blend, pen, drawMix};
    };
    if (mode == SpriteLayering::Melt) {
        p.feedbackBeforeLines = false;
        if (d.hasPen) {
            p.display = {SpriteOp::Blend, d.blend, d.pen};
            p.hasDisplay = true;
        }
        // Probe #22: an EPen-only sprite peaks at ~0.5 x luminance (the Mix raises it to 0.5 + 0.5 mix).
        add(SpriteOp::Blend, d.blend, d.hasPen ? d.epen : d.epen * (0.5 + 0.5 * m), m);
        return p;
    }
    // Probes #40-#42: Pen draws into the feedback, EPen onto the display. The default (no blend flag) and
    // ThresholdCopy are keyed copies, except an EPen-only default draw, which is the half-way lighten.
    const bool keyed = d.blend == SpriteBlend::Copy || d.blend == SpriteBlend::Threshold;
    if (d.hasPen) {
        add(keyed ? SpriteOp::KeyedCopy : SpriteOp::Fetch, d.blend, d.pen);
        // EPen 0 (or absent) draws nothing on the display (#41); any EPen > 0 is drawn like Pen.
        if (d.hasEPen && d.epen > 0.0) {
            p.display = {keyed ? SpriteOp::KeyedCopy : SpriteOp::Fetch, d.blend, d.epen, m};
            p.hasDisplay = true;
        } else if (m > 0.0) {
            // Sprite Mix: a display-only copy of the Pen stamp carries the mix (the feedback stays G-Force).
            p.display = {keyed ? SpriteOp::KeyedCopy : SpriteOp::Fetch, d.blend, d.pen, m};
            p.hasDisplay = true;
        }
    } else if (d.hasEPen) {
        p.display = {d.blend == SpriteBlend::Threshold ? SpriteOp::KeyedCopy : SpriteOp::Fetch, d.blend, d.epen, m};
        p.hasDisplay = true;
        if (mode == SpriteLayering::Trails && trailsAmount > 0.0)
            add(SpriteOp::Blend, SpriteBlend::Selective, d.epen * std::min(trailsAmount, 1.0));
    }
    return p;
}

SpriteSize fitSprite(double imgW, double imgH, double frameW, double frameH, unsigned flags) {
    SpriteSize s{imgW, imgH};
    if (imgW <= 0 || imgH <= 0 || frameW <= 0 || frameH <= 0) return s;
    const bool shrink = flags & sprite::ShrinkToFit, enlarge = flags & sprite::EnlargeToFit;
    if (flags & sprite::MaintainAspect) {
        const double sx = frameW / imgW, sy = frameH / imgH;
        // DontCrop: the whole image fits inside (contain); otherwise it covers
        // the frame and the overflow is cropped by the frame edges.
        const double fit = (flags & sprite::DontCrop) ? std::min(sx, sy) : std::max(sx, sy);
        double k = 1.0;
        if (fit < 1.0 && shrink) k = fit;
        if (fit > 1.0 && enlarge) k = fit;
        s.w = imgW * k;
        s.h = imgH * k;
    } else {
        // Each axis on its own (stretch).
        if ((s.w > frameW && shrink) || (s.w < frameW && enlarge)) s.w = frameW;
        if ((s.h > frameH && shrink) || (s.h < frameH && enlarge)) s.h = frameH;
    }
    return s;
}

bool coversFrame(const SpriteSize& s, double frameW, double frameH) {
    return s.w >= frameW - 0.5 && s.h >= frameH - 0.5;
}

SpriteName parseSpriteName(const std::string& stem) {
    SpriteName n;
    n.name = stem;
    const std::string t = trim(stem);
    n.invert = !t.empty() && t.back() == '~';
    n.scriptOnly = !t.empty() && t.front() == '(';
    return n;
}

bool isSpriteImageFile(const fs::path& p) {
    const std::string ext = lower(p.extension().string());
    static const char* const kExts[] = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tif", ".tiff"};
    if (std::find_if(std::begin(kExts), std::end(kExts), [&](const char* e) { return ext == e; }) == std::end(kExts))
        return false;
    const std::string file = p.filename().string();
    if (file.empty() || file[0] == '_' || file[0] == '.') return false;
    const std::string stem = lower(p.stem().string());
    return !(stem.size() >= 5 && stem.compare(stem.size() - 5, 5, "-icon") == 0);
}

bool isSpriteVideoFile(const fs::path& p) {
    const std::string ext = lower(p.extension().string());
    static const char* const kExts[] = {".mov", ".mp4", ".m4v", ".mpg", ".mpeg", ".avi", ".qt", ".3gp"};
    if (std::find_if(std::begin(kExts), std::end(kExts), [&](const char* e) { return ext == e; }) == std::end(kExts))
        return false;
    const std::string file = p.filename().string();
    if (file.empty() || file[0] == '_' || file[0] == '.') return false;
    if (isInterpolatedMovieName(p)) return false;  // "<stem>.rife4x.mov" plays in place of "<stem>.mov
    const std::string stem = lower(p.stem().string());
    return !(stem.size() >= 5 && stem.compare(stem.size() - 5, 5, "-icon") == 0);
}

bool rgbaHasAlpha(const std::uint8_t* rgba, int width, int height) {
    const std::size_t n = std::size_t(std::max(width, 0)) * std::size_t(std::max(height, 0));
    for (std::size_t i = 0; i < n; ++i)
        if (rgba[i * 4 + 3] != 255) return true;
    return false;
}

LumaImage toLumaAlpha(const std::uint8_t* rgba, int width, int height, bool invert, bool hasAlphaChannel) {
    LumaImage out;
    out.width = width;
    out.height = height;
    out.hasAlpha = hasAlphaChannel;
    const std::size_t n = std::size_t(std::max(width, 0)) * std::size_t(std::max(height, 0));
    out.la.resize(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* p = rgba + i * 4;
        // ImageIO decodes to premultiplied RGBA: un-premultiply to the straight luminance.
        const double a = p[3] / 255.0;
        double lum = (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) / 255.0;
        if (hasAlphaChannel) lum = a > 0 ? std::min(1.0, lum / a) : 0.0;
        if (invert) lum = 1.0 - lum;
        const double alpha = hasAlphaChannel ? a : lum;
        out.la[i * 2] = std::uint8_t(std::lround(std::clamp(lum, 0.0, 1.0) * 255.0));
        out.la[i * 2 + 1] = std::uint8_t(std::lround(std::clamp(alpha, 0.0, 1.0) * 255.0));
    }
    return out;
}

std::array<double, 2> Bouncer::at(double t) const {
    const double d = t - t0;
    return {reflect(x0 + vx * d), reflect(y0 + vy * d)};
}

Bouncer makeBouncer(double sped, double frameW, double frameH, double t0, double u1, double u2, double u3) {
    Bouncer b;
    b.t0 = t0;
    b.x0 = 2.0 * u2 - 1.0;
    b.y0 = 2.0 * u3 - 1.0;
    if (frameW <= 0 || frameH <= 0) return b;
    const double px = std::fabs(sped) * frameH / 1260.0;  // pixels/s at this frame size
    const double ang = 2.0 * 3.14159265358979323846 * u1;
    b.vx = px * std::cos(ang) * 2.0 / frameW;
    b.vy = px * std::sin(ang) * 2.0 / frameH;
    return b;
}

SpriteParamsText spriteParamsFor(const fs::path& image) {
    SpriteParamsText r;
    std::error_code ec;
    fs::path sibling = image;
    sibling.replace_extension(".txt");
    if (fs::is_regular_file(sibling, ec) && readText(sibling, r.text)) {
        r.origin = sibling.string();
        return r;
    }
    fs::path dir = image.parent_path();
    for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
        const fs::path def = dir / "_default_sprite_params.txt";
        if (fs::is_regular_file(def, ec) && readText(def, r.text)) {
            r.origin = def.string();
            return r;
        }
        if (lower(dir.filename().string()).find(".package") != std::string::npos) break;
        const fs::path up = dir.parent_path();
        if (up == dir) break;
        dir = up;
    }
    r.text = kBuiltinSpriteParams;
    r.origin = "builtin";
    return r;
}

// ---------------------------------------------------------------- SpriteRun

SpriteRun::SpriteRun(std::shared_ptr<const scene::CompiledConfig> params, const scene::Environment& env,
                     std::string name, bool nameInvert, double startTime, double fade, double defaultHold,
                     std::shared_ptr<const TransitionFunction> fn)
    : cfg_(std::move(params)), name_(std::move(name)), start_(startTime) {
    scene::Environment e = env;
    e.loadTime = startTime;
    inst_ = std::make_unique<scene::SpriteInstance>(*cfg_, e);
    // Whole-file params: an absent Flag is 0 (native size, copy).
    flags_ = inst_->hasFlags() ? inst_->flags() : 0u;
    invert_ = nameInvert || inst_->invt() || (flags_ & sprite::Invert);
    hasPen_ = inst_->hasPen();
    double hold = inst_->duration();
    holdFromParams_ = std::isfinite(hold) && hold >= 0;
    if (!holdFromParams_) hold = defaultHold;
    env_.fade = std::max(0.0, fade);
    env_.hold = std::min(hold, 24.0 * 3600.0);
    env_.fn = std::move(fn);
    auto& rng = inst_->context().rng;
    const double u1 = rng.uniform01Exclusive(), u2 = rng.uniform01Exclusive(), u3 = rng.uniform01Exclusive();
    bounce_ = makeBouncer(inst_->sped(), e.width, e.height, startTime, u1, u2, u3);
}

void SpriteRun::setImageSize(int w, int h, double frameW, double frameH) {
    size_ = fitSprite(double(w), double(h), frameW, frameH, flags_);
}

SpriteDrawSpec SpriteRun::evaluate(double t, double frameW, double frameH, const Audio& audio) {
    scene::SpriteFrameInput in;
    in.t = t;
    in.startTime = start_;
    in.endTime = endTime();
    in.dt = in.endTime > start_ ? std::clamp((t - start_) / (in.endTime - start_), 0.0, 1.0) : 0.0;
    in.fader = env_(t - start_);
    const auto pos = bounce_.at(t);
    in.xpos = pos[0];
    in.ypos = pos[1];
    in.bass = audio.bass;
    in.level = audio.level;
    in.energy = audio.energy;
    in.mid = audio.mid;
    in.treble = audio.treble;
    in.beat = audio.beat;
    in.tempo = audio.tempo;
    in.linkOn = audio.linkOn;
    in.linkBpm = audio.linkBpm;
    in.linkBeat = audio.linkBeat;
    in.linkPhase = audio.linkPhase;
    in.sync = audio.sync;
    const scene::SpriteFrame f = inst_->evaluateFrame(in);
    SpriteDrawSpec d;
    auto finite = [](double v, double fb) { return std::isfinite(v) ? v : fb; };
    // Probe #41: X/Y = +-1 put the sprite's edge on the frame's edge (centre = X * (1 - size / frame) per
    // axis); a sprite as large as the frame or larger on an axis stays centred on it.
    const double kx = frameW > 0 ? std::max(0.0, 1.0 - size_.w / frameW) : 0.0;
    const double ky = frameH > 0 ? std::max(0.0, 1.0 - size_.h / frameH) : 0.0;
    d.cx = (finite(f.x, 0.0) * kx + 1.0) * 0.5 * frameW;
    d.cy = (1.0 - finite(f.y, 0.0) * ky) * 0.5 * frameH;
    d.w = size_.w;
    d.h = size_.h;
    d.hasPen = f.hasPen;
    d.hasEPen = f.hasEPen;
    d.pen = std::clamp(finite(f.pen, 0.0), 0.0, 1.0);
    d.epen = std::clamp(finite(f.epen, 0.0), 0.0, 1.0);
    d.opacity = in.fader;
    d.blend = spriteBlendMode(flags_);
    return d;
}

}  // namespace reforce::engine

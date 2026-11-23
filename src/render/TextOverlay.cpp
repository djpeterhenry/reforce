#include "render/TextOverlay.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "engine/Perf.hpp"
#include "render/ShaderCompiler.hpp"

namespace reforce::render {

namespace {

const char* kShader = R"(
#include <metal_stdlib>
using namespace metal;
// rect: NDC left, top, right, bottom; spread: shadow brightness taps apart, in output uv
struct TextQuad { float4 rect; float opacity; float strength; float from; float spread; float4 mode; };  // mode.x 1: a color image
struct VOut { float4 position [[position]]; float2 uv; float2 screen; };
vertex VOut text_vs(uint vid [[vertex_id]], constant TextQuad& q [[buffer(0)]]) {
    const float2 p = float2(vid & 1, vid >> 1);
    const float2 ndc = float2(mix(q.rect.x, q.rect.z, p.x), mix(q.rect.y, q.rect.w, p.y));
    VOut o;
    o.position = float4(ndc, 0.0f, 1.0f);
    o.uv = p;
    o.screen = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    return o;
}
// White text (r) over a dark shadow (g) that shows as much as the averaged picture behind it is bright.
fragment float4 text_fs(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], texture2d<float> lum [[texture(1)]],
                        constant TextQuad& q [[buffer(0)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    const float4 t = tex.sample(smp, in.uv);
    if (q.mode.x > 0.5f) return t * q.opacity;  // a color image (speech bubble), premultiplied
    const float a = t.r;
    float h = 0.0f;
    if (q.strength > 0.0f) {
        const float2 d = float2(q.spread, q.spread * 0.6f);
        const float l = 0.2f * (lum.sample(smp, in.screen).r + lum.sample(smp, in.screen + float2(d.x, 0)).r +
                                lum.sample(smp, in.screen - float2(d.x, 0)).r + lum.sample(smp, in.screen + float2(0, d.y)).r +
                                lum.sample(smp, in.screen - float2(0, d.y)).r);
        h = q.strength * smoothstep(q.from, q.from + 0.3f, l) * saturate(t.g * 1.5f);
    }
    return float4(a, a, a, a + h * (1.0f - a)) * q.opacity;
}
// The frame's luminance at 1/8 size (16 taps per 8 x 8 block), blended into the running average.
struct LumOut { float4 position [[position]]; float2 uv; };
vertex LumOut lum_vs(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    LumOut o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = float2(p.x, 1.0f - p.y);
    return o;
}
fragment float4 lum_fs(LumOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    const float2 px = 1.0f / float2(src.get_width(), src.get_height());
    const float3 w = float3(0.2126f, 0.7152f, 0.0722f);
    float l = 0.0f;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            l += dot(src.sample(smp, in.uv + (float2(x, y) * 4.0f - 2.0f) * px).rgb, w);
    return float4(l * 0.25f, 0, 0, 1);
}
)";

struct TextQuad {
    float rect[4];
    float opacity, strength, from, spread;
    float mode[4];  // mode[0] 1: a color image (speech bubble)
};

CFStringRef cfString(const std::string& s, bool upper) {
    CFMutableStringRef m = CFStringCreateMutable(kCFAllocatorDefault, 0);
    CFStringRef src = CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(s.data()),
                                              CFIndex(s.size()), kCFStringEncodingUTF8, false);
    if (src) {
        CFStringAppend(m, src);
        CFRelease(src);
    }
    if (upper) CFStringUppercase(m, nullptr);
    return m;
}

CTLineRef makeLine(const std::string& text, CTFontRef font, bool upper) {
    CFStringRef str = cfString(text, upper);
    const void* keys[] = {kCTFontAttributeName, kCTForegroundColorFromContextAttributeName};
    const void* vals[] = {font, kCFBooleanTrue};
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(kCFAllocatorDefault, str, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CFRelease(as);
    CFRelease(attrs);
    CFRelease(str);
    return line;
}

}  // namespace

bool TextBlockSpec::operator==(const TextBlockSpec& o) const {
    if (maxWidth != o.maxWidth || uppercase != o.uppercase || bubble != o.bubble || lines.size() != o.lines.size())
        return false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const TextLine &a = lines[i], &b = o.lines[i];
        if (a.text != b.text || a.sizePx != b.sizePx || a.bold != b.bold || a.marginTop != b.marginTop ||
            a.marginBottom != b.marginBottom)
            return false;
    }
    return true;
}

TextImage rasterizeText(const TextBlockSpec& spec) {
    TextImage img;
    struct Laid {
        CTLineRef line = nullptr;
        CTFontRef font = nullptr;
        double top = 0, box = 0, ascent = 0, descent = 0;
    };
    std::vector<Laid> laid;
    double y = 0, width = 0, prevBottom = 0;
    bool first = true;
    for (const TextLine& l : spec.lines) {
        if (l.text.empty()) continue;
        Laid d;
        d.font = CTFontCreateUIFontForLanguage(l.bold ? kCTFontUIFontEmphasizedSystem : kCTFontUIFontSystem,
                                               CGFloat(l.sizePx), nullptr);
        if (!d.font) continue;
        CTLineRef line = makeLine(l.text, d.font, spec.uppercase);
        CGFloat asc = 0, desc = 0, lead = 0;
        double w = CTLineGetTypographicBounds(line, &asc, &desc, &lead);
        if (w > spec.maxWidth) {
            CTLineRef dots = makeLine("\xE2\x80\xA6", d.font, false);
            CTLineRef cut = CTLineCreateTruncatedLine(line, spec.maxWidth, kCTLineTruncationEnd, dots);
            CFRelease(dots);
            if (cut) {
                CFRelease(line);
                line = cut;
                w = CTLineGetTypographicBounds(line, &asc, &desc, &lead);
            }
        }
        d.line = line;
        d.ascent = asc;
        d.descent = desc;
        d.box = 1.2 * l.sizePx;
        y += first ? l.marginTop : std::max(prevBottom, l.marginTop);
        d.top = y;
        y += d.box;
        prevBottom = l.marginBottom;
        first = false;
        width = std::max(width, w);
        laid.push_back(d);
    }
    if (laid.empty()) return img;
    y += prevBottom;
    // Shadow blur: a fifth of the largest type size; the image leaves room for it all round.
    double biggest = 0;
    for (const TextLine& l : spec.lines) biggest = std::max(biggest, l.text.empty() ? 0.0 : l.sizePx);
    const double blur = std::max(2.0, 0.2 * biggest);
    const int pad = 2 + int(std::ceil(blur * 1.5));
    img.inset = pad;
    img.width = int(std::ceil(width)) + 2 * pad;
    img.height = int(std::ceil(y)) + 2 * pad;
    const std::size_t n = std::size_t(img.width) * std::size_t(img.height);
    // Two coverage masks: the letters, and the letters with their blurred shadow.
    std::vector<std::uint8_t> text(n, 0), halo(n, 0);
    for (int pass = 0; pass < 2; ++pass) {
        CGContextRef ctx = CGBitmapContextCreate(pass == 0 ? text.data() : halo.data(), std::size_t(img.width),
                                                 std::size_t(img.height), 8, std::size_t(img.width), nullptr,
                                                 uint32_t(kCGImageAlphaOnly));
        if (!ctx) continue;
        CGContextSetShouldAntialias(ctx, true);
        CGContextSetShouldSmoothFonts(ctx, false);
        CGContextSetGrayFillColor(ctx, 0, 1);
        if (pass == 1) {
            CGColorRef black = CGColorCreateGenericGray(0, 1);
            CGContextSetShadowWithColor(ctx, CGSizeMake(0, 0), CGFloat(blur), black);
            CGColorRelease(black);
        }
        for (const Laid& d : laid) {
            // Baseline: the glyphs centered in the line box (as a browser does); CG's y runs up from the bottom.
            const double baseline = pad + d.top + (d.box - (d.ascent + d.descent)) / 2 + d.ascent;
            CGContextSetTextPosition(ctx, pad, CGFloat(img.height - baseline));
            CTLineDraw(d.line, ctx);
        }
        CGContextRelease(ctx);
    }
    img.bgra.assign(n * 4, 0);
    for (std::size_t i = 0; i < n; ++i) {
        img.bgra[i * 4 + 1] = halo[i];
        img.bgra[i * 4 + 2] = text[i];
        img.bgra[i * 4 + 3] = text[i];
    }
    for (Laid& d : laid) {
        CFRelease(d.line);
        CFRelease(d.font);
    }
    return img;
}

TextImage rasterizeBubble(const TextBlockSpec& spec) {
    TextImage img;
    if (spec.lines.empty() || spec.lines[0].text.empty()) return img;
    const TextLine& l = spec.lines[0];
    const double size = l.sizePx;
    CTFontRef font = CTFontCreateUIFontForLanguage(l.bold ? kCTFontUIFontEmphasizedSystem : kCTFontUIFontSystem,
                                                   CGFloat(size), nullptr);
    if (!font) return img;
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    const CGFloat white[] = {1, 1, 1, 1};
    CGColorRef textColor = CGColorCreate(rgb, white);
    CFStringRef str = cfString(l.text, spec.uppercase);
    const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    const void* vals[] = {font, textColor};
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(kCFAllocatorDefault, str, attrs);
    CTFramesetterRef fs = CTFramesetterCreateWithAttributedString(as);
    // Wrapped to maxWidth, at most 5 lines (the rest is cut).
    const double lineH = 1.2 * size, maxH = 5 * lineH + 2;
    CFRange fit{};
    CGSize box = CTFramesetterSuggestFrameSizeWithConstraints(fs, CFRangeMake(0, 0), nullptr,
                                                              CGSizeMake(CGFloat(spec.maxWidth), CGFLOAT_MAX), &fit);
    const double textW = std::ceil(std::min<double>(box.width, spec.maxWidth));
    const double textH = std::ceil(std::min<double>(box.height, maxH));
    const double pad = 0.5 * size, radius = 0.75 * size, stroke = std::max(1.5, 0.09 * size);
    const double tail = 0.5 * size, tailHalf = 0.38 * size, o = stroke;  // tail height, half its width at the box
    img.width = int(std::ceil(textW + 2 * pad + 2 * o));
    img.height = int(std::ceil(textH + 2 * pad + tail + 2 * o));
    img.inset = 0;
    img.bgra.assign(std::size_t(img.width) * std::size_t(img.height) * 4, 0);
    CGContextRef ctx = CGBitmapContextCreate(img.bgra.data(), std::size_t(img.width), std::size_t(img.height), 8,
                                             std::size_t(img.width) * 4, rgb,
                                             uint32_t(kCGImageAlphaPremultipliedFirst) | uint32_t(kCGBitmapByteOrder32Little));
    if (ctx) {
        CGContextSetShouldAntialias(ctx, true);
        // The bubble: a rounded box (CG's y up: the box above the tail) with the tail at the bottom center, one path
        // so the outline runs round both.
        const double x0 = o, x1 = img.width - o, y0 = o + tail, y1 = img.height - o, cx = img.width * 0.5;
        const double r = std::min(radius, std::min(x1 - x0, y1 - y0) * 0.5);
        CGMutablePathRef p = CGPathCreateMutable();
        CGPathMoveToPoint(p, nullptr, x0 + r, y1);
        CGPathAddArcToPoint(p, nullptr, x1, y1, x1, y0, r);
        CGPathAddArcToPoint(p, nullptr, x1, y0, x0, y0, r);
        CGPathAddLineToPoint(p, nullptr, cx + tailHalf, y0);
        CGPathAddLineToPoint(p, nullptr, cx, o);
        CGPathAddLineToPoint(p, nullptr, cx - tailHalf, y0);
        CGPathAddArcToPoint(p, nullptr, x0, y0, x0, y1, r);
        CGPathAddArcToPoint(p, nullptr, x0, y1, x1, y1, r);
        CGPathCloseSubpath(p);
        CGContextAddPath(ctx, p);
        CGContextSetRGBFillColor(ctx, 0.07, 0.05, 0.12, 0.86);
        CGContextFillPath(ctx);
        CGContextAddPath(ctx, p);
        CGContextSetRGBStrokeColor(ctx, 0.86, 0.84, 1.0, 0.9);
        CGContextSetLineWidth(ctx, CGFloat(stroke));
        CGContextSetLineJoin(ctx, kCGLineJoinRound);
        CGContextStrokePath(ctx);
        CGPathRelease(p);
        // The text (white, emoji in color), inside the padding.
        CGMutablePathRef tp = CGPathCreateMutable();
        CGPathAddRect(tp, nullptr, CGRectMake(CGFloat(x0 + pad), CGFloat(y0 + pad), CGFloat(textW + 1), CGFloat(textH)));
        CTFrameRef frame = CTFramesetterCreateFrame(fs, CFRangeMake(0, 0), tp, nullptr);
        if (frame) {
            CTFrameDraw(frame, ctx);
            CFRelease(frame);
        }
        CGPathRelease(tp);
        CGContextRelease(ctx);
    }
    CFRelease(fs);
    CFRelease(as);
    CFRelease(attrs);
    CFRelease(str);
    CGColorRelease(textColor);
    CGColorSpaceRelease(rgb);
    CFRelease(font);
    return img;
}

TextOverlay::TextOverlay(MTL::Device* device, int slots)
    : device_(device), slots_(std::size_t(std::max(1, slots))), serial_(std::size_t(std::max(1, slots)), 0) {
    MTL::Library* lib = compileLibrary(device, kShader, "text overlay");
    MTL::Function* vs = loadFunction(lib, "text_vs");
    MTL::Function* fs = loadFunction(lib, "text_fs");
    MTL::RenderPipelineDescriptor* d = MTL::RenderPipelineDescriptor::alloc()->init();
    d->setLabel(NS::String::string("text overlay", NS::UTF8StringEncoding));
    d->setVertexFunction(vs);
    d->setFragmentFunction(fs);
    auto* ca = d->colorAttachments()->object(0);
    ca->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
    ca->setBlendingEnabled(true);
    ca->setRgbBlendOperation(MTL::BlendOperationAdd);
    ca->setAlphaBlendOperation(MTL::BlendOperationAdd);
    ca->setSourceRGBBlendFactor(MTL::BlendFactorOne);
    ca->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
    ca->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
    ca->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
    NS::Error* err = nullptr;
    pso_ = device->newRenderPipelineState(d, &err);
    d->release();
    vs->release();
    fs->release();
    if (!pso_) {
        lib->release();
        throw std::runtime_error(std::string("pipeline 'text overlay' failed: ") +
                                 (err ? err->localizedDescription()->utf8String() : "?"));
    }
    // Luminance average: new * c + old * (1 - c), c = the blend color (set per frame from dt).
    vs = loadFunction(lib, "lum_vs");
    fs = loadFunction(lib, "lum_fs");
    d = MTL::RenderPipelineDescriptor::alloc()->init();
    d->setLabel(NS::String::string("text shadow luminance", NS::UTF8StringEncoding));
    d->setVertexFunction(vs);
    d->setFragmentFunction(fs);
    ca = d->colorAttachments()->object(0);
    ca->setPixelFormat(MTL::PixelFormatR16Float);
    ca->setBlendingEnabled(true);
    ca->setRgbBlendOperation(MTL::BlendOperationAdd);
    ca->setAlphaBlendOperation(MTL::BlendOperationAdd);
    ca->setSourceRGBBlendFactor(MTL::BlendFactorBlendColor);
    ca->setSourceAlphaBlendFactor(MTL::BlendFactorBlendColor);
    ca->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusBlendColor);
    ca->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusBlendColor);
    lumPso_ = device->newRenderPipelineState(d, &err);
    d->release();
    vs->release();
    fs->release();
    lib->release();
    if (!lumPso_)
        throw std::runtime_error(std::string("pipeline 'text shadow luminance' failed: ") +
                                 (err ? err->localizedDescription()->utf8String() : "?"));
    thread_ = std::thread([this] { worker(); });
}

TextOverlay::~TextOverlay() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    for (Done& d : done_)
        if (d.tex) d.tex->release();
    for (Slot& s : slots_)
        if (s.tex) s.tex->release();
    if (pso_) pso_->release();
    if (lumPso_) lumPso_->release();
    if (lum_) lum_->release();
}

void TextOverlay::set(int slot, const TextBlockSpec& spec) {
    if (!ok(slot)) return;
    Slot& s = slots_[std::size_t(slot)];
    if (s.haveWant && s.want == spec) return;
    s.want = spec;
    s.haveWant = true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        // Only the newest request per slot matters.
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const Job& j) { return j.slot == slot; }),
                    jobs_.end());
        jobs_.push_back(Job{slot, spec, ++serial_[std::size_t(slot)]});
    }
    cv_.notify_one();
}

void TextOverlay::worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.erase(jobs_.begin());
        }
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        const TextImage img = job.spec.bubble ? rasterizeBubble(job.spec) : rasterizeText(job.spec);
        Done done{job.slot, nullptr, img.width, img.height, img.inset, job.serial};
        if (img.width > 0 && img.height > 0) {
            MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
                MTL::PixelFormatBGRA8Unorm, NS::UInteger(img.width), NS::UInteger(img.height), false);
            td->setUsage(MTL::TextureUsageShaderRead);
            td->setStorageMode(MTL::StorageModeShared);
            done.tex = device_->newTexture(td);
            if (done.tex)
                done.tex->replaceRegion(MTL::Region(0, 0, NS::UInteger(img.width), NS::UInteger(img.height)), 0,
                                        img.bgra.data(), NS::UInteger(img.width) * 4);
        }
        pool->release();
        std::lock_guard<std::mutex> lock(mu_);
        done_.push_back(done);
    }
}

void TextOverlay::adopt() {
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> lock(mu_);
        done.swap(done_);
    }
    for (Done& d : done) {
        // A result older than the newest request for its slot is dropped (a newer one is on its way).
        if (d.serial != serial_[std::size_t(d.slot)]) {
            if (d.tex) d.tex->release();
            continue;
        }
        Slot& s = slots_[std::size_t(d.slot)];
        if (s.tex) s.tex->release();   // command buffers already encoded keep their own reference
        s.tex = d.tex;
        s.w = d.w;
        s.h = d.h;
        s.inset = d.inset;
    }
}

void TextOverlay::encode(MTL::CommandBuffer* cmd, MTL::Texture* output, const Draw* draws, int count,
                         const Shadow& shadow, double dt) {
    adopt();
    bool any = false;
    for (int i = 0; i < count; ++i)
        if (draws[i].opacity > 0.001 && ok(draws[i].slot) && slots_[std::size_t(draws[i].slot)].tex)
            any = true;
    if (!any || !output) {
        lumFresh_ = true;  // the next text starts from what is behind it then, not from a stale average
        return;
    }
    const double W = double(output->width()), H = double(output->height());
    const bool shade = shadow.strength > 0.001;
    if (shade) {
        const NS::UInteger lw = std::max<NS::UInteger>(1, output->width() / 8), lh = std::max<NS::UInteger>(1, output->height() / 8);
        if (!lum_ || lum_->width() != lw || lum_->height() != lh) {
            if (lum_) lum_->release();
            MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatR16Float, lw, lh, false);
            td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            td->setStorageMode(MTL::StorageModePrivate);
            lum_ = device_->newTexture(td);
            perf::countRenderAlloc();  // once (and on an output size change)
            lumFresh_ = true;
        }
        if (lum_) {
            MTL::RenderPassDescriptor* lp = MTL::RenderPassDescriptor::renderPassDescriptor();
            auto* la = lp->colorAttachments()->object(0);
            la->setTexture(lum_);
            // Fresh: cleared, not DontCare. The blend still reads the old value (times 0), and 0 * NaN from leftover
            // memory is NaN, which would then stay in the average and leave that patch without a shadow.
            la->setLoadAction(lumFresh_ ? MTL::LoadActionClear : MTL::LoadActionLoad);
            la->setClearColor(MTL::ClearColor(0, 0, 0, 1));
            la->setStoreAction(MTL::StoreActionStore);
            MTL::RenderCommandEncoder* le = cmd->renderCommandEncoder(lp);
            le->setLabel(NS::String::string("text shadow luminance", NS::UTF8StringEncoding));
            le->setRenderPipelineState(lumPso_);
            const float c = lumFresh_ ? 1.0f : float(1.0 - std::exp(-std::max(0.0, dt) / 0.25));
            le->setBlendColor(c, c, c, c);
            le->setFragmentTexture(output, 0);
            le->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
            le->endEncoding();
            lumFresh_ = false;
        }
    }
    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* att = pass->colorAttachments()->object(0);
    att->setTexture(output);
    att->setLoadAction(MTL::LoadActionLoad);
    att->setStoreAction(MTL::StoreActionStore);
    MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(pass);
    enc->setLabel(NS::String::string("text overlay", NS::UTF8StringEncoding));
    enc->setRenderPipelineState(pso_);
    for (int i = 0; i < count; ++i) {
        const Draw& d = draws[i];
        if (d.opacity <= 0.001 || !ok(d.slot) || !(d.scale > 0.01)) continue;
        const Slot& s = slots_[std::size_t(d.slot)];
        if (!s.tex) continue;
        // Unscaled: whole pixels (sharp text). Scaled: around the text box's center.
        double x0 = std::round(d.x) - s.inset, y0 = std::round(d.y) - s.inset, w = s.w, h = s.h;
        if (std::fabs(d.scale - 1) > 1e-3) {
            const double cx = d.x + 0.5 * (s.w - 2 * s.inset), cy = d.y + 0.5 * (s.h - 2 * s.inset);
            w = s.w * d.scale;
            h = s.h * d.scale;
            x0 = cx - 0.5 * w;
            y0 = cy - 0.5 * h;
        }
        TextQuad q{};
        q.rect[0] = float(x0 / W * 2 - 1);
        q.rect[1] = float(1 - y0 / H * 2);
        q.rect[2] = float((x0 + w) / W * 2 - 1);
        q.rect[3] = float(1 - (y0 + h) / H * 2);
        q.opacity = float(std::clamp(d.opacity, 0.0, 1.0));
        q.strength = shade && lum_ ? float(std::clamp(shadow.strength, 0.0, 1.0)) : 0.0f;
        q.from = float(std::clamp(shadow.from, 0.0, 1.0));
        q.spread = float(0.025 * H / W);
        q.mode[0] = d.image ? 1.0f : 0.0f;
        enc->setVertexBytes(&q, sizeof q, 0);
        enc->setFragmentBytes(&q, sizeof q, 0);
        enc->setFragmentTexture(s.tex, 0);
        enc->setFragmentTexture(shade && lum_ ? lum_ : s.tex, 1);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4));
    }
    enc->endEncoding();
}

}  // namespace reforce::render

#include "render/ImageFiles.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

namespace reforce::render {
namespace {

CFURLRef fileUrl(const std::string& path) {
    return CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
                                                   reinterpret_cast<const UInt8*>(path.c_str()),
                                                   CFIndex(path.size()), false);
}

// The same pixels with the alpha channel reinterpreted as padding (NoneSkipLast / NoneSkipFirst), so drawing
// keeps the straight colour. Null when the image has no straight alpha or Quartz rejects the layout.
CGImageRef withoutAlpha(CGImageRef img) {
    const CGImageAlphaInfo a = CGImageGetAlphaInfo(img);
    CGImageAlphaInfo skip;
    if (a == kCGImageAlphaLast) skip = kCGImageAlphaNoneSkipLast;
    else if (a == kCGImageAlphaFirst) skip = kCGImageAlphaNoneSkipFirst;
    else return nullptr;
    const CGBitmapInfo info = CGBitmapInfo((CGImageGetBitmapInfo(img) & ~CGBitmapInfo(kCGBitmapAlphaInfoMask)) | uint32_t(skip));
    return CGImageCreate(CGImageGetWidth(img), CGImageGetHeight(img), CGImageGetBitsPerComponent(img),
                         CGImageGetBitsPerPixel(img), CGImageGetBytesPerRow(img), CGImageGetColorSpace(img), info,
                         CGImageGetDataProvider(img), nullptr, false, CGImageGetRenderingIntent(img));
}

}  // namespace

bool loadImageRgba8(const std::string& path, ImageRgba8& out, bool ignoreAlpha) {
    CFURLRef url = fileUrl(path);
    if (!url) return false;
    CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
    CFRelease(url);
    if (!src) return false;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img) return false;
    if (ignoreAlpha)
        if (CGImageRef opaque = withoutAlpha(img)) {
            CGImageRelease(img);
            img = opaque;
        }
    const int w = int(CGImageGetWidth(img)), h = int(CGImageGetHeight(img));
    out.width = w;
    out.height = h;
    out.rgba.assign(std::size_t(w) * std::size_t(h) * 4, 0);
    // Draw into an sRGB RGBA8 context. Premultiplied is the only RGBA8 option
    // CGBitmapContext offers; ColorMap strips are opaque, so it does not matter.
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(out.rgba.data(), std::size_t(w), std::size_t(h), 8, std::size_t(w) * 4, cs,
                                             uint32_t(kCGImageAlphaPremultipliedLast) | uint32_t(kCGBitmapByteOrder32Big));
    CGColorSpaceRelease(cs);
    bool ok = false;
    if (ctx) {
        CGContextSetBlendMode(ctx, kCGBlendModeCopy);
        CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
        CGContextRelease(ctx);
        ok = true;
    }
    CGImageRelease(img);
    return ok;
}

bool writePngBgra8(const std::string& path, const std::uint8_t* bgra, int width, int height,
                   std::size_t bytesPerRow) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, bgra, CFIndex(bytesPerRow * std::size_t(height)));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGImageRef img = CGImageCreate(std::size_t(width), std::size_t(height), 8, 32, bytesPerRow, cs,
                                   CGBitmapInfo(uint32_t(kCGImageAlphaNoneSkipFirst) | uint32_t(kCGBitmapByteOrder32Little)), provider, nullptr,
                                   false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CFRelease(data);
    CGColorSpaceRelease(cs);
    if (!img) return false;
    bool ok = false;
    if (CFURLRef url = fileUrl(path)) {
        if (CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr)) {
            CGImageDestinationAddImage(dst, img, nullptr);
            ok = CGImageDestinationFinalize(dst);
            CFRelease(dst);
        }
        CFRelease(url);
    }
    CGImageRelease(img);
    return ok;
}

}  // namespace reforce::render

// Generates reforce's app icon (original artwork) with CoreGraphics.
//
//   clang -fobjc-arc -framework Foundation -framework CoreGraphics -framework ImageIO \
//         -framework UniformTypeIdentifiers tools/icon/make_icon.m -o make_icon
//   ./make_icon OUT.iconset
//
// scripts/make-icon.sh builds it and runs iconutil to produce resources/AppIcon.icns.
//
// The picture imitates a video-feedback swirl, the kind of image reforce
// draws: one wavy ring is redrawn ~110 times, each copy rotated a little,
// shrunk a little and shifted along a magenta -> amber -> cyan palette,
// composited additively, on a dark rounded-square plate (macOS icon grid:
// 824 px body in a 1024 px canvas). Fully deterministic.

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <math.h>

static const CGFloat kCanvas = 1024;

static CGContextRef makeContext(size_t px) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(NULL, px, px, 8, 0, cs,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    return ctx;
}

// Smooth palette through magenta, amber, pale yellow and cyan (t in 0..1).
static void palette(CGFloat t, CGFloat rgb[3]) {
    static const CGFloat stops[][3] = {
        {0.55, 0.10, 0.85},  // violet
        {0.95, 0.15, 0.55},  // magenta
        {1.00, 0.55, 0.15},  // amber
        {1.00, 0.92, 0.55},  // pale yellow
        {0.25, 0.90, 1.00},  // cyan
    };
    const int n = sizeof stops / sizeof stops[0];
    t = fmin(fmax(t, 0), 1) * (n - 1);
    int i = (int)floor(t);
    if (i >= n - 1) i = n - 2;
    const CGFloat f = t - i, s = f * f * (3 - 2 * f);
    for (int k = 0; k < 3; ++k) rgb[k] = stops[i][k] + (stops[i + 1][k] - stops[i][k]) * s;
}

static CGPathRef plate(void) {
    const CGFloat inset = 100, size = kCanvas - 2 * inset, radius = 185;
    return CGPathCreateWithRoundedRect(CGRectMake(inset, inset, size, size), radius, radius, NULL);
}

static CGImageRef renderMaster(void) {
    CGContextRef ctx = makeContext((size_t)kCanvas);
    CGPathRef body = plate();

    // Drop shadow under the plate.
    CGContextSaveGState(ctx);
    CGColorRef shadow = CGColorCreateSRGB(0, 0, 0, 0.45);
    CGContextSetShadowWithColor(ctx, CGSizeMake(0, -12), 28, shadow);
    CGColorRelease(shadow);
    CGContextAddPath(ctx, body);
    CGContextSetRGBFillColor(ctx, 0.02, 0.01, 0.06, 1);
    CGContextFillPath(ctx);
    CGContextRestoreGState(ctx);

    CGContextSaveGState(ctx);
    CGContextAddPath(ctx, body);
    CGContextClip(ctx);

    // Background: deep indigo, a little lighter in the middle.
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const CGFloat bg[] = {0.10, 0.04, 0.20, 1, 0.01, 0.00, 0.04, 1};
    const CGFloat bgLoc[] = {0, 1};
    CGGradientRef bgGrad = CGGradientCreateWithColorComponents(cs, bg, bgLoc, 2);
    const CGPoint c = CGPointMake(kCanvas / 2, kCanvas / 2);
    CGContextDrawRadialGradient(ctx, bgGrad, c, 0, c, 520, kCGGradientDrawsAfterEndLocation);
    CGGradientRelease(bgGrad);

    // Feedback swirl: the same wavy ring, rotated/shrunk per generation.
    CGContextSetBlendMode(ctx, kCGBlendModePlusLighter);
    CGContextSetLineJoin(ctx, kCGLineJoinRound);
    const int generations = 110;
    const int lobes = 5;
    for (int g = generations - 1; g >= 0; --g) {
        const CGFloat age = (CGFloat)g / (generations - 1);  // 0 = newest (outer), 1 = oldest (center)
        const CGFloat scale = pow(0.962, g);
        const CGFloat rot = g * 0.118;                      // radians per generation: the swirl
        const CGFloat r0 = 330 * scale;
        CGMutablePathRef ring = CGPathCreateMutable();
        const int steps = 360;
        for (int i = 0; i <= steps; ++i) {
            const CGFloat a = 2 * M_PI * i / steps;
            const CGFloat wobble = 1 + 0.16 * sin(lobes * a + g * 0.21) + 0.05 * sin(3 * a - g * 0.13);
            const CGFloat rr = r0 * wobble;
            const CGFloat x = c.x + rr * cos(a + rot), y = c.y + rr * sin(a + rot);
            if (i == 0) CGPathMoveToPoint(ring, NULL, x, y);
            else CGPathAddLineToPoint(ring, NULL, x, y);
        }
        CGFloat rgb[3];
        palette(1 - age, rgb);
        const CGFloat alpha = 0.10 + 0.55 * pow(1 - age, 1.6);
        // Soft glow pass, then the crisp line.
        CGContextAddPath(ctx, ring);
        CGContextSetRGBStrokeColor(ctx, rgb[0], rgb[1], rgb[2], alpha * 0.25);
        CGContextSetLineWidth(ctx, fmax(2.0, 26 * scale));
        CGContextStrokePath(ctx);
        CGContextAddPath(ctx, ring);
        CGContextSetRGBStrokeColor(ctx, rgb[0], rgb[1], rgb[2], alpha);
        CGContextSetLineWidth(ctx, fmax(1.0, 7 * scale));
        CGContextStrokePath(ctx);
        CGPathRelease(ring);
    }

    // Hot core where the generations converge.
    const CGFloat core[] = {1.0, 0.95, 0.85, 0.85, 1.0, 0.45, 0.30, 0.25, 0.4, 0.1, 0.6, 0.0};
    const CGFloat coreLoc[] = {0, 0.35, 1};
    CGGradientRef coreGrad = CGGradientCreateWithColorComponents(cs, core, coreLoc, 3);
    CGContextDrawRadialGradient(ctx, coreGrad, c, 0, c, 150, 0);
    CGGradientRelease(coreGrad);
    CGContextSetBlendMode(ctx, kCGBlendModeNormal);

    // Vignette towards the plate edge.
    const CGFloat vig[] = {0, 0, 0, 0, 0, 0, 0, 0.55};
    const CGFloat vigLoc[] = {0.62, 1};
    CGGradientRef vigGrad = CGGradientCreateWithColorComponents(cs, vig, vigLoc, 2);
    CGContextDrawRadialGradient(ctx, vigGrad, c, 0, c, 600, kCGGradientDrawsAfterEndLocation);
    CGGradientRelease(vigGrad);
    CGContextRestoreGState(ctx);

    // Thin highlight on the plate edge.
    CGContextAddPath(ctx, body);
    CGContextSetRGBStrokeColor(ctx, 1, 1, 1, 0.12);
    CGContextSetLineWidth(ctx, 3);
    CGContextStrokePath(ctx);

    CGColorSpaceRelease(cs);
    CGPathRelease(body);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    return img;
}

static BOOL writePNG(CGImageRef master, size_t px, NSString* path) {
    CGContextRef ctx = makeContext(px);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextDrawImage(ctx, CGRectMake(0, 0, px, px), master);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)[NSURL fileURLWithPath:path], (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (!dst) {
        CGImageRelease(img);
        return NO;
    }
    CGImageDestinationAddImage(dst, img, NULL);
    const BOOL ok = CGImageDestinationFinalize(dst);
    CFRelease(dst);
    CGImageRelease(img);
    return ok;
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc != 2) {
            fprintf(stderr, "usage: make_icon OUT.iconset\n");
            return 2;
        }
        NSString* dir = [NSString stringWithUTF8String:argv[1]];
        [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        CGImageRef master = renderMaster();
        const int sizes[] = {16, 32, 128, 256, 512};
        for (int i = 0; i < 5; ++i) {
            const int s = sizes[i];
            NSString* one = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"icon_%dx%d.png", s, s]];
            NSString* two = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"icon_%dx%d@2x.png", s, s]];
            if (!writePNG(master, (size_t)s, one) || !writePNG(master, (size_t)s * 2, two)) {
                fprintf(stderr, "make_icon: cannot write %s\n", dir.UTF8String);
                return 1;
            }
        }
        CGImageRelease(master);
    }
    return 0;
}

// reforce-syphon-monitor: watches a Syphon server's frames the way a client like OBS sees them, and counts
// black or transparent frames. Run it while reforce is running:
//
//   reforce-syphon-monitor [--name reforce] [--seconds 20] [--rate 240] [--gl]
//
// --gl: the timed samples read the shared IOSurface through OpenGL like OBS's Syphon Client source (OpenGL
// renderer): the IOSurface bound as a GL_TEXTURE_RECTANGLE texture, drawn into an offscreen framebuffer, then
// read back. OpenGL and the server's Metal writes are not synchronized with each other, which is what this
// mode exercises.
//
// Two kinds of samples:
//   on-notify   right after the server announces a new frame (the client's new-frame handler)
//   timed       on a free-running timer at --rate Hz, like a client that draws at its own frame rate and
//               samples the shared surface whenever it happens to be. This catches a server that is still
//               writing the surface when a client reads it.
// Each sample is a GPU blit of the shared IOSurface texture to a small CPU buffer. A sample is "black" when its
// mean RGB is < 2/255 and "transparent" when its mean alpha is < 0.5, while the stream as a whole is not black.
// A summary is printed at the end; exit code 1 when any sample was black or transparent.

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>
#import <OpenGL/CGLIOSurface.h>
#import <Syphon/SyphonMetalClient.h>
#import <Syphon/SyphonServerDirectory.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

struct Stats {
    long samples = 0, black = 0, transparent = 0, dips = 0, spikes = 0;
    double sumMean = 0;
    std::vector<double> blackTimes, dipTimes, spikeTimes, recent;  // recent: the last few means (dip detection)
    // Spike detection: a single sample unlike both neighbors (which agree), in brightness or alpha, either way.
    double prevRgb = -1, prevA = -1, prev2Rgb = -1, prev2A = -1, prevT = 0;
};

struct Sampler {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    // Downsampled read: a grid of points across the texture (blits of 1x1 regions into a buffer).
    static constexpr int kGrid = 16;
    double t0 = 0;

    // Returns false if no texture.
    bool sample(id<MTLTexture> tex, double& meanRgb, double& meanA) {
        if (!tex) return false;
        const NSUInteger w = tex.width, h = tex.height;
        id<MTLBuffer> buf = [device newBufferWithLength:kGrid * kGrid * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        for (int gy = 0; gy < kGrid; ++gy)
            for (int gx = 0; gx < kGrid; ++gx) {
                const NSUInteger x = (gx * 2 + 1) * w / (kGrid * 2), y = (gy * 2 + 1) * h / (kGrid * 2);
                [blit copyFromTexture:tex
                              sourceSlice:0
                              sourceLevel:0
                             sourceOrigin:MTLOriginMake(x, y, 0)
                               sourceSize:MTLSizeMake(1, 1, 1)
                                 toBuffer:buf
                        destinationOffset:(gy * kGrid + gx) * 4
                   destinationBytesPerRow:4
                 destinationBytesPerImage:4];
            }
        [blit endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        const uint8_t* p = (const uint8_t*)buf.contents;
        double rgb = 0, a = 0;
        for (int i = 0; i < kGrid * kGrid; ++i) {
            rgb += (p[i * 4] + p[i * 4 + 1] + p[i * 4 + 2]) / 3.0;
            a += p[i * 4 + 3];
        }
        meanRgb = rgb / (kGrid * kGrid);
        meanA = a / (kGrid * kGrid);
        return true;
    }
};

// OBS-like OpenGL reader: draws the IOSurface (as a rectangle texture) into a small FBO and reads it back.
struct GLReader {
    CGLContextObj ctx = nullptr;
    GLuint tex = 0, fbo = 0, rb = 0;
    IOSurfaceRef bound = nullptr;
    static constexpr int kN = 64;  // read-back size

    bool init() {
        CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers, (CGLPixelFormatAttribute)0};
        CGLPixelFormatObj pf = nullptr;
        GLint n = 0;
        if (CGLChoosePixelFormat(attrs, &pf, &n) != kCGLNoError || !pf) return false;
        CGLCreateContext(pf, nullptr, &ctx);
        CGLDestroyPixelFormat(pf);
        if (!ctx) return false;
        CGLSetCurrentContext(ctx);
        glGenTextures(1, &tex);
        glGenFramebuffersEXT(1, &fbo);
        glGenRenderbuffersEXT(1, &rb);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, kN, kN);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
        return glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT;
    }
    bool sample(IOSurfaceRef surf, double& meanRgb, double& meanA) {
        if (!surf) return false;
        CGLSetCurrentContext(ctx);
        const GLsizei w = (GLsizei)IOSurfaceGetWidth(surf), h = (GLsizei)IOSurfaceGetHeight(surf);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
        if (surf != bound) {
            if (CGLTexImageIOSurface2D(ctx, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, w, h, GL_BGRA,
                                       GL_UNSIGNED_INT_8_8_8_8_REV, surf, 0) != kCGLNoError)
                return false;
            bound = surf;
        }
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
        glViewport(0, 0, kN, kN);
        glClearColor(1, 0, 1, 1);  // magenta: a draw that never happened would not read as black
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_TEXTURE_RECTANGLE_ARB);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, 1, 0, 1, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glDisable(GL_BLEND);  // like a capture source drawn opaque
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(0, 0);
        glTexCoord2f((float)w, 0); glVertex2f(1, 0);
        glTexCoord2f((float)w, (float)h); glVertex2f(1, 1);
        glTexCoord2f(0, (float)h); glVertex2f(0, 1);
        glEnd();
        glDisable(GL_TEXTURE_RECTANGLE_ARB);
        static uint8_t px[kN * kN * 4];
        glReadPixels(0, 0, kN, kN, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
        double rgb = 0, a = 0;
        for (int i = 0; i < kN * kN; ++i) {
            rgb += (px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2]) / 3.0;
            a += px[i * 4 + 3];
        }
        meanRgb = rgb / (kN * kN);
        meanA = a / (kN * kN);
        return true;
    }
};

void spin(double seconds) { [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]]; }

void record(Stats& s, double rgb, double a, double t) {
    ++s.samples;
    s.sumMean += rgb;
    const bool black = rgb < 2.0, transparent = a < 127.5;
    if (black) ++s.black;
    if (transparent) ++s.transparent;
    if ((black || transparent) && s.blackTimes.size() < 40) s.blackTimes.push_back(t);
    // A dip: much darker than the frames just before it (a flash to black over a non-black picture).
    if (s.recent.size() >= 5) {
        std::vector<double> r = s.recent;
        std::sort(r.begin(), r.end());
        const double med = r[r.size() / 2];
        if (med > 20 && rgb < 0.25 * med) {
            ++s.dips;
            if (s.dipTimes.size() < 40) s.dipTimes.push_back(t);
        }
    }
    s.recent.push_back(rgb);
    if (s.recent.size() > 8) s.recent.erase(s.recent.begin());
    // A spike (a one-sample flash, brighter or darker, or a jump in alpha): the previous sample differs from both
    // its neighbors by more than the threshold, in the same direction, while those two agree.
    if (s.prev2Rgb >= 0) {
        auto spike = [](double before, double mid, double after, double thr) {
            const double d1 = mid - before, d2 = mid - after;
            return std::fabs(d1) > thr && std::fabs(d2) > thr && d1 * d2 > 0 && std::fabs(before - after) < thr * 0.5;
        };
        if (spike(s.prev2Rgb, s.prevRgb, rgb, 8.0) || spike(s.prev2A, s.prevA, a, 15.0)) {
            ++s.spikes;
            if (s.spikeTimes.size() < 40) s.spikeTimes.push_back(s.prevT);
        }
    }
    s.prev2Rgb = s.prevRgb;
    s.prev2A = s.prevA;
    s.prevRgb = rgb;
    s.prevA = a;
    s.prevT = t;
}

void report(const char* what, const Stats& s) {
    std::printf("%-10s samples %6ld  black %5ld  transparent %5ld  dips %4ld  spikes %4ld  mean RGB %.1f", what,
                s.samples, s.black, s.transparent, s.dips, s.spikes, s.samples ? s.sumMean / s.samples : 0.0);
    if (!s.blackTimes.empty()) {
        std::printf("\n           black/transparent at:");
        for (double t : s.blackTimes) std::printf(" %.3f", t);
    }
    if (!s.dipTimes.empty()) {
        std::printf("\n           dips at:");
        for (double t : s.dipTimes) std::printf(" %.3f", t);
    }
    if (!s.spikeTimes.empty()) {
        std::printf("\n           spikes at:");
        for (double t : s.spikeTimes) std::printf(" %.3f", t);
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        NSString* wanted = @"reforce";
        double seconds = 20.0, rate = 240.0;
        bool gl = false;
        for (int i = 1; i < argc; ++i)
            if (!std::strcmp(argv[i], "--gl")) gl = true;
        for (int i = 1; i + 1 < argc; i += 2) {
            if (!std::strcmp(argv[i], "--gl")) { --i; continue; }
            if (!std::strcmp(argv[i], "--name")) wanted = [NSString stringWithUTF8String:argv[i + 1]];
            else if (!std::strcmp(argv[i], "--seconds")) seconds = std::atof(argv[i + 1]);
            else if (!std::strcmp(argv[i], "--rate")) rate = std::max(1.0, std::atof(argv[i + 1]));
        }
        SyphonServerDirectory* dir = [SyphonServerDirectory sharedDirectory];
        NSDictionary* server = nil;
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:10];
        while (!server && [deadline timeIntervalSinceNow] > 0) {
            spin(0.1);
            server = [dir serversMatchingName:wanted appName:nil].firstObject;
        }
        if (!server) {
            std::printf("FAIL: no Syphon server named \"%s\"\n", wanted.UTF8String);
            return 2;
        }
        struct Shared {
            Sampler sampler;
            std::mutex m;
            Stats notified, timed;
            std::atomic<long> frames{0};
        };
        Shared* sh = new Shared;  // captured by pointer in the block (blocks copy C++ objects as const)
        sh->sampler.device = MTLCreateSystemDefaultDevice();
        sh->sampler.queue = [sh->sampler.device newCommandQueue];
        sh->sampler.t0 = CFAbsoluteTimeGetCurrent();
        SyphonMetalClient* client = [[SyphonMetalClient alloc]
            initWithServerDescription:server
                               device:sh->sampler.device
                              options:nil
                      newFrameHandler:^(SyphonMetalClient* c) {
                          sh->frames++;
                          id<MTLTexture> tex = [c newFrameImage];
                          double rgb = 0, a = 0;
                          if (sh->sampler.sample(tex, rgb, a)) {
                              std::lock_guard<std::mutex> lock(sh->m);
                              record(sh->notified, rgb, a, CFAbsoluteTimeGetCurrent() - sh->sampler.t0);
                          }
                      }];
        GLReader glr;
        if (gl && !glr.init()) {
            std::printf("FAIL: no OpenGL context\n");
            return 2;
        }
        const double period = 1.0 / rate;
        const double end = sh->sampler.t0 + seconds;
        while (CFAbsoluteTimeGetCurrent() < end) {
            spin(period);
            id<MTLTexture> tex = [client newFrameImage];
            double rgb = 0, a = 0;
            const bool ok = gl ? glr.sample(tex.iosurface, rgb, a) : sh->sampler.sample(tex, rgb, a);
            if (ok) {
                std::lock_guard<std::mutex> lock(sh->m);
                record(sh->timed, rgb, a, CFAbsoluteTimeGetCurrent() - sh->sampler.t0);
            }
        }
        [client stop];
        spin(0.2);  // let a handler in flight finish
        std::lock_guard<std::mutex> lock(sh->m);
        std::printf("server \"%s\": %ld new-frame notifications in %.1f s (%.1f fps)\n", wanted.UTF8String,
                    sh->frames.load(), seconds, sh->frames.load() / seconds);
        report("on-notify", sh->notified);
        report(gl ? "timed (GL)" : "timed", sh->timed);
        return (sh->notified.black + sh->notified.transparent + sh->timed.black + sh->timed.transparent +
                sh->notified.dips + sh->timed.dips) ? 1 : 0;
    }
}

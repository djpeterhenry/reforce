// reforce-syphon-tear-test: measures how often a Syphon client reads a frame while the server is still writing
// it (tearing). Syphon shares ONE IOSurface per server; the server overwrites it every frame and clients like
// OBS read it whenever they draw, without synchronizing with the server.
//
//   reforce-syphon-tear-test [--seconds 20] [--rate 30] [--fps 60] [--size 1920x1080] [--mode flipped|blit|none]
//                            [--role both|server|reader] [--name NAME]
// --role server / reader run the two halves in separate processes (like reforce and OBS); --name links them.
//
// A server in this process publishes full frames alternating black / white at --fps, through
// SyphonMetalServer exactly like reforce:
//   flipped  publishFrameTexture:...flipped:YES (reforce today: a render pass that clears, then draws)
//   blit     flipped:NO (Syphon's blit copy)
//   none     the server doesn't publish after the first frame (control: no writes, so no tears)
// A reader samples the shared surface through OpenGL like OBS's Syphon Client source (OpenGL renderer) at
// --rate Hz. A sample with both black and white pixels is torn; one that is neither is "other".

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <OpenGL/CGLIOSurface.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>
#import <Syphon/SyphonMetalClient.h>
#import <Syphon/SyphonMetalServer.h>
#import <Syphon/SyphonServerDirectory.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void spin(double seconds) { [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]]; }

struct GLReader {
    CGLContextObj ctx = nullptr;
    GLuint tex = 0, fbo = 0, rb = 0;
    IOSurfaceRef bound = nullptr;
    static constexpr int kN = 128;
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
    // Returns pixel counts: dark (< 40), light (> 215).
    bool sample(IOSurfaceRef surf, int& dark, int& light) {
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
        glClearColor(1, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_TEXTURE_RECTANGLE_ARB);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, 1, 0, 1, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(0, 0);
        glTexCoord2f((float)w, 0); glVertex2f(1, 0);
        glTexCoord2f((float)w, (float)h); glVertex2f(1, 1);
        glTexCoord2f(0, (float)h); glVertex2f(0, 1);
        glEnd();
        glDisable(GL_TEXTURE_RECTANGLE_ARB);
        static uint8_t px[kN * kN * 4];
        glReadPixels(0, 0, kN, kN, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
        dark = light = 0;
        for (int i = 0; i < kN * kN; ++i) {
            const int g = (px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2]) / 3;
            if (g < 40) ++dark;
            else if (g > 215) ++light;
        }
        return true;
    }
};

}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        double seconds = 20, rate = 30, fps = 60;
        int W = 1920, H = 1080;
        std::string mode = "flipped", role = "both", fixedName;
        for (int i = 1; i + 1 < argc; i += 2) {
            if (!std::strcmp(argv[i], "--seconds")) seconds = std::atof(argv[i + 1]);
            else if (!std::strcmp(argv[i], "--rate")) rate = std::atof(argv[i + 1]);
            else if (!std::strcmp(argv[i], "--fps")) fps = std::atof(argv[i + 1]);
            else if (!std::strcmp(argv[i], "--mode")) mode = argv[i + 1];
            else if (!std::strcmp(argv[i], "--size")) std::sscanf(argv[i + 1], "%dx%d", &W, &H);
            else if (!std::strcmp(argv[i], "--role")) role = argv[i + 1];
            else if (!std::strcmp(argv[i], "--name")) fixedName = argv[i + 1];
        }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:W
                                                                                     height:H
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> frame = [dev newTextureWithDescriptor:td];
        NSString* name = fixedName.empty() ? [NSString stringWithFormat:@"tear-test-%d", getpid()]
                                           : [NSString stringWithUTF8String:fixedName.c_str()];
        const bool serve = role != "reader", read = role != "server";
        SyphonMetalServer* server = serve ? [[SyphonMetalServer alloc] initWithName:name device:dev options:nil] : nil;
        __block long published = 0;
        __block bool white = false;
        const bool flipped = mode != "blit";
        void (^publishOne)(void) = ^{
            id<MTLCommandBuffer> cmd = [queue commandBuffer];
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = frame;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            white = !white;
            rp.colorAttachments[0].clearColor = white ? MTLClearColorMake(1, 1, 1, 1) : MTLClearColorMake(0, 0, 0, 1);
            [[cmd renderCommandEncoderWithDescriptor:rp] endEncoding];
            [server publishFrameTexture:frame onCommandBuffer:cmd imageRegion:NSMakeRect(0, 0, W, H) flipped:flipped];
            [cmd commit];
            ++published;
        };
        if (serve) publishOne();  // the first frame in every mode
        dispatch_queue_t q = dispatch_queue_create("tear.server", DISPATCH_QUEUE_SERIAL);
        dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), (uint64_t)(1e9 / fps), 100000);
        dispatch_source_set_event_handler(timer, ^{
            if (serve && mode != "none") publishOne();
        });
        dispatch_resume(timer);
        if (!read) {
            spin(seconds);
            dispatch_source_cancel(timer);
            std::printf("server %s: published %ld frames\n", name.UTF8String, published);
            [server stop];
            return 0;
        }

        SyphonServerDirectory* dir = [SyphonServerDirectory sharedDirectory];
        NSDictionary* desc = nil;
        for (int i = 0; i < 100 && !desc; ++i) {
            spin(0.05);
            desc = [dir serversMatchingName:name appName:nil].firstObject;
        }
        if (!desc) {
            std::printf("FAIL: own server not visible\n");
            return 2;
        }
        SyphonMetalClient* client = [[SyphonMetalClient alloc] initWithServerDescription:desc
                                                                                  device:dev
                                                                                 options:nil
                                                                         newFrameHandler:nil];
        spin(0.3);
        GLReader gl;
        if (!gl.init()) {
            std::printf("FAIL: no OpenGL\n");
            return 2;
        }
        long samples = 0, torn = 0, other = 0, black = 0, whiteN = 0;
        const double t0 = CFAbsoluteTimeGetCurrent();
        while (CFAbsoluteTimeGetCurrent() - t0 < seconds) {
            spin(1.0 / rate);
            id<MTLTexture> tex = [client newFrameImage];
            int dark = 0, light = 0;
            if (!tex || !gl.sample(tex.iosurface, dark, light)) continue;
            ++samples;
            const int n = GLReader::kN * GLReader::kN, minority = n / 100;
            if (dark > minority && light > minority) ++torn;
            else if (dark >= n - minority) ++black;
            else if (light >= n - minority) ++whiteN;
            else ++other;
        }
        dispatch_source_cancel(timer);
        std::printf("mode %-7s %dx%d, server %.0f fps (%ld published), reader %.0f Hz: %ld samples, %ld torn (%.2f%%), "
                    "%ld black, %ld white, %ld other\n",
                    mode.c_str(), W, H, fps, published, rate, samples, torn, samples ? 100.0 * torn / samples : 0.0,
                    black, whiteN, other);
        [client stop];
        [server stop];
        return 0;
    }
}

// reforce-syphon-probe: checks that a Syphon server is announced and delivers
// frames, without OBS. Run it while reforce is running:
//
//   reforce-syphon-probe [--name reforce] [--timeout 5] [--alpha]
//
// --alpha: also prints the frame's alpha (reforce.Syphon.Alpha): shares and a coarse map.
// --save PATH: writes one received frame as a PNG (color only, top row first).
//
// Exit code 0 = server found and >= 1 frame received; 1 = not found / no frames.

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <Metal/Metal.h>
#import <Syphon/SyphonMetalClient.h>
#import <Syphon/SyphonServerDirectory.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

static void spin(double seconds) {
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

int main(int argc, char** argv) {
    @autoreleasepool {
        NSString* wanted = @"reforce";
        double timeout = 5.0;
        for (int i = 1; i + 1 < argc; i += 2) {
            if (!strcmp(argv[i], "--name")) wanted = [NSString stringWithUTF8String:argv[i + 1]];
            else if (!strcmp(argv[i], "--timeout")) timeout = atof(argv[i + 1]);
        }
        bool alphaMap = false;
        NSString* savePath = nil;
        for (int i = 1; i < argc; ++i) {
            if (!strcmp(argv[i], "--alpha")) alphaMap = true;
            if (!strcmp(argv[i], "--save") && i + 1 < argc) savePath = [NSString stringWithUTF8String:argv[i + 1]];
        }

        // Directory discovery is asynchronous (distributed notifications).
        SyphonServerDirectory* dir = [SyphonServerDirectory sharedDirectory];
        NSDictionary* server = nil;
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:timeout];
        while (!server && [deadline timeIntervalSinceNow] > 0) {
            spin(0.1);
            server = [dir serversMatchingName:wanted appName:nil].firstObject;
        }

        printf("Syphon servers visible:\n");
        for (NSDictionary* s in dir.servers) {
            printf("  name=\"%s\" app=\"%s\"\n",
                   [s[SyphonServerDescriptionNameKey] description].UTF8String,
                   [s[SyphonServerDescriptionAppNameKey] description].UTF8String);
        }
        if (!server) {
            printf("FAIL: no server named \"%s\" within %.1fs\n", wanted.UTF8String, timeout);
            return 1;
        }

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        auto frames = std::make_shared<std::atomic<int>>(0);
        SyphonMetalClient* client =
            [[SyphonMetalClient alloc] initWithServerDescription:server
                                                          device:device
                                                         options:nil
                                                 newFrameHandler:^(SyphonMetalClient*) { (*frames)++; }];
        spin(1.0);
        id<MTLTexture> tex = [client newFrameImage];
        int n = frames->load();
        printf("client: %d frames in 1s; texture %lux%lu pixelFormat=%lu\n", n,
               (unsigned long)tex.width, (unsigned long)tex.height, (unsigned long)tex.pixelFormat);
        if (tex && savePath) {
            const NSUInteger w = tex.width, h = tex.height;
            id<MTLBuffer> buf = [device newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
            id<MTLCommandQueue> q = [device newCommandQueue];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:w * 4
                destinationBytesPerImage:w * h * 4];
            [bl endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            // The surface's first row is the bottom (OpenGL convention): flip to top-first.
            std::vector<uint8_t> px(w * h * 4);
            const uint8_t* src = static_cast<const uint8_t*>(buf.contents);
            for (NSUInteger y = 0; y < h; ++y) memcpy(&px[y * w * 4], src + (h - 1 - y) * w * 4, w * 4);
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs,
                                                     uint32_t(kCGBitmapByteOrder32Little) | uint32_t(kCGImageAlphaNoneSkipFirst));
            CGImageRef img = CGBitmapContextCreateImage(ctx);
            CGImageDestinationRef dst = CGImageDestinationCreateWithURL(
                (__bridge CFURLRef)[NSURL fileURLWithPath:savePath], (__bridge CFStringRef)UTTypePNG.identifier, 1,
                nullptr);
            if (dst) {
                CGImageDestinationAddImage(dst, img, nullptr);
                printf("saved %s: %s\n", savePath.UTF8String, CGImageDestinationFinalize(dst) ? "ok" : "FAILED");
                CFRelease(dst);
            }
            CGImageRelease(img);
            CGContextRelease(ctx);
            CGColorSpaceRelease(cs);
        }
        if (tex && alphaMap) {
            // The frame's alpha (reforce.Syphon.Alpha): shares of transparent / partial / opaque pixels and a coarse
            // map (' ' transparent, '.' up to a third, '+' up to two thirds, '#' more), top row first.
            const NSUInteger w = tex.width, h = tex.height;
            id<MTLBuffer> buf = [device newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
            id<MTLCommandQueue> q = [device newCommandQueue];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:w * 4
                destinationBytesPerImage:w * h * 4];
            [bl endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const uint8_t* px = static_cast<const uint8_t*>(buf.contents);
            std::size_t zero = 0, full = 0;
            double sum = 0;
            for (NSUInteger i = 0; i < w * h; ++i) {
                const uint8_t a = px[i * 4 + 3];
                zero += a == 0;
                full += a == 255;
                sum += a;
            }
            const double N = double(w * h);
            printf("alpha: mean %.3f, transparent %.1f %%, partial %.1f %%, opaque %.1f %%\n", sum / N / 255.0,
                   100.0 * zero / N, 100.0 * (N - zero - full) / N, 100.0 * full / N);
            // The surface's first row is the bottom (OpenGL convention): print from the last row up.
            const int cols = 64, rows = 18;
            for (int r = 0; r < rows; ++r) {
                std::string line;
                for (int c = 0; c < cols; ++c) {
                    double a = 0;
                    int cnt = 0;
                    const NSUInteger y0 = NSUInteger(rows - 1 - r) * h / rows, y1 = NSUInteger(rows - r) * h / rows;
                    const NSUInteger x0 = NSUInteger(c) * w / cols, x1 = NSUInteger(c + 1) * w / cols;
                    for (NSUInteger y = y0; y < y1; y += 4)
                        for (NSUInteger x = x0; x < x1; x += 4, ++cnt) a += px[(y * w + x) * 4 + 3];
                    a /= std::max(1, cnt) * 255.0;
                    line += a < 0.02 ? ' ' : a < 0.34 ? '.' : a < 0.67 ? '+' : '#';
                }
                printf("  |%s|\n", line.c_str());
            }
        }
        [client stop];
        if (n == 0 || !tex) {
            printf("FAIL: server found but no frames received\n");
            return 1;
        }
        printf("OK\n");
        return 0;
    }
}

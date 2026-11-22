// Video sources on macOS (see VideoSource.hpp): AVAssetReader files,
// AVCaptureSession cameras, SyphonMetalClient inputs, and the AVAssetWriter
// test-video generator. Compiled with ARC.
#include "video/VideoSource.hpp"

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <Syphon/SyphonMetalClient.h>
#import <Syphon/SyphonServerDirectory.h>

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace reforce::video {
namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

std::string str(NSString* s) { return s ? std::string(s.UTF8String ?: "") : std::string(); }

// ---------------------------------------------------------------- frames

// A CVPixelBuffer (IOSurface-backed) wrapped as a Metal texture, zero copy.
class CVFrame final : public Frame {
public:
    CVFrame(CVPixelBufferRef pb, CVMetalTextureRef mt, double time, std::uint64_t index) : pb_(pb), mt_(mt) {
        CVPixelBufferRetain(pb_);
        id<MTLTexture> tex = CVMetalTextureGetTexture(mt_);
        texture_ = static_cast<MTL::Texture*>((__bridge void*)tex);
        width_ = int(CVPixelBufferGetWidth(pb_));
        height_ = int(CVPixelBufferGetHeight(pb_));
        time_ = time;
        index_ = index;
    }
    ~CVFrame() override {
        CFRelease(mt_);
        CVPixelBufferRelease(pb_);
    }
    void* pixelBuffer() const override { return pb_; }

private:
    CVPixelBufferRef pb_;
    CVMetalTextureRef mt_;
};

// A Metal texture we own a reference to (Syphon).
class TexFrame final : public Frame {
public:
    TexFrame(id<MTLTexture> tex, double time, std::uint64_t index) : tex_(tex) {
        texture_ = static_cast<MTL::Texture*>((__bridge void*)tex_);
        width_ = int(tex.width);
        height_ = int(tex.height);
        time_ = time;
        index_ = index;
    }

private:
    id<MTLTexture> tex_;
};

std::shared_ptr<const Frame> wrapPixelBuffer(CVMetalTextureCacheRef cache, CVPixelBufferRef pb, double time,
                                             std::uint64_t index) {
    if (!cache || !pb) return nullptr;
    const size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
    CVMetalTextureRef mt = nullptr;
    const CVReturn rc = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pb, nullptr,
                                                                  MTLPixelFormatBGRA8Unorm, w, h, 0, &mt);
    if (rc != kCVReturnSuccess || !mt) return nullptr;
    return std::make_shared<CVFrame>(pb, mt, time, index);
}

struct DecodeTiming {
    double sum = 0, max = 0;
    std::uint64_t n = 0;
    void add(double ms) {
        sum += ms;
        max = std::max(max, ms);
        ++n;
    }
};

// ---------------------------------------------------------------- file source

struct FileState {
    std::mutex m;
    std::condition_variable cv;
    std::deque<FramePtr> queue;  // ascending media time (unwrapped)
    FramePtr current;            // render thread's pick (under m)
    double target = 0;           // unwrapped media time the render thread wants
    bool stop = false, ended = false;
    std::atomic<bool> ready{false}, failed{false};
    std::string error;
    std::atomic<int> width{0}, height{0};
    std::atomic<double> duration{0}, fps{0};
    std::atomic<bool> loop{false};  // spec.loop, changeable while playing (setLoop)
    SourceStats stats;
    DecodeTiming timing;
};

class FileSource final : public VideoSource {
public:
    FileSource(id<MTLDevice> device, const SourceSpec& spec, const OpenOptions& opts)
        : st_(std::make_shared<FileState>()), opts_(opts) {
        spec_ = spec;
        st_->target = spec.offset;
        st_->loop = spec.loop;
        std::thread([st = st_, device, spec, opts] { decodeLoop(st, device, spec, opts); }).detach();
    }
    ~FileSource() override {
        // Never joins (a decode may take a few ms): the thread exits on its own.
        std::lock_guard<std::mutex> lock(st_->m);
        st_->stop = true;
        st_->cv.notify_all();
    }

    bool ready() const override { return st_->ready.load(); }
    bool failed() const override { return st_->failed.load(); }
    std::string error() const override {
        std::lock_guard<std::mutex> lock(st_->m);
        return st_->error;
    }
    int width() const override { return st_->width.load(); }
    int height() const override { return st_->height.load(); }
    double duration() const override { return st_->duration.load(); }
    double frameRate() const override { return st_->fps.load(); }
    void setLoop(bool on) override {
        spec_.loop = on;
        st_->loop = on;
    }
    SourceStats stats() const override {
        std::lock_guard<std::mutex> lock(st_->m);
        SourceStats s = st_->stats;
        s.decodeMsAvg = st_->timing.n ? st_->timing.sum / double(st_->timing.n) : 0.0;
        s.decodeMsMax = st_->timing.max;
        return s;
    }

    void waitReady(double timeoutSeconds) override {
        const auto until = Clock::now() + std::chrono::duration<double>(timeoutSeconds);
        std::unique_lock<std::mutex> lock(st_->m);
        st_->cv.wait_until(lock, until, [&] { return st_->ready.load() || st_->failed.load(); });
    }

    FramePtr frameAt(double t) override {
        return frameAtMedia(spec_.offset + std::max(0.0, t) * std::max(0.0, spec_.rate));  // unwrapped media time
    }

    FramePtr frameAtMedia(double u) override {
        u = std::max(u, spec_.offset);
        FileState& s = *st_;
        if (opts_.deterministic) {
            std::unique_lock<std::mutex> lock(s.m);
            const auto until = Clock::now() + std::chrono::seconds(20);
            for (;;) {
                popUpTo(u);
                if (!s.queue.empty() || s.ended || s.failed.load() || s.stop) break;
                s.target = u;
                s.cv.notify_all();
                if (s.cv.wait_until(lock, until) == std::cv_status::timeout) {
                    std::fprintf(stderr, "[reforce] video %s: timed out waiting for the frame at %.3f s\n",
                                 spec_.id.c_str(), u);
                    break;
                }
            }
            s.target = u;
            s.cv.notify_all();
            return s.current;
        }
        std::unique_lock<std::mutex> lock(s.m, std::try_to_lock);
        if (!lock.owns_lock()) return last_;  // decoder holds the lock this instant: keep the previous frame
        const std::size_t before = s.queue.size();
        popUpTo(u);
        s.target = u;
        if (s.queue.size() != before) s.cv.notify_all();
        last_ = s.current;
        return last_;
    }

private:
    // Under s.m: the frame on screen at u = the last one with time <= u (the
    // first frame when u is before it).
    void popUpTo(double u) {
        FileState& s = *st_;
        while (!s.queue.empty() && (s.queue.front()->time() <= u + 1e-6 || !s.current)) {
            if (s.current && s.current != s.queue.front() && !shownCurrent_) ++s.stats.dropped;
            s.current = s.queue.front();
            s.queue.pop_front();
            shownCurrent_ = false;
            ++s.stats.picked;
        }
        shownCurrent_ = true;
    }

    static void fail(const std::shared_ptr<FileState>& st, const std::string& msg) {
        std::lock_guard<std::mutex> lock(st->m);
        st->error = msg;
        st->failed = true;
        st->cv.notify_all();
    }

    static void decodeLoop(std::shared_ptr<FileState> st, id<MTLDevice> device, SourceSpec spec, OpenOptions opts) {
        pthread_setname_np("reforce.video.decode");
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
        AVURLAsset* asset = nil;
        AVAssetTrack* track = nil;
        double duration = 0, fps = 0;
        @autoreleasepool {
            NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:spec.id.c_str()]];
            if (![NSFileManager.defaultManager fileExistsAtPath:url.path]) {
                fail(st, "no such file: " + spec.id);
                return;
            }
            asset = [AVURLAsset URLAssetWithURL:url options:@{AVURLAssetPreferPreciseDurationAndTimingKey : @YES}];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            track = [[asset tracksWithMediaType:AVMediaTypeVideo] firstObject];
            duration = CMTimeGetSeconds(asset.duration);
#pragma clang diagnostic pop
            if (!track) {
                fail(st, "no video track in " + spec.id + " (not a movie, or damaged)");
                return;
            }
            fps = track.nominalFrameRate;
        }
        if (!std::isfinite(duration) || duration <= 0) duration = 0;
        st->duration = duration;
        st->fps = fps;
        const double frameDur = fps > 0 ? 1.0 / fps : 1.0 / 30.0;
        CVMetalTextureCacheRef cache = nullptr;
        if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device, nullptr, &cache) != kCVReturnSuccess) {
            fail(st, "CVMetalTextureCacheCreate failed");
            return;
        }
        NSDictionary* settings = @{
            (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferMetalCompatibilityKey : @YES,
            (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
        };
        AVAssetReader* reader = nil;
        AVAssetReaderTrackOutput* output = nil;
        double loopBase = 0;     // unwrapped time of this pass's media time 0
        double passStart = std::max(0.0, spec.offset);
        if (duration > 0 && passStart >= duration) {
            if (spec.loop) {
                loopBase = std::floor(passStart / duration) * duration;
                passStart -= loopBase;
            } else {
                passStart = std::max(0.0, duration - frameDur);
            }
        }
        // The played section [segA, segB): a loop jumps back to segA. segB < duration
        // only with a per-movie End (MediaOptions); then each pass stops there.
        const double segA = passStart;
        double segB = duration;
        if (spec.end > spec.offset && (duration <= 0 || spec.end < duration) && spec.end - loopBase > segA)
            segB = spec.end - loopBase;
        if (spec.loopUnit > 0 && segB > segA) {  // (also play-once: it may be held later)
            // Whole units (bars of a beat-synced movie): every pass keeps the phase.
            // (within 2% of a whole number: plays whole, like video::sectionEnd)
            const double units = std::floor((segB - segA) / spec.loopUnit + 0.02);
            if (units >= 1) segB = std::min(segB, segA + units * spec.loopUnit);
        }
        const bool trimmedEnd = duration > 0 && segB < duration - 1e-6;
        std::uint64_t index = 0, passFrames = 0;
        bool shownAny = false;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(st->m);
                st->cv.wait(lock, [&] { return st->stop || (!st->ended && int(st->queue.size()) < opts.queueDepth); });
                if (st->stop) break;
            }
            @autoreleasepool {
                if (!reader) {
                    NSError* err = nil;
                    reader = [[AVAssetReader alloc] initWithAsset:asset error:&err];
                    output = [[AVAssetReaderTrackOutput alloc] initWithTrack:track outputSettings:settings];
                    output.alwaysCopiesSampleData = NO;
                    if (!reader || ![reader canAddOutput:output]) {
                        fail(st, "cannot read " + spec.id + ": " + str(err.localizedDescription));
                        break;
                    }
                    [reader addOutput:output];
                    if (passStart > 0 || trimmedEnd)
                        reader.timeRange = CMTimeRangeMake(
                            CMTimeMakeWithSeconds(passStart, 600000),
                            trimmedEnd ? CMTimeMakeWithSeconds(segB - passStart, 600000) : kCMTimePositiveInfinity);
                    if (![reader startReading]) {
                        fail(st, "cannot decode " + spec.id + ": " + str(reader.error.localizedDescription));
                        break;
                    }
                    passFrames = 0;
                }
                const auto t0 = Clock::now();
                CMSampleBufferRef sb = [output copyNextSampleBuffer];
                if (!sb) {
                    if (reader.status == AVAssetReaderStatusFailed) {
                        fail(st, "decode error in " + spec.id + ": " + str(reader.error.localizedDescription));
                        break;
                    }
                    [reader cancelReading];
                    reader = nil;
                    output = nil;
                    if (passFrames == 0 && index == 0) {
                        fail(st, "no frames in " + spec.id);
                        break;
                    }
                    if (st->loop.load() && duration > 0 && passFrames > 0) {
                        // Next pass from segA; unwrapped time continues at segB.
                        loopBase += (trimmedEnd || segA > 0) ? segB - segA : duration;
                        passStart = segA;
                        continue;
                    }
                    std::lock_guard<std::mutex> lock(st->m);
                    st->ended = true;
                    st->cv.notify_all();
                    continue;
                }
                CVImageBufferRef pb = CMSampleBufferGetImageBuffer(sb);
                if (!pb) {
                    CFRelease(sb);
                    continue;
                }
                const double pts = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb));
                const double at = loopBase + (std::isfinite(pts) ? pts : 0.0);
                ++passFrames;
                double target;
                {
                    std::lock_guard<std::mutex> lock(st->m);
                    target = st->target;
                }
                // Live playback that fell behind (e.g. rate > 1): skip frames that
                // are over before the wanted time without wrapping them.
                if (!opts.deterministic && shownAny && at + frameDur < target) {
                    CFRelease(sb);
                    std::lock_guard<std::mutex> lock(st->m);
                    ++st->stats.dropped;
                    continue;
                }
                FramePtr f = wrapPixelBuffer(cache, pb, at, index++);
                CFRelease(sb);
                const double ms = msSince(t0);
                if (!f) {
                    fail(st, "cannot map a decoded frame of " + spec.id + " into Metal");
                    break;
                }
                if (!shownAny) {
                    st->width = f->width();
                    st->height = f->height();
                }
                shownAny = true;
                std::lock_guard<std::mutex> lock(st->m);
                st->queue.push_back(std::move(f));
                ++st->stats.decoded;
                st->timing.add(ms);
                st->ready = true;
                st->cv.notify_all();
                if ((index & 63) == 0) CVMetalTextureCacheFlush(cache, 0);
            }
        }
        @autoreleasepool {
            [reader cancelReading];
            reader = nil;
        }
        {
            std::lock_guard<std::mutex> lock(st->m);
            st->queue.clear();
        }
        CFRelease(cache);
    }

    std::shared_ptr<FileState> st_;
    OpenOptions opts_;
    FramePtr last_;
    bool shownCurrent_ = true;
};

// ---------------------------------------------------------------- live sources (shared)

struct LiveState {
    std::mutex m;
    FramePtr latest;
    std::atomic<bool> ready{false}, failed{false}, stop{false};
    std::string error;
    std::string waiting;  // what the source waits for while it shows nothing
    std::atomic<int> width{0}, height{0};
    SourceStats stats;
    DecodeTiming timing;
    Clock::time_point t0 = Clock::now();
    std::uint64_t index = 0;

    void push(FramePtr f, double ms) {
        if (!f) return;
        width = f->width();
        height = f->height();
        {
            std::lock_guard<std::mutex> lock(m);
            if (latest && latest->index() + 1 < f->index()) ++stats.dropped;
            latest = f;
            waiting.clear();
            ++stats.decoded;
            timing.add(ms);
            ready = true;
        }
        VideoSource::FrameListener fn;
        {
            std::lock_guard<std::mutex> lock(listenerM);
            fn = listener;
        }
        if (fn) fn(f);
    }
    std::mutex listenerM;
    VideoSource::FrameListener listener;  // setFrameListener (body tracking)
    void fail(const std::string& msg) {
        std::lock_guard<std::mutex> lock(m);
        error = msg;
        waiting.clear();
        failed = true;
    }
    void setWaiting(const std::string& what) {
        std::lock_guard<std::mutex> lock(m);
        waiting = what;
    }
    // The input went away: show nothing (not a frozen frame) until it is back.
    void lost(const std::string& what) {
        std::lock_guard<std::mutex> lock(m);
        latest = nullptr;
        waiting = what;
    }
    double now() const { return std::chrono::duration<double>(Clock::now() - t0).count(); }
};

class LiveSource : public VideoSource {
public:
    explicit LiveSource(std::shared_ptr<LiveState> st) : st_(std::move(st)) {}
    bool ready() const override { return st_->ready.load(); }
    bool failed() const override { return st_->failed.load(); }
    std::string error() const override {
        std::lock_guard<std::mutex> lock(st_->m);
        return st_->error;
    }
    int width() const override { return st_->width.load(); }
    int height() const override { return st_->height.load(); }
    std::string waiting() const override {
        std::lock_guard<std::mutex> lock(st_->m);
        return st_->failed.load() ? std::string() : st_->waiting;
    }
    FramePtr frameAt(double) override {
        std::unique_lock<std::mutex> lock(st_->m, std::try_to_lock);
        if (!lock.owns_lock()) return last_;
        if (st_->latest != last_) ++st_->stats.picked;
        last_ = st_->latest;
        return last_;
    }
    bool setFrameListener(FrameListener fn) override {
        std::lock_guard<std::mutex> lock(st_->listenerM);
        st_->listener = std::move(fn);
        return true;
    }
    SourceStats stats() const override {
        std::lock_guard<std::mutex> lock(st_->m);
        SourceStats s = st_->stats;
        s.decodeMsAvg = st_->timing.n ? st_->timing.sum / double(st_->timing.n) : 0.0;
        s.decodeMsMax = st_->timing.max;
        return s;
    }

protected:
    std::shared_ptr<LiveState> st_;
    FramePtr last_;
};

}  // namespace
}  // namespace reforce::video

// ---------------------------------------------------------------- camera

using reforce::video::LiveState;

@interface RFCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
@public
    std::shared_ptr<LiveState> state;
    CVMetalTextureCacheRef cache;
}
@end

@implementation RFCaptureDelegate
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection {
    if (!state || state->stop.load()) return;
    const auto t0 = std::chrono::steady_clock::now();
    CVImageBufferRef pb = CMSampleBufferGetImageBuffer(sampleBuffer);
    auto f = reforce::video::wrapPixelBuffer(cache, pb, state->now(), state->index++);
    state->push(std::move(f), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}
- (void)dealloc {
    if (cache) CFRelease(cache);
}
@end

namespace reforce::video {
namespace {

AVCaptureDevice* findCamera(const std::string& id) {
    if (id.empty() || id == "default") return [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    NSString* want = [NSString stringWithUTF8String:id.c_str()];
    if (AVCaptureDevice* d = [AVCaptureDevice deviceWithUniqueID:want]) return d;
    AVCaptureDeviceDiscoverySession* ds = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal,
                                           AVCaptureDeviceTypeContinuityCamera ]
                              mediaType:AVMediaTypeVideo
                               position:AVCaptureDevicePositionUnspecified];
    for (AVCaptureDevice* d in ds.devices)
        if ([d.localizedName caseInsensitiveCompare:want] == NSOrderedSame) return d;
    return nil;
}


// Cameras may come up capped below 30 fps even in good light (the BRIO started at 1080p allowing 5..24 fps). Raise the
// ceiling to `fps` and keep the floor at the mode's slowest rate: the camera still slows down to expose longer in the
// dark, as it likes, but runs at full rate when it can. The current mode is kept if it offers `fps`, else the largest
// one up to 1920 x 1080 that does. Durations are the camera's exact values (it rejects a rounded 1/30). Returns what
// was set, for the log.
std::string raiseFrameRate(AVCaptureDevice* cam, double fps) {
    auto rangeFor = [fps](AVCaptureDeviceFormat* f) -> AVFrameRateRange* {
        for (AVFrameRateRange* r in f.videoSupportedFrameRateRanges)
            if (r.maxFrameRate >= fps - 0.01 && r.minFrameRate <= fps + 0.01) return r;
        return nil;
    };
    AVCaptureDeviceFormat* best = rangeFor(cam.activeFormat) ? cam.activeFormat : nil;
    if (!best) {
        long bestArea = 0;
        for (AVCaptureDeviceFormat* f in cam.formats) {
            const CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(f.formatDescription);
            const long area = long(d.width) * long(d.height);
            if (d.width <= 1920 && d.height <= 1080 && rangeFor(f) && area > bestArea) {
                best = f;
                bestArea = area;
            }
        }
    }
    if (!best) return "its own frame rate: no mode offers " + std::to_string(int(fps)) + " fps";
    // The fastest frame time: `fps` (or the closest the camera offers at or above it); the slowest: the mode's
    // slowest range (it may drop that far in the dark).
    AVFrameRateRange* fast = rangeFor(best);
    AVFrameRateRange* slow = fast;
    for (AVFrameRateRange* r in best.videoSupportedFrameRateRanges)
        if (r.minFrameRate < slow.minFrameRate) slow = r;
    NSError* err = nil;
    if (![cam lockForConfiguration:&err]) return "its own frame rate: " + str(err.localizedDescription);
    std::string out;
    @try {
        if (cam.activeFormat != best) cam.activeFormat = best;
        cam.activeVideoMinFrameDuration = fast.minFrameDuration;
        cam.activeVideoMaxFrameDuration = slow.maxFrameDuration;
        const CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(best.formatDescription);
        char buf[96];
        std::snprintf(buf, sizeof buf, "%dx%d, %.0f fps down to %.0f in the dark", d.width, d.height,
                      1.0 / CMTimeGetSeconds(cam.activeVideoMinFrameDuration),
                      1.0 / CMTimeGetSeconds(cam.activeVideoMaxFrameDuration));
        out = buf;
    } @catch (NSException* e) {
        out = "its own frame rate: " + str(e.reason);
    }
    [cam unlockForConfiguration];
    return out;
}

class CameraSource final : public LiveSource {
public:
    CameraSource(id<MTLDevice> device, const SourceSpec& spec) : LiveSource(std::make_shared<LiveState>()) {
        spec_ = spec;
        queue_ = dispatch_queue_create("reforce.video.camera", DISPATCH_QUEUE_SERIAL);
        holder_ = [NSMutableArray array];
        observers_ = [NSMutableArray array];
        auto st = st_;
        dispatch_queue_t q = queue_;
        NSMutableArray* holder = holder_;
        NSMutableArray* observers = observers_;
        const std::string camId = spec.id;
        st->setWaiting("opening the camera");
        // Runs on the camera queue: device open and startRunning (which blocks)
        // never touch the render thread. The session and delegate live in
        // `holder` until the source goes away.
        void (^startCapture)(void) = ^{
            if (st->stop.load()) return;
            AVCaptureDevice* cam = findCamera(camId);
            if (!cam) {
                st->fail("no camera \"" + camId + "\" (unplugged?)");
                return;
            }
            NSError* err = nil;
            AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:cam error:&err];
            if (!input) {
                st->fail("cannot open camera " + str(cam.localizedName) + ": " + str(err.localizedDescription));
                return;
            }
            AVCaptureSession* session = [[AVCaptureSession alloc] init];
            if (![session canAddInput:input]) {
                st->fail("camera input refused (in use by another app?)");
                return;
            }
            [session addInput:input];
            AVCaptureVideoDataOutput* videoOut = [[AVCaptureVideoDataOutput alloc] init];
            videoOut.videoSettings = @{
                (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
                (id)kCVPixelBufferMetalCompatibilityKey : @YES,
            };
            videoOut.alwaysDiscardsLateVideoFrames = YES;
            RFCaptureDelegate* del = [[RFCaptureDelegate alloc] init];
            del->state = st;
            CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device, nullptr, &del->cache);
            [videoOut setSampleBufferDelegate:del queue:q];
            if (![session canAddOutput:videoOut]) {
                st->fail("camera output refused");
                return;
            }
            [session addOutput:videoOut];
            [holder addObject:session];
            [holder addObject:del];
            // Unplugged camera / capture errors: fail (the layer or sprite fades
            // out with the message) instead of freezing on the last frame.
            const std::string name = str(cam.localizedName);
            NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
            [observers addObject:[nc addObserverForName:AVCaptureDeviceWasDisconnectedNotification
                                                 object:cam
                                                  queue:nil
                                             usingBlock:^(NSNotification*) {
                                                 st->fail("camera \"" + name + "\" disconnected");
                                             }]];
            [observers addObject:[nc addObserverForName:AVCaptureSessionRuntimeErrorNotification
                                                 object:session
                                                  queue:nil
                                             usingBlock:^(NSNotification* n) {
                                                 NSError* e = n.userInfo[AVCaptureSessionErrorKey];
                                                 st->fail("camera \"" + name + "\": " + str(e.localizedDescription));
                                             }]];
            st->setWaiting("starting camera \"" + name + "\"");
            const std::string rate = raiseFrameRate(cam, 30);
            [session startRunning];
            if (!session.running && !st->failed.load()) st->fail("camera \"" + name + "\" did not start");
            else std::fprintf(stderr, "[reforce] camera \"%s\" running (%s)\n", name.c_str(), rate.c_str());
        };
        // Permission: asked lazily, the first time a camera source is opened
        // (one shared prompt); never blocks this thread.
        CameraGate& gate = systemCameraGate();
        if (gate.access() != CameraAccess::Granted) {
            st->setWaiting(cameraAccessMessage(CameraAccess::Requesting));
            std::fprintf(stderr, "[reforce] camera: %s\n", cameraAccessName(gate.access()));
        }
        gate.whenDecided([st, q, startCapture](bool granted) {
            if (!granted) {
                const CameraAccess a = systemCameraGate().access();
                st->fail(cameraAccessMessage(a == CameraAccess::NoUsageKey ? a : CameraAccess::Denied));
                return;
            }
            st->setWaiting("opening the camera");
            dispatch_async(q, startCapture);
        });
    }
    ~CameraSource() override {
        st_->stop = true;
        NSMutableArray* holder = holder_;
        NSMutableArray* observers = observers_;
        dispatch_async(queue_, ^{
            for (id o in observers) [NSNotificationCenter.defaultCenter removeObserver:o];
            [observers removeAllObjects];
            for (id o in holder)
                if ([o isKindOfClass:[AVCaptureSession class]]) [(AVCaptureSession*)o stopRunning];
            [holder removeAllObjects];
        });
    }

private:
    dispatch_queue_t queue_;
    NSMutableArray* holder_;
    NSMutableArray* observers_;
};

// ---------------------------------------------------------------- Syphon input

std::mutex& ownMutex() {
    static std::mutex m;
    return m;
}
std::string& ownName() {
    static std::string s = "reforce";
    return s;
}
std::string& ownUuid() {
    static std::string s;
    return s;
}
std::string ownApp() { return str(NSProcessInfo.processInfo.processName); }

SyphonServerInfo infoFrom(NSDictionary* d) {
    SyphonServerInfo s;
    s.uuid = str(d[SyphonServerDescriptionUUIDKey]);
    s.name = str(d[SyphonServerDescriptionNameKey]);
    s.app = str(d[SyphonServerDescriptionAppNameKey]);
    std::lock_guard<std::mutex> lock(ownMutex());
    s.self = isSelfSyphonServer(s, ownName(), ownApp(), ownUuid());
    return s;
}

class SyphonSource final : public LiveSource {
public:
    SyphonSource(id<MTLDevice> device, const SourceSpec& spec) : LiveSource(std::make_shared<LiveState>()) {
        spec_ = spec;
        auto st = st_;
        std::thread([st, device, spec] {
            pthread_setname_np("reforce.video.syphon");
            SyphonMetalClient* client = nil;
            bool waitingLogged = false;
            while (!st->stop.load()) {
                @autoreleasepool {
                    if (client && !client.isValid) {
                        std::fprintf(stderr, "[reforce] Syphon server \"%s\" went away; waiting for it\n",
                                     spec.id.c_str());
                        [client stop];
                        client = nil;
                        st->lost("Syphon server \"" + spec.id + "\" quit; waiting for it to come back");
                    }
                    if (!client) {
                        NSDictionary* match = nil;
                        bool selfMatch = false;
                        for (NSDictionary* d in [SyphonServerDirectory sharedDirectory].servers) {
                            const SyphonServerInfo info = infoFrom(d);
                            if (!syphonMatches(info, spec.id)) continue;
                            if (info.self && !spec.allowFeedback) {
                                selfMatch = true;
                                continue;
                            }
                            match = d;
                            break;
                        }
                        if (!match && selfMatch) {
                            st->fail("refusing to subscribe to our own Syphon server \"" + spec.id +
                                     "\" (feedback loop; settings reforce.Syphon.AllowFeedback = 1 allows it)");
                            break;
                        }
                        if (match) {
                            client = [[SyphonMetalClient alloc]
                                initWithServerDescription:match
                                                   device:device
                                                  options:nil
                                          newFrameHandler:^(SyphonMetalClient* c) {
                                              const auto t0 = Clock::now();
                                              id<MTLTexture> tex = [c newFrameImage];
                                              if (!tex || st->stop.load()) return;
                                              st->push(std::make_shared<TexFrame>(tex, st->now(), st->index++),
                                                       msSince(t0));
                                          }];
                            if (client.isValid)
                                std::fprintf(stderr, "[reforce] Syphon input \"%s\" connected\n", spec.id.c_str());
                            else
                                client = nil;
                        } else if (!waitingLogged) {
                            if (!st->ready.load())
                                st->setWaiting("waiting for the Syphon server \"" + spec.id + "\"");
                            std::fprintf(stderr, "[reforce] Syphon input \"%s\": waiting for the server\n",
                                         spec.id.c_str());
                            waitingLogged = true;
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            @autoreleasepool {
                [client stop];
                client = nil;
            }
        }).detach();
    }
    ~SyphonSource() override { st_->stop = true; }
};

}  // namespace

// ---------------------------------------------------------------- public API

std::unique_ptr<VideoSource> openSource(MTL::Device* device, const SourceSpec& spec, const OpenOptions& opts) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)static_cast<void*>(device);
    switch (spec.kind) {
        case SourceKind::File: return std::make_unique<FileSource>(dev, spec, opts);
        case SourceKind::Camera: return std::make_unique<CameraSource>(dev, spec);
        case SourceKind::Syphon: return std::make_unique<SyphonSource>(dev, spec);
    }
    return nullptr;
}

std::vector<CameraInfo> listCameras() {
    std::vector<CameraInfo> out;
    @autoreleasepool {
        AVCaptureDeviceDiscoverySession* ds = [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal,
                                               AVCaptureDeviceTypeContinuityCamera ]
                                  mediaType:AVMediaTypeVideo
                                   position:AVCaptureDevicePositionUnspecified];
        for (AVCaptureDevice* d in ds.devices) out.push_back({str(d.uniqueID), str(d.localizedName)});
    }
    return out;
}

namespace {

// A bundled app without NSCameraUsageDescription is killed by TCC when it
// asks; a plain executable is attributed to its responsible process (the
// terminal), which has its own description.
bool missingCameraUsageKey() {
    @autoreleasepool {
        NSBundle* b = NSBundle.mainBundle;
        if (![b.bundlePath.pathExtension isEqualToString:@"app"]) return false;
        return [b objectForInfoDictionaryKey:@"NSCameraUsageDescription"] == nil;
    }
}

CameraAccess systemCameraStatus() {
    static const bool noKey = missingCameraUsageKey();
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
        case AVAuthorizationStatusAuthorized: return CameraAccess::Granted;
        case AVAuthorizationStatusNotDetermined: return noKey ? CameraAccess::NoUsageKey : CameraAccess::NotDetermined;
        default: return CameraAccess::Denied;
    }
}

}  // namespace

CameraGate& systemCameraGate() {
    static CameraGate gate(systemCameraStatus, [](std::function<void(bool)> done) {
        std::fprintf(stderr, "[reforce] asking for camera permission (macOS prompt)\n");
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                                 completionHandler:^(BOOL granted) {
                                     std::fprintf(stderr, "[reforce] camera permission %s\n",
                                                  granted ? "granted" : "denied");
                                     done(granted);
                                 }];
    });
    return gate;
}

int cameraPermission() {
    switch (systemCameraGate().access()) {
        case CameraAccess::NotDetermined: return 0;
        case CameraAccess::Granted: return 1;
        case CameraAccess::Denied: return 2;
        case CameraAccess::Requesting: return 3;
        case CameraAccess::NoUsageKey: return 4;
    }
    return 0;
}

void openCameraPrivacySettings() {
    @autoreleasepool {
        NSURL* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Camera"];
        [NSWorkspace.sharedWorkspace openURL:url];
    }
}

void setOwnSyphonServer(const std::string& name, const std::string& uuid) {
    std::lock_guard<std::mutex> lock(ownMutex());
    ownName() = name;
    ownUuid() = uuid;
}

void initSyphonDirectory() {
    @autoreleasepool {
        (void)[SyphonServerDirectory sharedDirectory];
    }
}

std::vector<SyphonServerInfo> listSyphonServers() {
    std::vector<SyphonServerInfo> out;
    @autoreleasepool {
        for (NSDictionary* d in [SyphonServerDirectory sharedDirectory].servers) out.push_back(infoFrom(d));
    }
    return out;
}

// ---------------------------------------------------------------- test videos

namespace {

void fillCard(std::uint8_t* base, std::size_t bpr, int w, int h, int frame) {
    for (int y = 0; y < h; ++y) {
        std::uint8_t* row = base + std::size_t(y) * bpr;
        for (int x = 0; x < w; ++x) {
            std::uint8_t r, g, b;
            if (y < h / 4) {
                const int k = std::min(7, x * 8 / w);
                const std::uint8_t v = ((frame & 255) >> (7 - k)) & 1 ? 255 : 0;
                r = g = b = v;
            } else if (y < h / 2) {
                if (x < w / 2) r = 255, g = 0, b = 0;
                else r = g = b = 128;
            } else {
                if (x < w / 2) r = g = b = 255;
                else r = 0, g = 255, b = 0;
            }
            std::uint8_t* p = row + std::size_t(x) * 4;
            p[0] = b;
            p[1] = g;
            p[2] = r;
            p[3] = 255;
        }
    }
}

void fillMotion(std::uint8_t* base, std::size_t bpr, int w, int h, int frame) {
    // Moving gradients plus a scrolling value-noise texture: enough detail and
    // motion for the encoder to produce a realistic bitrate.
    auto hash = [](std::uint32_t x, std::uint32_t y) {
        std::uint32_t v = x * 0x8da6b343u ^ y * 0xd8163841u;
        v ^= v >> 13;
        v *= 0x5bd1e995u;
        v ^= v >> 15;
        return std::uint8_t(v >> 24);
    };
    const int sx = frame * 3, sy = frame * 2;
    for (int y = 0; y < h; ++y) {
        std::uint8_t* row = base + std::size_t(y) * bpr;
        const int gy = (y * 256 / std::max(1, h) + frame * 2) & 255;
        for (int x = 0; x < w; ++x) {
            const std::uint8_t n = hash(std::uint32_t((x + sx) >> 3), std::uint32_t((y + sy) >> 3));
            const int gx = (x * 256 / std::max(1, w) + frame * 3) & 255;
            std::uint8_t* p = row + std::size_t(x) * 4;
            p[0] = std::uint8_t((gx + n) >> 1);
            p[1] = std::uint8_t((gy + (n ^ 0x55)) >> 1);
            p[2] = std::uint8_t((255 - gx + gy) >> 1);
            p[3] = 255;
        }
    }
}

}  // namespace

bool writeTestVideo(const std::filesystem::path& path, const TestVideoSpec& spec, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    if (spec.width <= 0 || spec.height <= 0 || spec.frames <= 0 || spec.fps <= 0) return fail("bad test video spec");
    @autoreleasepool {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.string().c_str()]];
        std::string ext = path.extension().string();
        for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        const bool mp4 = ext == ".mp4" || ext == ".m4v";
        if (mp4 && spec.codec == TestCodec::ProRes422) return fail("ProRes needs a .mov file");
        NSError* err = nil;
        AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:url
                                                         fileType:mp4 ? AVFileTypeMPEG4 : AVFileTypeQuickTimeMovie
                                                            error:&err];
        if (!writer) return fail("AVAssetWriter: " + str(err.localizedDescription));
        NSMutableDictionary* settings = [@{
            AVVideoWidthKey : @(spec.width),
            AVVideoHeightKey : @(spec.height),
            AVVideoColorPropertiesKey : @{
                AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_709_2,
                AVVideoTransferFunctionKey : AVVideoTransferFunction_ITU_R_709_2,
                AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_709_2,
            },
        } mutableCopy];
        switch (spec.codec) {
            case TestCodec::H264: settings[AVVideoCodecKey] = AVVideoCodecTypeH264; break;
            case TestCodec::HEVC: settings[AVVideoCodecKey] = AVVideoCodecTypeHEVC; break;
            case TestCodec::ProRes422: settings[AVVideoCodecKey] = AVVideoCodecTypeAppleProRes422; break;
        }
        if (spec.codec != TestCodec::ProRes422) {
            const double bitsPerPixel = spec.codec == TestCodec::H264 ? 0.25 : 0.15;
            settings[AVVideoCompressionPropertiesKey] = @{
                AVVideoAverageBitRateKey : @(std::llround(double(spec.width) * spec.height * spec.fps * bitsPerPixel)),
                AVVideoMaxKeyFrameIntervalKey : @(std::max(1, int(std::lround(spec.fps)))),
            };
        }
        AVAssetWriterInput* input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                                       outputSettings:settings];
        input.expectsMediaDataInRealTime = NO;
        AVAssetWriterInputPixelBufferAdaptor* adaptor = [AVAssetWriterInputPixelBufferAdaptor
            assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input
                                       sourcePixelBufferAttributes:@{
                                           (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
                                           (id)kCVPixelBufferWidthKey : @(spec.width),
                                           (id)kCVPixelBufferHeightKey : @(spec.height),
                                       }];
        if (![writer canAddInput:input]) return fail("the writer refuses this codec/size");
        [writer addInput:input];
        if (![writer startWriting]) return fail("startWriting: " + str(writer.error.localizedDescription));
        [writer startSessionAtSourceTime:kCMTimeZero];
        const int32_t timescale = int32_t(std::lround(spec.fps * 1000.0));
        for (int i = 0; i < spec.frames; ++i) {
            @autoreleasepool {
                while (!input.readyForMoreMediaData) {
                    if (writer.status == AVAssetWriterStatusFailed)
                        return fail("writer: " + str(writer.error.localizedDescription));
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                CVPixelBufferRef pb = nullptr;
                if (CVPixelBufferPoolCreatePixelBuffer(nullptr, adaptor.pixelBufferPool, &pb) != kCVReturnSuccess || !pb)
                    return fail("no pixel buffer");
                CVPixelBufferLockBaseAddress(pb, 0);
                auto* base = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(pb));
                const std::size_t bpr = CVPixelBufferGetBytesPerRow(pb);
                if (spec.pattern == TestPattern::Card) fillCard(base, bpr, spec.width, spec.height, i);
                else fillMotion(base, bpr, spec.width, spec.height, i);
                CVPixelBufferUnlockBaseAddress(pb, 0);
                const BOOL ok = [adaptor appendPixelBuffer:pb withPresentationTime:CMTimeMake(int64_t(i) * 1000, timescale)];
                CVPixelBufferRelease(pb);
                if (!ok) return fail("append: " + str(writer.error.localizedDescription));
            }
        }
        [input markAsFinished];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [writer finishWritingWithCompletionHandler:^{
            dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        if (writer.status != AVAssetWriterStatusCompleted)
            return fail("finishWriting: " + str(writer.error.localizedDescription));
    }
    return true;
}

}  // namespace reforce::video

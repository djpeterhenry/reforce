// reforce-body-probe: can Apple Vision find a body in the webcam fast and reliably enough to drive visuals?
// Standalone (no reforce code). Captures a camera (or reads a movie file) and runs, per frame:
//   - person segmentation (silhouette mask) at the chosen quality levels (fast / balanced / accurate),
//     each through its own VNSequenceRequestHandler (temporal smoothing, as a live tracker would use it);
//   - 2D body pose (19 joints with confidence);
//   - optionally 3D body pose (--pose3d).
// Prints one line per second (scene brightness, camera fps, ms per request, mask coverage, joints found)
// so you can walk around / change the lights and watch it react; at the end a summary (also summary.txt).
// Every --every seconds it writes snapshots to --out: the camera frame, each quality's mask, and an overlay
// (mask tinted red, skeleton in green: bright = confident). Snapshots stay local (they show you).
//
//   reforce-body-probe [--camera BRIO] [--file clip.mov] [--seconds 30] [--size 720|1080|4k]
//                      [--quality fast,balanced,accurate] [--pose3d] [--no-pose] [--every 3] [--out DIR] [--window]
//
// --window: live preview (mask in red, skeleton in green; c = hide the camera image, m = mirror, q / Esc / close
// = stop). Nothing is saved. --seconds 0 = run until stopped. Ctrl-C stops early and still prints the summary.
//
// --camera: name substring or unique id ("default" = the system default). --file instead of a camera:
// every frame of the movie is processed (not real time), so the same clip can be re-run.
// Exit code 0 = frames were processed.

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <CoreImage/CoreImage.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <Vision/Vision.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <functional>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Series {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    double mean() const {
        if (v.empty()) return 0;
        double s = 0;
        for (double x : v) s += x;
        return s / v.size();
    }
    double pct(double p) const {
        if (v.empty()) return 0;
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        return s[std::min(s.size() - 1, size_t(p * (s.size() - 1) + 0.5))];
    }
    double max() const { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }
};

struct Options {
    std::string camera = "BRIO";
    std::string file;
    double seconds = 30;
    std::string size = "1080";
    std::vector<std::string> qualities = {"fast", "balanced", "accurate"};
    bool pose = true;
    bool pose3d = false;
    bool window = false;
    double every = 3;
    std::string out;
};

struct Joint {
    VNHumanBodyPoseObservationJointName name;
    const char* label;
};

std::vector<Joint> allJoints() {
    return {
        {VNHumanBodyPoseObservationJointNameNose, "nose"},
        {VNHumanBodyPoseObservationJointNameLeftEye, "l_eye"},
        {VNHumanBodyPoseObservationJointNameRightEye, "r_eye"},
        {VNHumanBodyPoseObservationJointNameLeftEar, "l_ear"},
        {VNHumanBodyPoseObservationJointNameRightEar, "r_ear"},
        {VNHumanBodyPoseObservationJointNameNeck, "neck"},
        {VNHumanBodyPoseObservationJointNameLeftShoulder, "l_shoulder"},
        {VNHumanBodyPoseObservationJointNameRightShoulder, "r_shoulder"},
        {VNHumanBodyPoseObservationJointNameLeftElbow, "l_elbow"},
        {VNHumanBodyPoseObservationJointNameRightElbow, "r_elbow"},
        {VNHumanBodyPoseObservationJointNameLeftWrist, "l_wrist"},
        {VNHumanBodyPoseObservationJointNameRightWrist, "r_wrist"},
        {VNHumanBodyPoseObservationJointNameRoot, "root"},
        {VNHumanBodyPoseObservationJointNameLeftHip, "l_hip"},
        {VNHumanBodyPoseObservationJointNameRightHip, "r_hip"},
        {VNHumanBodyPoseObservationJointNameLeftKnee, "l_knee"},
        {VNHumanBodyPoseObservationJointNameRightKnee, "r_knee"},
        {VNHumanBodyPoseObservationJointNameLeftAnkle, "l_ankle"},
        {VNHumanBodyPoseObservationJointNameRightAnkle, "r_ankle"},
    };
}

const std::vector<std::pair<int, int>> kBones = {
    {0, 5},  {5, 6},  {5, 7},   {6, 8},   {8, 10},  {7, 9},   {9, 11},  {5, 12}, {12, 13},
    {12, 14}, {13, 15}, {15, 17}, {14, 16}, {16, 18}, {0, 1},  {0, 2},  {1, 3},  {2, 4},
};

constexpr float kConfident = 0.3f;  // a joint counts as found at or above this confidence

VNGeneratePersonSegmentationRequestQualityLevel qualityLevel(const std::string& q) {
    if (q == "fast") return VNGeneratePersonSegmentationRequestQualityLevelFast;
    if (q == "accurate") return VNGeneratePersonSegmentationRequestQualityLevelAccurate;
    return VNGeneratePersonSegmentationRequestQualityLevelBalanced;
}

// Mean of a sample grid of a pixel buffer: BGRA -> luma (Rec. 709), OneComponent8 -> value. 0..1.
double meanValue(CVPixelBufferRef pb) {
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const auto* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pb));
    size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb), row = CVPixelBufferGetBytesPerRow(pb);
    bool bgra = CVPixelBufferGetPixelFormatType(pb) == kCVPixelFormatType_32BGRA;
    size_t step = std::max<size_t>(1, std::min(w, h) / 90);
    double sum = 0;
    size_t n = 0;
    for (size_t y = 0; y < h; y += step)
        for (size_t x = 0; x < w; x += step) {
            if (bgra) {
                const uint8_t* p = base + y * row + x * 4;
                sum += 0.0722 * p[0] + 0.7152 * p[1] + 0.2126 * p[2];
            } else {
                sum += base[y * row + x];
            }
            ++n;
        }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    return n ? sum / n / 255.0 : 0;
}

bool writeImage(CGImageRef img, const std::string& path, bool jpeg) {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CFStringRef type = (__bridge CFStringRef)(jpeg ? UTTypeJPEG.identifier : UTTypePNG.identifier);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, type, 1, nullptr);
    if (!dst) return false;
    NSDictionary* props = jpeg ? @{(id)kCGImageDestinationLossyCompressionQuality : @0.85} : @{};
    CGImageDestinationAddImage(dst, img, (__bridge CFDictionaryRef)props);
    bool ok = CGImageDestinationFinalize(dst);
    CFRelease(dst);
    return ok;
}

struct PoseResult {
    bool found = false;
    int people = 0;
    std::vector<CGPoint> pts;  // normalized, lower-left origin
    std::vector<float> conf;
};

class Probe {
public:
    explicit Probe(const Options& o) : opt_(o), joints_(allJoints()) {
        ci_ = [CIContext contextWithOptions:nil];
        for (auto& q : opt_.qualities) {
            auto* r = [[VNGeneratePersonSegmentationRequest alloc] init];
            r.qualityLevel = qualityLevel(q);
            r.outputPixelFormat = kCVPixelFormatType_OneComponent8;
            segReq_.push_back(r);
            segSeq_.push_back([[VNSequenceRequestHandler alloc] init]);
            segMs_.emplace_back();
            cover_.emplace_back();
            secSegMs_.emplace_back();
            maskSize_.emplace_back("?");
        }
        poseReq_ = [[VNDetectHumanBodyPoseRequest alloc] init];
        if (@available(macOS 14.0, *)) pose3dReq_ = [[VNDetectHumanBodyPose3DRequest alloc] init];
        jointHits_.assign(joints_.size(), 0);
        jointConf_.assign(joints_.size(), Series{});
    }

    // One frame (capture queue or file loop). t = seconds since the first frame.
    void process(CVPixelBufferRef pb, double t) {
        auto tFrame = Clock::now();
        ++frames_;
        ++secFrames_;
        double lum = meanValue(pb);
        lum_.add(lum);
        secLum_ = lum;
        if (width_ == 0) {
            width_ = int(CVPixelBufferGetWidth(pb));
            height_ = int(CVPixelBufferGetHeight(pb));
        }

        std::vector<CVPixelBufferRef> masks(segReq_.size(), nullptr);
        for (size_t i = 0; i < segReq_.size(); ++i) {
            NSError* err = nil;
            auto t0 = Clock::now();
            [segSeq_[i] performRequests:@[ segReq_[i] ] onCVPixelBuffer:pb error:&err];
            double ms = msSince(t0);
            segMs_[i].add(ms);
            secSegMs_[i] = std::max(secSegMs_[i], ms);
            VNPixelBufferObservation* obs = segReq_[i].results.firstObject;
            if (err || !obs) continue;
            masks[i] = obs.pixelBuffer;
            double c = meanValue(obs.pixelBuffer);
            cover_[i].add(c);
            maskSize_[i] = std::to_string(CVPixelBufferGetWidth(obs.pixelBuffer)) + "x" +
                           std::to_string(CVPixelBufferGetHeight(obs.pixelBuffer));
            if (i == segReq_.size() / 2) secCover_ = c;
        }

        PoseResult pose;
        if (opt_.pose || opt_.pose3d) {
            VNImageRequestHandler* h = [[VNImageRequestHandler alloc] initWithCVPixelBuffer:pb options:@{}];
            if (opt_.pose) {
                NSError* err = nil;
                auto t0 = Clock::now();
                [h performRequests:@[ poseReq_ ] error:&err];
                poseMs_.add(msSince(t0));
                pose = readPose();
            }
            if (opt_.pose3d && pose3dReq_) {
                NSError* err = nil;
                auto t0 = Clock::now();
                [h performRequests:@[ pose3dReq_ ] error:&err];
                pose3dMs_.add(msSince(t0));
                if (pose3dReq_.results.count) ++pose3dFound_;
            }
        }
        if (pose.found) {
            ++poseFrames_;
            int n = 0;
            for (size_t j = 0; j < joints_.size(); ++j) {
                jointConf_[j].add(pose.conf[j]);
                if (pose.conf[j] >= kConfident) {
                    ++jointHits_[j];
                    ++n;
                }
            }
            jointsPerFrame_.add(n);
            secJoints_ = n;
            maxPeople_ = std::max(maxPeople_, pose.people);
        } else {
            jointsPerFrame_.add(0);
            secJoints_ = 0;
        }
        totalMs_.add(msSince(tFrame));

        if (t >= nextSecond_) {
            printSecond(t);
            nextSecond_ = std::floor(t) + 1;
        }
        if (!opt_.out.empty() && t >= nextDump_) {
            dump(pb, masks, pose, t);
            nextDump_ = t + opt_.every;
        }
        if (onPreview && !previewBusy.exchange(true)) {
            size_t W = 960, H = 960 * CVPixelBufferGetHeight(pb) / CVPixelBufferGetWidth(pb);
            CGImageRef img = renderOverlay(pb, masks.empty() ? nullptr : masks[masks.size() / 2], pose, W, H,
                                           showCamera, mirror);
            onPreview(img, status);
        }
    }

    void cameraFrameDropped() { ++dropped_; }

    void summary(double wallSeconds) {
        std::string s;
        char line[512];
#define add(...) (snprintf(line, sizeof line, __VA_ARGS__), s += line)
        add("\n== reforce-body-probe summary ==\n");
        add("source: %s  %dx%d  frames processed: %llu", source_.c_str(), width_, height_,
            (unsigned long long)frames_);
        if (wallSeconds > 0)
            add("  (%.1f fps processed; camera delivered %.1f fps, %llu dropped while busy)",
                frames_ / wallSeconds, (frames_ + dropped_) / wallSeconds, (unsigned long long)dropped_);
        add("\nbrightness (luma 0..1): mean %.2f  min %.2f  max %.2f\n\n", lum_.mean(), lum_.pct(0), lum_.max());
        add("%-22s %8s %8s %8s %8s   %s\n", "request", "mean ms", "p50", "p95", "max", "notes");
        for (size_t i = 0; i < segReq_.size(); ++i)
            add("seg %-18s %8.1f %8.1f %8.1f %8.1f   mask %s, coverage mean %.0f%%\n", opt_.qualities[i].c_str(),
                segMs_[i].mean(), segMs_[i].pct(.5), segMs_[i].pct(.95), segMs_[i].max(), maskSize_[i].c_str(),
                cover_[i].mean() * 100);
        if (opt_.pose)
            add("%-22s %8.1f %8.1f %8.1f %8.1f   person in %.0f%% of frames, up to %d people\n", "pose 2D",
                poseMs_.mean(), poseMs_.pct(.5), poseMs_.pct(.95), poseMs_.max(),
                frames_ ? 100.0 * poseFrames_ / frames_ : 0.0, maxPeople_);
        if (opt_.pose3d)
            add("%-22s %8.1f %8.1f %8.1f %8.1f   person in %.0f%% of frames\n", "pose 3D", pose3dMs_.mean(),
                pose3dMs_.pct(.5), pose3dMs_.pct(.95), pose3dMs_.max(),
                frames_ ? 100.0 * pose3dFound_ / frames_ : 0.0);
        add("%-22s %8.1f %8.1f %8.1f %8.1f   all of the above, one after another\n", "whole frame",
            totalMs_.mean(), totalMs_.pct(.5), totalMs_.pct(.95), totalMs_.max());
        if (opt_.pose && frames_) {
            add("\njoints (found = confidence >= %.1f; %% of all frames, mean confidence when a person was seen)\n",
                kConfident);
            for (size_t j = 0; j < joints_.size(); ++j)
                add("  %-11s %5.0f%%  %.2f%s", joints_[j].label, 100.0 * jointHits_[j] / frames_,
                    jointConf_[j].mean(), j % 3 == 2 ? "\n" : "   ");
            add("\n  joints per frame: mean %.1f of 19\n", jointsPerFrame_.mean());
        }
#undef add
        fputs(s.c_str(), stdout);
        if (!opt_.out.empty()) {
            FILE* f = fopen((opt_.out + "/summary.txt").c_str(), "w");
            if (f) {
                fputs(s.c_str(), f);
                fclose(f);
            }
        }
    }

    void setSource(std::string s) { source_ = std::move(s); }
    uint64_t frames() const { return frames_; }

private:
    PoseResult readPose() {
        PoseResult r;
        NSArray<VNHumanBodyPoseObservation*>* obs = poseReq_.results;
        r.people = int(obs.count);
        // The person with the most confident joints (the probe follows one body, like the tracker would).
        float best = -1;
        for (VNHumanBodyPoseObservation* o in obs) {
            NSError* err = nil;
            NSDictionary* pts = [o recognizedPointsForJointsGroupName:VNHumanBodyPoseObservationJointsGroupNameAll
                                                                 error:&err];
            if (!pts) continue;
            std::vector<CGPoint> p(joints_.size(), CGPointZero);
            std::vector<float> c(joints_.size(), 0);
            float sum = 0;
            for (size_t j = 0; j < joints_.size(); ++j) {
                VNRecognizedPoint* rp = pts[joints_[j].name];
                if (!rp) continue;
                p[j] = rp.location;
                c[j] = rp.confidence;
                sum += rp.confidence;
            }
            if (sum > best) {
                best = sum;
                r.pts = p;
                r.conf = c;
                r.found = true;
            }
        }
        return r;
    }

    void printSecond(double t) {
        std::string seg;
        char buf[64];
        for (size_t i = 0; i < segReq_.size(); ++i) {
            snprintf(buf, sizeof buf, " %s %4.1f", opt_.qualities[i].substr(0, 3).c_str(), secSegMs_[i]);
            seg += buf;
            secSegMs_[i] = 0;
        }
        printf("t=%5.1fs  fps %3llu  light %.2f  seg ms(max)%s  pose %4.1f ms  person %3.0f%%  joints %2d/19\n", t,
               (unsigned long long)secFrames_, secLum_, seg.c_str(), poseMs_.v.empty() ? 0 : poseMs_.v.back(),
               secCover_ * 100, secJoints_);
        fflush(stdout);
        snprintf(buf, sizeof buf, "fps %llu  light %.2f  person %.0f%%  joints %d/19", (unsigned long long)secFrames_,
                 secLum_, secCover_ * 100, secJoints_);
        status = buf;
        secFrames_ = 0;
    }

    void dump(CVPixelBufferRef pb, const std::vector<CVPixelBufferRef>& masks, const PoseResult& pose, double t) {
        char tbuf[16];
        snprintf(tbuf, sizeof tbuf, "/t%05.1f", t);
        std::string stem = opt_.out + tbuf;
        CIImage* frame = [CIImage imageWithCVPixelBuffer:pb];
        CGImageRef frameImg = [ci_ createCGImage:frame fromRect:frame.extent];
        writeImage(frameImg, stem + "_camera.jpg", true);
        CGImageRelease(frameImg);
        for (size_t i = 0; i < masks.size(); ++i) {
            if (!masks[i]) continue;
            CGImageRef mi = maskImage(masks[i]);
            writeImage(mi, stem + "_mask_" + opt_.qualities[i] + ".png", false);
            CGImageRelease(mi);
        }
        CGImageRef overlay = renderOverlay(pb, masks.empty() ? nullptr : masks[masks.size() / 2], pose,
                                           CVPixelBufferGetWidth(pb), CVPixelBufferGetHeight(pb), true, false);
        writeImage(overlay, stem + "_overlay.jpg", true);
        CGImageRelease(overlay);
    }

    CGImageRef maskImage(CVPixelBufferRef mask) {
        CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
        CIImage* m = [CIImage imageWithCVPixelBuffer:mask];
        CGImageRef mi = [ci_ createCGImage:m fromRect:m.extent format:kCIFormatL8 colorSpace:gray];
        CGColorSpaceRelease(gray);
        return mi;
    }

    // Camera frame (dimmed, or black when !showCamera) + the mask tinted red + the skeleton (green = confident,
    // yellow dots = weak joints), W x H, optionally mirrored (selfie view). Caller releases.
    CGImageRef renderOverlay(CVPixelBufferRef pb, CVPixelBufferRef mask, const PoseResult& pose, size_t W, size_t H,
                             bool showCamera, bool mirror) {
        CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(nullptr, W, H, 8, 0, rgb, kCGImageAlphaPremultipliedLast);
        CGColorSpaceRelease(rgb);
        if (mirror) {
            CGContextTranslateCTM(ctx, W, 0);
            CGContextScaleCTM(ctx, -1, 1);
        }
        CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
        CGContextFillRect(ctx, CGRectMake(0, 0, W, H));
        if (showCamera) {
            CIImage* frame = [CIImage imageWithCVPixelBuffer:pb];
            CGImageRef frameImg = [ci_ createCGImage:frame fromRect:frame.extent];
            CGContextSetInterpolationQuality(ctx, kCGInterpolationLow);
            CGContextDrawImage(ctx, CGRectMake(0, 0, W, H), frameImg);
            CGImageRelease(frameImg);
            CGContextSetRGBFillColor(ctx, 0, 0, 0, 0.35);
            CGContextFillRect(ctx, CGRectMake(0, 0, W, H));
        }
        if (mask) {
            CGImageRef mi = maskImage(mask);
            CGContextSaveGState(ctx);
            CGContextClipToMask(ctx, CGRectMake(0, 0, W, H), mi);
            if (showCamera) CGContextSetRGBFillColor(ctx, 1, 0.1, 0.1, 0.5);
            else CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
            CGContextFillRect(ctx, CGRectMake(0, 0, W, H));
            CGContextRestoreGState(ctx);
            CGImageRelease(mi);
        }
        if (pose.found) {
            CGFloat lw = std::max<CGFloat>(3, W / 250.0);
            CGContextSetLineWidth(ctx, lw);
            CGContextSetLineCap(ctx, kCGLineCapRound);
            auto at = [&](int j) { return CGPointMake(pose.pts[j].x * W, pose.pts[j].y * H); };
            for (auto [a, b] : kBones) {
                if (pose.conf[a] < kConfident || pose.conf[b] < kConfident) continue;
                CGContextSetRGBStrokeColor(ctx, 0.2, 1, 0.3, 0.4 + 0.6 * std::min(pose.conf[a], pose.conf[b]));
                CGContextMoveToPoint(ctx, at(a).x, at(a).y);
                CGContextAddLineToPoint(ctx, at(b).x, at(b).y);
                CGContextStrokePath(ctx);
            }
            for (size_t j = 0; j < pose.pts.size(); ++j) {
                if (pose.conf[j] <= 0) continue;
                bool ok = pose.conf[j] >= kConfident;
                CGContextSetRGBFillColor(ctx, ok ? 0.2 : 1, ok ? 1 : 0.8, ok ? 0.3 : 0, 1);
                CGPoint p = at(int(j));
                CGContextFillEllipseInRect(ctx, CGRectMake(p.x - lw * 1.5, p.y - lw * 1.5, lw * 3, lw * 3));
            }
        }
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        CGContextRelease(ctx);
        return img;
    }

public:
    // Live preview (--window): called on the capture queue with a new overlay (the callee releases it) and a
    // status line. Set before capture starts; the flags are toggled from the main thread.
    std::function<void(CGImageRef, std::string)> onPreview;
    std::atomic<bool> previewBusy{false}, showCamera{true}, mirror{true};
    std::string status;

private:
    Options opt_;
    std::vector<Joint> joints_;
    CIContext* ci_;
    std::vector<VNGeneratePersonSegmentationRequest*> segReq_;
    std::vector<VNSequenceRequestHandler*> segSeq_;
    VNDetectHumanBodyPoseRequest* poseReq_;
    VNDetectHumanBodyPose3DRequest* pose3dReq_ API_AVAILABLE(macos(14.0)) = nil;
    std::vector<Series> segMs_, cover_;
    std::vector<double> secSegMs_;
    std::vector<std::string> maskSize_;
    Series poseMs_, pose3dMs_, totalMs_, lum_, jointsPerFrame_;
    std::vector<int> jointHits_;
    std::vector<Series> jointConf_;
    uint64_t frames_ = 0, dropped_ = 0, poseFrames_ = 0, pose3dFound_ = 0, secFrames_ = 0;
    int maxPeople_ = 0, secJoints_ = 0, width_ = 0, height_ = 0;
    double secLum_ = 0, secCover_ = 0, nextSecond_ = 1, nextDump_ = 0.5;
    std::string source_;
};

}  // namespace

@interface RFProbeDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic) Probe* probe;
@property(nonatomic) double startTime;
@end

@implementation RFProbeDelegate
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sb
           fromConnection:(AVCaptureConnection*)connection {
    CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
    if (!pb) return;
    double now = CACurrentMediaTime();
    if (self.startTime == 0) self.startTime = now;
    @autoreleasepool {
        self.probe->process(pb, now - self.startTime);
    }
}
- (void)captureOutput:(AVCaptureOutput*)output
    didDropSampleBuffer:(CMSampleBufferRef)sb
         fromConnection:(AVCaptureConnection*)connection {
    self.probe->cameraFrameDropped();
}
@end

static AVCaptureDevice* findCamera(const std::string& id) {
    if (id == "default") return [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    NSString* want = [NSString stringWithUTF8String:id.c_str()];
    if (AVCaptureDevice* d = [AVCaptureDevice deviceWithUniqueID:want]) return d;
    AVCaptureDeviceDiscoverySession* ds = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal,
                                           AVCaptureDeviceTypeContinuityCamera ]
                              mediaType:AVMediaTypeVideo
                               position:AVCaptureDevicePositionUnspecified];
    for (AVCaptureDevice* d in ds.devices)
        if ([d.localizedName rangeOfString:want options:NSCaseInsensitiveSearch].location != NSNotFound) return d;
    fprintf(stderr, "cameras:\n");
    for (AVCaptureDevice* d in ds.devices) fprintf(stderr, "  %s  (%s)\n", d.localizedName.UTF8String, d.uniqueID.UTF8String);
    return nil;
}

static int runCamera(const Options& opt, Probe& probe) {
    __block BOOL granted = NO;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                             completionHandler:^(BOOL ok) {
                               granted = ok;
                               dispatch_semaphore_signal(sem);
                             }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (!granted) {
        fprintf(stderr, "camera access denied (System Settings > Privacy & Security > Camera: allow the app this "
                        "runs in, e.g. Terminal or Visual Studio Code)\n");
        return 1;
    }
    AVCaptureDevice* cam = findCamera(opt.camera);
    if (!cam) {
        fprintf(stderr, "no camera matching \"%s\"\n", opt.camera.c_str());
        return 1;
    }
    AVCaptureSession* session = [[AVCaptureSession alloc] init];
    AVCaptureSessionPreset preset = opt.size == "4k"    ? AVCaptureSessionPreset3840x2160
                                    : opt.size == "720" ? AVCaptureSessionPreset1280x720
                                                        : AVCaptureSessionPreset1920x1080;
    if ([session canSetSessionPreset:preset]) session.sessionPreset = preset;
    NSError* err = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:cam error:&err];
    if (!input || ![session canAddInput:input]) {
        fprintf(stderr, "cannot open %s: %s\n", cam.localizedName.UTF8String,
                err ? err.localizedDescription.UTF8String : "refused");
        return 1;
    }
    [session addInput:input];
    AVCaptureVideoDataOutput* out = [[AVCaptureVideoDataOutput alloc] init];
    out.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
    out.alwaysDiscardsLateVideoFrames = YES;  // like the live tracker: always the newest frame
    RFProbeDelegate* del = [[RFProbeDelegate alloc] init];
    del.probe = &probe;
    dispatch_queue_t q = dispatch_queue_create("reforce.body-probe", DISPATCH_QUEUE_SERIAL);
    [out setSampleBufferDelegate:del queue:q];
    if (![session canAddOutput:out]) {
        fprintf(stderr, "camera output refused\n");
        return 1;
    }
    [session addOutput:out];
    probe.setSource(std::string("camera ") + cam.localizedName.UTF8String);
    printf("camera: %s, preset %s, %s\n", cam.localizedName.UTF8String, preset.UTF8String,
           opt.seconds > 0 ? (std::to_string(int(opt.seconds)) + " s (Ctrl-C stops early)").c_str()
                           : "until Ctrl-C or the window is closed");

    // Ctrl-C ends the run normally (summary still printed).
    static std::atomic<bool> stop{false};
    signal(SIGINT, SIG_IGN);
    dispatch_source_t sigint = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(sigint, ^{
      stop = true;
    });
    dispatch_resume(sigint);

    NSWindow* window = nil;
    CALayer* layer = nil;
    if (opt.window) {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 960, 540)
                                             styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                       NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        window.releasedWhenClosed = NO;
        window.title = @"reforce body probe";
        window.contentAspectRatio = NSMakeSize(16, 9);
        window.contentView.wantsLayer = YES;
        layer = window.contentView.layer;
        layer.backgroundColor = NSColor.blackColor.CGColor;
        layer.contentsGravity = kCAGravityResizeAspect;
        [window center];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        Probe* pp = &probe;
        probe.onPreview = [pp, layer, window](CGImageRef img, std::string status) {
            NSString* title = [NSString stringWithFormat:@"%s   ·   c camera %s · m mirror %s",
                                                         status.empty() ? "starting" : status.c_str(),
                                                         pp->showCamera ? "on" : "off", pp->mirror ? "on" : "off"];
            dispatch_async(dispatch_get_main_queue(), ^{
              [CATransaction begin];
              [CATransaction setDisableActions:YES];
              layer.contents = (__bridge id)img;
              [CATransaction commit];
              CGImageRelease(img);
              window.title = title;
              pp->previewBusy = false;
            });
        };
        printf("window: c = camera image on/off (off: only what the tracker sees), m = mirror, close it to stop\n");
    }

    [session startRunning];
    double t0 = CACurrentMediaTime();
    while (!stop) {
        if (opt.seconds > 0 && CACurrentMediaTime() - t0 >= opt.seconds) break;
        if (window && !window.visible) break;
        if (opt.window) {
            NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (!e) continue;
            if (e.type == NSEventTypeKeyDown) {
                NSString* k = e.charactersIgnoringModifiers.lowercaseString;
                if ([k isEqualToString:@"c"]) probe.showCamera = !probe.showCamera;
                else if ([k isEqualToString:@"m"]) probe.mirror = !probe.mirror;
                else if ([k isEqualToString:@"q"] || e.keyCode == 53) break;  // q / Esc
                continue;
            }
            [NSApp sendEvent:e];
        } else {
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        }
    }
    [session stopRunning];
    dispatch_sync(q, ^{
                  });
    double wall = CACurrentMediaTime() - std::max(t0, del.startTime);
    probe.summary(wall);
    return probe.frames() ? 0 : 1;
}

static int runFile(const Options& opt, Probe& probe) {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:opt.file.c_str()]];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
    __block NSArray<AVAssetTrack*>* tracks = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [asset loadTracksWithMediaType:AVMediaTypeVideo
                 completionHandler:^(NSArray<AVAssetTrack*>* t, NSError* e) {
                   tracks = t;
                   dispatch_semaphore_signal(sem);
                 }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (!tracks.count) {
        fprintf(stderr, "no video track in %s\n", opt.file.c_str());
        return 1;
    }
    NSError* err = nil;
    AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&err];
    AVAssetReaderTrackOutput* out = [AVAssetReaderTrackOutput
        assetReaderTrackOutputWithTrack:tracks[0]
                         outputSettings:@{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)}];
    [reader addOutput:out];
    if (![reader startReading]) {
        fprintf(stderr, "cannot read %s\n", opt.file.c_str());
        return 1;
    }
    probe.setSource("file " + opt.file);
    double first = -1;
    double t0 = CACurrentMediaTime();
    while (CMSampleBufferRef sb = [out copyNextSampleBuffer]) {
        @autoreleasepool {
            double t = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb));
            if (first < 0) first = t;
            if (t - first > opt.seconds) {
                CFRelease(sb);
                break;
            }
            if (CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb)) probe.process(pb, t - first);
        }
        CFRelease(sb);
    }
    probe.summary(CACurrentMediaTime() - t0);
    return probe.frames() ? 0 : 1;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        Options opt;
        bool secondsGiven = false;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
            if (a == "--camera") opt.camera = next();
            else if (a == "--file") opt.file = next();
            else if (a == "--seconds") opt.seconds = atof(next().c_str()), secondsGiven = true;
            else if (a == "--size") opt.size = next();
            else if (a == "--every") opt.every = atof(next().c_str());
            else if (a == "--out") opt.out = next();
            else if (a == "--pose3d") opt.pose3d = true;
            else if (a == "--no-pose") opt.pose = false;
            else if (a == "--window") opt.window = true;
            else if (a == "--quality") {
                opt.qualities.clear();
                std::string s = next();
                size_t p = 0;
                while (p <= s.size()) {
                    size_t c = s.find(',', p);
                    std::string q = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
                    if (q == "fast" || q == "balanced" || q == "accurate") opt.qualities.push_back(q);
                    else if (!q.empty() && q != "none") {
                        fprintf(stderr, "unknown quality %s\n", q.c_str());
                        return 2;
                    }
                    if (c == std::string::npos) break;
                    p = c + 1;
                }
            } else {
                fprintf(stderr,
                        "usage: reforce-body-probe [--camera BRIO] [--file clip.mov] [--seconds 30] "
                        "[--size 720|1080|4k] [--quality fast,balanced,accurate|none] [--pose3d] [--no-pose] "
                        "[--every 3] [--out DIR] [--window]\n");
                return 2;
            }
        }
        if (opt.window && !secondsGiven) opt.seconds = 0;  // until the window is closed
        if (!opt.out.empty())
            [NSFileManager.defaultManager createDirectoryAtPath:[NSString stringWithUTF8String:opt.out.c_str()]
                                    withIntermediateDirectories:YES
                                                     attributes:nil
                                                          error:nil];
        Probe probe(opt);
        return opt.file.empty() ? runCamera(opt, probe) : runFile(opt, probe);
    }
}

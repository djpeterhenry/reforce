// Body tracking with Apple Vision (BodyTracker.hpp, docs/body.md).

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include "video/BodyTracker.hpp"

#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>

namespace reforce::video {
namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

NSArray<VNHumanBodyPoseObservationJointName>* jointNames() {
    static NSArray* names = @[
        VNHumanBodyPoseObservationJointNameNose, VNHumanBodyPoseObservationJointNameLeftEye,
        VNHumanBodyPoseObservationJointNameRightEye, VNHumanBodyPoseObservationJointNameLeftEar,
        VNHumanBodyPoseObservationJointNameRightEar, VNHumanBodyPoseObservationJointNameNeck,
        VNHumanBodyPoseObservationJointNameLeftShoulder, VNHumanBodyPoseObservationJointNameRightShoulder,
        VNHumanBodyPoseObservationJointNameLeftElbow, VNHumanBodyPoseObservationJointNameRightElbow,
        VNHumanBodyPoseObservationJointNameLeftWrist, VNHumanBodyPoseObservationJointNameRightWrist,
        VNHumanBodyPoseObservationJointNameRoot, VNHumanBodyPoseObservationJointNameLeftHip,
        VNHumanBodyPoseObservationJointNameRightHip, VNHumanBodyPoseObservationJointNameLeftKnee,
        VNHumanBodyPoseObservationJointNameRightKnee, VNHumanBodyPoseObservationJointNameLeftAnkle,
        VNHumanBodyPoseObservationJointNameRightAnkle,
    ];
    return names;
}

}  // namespace

struct BodyTracker::Impl {
    dispatch_queue_t queue = dispatch_queue_create("reforce.body", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_t poseQueue = dispatch_queue_create("reforce.body.pose", DISPATCH_QUEUE_SERIAL);
    std::atomic<bool> busy{false};
    std::atomic<int> quality{int(BodyQuality::Balanced)};
    std::atomic<bool> poseOn{true};
    std::atomic<std::uint64_t> generation{0};  // reset() bumps it: results of older frames are dropped

    mutable std::mutex m;
    BodyResult result;
    std::uint64_t seq = 0;
    std::uint64_t processed = 0, skipped = 0;
    struct Timing {
        Clock::time_point at;
        double total, mask, pose;
    };
    std::deque<Timing> recent;  // over the last second
    std::string error;

    std::mutex offerM;  // offer() from the capture thread, reset() from the render thread
    const void* lastSource = nullptr;
    std::uint64_t lastIndex = ~std::uint64_t(0);

    // Tracker queues only (seg* on `queue`, pose* on `poseQueue`).
    VNSequenceRequestHandler* handler = nil;  // temporal smoothing across frames for the mask
    VNSequenceRequestHandler* poseHandler = nil;
    VNGeneratePersonSegmentationRequest* seg = nil;
    VNDetectHumanBodyPoseRequest* pose = nil;
    int segQuality = -1;

    void process(const FramePtr& frame, std::uint64_t gen) {
        @autoreleasepool {
            auto pb = static_cast<CVPixelBufferRef>(frame->pixelBuffer());
            if (!handler) handler = [[VNSequenceRequestHandler alloc] init];
            const int q = quality.load();
            if (!seg || q != segQuality) {
                seg = [[VNGeneratePersonSegmentationRequest alloc] init];
                seg.qualityLevel = q == int(BodyQuality::Fast) ? VNGeneratePersonSegmentationRequestQualityLevelFast
                                                               : VNGeneratePersonSegmentationRequestQualityLevelBalanced;
                seg.outputPixelFormat = kCVPixelFormatType_OneComponent8;
                segQuality = q;
            }
            BodyResult r;
            r.frameTime = frame->time();
            r.frame = frame;
            // The pose on its own queue, at the same time as the mask.
            const bool wantPose = poseOn.load();
            __block std::array<BodyPoint, kBodyJoints> joints{};
            __block bool person = false;
            __block double poseMs = 0;
            __block std::string poseErr;
            dispatch_group_t group = dispatch_group_create();
            if (wantPose) {
                FramePtr keep = frame;
                dispatch_group_async(group, poseQueue, ^{
                  @autoreleasepool {
                      if (!poseHandler) {
                          poseHandler = [[VNSequenceRequestHandler alloc] init];
                          pose = [[VNDetectHumanBodyPoseRequest alloc] init];
                      }
                      auto ppb = static_cast<CVPixelBufferRef>(keep->pixelBuffer());
                      NSError* perr = nil;
                      const auto p0 = Clock::now();
                      [poseHandler performRequests:@[ pose ] onCVPixelBuffer:ppb error:&perr];
                      poseMs = msSince(p0);
                      if (perr) poseErr = std::string("pose: ") + perr.localizedDescription.UTF8String;
                      // The person with the most confident joints.
                      float best = 0;
                      NSArray* names = jointNames();
                      for (VNHumanBodyPoseObservation* o in pose.results) {
                          NSDictionary* pts =
                              [o recognizedPointsForJointsGroupName:VNHumanBodyPoseObservationJointsGroupNameAll
                                                              error:nil];
                          if (!pts) continue;
                          std::array<BodyPoint, kBodyJoints> j{};
                          float sum = 0;
                          for (int i = 0; i < kBodyJoints; ++i) {
                              VNRecognizedPoint* p = pts[names[NSUInteger(i)]];
                              if (!p || p.confidence <= 0) continue;
                              j[std::size_t(i)] = {float(p.location.x), float(1.0 - p.location.y), p.confidence};
                              sum += p.confidence;
                          }
                          if (sum > best) {
                              best = sum;
                              joints = j;
                              person = true;
                          }
                      }
                  }
                });
            }
            const auto t0 = Clock::now();
            NSError* err = nil;
            [handler performRequests:@[ seg ] onCVPixelBuffer:pb error:&err];
            r.maskMs = msSince(t0);
            std::string errText;
            if (err) errText = std::string("segmentation: ") + err.localizedDescription.UTF8String;
            if (VNPixelBufferObservation* obs = seg.results.firstObject) {
                CVPixelBufferRef mb = obs.pixelBuffer;
                CVPixelBufferLockBaseAddress(mb, kCVPixelBufferLock_ReadOnly);
                const int w = int(CVPixelBufferGetWidth(mb)), h = int(CVPixelBufferGetHeight(mb));
                const size_t row = CVPixelBufferGetBytesPerRow(mb);
                const auto* base = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(mb));
                auto mask = std::make_shared<std::vector<std::uint8_t>>(std::size_t(w) * std::size_t(h));
                for (int y = 0; y < h; ++y)
                    std::memcpy(mask->data() + std::size_t(y) * std::size_t(w), base + std::size_t(y) * row,
                                std::size_t(w));
                CVPixelBufferUnlockBaseAddress(mb, kCVPixelBufferLock_ReadOnly);
                r.maskW = w;
                r.maskH = h;
                r.mask = std::move(mask);
            }
            dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
            r.poseMs = poseMs;
            r.joints = joints;
            r.person = person;
            if (errText.empty()) errText = poseErr;
            const double total = msSince(t0);
            std::lock_guard<std::mutex> lock(m);
            ++processed;
            const auto now = Clock::now();
            recent.push_back({now, total, r.maskMs, r.poseMs});
            while (!recent.empty() && now - recent.front().at > std::chrono::seconds(1)) recent.pop_front();
            error = errText;
            if (gen != generation.load()) return;  // reset() while this frame was processed
            r.seq = ++seq;
            result = std::move(r);
        }
    }
};

BodyTracker::BodyTracker() : impl_(std::make_shared<Impl>()) {}

BodyTracker::~BodyTracker() = default;  // a frame in flight keeps the Impl alive until it is done

bool BodyTracker::offer(const FramePtr& frame, const void* source) {
    if (!frame || !frame->pixelBuffer()) return false;
    Impl& im = *impl_;
    {
        std::lock_guard<std::mutex> lock(im.offerM);
        if (source == im.lastSource && frame->index() == im.lastIndex) return false;
        if (im.busy.exchange(true)) {
            std::lock_guard<std::mutex> lock2(im.m);
            ++im.skipped;
            return false;
        }
        im.lastSource = source;
        im.lastIndex = frame->index();
    }
    std::shared_ptr<Impl> keep = impl_;
    FramePtr f = frame;  // keeps the pixel buffer until Vision is done
    const std::uint64_t gen = im.generation.load();
    dispatch_async(im.queue, ^{
      keep->process(f, gen);
      keep->busy = false;
    });
    return true;
}

BodyResult BodyTracker::latest() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->result;
}

BodyTrackerStats BodyTracker::stats() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    BodyTrackerStats s;
    s.processed = impl_->processed;
    s.skipped = impl_->skipped;
    s.error = impl_->error;
    const auto now = Clock::now();
    double sum = 0, mask = 0, pose = 0;
    int n = 0;
    for (const auto& t : impl_->recent)
        if (now - t.at <= std::chrono::seconds(1)) {
            sum += t.total;
            mask += t.mask;
            pose += t.pose;
            ++n;
        }
    s.fps = n;
    s.msAvg = n ? sum / n : 0;
    s.maskMsAvg = n ? mask / n : 0;
    s.poseMsAvg = n ? pose / n : 0;
    return s;
}

void BodyTracker::setQuality(BodyQuality q) { impl_->quality = int(q); }

void BodyTracker::setPose(bool on) { impl_->poseOn = on; }

void BodyTracker::reset() {
    ++impl_->generation;
    {
        std::lock_guard<std::mutex> lock(impl_->offerM);
        impl_->lastSource = nullptr;
        impl_->lastIndex = ~std::uint64_t(0);
    }
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->result = BodyResult{};
}

}  // namespace reforce::video

#pragma once
// Body tracking (docs/body.md): Apple Vision person segmentation (a silhouette mask) and 2D body pose (19 joints)
// on camera or movie frames. Implemented in BodyTracker.mm; this header has no Objective-C.
//
// Frames are offered as they arrive (a live source's frame listener, on the capture thread; movie files from the
// render thread); Vision runs on the tracker's own queues, one frame at a time: a frame offered while the previous
// one is still being processed is skipped, so nobody waits and the result is always about the newest frame. The
// silhouette and the pose (only when asked for: setPose) run at the same time on two queues. The first frame takes
// ~1 s (Vision loads its models), later ones ~10 ms (BRIO 1080p, M4 Pro: mask ~10 ms balanced, pose ~5 ms alongside).
//
// Coordinates are camera-normalized with a top-left origin (x right, y down, 0..1 across the whole camera frame).
// The mask (512 x 384 at balanced, 256 x 192 at fast quality) covers the whole frame whatever its aspect.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "video/VideoSource.hpp"

namespace reforce::video {

// Vision's joints, in this order everywhere (engine/Body.hpp names them).
enum class BodyJoint : int {
    Nose, LeftEye, RightEye, LeftEar, RightEar, Neck, LeftShoulder, RightShoulder, LeftElbow, RightElbow, LeftWrist,
    RightWrist, Root, LeftHip, RightHip, LeftKnee, RightKnee, LeftAnkle, RightAnkle, Count
};
constexpr int kBodyJoints = int(BodyJoint::Count);

struct BodyPoint {
    float x = 0, y = 0;  // camera-normalized, top-left origin
    float conf = 0;      // 0..1 (0 = not found)
};

struct BodyResult {
    std::uint64_t seq = 0;       // 1, 2, ... per processed frame (0 = none yet)
    double frameTime = 0;        // the frame's time (Frame::time)
    bool person = false;         // a body pose was found
    std::array<BodyPoint, kBodyJoints> joints{};
    int maskW = 0, maskH = 0;    // 0 = no mask
    std::shared_ptr<const std::vector<std::uint8_t>> mask;  // maskW * maskH, 0..255, rows top first, tightly packed
    double maskMs = 0, poseMs = 0;  // Vision time for this frame (they run at the same time)
    FramePtr frame;               // the frame it was computed from (the backdrop can show exactly it: in sync)
};

struct BodyTrackerStats {
    double fps = 0;           // processed frames per second (last second)
    double msAvg = 0;         // Vision time per frame (the slower of mask and pose), last second
    double maskMsAvg = 0, poseMsAvg = 0;  // each, last second (pose 0 while off)
    std::uint64_t processed = 0, skipped = 0;
    std::string error;        // last Vision error ("" = none)
};

enum class BodyQuality { Fast, Balanced };

class BodyTracker {
public:
    BodyTracker();
    ~BodyTracker();  // does not wait: a frame in flight finishes on its own and is dropped
    BodyTracker(const BodyTracker&) = delete;
    BodyTracker& operator=(const BodyTracker&) = delete;

    // Any thread: hands `frame` to Vision unless one is in flight or it was offered before (same index from the same
    // source). Frames without a pixel buffer (Syphon) are ignored. Never blocks.
    bool offer(const FramePtr& frame, const void* source);
    BodyResult latest() const;
    BodyTrackerStats stats() const;
    void setQuality(BodyQuality q);  // from the next frame
    void setPose(bool on);           // run the pose too (hand trails, joint marks); from the next frame
    // Forget the last result (a new source): latest() returns seq 0 until the next frame is done.
    void reset();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;  // shared with the block in flight
};

}  // namespace reforce::video

#pragma once
// Body tracking reactions (docs/body.md): settings, pose smoothing, camera -> screen mapping, hand trails.
// Pure C++, unit-tested (tests/test_engine_body.cpp). The tracker itself is video/BodyTracker (Apple Vision); the
// GPU passes are render/ScenePipeline (encodeBodyIntensity, the backdrop pass).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/LineGeometry.hpp"
#include "engine/VideoComposite.hpp"

namespace reforce::engine {

constexpr int kPoseJoints = 19;  // video::BodyJoint order: nose, eyes, ears, neck, shoulders, elbows, wrists, root,
                                 // hips, knees, ankles (left before right)
enum PoseJoint : int {
    JNose = 0, JLeftEye, JRightEye, JLeftEar, JRightEar, JNeck, JLeftShoulder, JRightShoulder, JLeftElbow,
    JRightElbow, JLeftWrist, JRightWrist, JRoot, JLeftHip, JRightHip, JLeftKnee, JRightKnee, JLeftAnkle, JRightAnkle
};
const char* poseJointName(int j);

// The silhouette drawn into the visuals' trail buffer every step (the flow field then carries it).
enum class BodyBurn : int { Off = 0, Fill = 1, Outline = 2, Motion = 3 };
const char* bodyBurnName(BodyBurn b);
std::optional<BodyBurn> parseBodyBurn(const std::string& s);

struct BodySettings {
    bool track = false;            // reforce.Body.Track: run the tracker on the backdrop source
    bool fast = false;             // reforce.Body.Quality = fast (coarser mask, less GPU) | balanced
    double front = 0;              // reforce.Body.Front 0..1: the person cuts through the visuals (camera shows)
    BodyBurn burn = BodyBurn::Off;  // reforce.Body.Burn: off | fill | outline | motion
    double burnAmount = 0.5;       // reforce.Body.BurnAmount 0..1
    double trails = 0;             // reforce.Body.Trails 0..1: hand light trails (0 = off)
    double trailWidth = 8;         // reforce.Body.TrailWidth: px at 1080 lines
    bool show = false;             // reforce.Body.Show: tint the mask and mark the joints (checking the tracking)
    bool sync = true;              // reforce.Body.Sync: the backdrop shows the frame the silhouette is from (in step
                                   //   with it, a tracker step later) instead of the newest one
};
// One setting by its panel key (track, quality, front, burn, burnamount, trails, trailwidth, show, sync); "" or an
// error.
std::string applyBodySetting(BodySettings& s, const std::string& key, const std::string& value);
// The settings.json key for a panel key ("" = unknown) and the value to save (normalized), or nullopt if invalid.
std::string bodySettingKey(const std::string& key);
std::optional<std::string> bodySettingValue(const std::string& key, const std::string& value);

// One-euro filter (Casiez et al. 2012): little smoothing when moving fast, a lot when nearly still.
class OneEuro {
public:
    OneEuro() = default;
    OneEuro(double minCutoff, double beta, double dCutoff)
        : minCutoff_(minCutoff), beta_(beta), dCutoff_(dCutoff) {}
    double filter(double x, double dt);
    void reset() { primed_ = false; }
    bool primed() const { return primed_; }
    double value() const { return x_; }

private:
    double minCutoff_ = 2.0, beta_ = 4.0, dCutoff_ = 1.0;
    bool primed_ = false;
    double x_ = 0, dx_ = 0;
};

// The tracker's latest pose (camera-normalized, top-left origin).
struct PoseSample {
    std::uint64_t seq = 0;
    std::array<float, kPoseJoints> x{}, y{}, conf{};
};

// Smoothed joints, stepped at the simulation rate from the latest (slower) pose results. A joint fades in while it
// is seen with confidence >= minConf and fades out (holding its last place) when it is lost or the pose is stale.
class PoseSmoother {
public:
    static constexpr double kMinConf = 0.3;
    static constexpr double kFade = 0.25;     // seconds to fade a joint in / out
    static constexpr double kStale = 0.6;     // seconds without a new pose: everything fades out
    void step(const PoseSample& latest, double dt);
    void reset();
    struct Joint {
        double x = 0, y = 0;   // camera-normalized
        double weight = 0;     // 0..1 visibility
    };
    const Joint& joint(int j) const { return joints_[std::size_t(j)]; }
    double presence() const;   // max joint weight

private:
    std::array<Joint, kPoseJoints> joints_{};
    std::array<OneEuro, kPoseJoints> fx_, fy_;
    std::uint64_t lastSeq_ = 0;
    double sinceNew_ = 1e9;
};

// Camera-normalized (top-left) -> output uv (top-left, 0..1) for a backdrop drawn with `uv` (fitTransform) and
// optionally mirrored: the inverse of the backdrop's sampling, so effects line up with the camera image.
std::array<double, 2> cameraToScreen(double x, double y, const UvTransform& uv, bool mirror);

// Hand light trails: each step a line from where each wrist was to where it is now, into the trail buffer.
class HandTrails {
public:
    // `px`: wrist positions in output pixels with their weights (left, right). Width in px at 1080 lines.
    void step(const std::array<std::array<double, 3>, 2>& px, double dt, double brightness, double widthPx,
              double frameW, double frameH, std::vector<LineSegment>& out);
    void reset() { have_ = {false, false}; }

private:
    std::array<std::array<double, 2>, 2> last_{};
    std::array<bool, 2> have_{false, false};
};

}  // namespace reforce::engine

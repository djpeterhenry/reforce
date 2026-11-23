#pragma once
// reforce extension (PLAN 5R.7, docs/model-design.md "The camera"): camera paths for 3D model Objects. Pure C++.
//
// Units: the model is normalized by engine/ModelFile (centered, bounding sphere radius 1, front -z, top +y).
//
// A path is keyframes, written in the scene file or in a preset (content/Camera/<name>.txt, or Camera/<name>.txt in
// the model folder), all with the same keys:
//   K1, K2, ...  "t  eyeX eyeY eyeZ  aimX aimY aimZ  [fov [roll]]": where the camera is and what it looks at at
//                time t (0..1 over the move), its vertical field of view in degrees (40) and roll in radians (0)
//   Ease         0..1: smoothstep the timeline (0 = linear)
//   Step         1 = beat cuts: each keyframe held for CutB beats (t is ignored), in order or shuffled (Rand = 1)
//   Rand         1 = shuffled order for Step (by Seed, never the same keyframe twice in a row)
//   Push         Step: push in by this share of the distance during each held keyframe (0)
// Between keyframes the camera follows a Catmull-Rom spline (eye, aim, fov and roll alike), so it never jerks.
// Side (+1 / -1) mirrors the path left / right, Dist scales the camera's distance, Hite raises it.

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/Meshes.hpp"

namespace reforce::engine {

struct CameraMoveParams {
    double dist = 1.0;    // Dist: distance scale (> 0)
    double height = 0.0;  // Hite: added to the camera's height
    double side = 1.0;    // Side: +1 / -1 mirrors the path left / right
    double roll = 0.0;    // Roll: radians, added to the path's
    double fov = 0.0;     // Fov: degrees; 0 = the path's own
    double seed = 0.0;    // Seed: Rand's order
    double cutBeats = 4;  // CutB: Step, beats per keyframe
    double beat = 0.0;    // Step: the beat count (SYNC_BEAT while the clock runs, else seconds x 2)
};

struct ModelShot {
    std::array<double, 3> eye{0, 0, 3};
    std::array<double, 3> aim{0, 0, 0};
    double roll = 0;     // radians about the view axis
    double fov = 40;     // vertical, degrees
};

struct CameraKey {
    double t = 0;
    std::array<double, 3> eye{0, 0.3, 3};
    std::array<double, 3> aim{0, 0, 0};
    double fov = 40;
    double roll = 0;
};

struct CameraPath {
    std::vector<CameraKey> keys;  // sorted by t
    double ease = 0;
    bool step = false, random = false;
    double push = 0;
    bool empty() const { return keys.empty(); }
};

// "t ex ey ez ax ay az [fov [roll]]" (spaces or commas). nullopt (and *error) when malformed.
std::optional<CameraKey> parseCameraKey(std::string_view text, std::string* error = nullptr);
// The path keys read through `get` (a key name like "K1" or "EASE" -> its text, "" when absent). Problems are
// appended to *errors ("K3: ...; ").
CameraPath cameraPathFrom(const std::function<std::string(const char*)>& get, std::string* errors = nullptr);
// A preset file (the same keys, `Key = "value"` lines). nullopt when it can't be read or has no keyframes.
std::optional<CameraPath> loadCameraPreset(const std::filesystem::path& file, std::string* errors = nullptr);
// Where a preset named `name` lives: <modelDir>/Camera/<name>.txt first (your own), then <contentDir>/Camera/.
std::filesystem::path findCameraPreset(const std::string& name, const std::filesystem::path& modelDir,
                                       const std::filesystem::path& contentDir);

// The shot at phase p (0..1 over the move; Step uses params.beat instead). An empty path gives the default view.
ModelShot pathShot(const CameraPath& path, double p, const CameraMoveParams& params);

// View-projection for a shot (column-major, Metal clip depth 0..1, right-handed, camera looking down its -z).
Mat4 shotView(const ModelShot& s);
Mat4 shotProjection(const ModelShot& s, double aspect, double nearZ = 0.02, double farZ = 60.0);

// Easing on the timeline: smoothstep blended with linear (k = 0 linear, 1 full smoothstep).
double easeMove(double p, double k);

}  // namespace reforce::engine

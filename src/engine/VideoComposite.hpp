#pragma once
// Colour video layer (Phase 5F (b)): settings, parsing, and the CPU reference
// of the composite the colorize shader does (render/ScenePipeline.cpp,
// colorize_video_fs). Pure C++, unit-tested; the GPU path is checked against
// it by the video render test.
//
// Per output pixel (all values 0..1, BGRA8 output, no gamma conversion):
//   1. each source S (A = outgoing, B = incoming) is fitted to the frame
//      (cover / contain / stretch) and sampled: colour c_S, alpha a_S =
//      inside(frame of the video) * key(c_S)
//   2. crossfade with weight w (A -> B; "no source" = alpha 0, so starting
//      and stopping the layer are crossfades from / to nothing):
//        a = mix(a_A, a_B, w),   c = mix(c_A a_A, c_B a_B, w) / a
//   3. opacity o, visualizer colour v, blend B(base, top):
//        normal B = top, add min(1, base + top), screen 1 - (1-base)(1-top),
//        multiply base * top
//      over  (video on top):          out = v + (B(v, c) - v) * (o a)
//      under (visualizer on top):     u = c * (o a)   (video faded to black)
//                                     normal: out = v + u (1 - max(v.rgb))
//                                     (dark parts of the visualizer show the
//                                     video), others: out = B(u, v)
// Keys (on the video's colour, before the crossfade):
//   luma    a = smoothstep(threshold, threshold + softness, Y601)
//           (keys out dark pixels; softness 0 = hard cut)
//   chroma  a = smoothstep(tolerance, tolerance + softness, |CbCr - CbCr_key|)
//           (BT.601 full-range chroma distance; keys out the key colour)

#include <array>
#include <optional>
#include <string>

namespace reforce::engine {

enum class VideoBlend : int { Normal = 0, Add = 1, Screen = 2, Multiply = 3 };
enum class VideoFit : int { Cover = 0, Contain = 1, Stretch = 2 };
enum class VideoOrder : int { Over = 0, Under = 1 };
enum class VideoKey : int { None = 0, Luma = 1, Chroma = 2 };

const char* videoBlendName(VideoBlend b);
const char* videoFitName(VideoFit f);
const char* videoOrderName(VideoOrder o);
const char* videoKeyName(VideoKey k);
std::optional<VideoBlend> parseVideoBlend(const std::string& s);
std::optional<VideoFit> parseVideoFit(const std::string& s);
std::optional<VideoOrder> parseVideoOrder(const std::string& s);
std::optional<VideoKey> parseVideoKey(const std::string& s);
// "#00ff00", "00ff00", "0,1,0" (0..1) or "0,255,0" (any component > 1: 0..255).
std::optional<std::array<float, 3>> parseColor(const std::string& s);
std::string colorToHex(const std::array<float, 3>& c);

using Rgb = std::array<float, 3>;

struct VideoLayerParams {
    VideoBlend blend = VideoBlend::Normal;
    VideoFit fit = VideoFit::Cover;
    VideoOrder order = VideoOrder::Over;
    float opacity = 1.0f;
    VideoKey key = VideoKey::None;
    float lumaThreshold = 0.1f;
    Rgb keyColor{0.0f, 1.0f, 0.0f};
    float tolerance = 0.15f;  // chroma distance (CbCr, 0..~0.7)
    float softness = 0.1f;
    double crossfade = 1.0;   // seconds for source changes, start and stop
    bool loop = false;        // file sources (off: play once, then the layer fades out; reforce.Video.Loop lists
                              //   movies that loop anyway)
    double rate = 1.0;        // file sources (>= 0)
    double offset = 0.0;      // file sources: start offset (s)
    bool toSyphon = true;     // Syphon output shows the composite (else the plain visualizer)
};

// Applies one `videoset` key ("opacity", "blend", "fit", "order", "key",
// "keycolor", "tolerance", "softness", "threshold", "crossfade", "loop",
// "rate", "offset", "syphon") with its text value. Returns "" or an error.
std::string applyVideoSetting(VideoLayerParams& p, const std::string& key, const std::string& value);
// The keys applyVideoSetting knows.
const char* const* videoSettingKeys();

// Maps output uv (0..1, top-left origin) to video uv: src = uv * scale + offset.
// Contain leaves parts of the frame outside the video (uv outside 0..1).
struct UvTransform {
    float scale[2] = {1, 1};
    float offset[2] = {0, 0};
};
UvTransform fitTransform(double videoW, double videoH, double frameW, double frameH, VideoFit fit);

float lumaKeyAlpha(const Rgb& c, float threshold, float softness);
float chromaKeyAlpha(const Rgb& c, const Rgb& key, float tolerance, float softness);
float keyAlpha(const Rgb& c, const VideoLayerParams& p);

// A sampled source (already keyed): colour and alpha.
struct VideoSample {
    Rgb c{0, 0, 0};
    float a = 0;
};
VideoSample crossfadeSamples(const VideoSample& a, const VideoSample& b, float w);
// Step 3 above: visualizer colour v, video sample s.
Rgb compositeVideo(const Rgb& v, const VideoSample& s, const VideoLayerParams& p);

}  // namespace reforce::engine

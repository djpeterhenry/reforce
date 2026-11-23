#pragma once
// Video sources (Phase 5F): one abstraction for movie files, cameras and
// Syphon inputs, used by movie/live sprites (engine sprite pass) and by the
// color video layer (colorize pass). Implemented in VideoSourceMac.mm
// (AVFoundation, CoreVideo, Syphon); this header has no Objective-C.
//
// Every source decodes/captures on its own thread(s) and hands out frames as
// BGRA8 Metal textures that wrap IOSurface-backed CVPixelBuffers (zero copy,
// CVMetalTextureCache; the decoder's pixel-buffer pool recycles the
// surfaces). The render thread only picks a frame (frameAt) and keeps the
// FramePtr alive until the GPU has finished with it.
//
// Files: AVAssetReader (hardware decode of H.264 / HEVC / ProRes through
// VideoToolbox), not AVPlayer: frames are chosen by *simulation* time, so
// deterministic runs are exact and the rate / offset / loop mapping does not
// depend on the wall clock. Media time for source time t (seconds since the
// source started):  m = offset + t * rate  (looping: modulo the duration).
// The frame shown at m is the last one whose presentation time <= m. The
// decoder keeps a few frames ahead of m (preroll before start: the source is
// ready() once the first frame is decoded). Deterministic sources make
// frameAt wait for the exact frame. Audio tracks are never read (muted).
//
// Cameras: AVCaptureSession; permission is requested lazily on open (never
// at startup) through systemCameraGate() (CameraGate.hpp: async, coalesced),
// the source reports waiting() meanwhile and fails cleanly when access is
// denied, and fails when the camera is unplugged. Syphon: a SyphonMetalClient
// on the named server; our own server is refused (feedback loop) unless
// allowFeedback. When the server quits the source shows nothing (frameAt
// returns null) and reconnects when it comes back.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "video/CameraGate.hpp"

namespace MTL { class Device; class Texture; }

namespace reforce::video {

enum class SourceKind { File, Camera, Syphon };
const char* sourceKindName(SourceKind k);

struct SourceSpec {
    SourceKind kind = SourceKind::File;
    std::string id;            // file path; camera unique id / name / "default"; Syphon "App - Name", name or uuid
    bool loop = true;          // files
    double offset = 0;         // files: seconds into the movie at the start (a loop jumps back here)
    double end = 0;            // files: media time where playback stops or loops back (0 = the movie's end)
    double rate = 1.0;         // files: playback rate (>= 0; 0 = still)
    double loopUnit = 0;       // files: > 0 = the played section is cut to a whole number of these seconds (at
                               //   least one), e.g. bars of a beat-synced movie so loops keep the phase
    bool allowFeedback = false;  // Syphon: allow our own server
    // Files: luminance levels for movie sprites (PLAN 5K; Black / White / Gamma in the movie's .reforce.txt,
    // engine/MediaOptions). Not used by the decoder.
    double black = 0, white = 1, gamma = 1;
    // Files: ColorKey in the movie's .reforce.txt: false = opaque in the Syphon alpha (reforce.Syphon.Alpha).
    bool colorKey = true;
};

// "file:PATH", "camera:ID" ("camera" alone = default camera), "syphon:NAME";
// anything else is a file path. Returns nullopt (and *error) when empty.
std::optional<SourceSpec> parseSourceSpec(const std::string& text, std::string* error = nullptr);
std::string specToString(const SourceSpec& s);  // the parseSourceSpec form
std::string specLabel(const SourceSpec& s);     // short display name (file stem, camera / server name)

// Movie file extensions AVFoundation decodes (.mov .mp4 .m4v .mpg .mpeg .avi
// .qt .3gp); hidden files, '_' names and "-icon" thumbnails excluded like images.
bool isVideoFile(const std::filesystem::path& p);

// Media time (see above): offset + t * rate, looping within [offset, end) or
// stopping at end, where end = `end` when offset < end < duration, else the
// movie's duration. duration <= 0 and no end: unknown (no wrap).
double mediaTime(double t, double offset, double rate, double duration, bool loop, double end = 0);
// End of the played section: `spec.end` when it lies inside the movie, else
// `duration` (0 = unknown); cut to whole spec.loopUnit lengths when that is set.
double sectionEnd(const SourceSpec& spec, double duration);
// Seconds one pass of the played section [offset, end) lasts at spec.rate (0 = unknown or still).
double playLength(const SourceSpec& spec, double duration);

// A decoded frame. The texture (BGRA8Unorm, top-left origin) stays valid while
// the Frame lives; keep it until the GPU is done.
class Frame {
public:
    virtual ~Frame() = default;
    MTL::Texture* texture() const { return texture_; }
    int width() const { return width_; }
    int height() const { return height_; }
    double time() const { return time_; }          // media time, unwrapped across loops (files); capture time (live)
    std::uint64_t index() const { return index_; }  // decode order
    // The CVPixelBufferRef (BGRA, IOSurface-backed) behind the texture for camera and movie frames, valid while the
    // Frame lives; null for Syphon frames. As void* so this header stays free of CoreVideo (body tracking reads it).
    virtual void* pixelBuffer() const { return nullptr; }

protected:
    MTL::Texture* texture_ = nullptr;
    int width_ = 0, height_ = 0;
    double time_ = 0;
    std::uint64_t index_ = 0;
};
using FramePtr = std::shared_ptr<const Frame>;

struct SourceStats {
    std::uint64_t decoded = 0;   // frames produced
    std::uint64_t dropped = 0;   // decoded but never shown (late, or skipped to catch up)
    std::uint64_t picked = 0;    // frameAt calls that returned a newer frame
    double decodeMsAvg = 0, decodeMsMax = 0;  // per frame, on the decode thread
};

class VideoSource {
public:
    virtual ~VideoSource() = default;
    const SourceSpec& spec() const { return spec_; }
    std::string label() const { return specLabel(spec_); }

    virtual bool ready() const = 0;    // first frame available (preroll done)
    virtual bool failed() const = 0;
    virtual std::string error() const = 0;
    // What a source that shows nothing is waiting for ("waiting for camera
    // permission", "waiting for the Syphon server", ...); "" otherwise.
    virtual std::string waiting() const { return {}; }
    virtual int width() const = 0;     // once ready
    virtual int height() const = 0;
    virtual double duration() const { return 0; }  // files, seconds (0 = live / unknown)
    virtual double frameRate() const { return 0; }
    // Render thread: the frame for source time t (seconds since the source
    // started; live sources return the newest frame). Never blocks unless the
    // source is deterministic. nullptr until ready.
    virtual FramePtr frameAt(double t) = 0;
    // Files: the frame for an unwrapped media time u chosen by the caller (beat-synced playback,
    // engine/MovieBeatSync.hpp) instead of offset + t * rate. u should only move forward. Others: frameAt.
    virtual FramePtr frameAtMedia(double u) {
        return frameAt(spec_.rate > 0 ? (u - spec_.offset) / spec_.rate : 0.0);
    }
    // Deterministic file sources: blocks until ready() or failed() (startup).
    virtual void waitReady(double timeoutSeconds) {}
    // Files: change looping while playing (render thread). Takes effect at the
    // end of the current pass; a movie that already played out stays ended.
    virtual void setLoop(bool on) { spec_.loop = on; }
    virtual SourceStats stats() const = 0;
    // Live sources (cameras, Syphon): `fn` is called on the capture thread with every new frame, as it arrives
    // (body tracking starts on it at once, not at the next render step). Null clears it. False: not supported (files:
    // pick frames with frameAt instead).
    using FrameListener = std::function<void(const FramePtr&)>;
    virtual bool setFrameListener(FrameListener fn) { return false; }

protected:
    SourceSpec spec_;
};

struct OpenOptions {
    bool deterministic = false;  // files: frameAt waits for the exact frame
    int queueDepth = 3;          // files: decoded frames kept ahead
};

// Never blocks: opening, permission requests and server lookup happen on the
// source's own threads. Returns null only for an unsupported spec.
std::unique_ptr<VideoSource> openSource(MTL::Device* device, const SourceSpec& spec, const OpenOptions& opts = {});

// ---- discovery (any thread)
struct CameraInfo {
    std::string id;    // AVCaptureDevice uniqueID
    std::string name;  // localized name
};
std::vector<CameraInfo> listCameras();
// 0 = not asked yet, 1 = granted, 2 = denied/restricted, 3 = our prompt is up
// (waiting for the answer), 4 = unavailable (bundle without NSCameraUsageDescription).
int cameraPermission();
// The AVFoundation-backed permission gate (never shows the prompt by itself:
// only whenDecided() on NotDetermined does, i.e. opening a camera source).
CameraGate& systemCameraGate();
// Opens System Settings > Privacy & Security > Camera (main thread).
void openCameraPrivacySettings();

struct SyphonServerInfo {
    std::string uuid, name, app;
    bool self = false;  // our own output (isSelfSyphonServer)
};
// Our own Syphon server (set once by the app after creating it).
void setOwnSyphonServer(const std::string& name, const std::string& uuid);
// Same uuid, or the same app and server name as ours (another reforce instance
// with our server name counts too: subscribing would mirror our own output).
bool isSelfSyphonServer(const SyphonServerInfo& s, const std::string& ownName, const std::string& ownApp,
                        const std::string& ownUuid);
// "App - Name" (or just one of them when the other is empty).
std::string syphonDisplayName(const SyphonServerInfo& s);
// Does `want` (uuid, "App - Name", name or app, case-insensitive) name `s`?
bool syphonMatches(const SyphonServerInfo& s, const std::string& want);
// Servers on this machine (self marked). Call initSyphonDirectory() on the main
// thread first (Syphon's directory listens for announcements on its run loop).
std::vector<SyphonServerInfo> listSyphonServers();
void initSyphonDirectory();

// ---- test videos (tools and tests; our own synthetic content)
enum class TestCodec { H264, HEVC, ProRes422 };
enum class TestPattern {
    Card,    // frame index as 8 bit blocks on top, color patches below (exact tests)
    Motion,  // moving gradients + scrolling noise (realistic decode load)
};
struct TestVideoSpec {
    int width = 320, height = 180;
    double fps = 30;
    int frames = 60;
    TestCodec codec = TestCodec::H264;
    TestPattern pattern = TestPattern::Card;
};
// Card layout (fractions of the frame): y < 1/4: 8 blocks, block k white when
// bit (7 - k) of (frame index & 255) is set; 1/4 <= y < 1/2: red | gray 128;
// y >= 1/2: white | pure green.
bool writeTestVideo(const std::filesystem::path& path, const TestVideoSpec& spec, std::string* error = nullptr);
// Reads the frame index back from BGRA8 pixels of a Card frame.
int decodeCardIndex(const std::uint8_t* bgra, int width, int height, std::size_t bytesPerRow);

}  // namespace reforce::video

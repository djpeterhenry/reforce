#include "app/App.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <cstdio>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "app/AudioDriver.hpp"
#include "app/Director.hpp"
#include "app/Hotkeys.hpp"
#include "app/Visualizer.hpp"
#include "audio/Analyzer.hpp"
#include "audio/CoreAudioInput.hpp"
#include "engine/Blend.hpp"
#include "engine/Commands.hpp"
#include "engine/Log.hpp"
#include "platform/SyphonPublisher.hpp"
#include "render/Downsample.hpp"
#include "render/GpuPoison.hpp"
#include "render/Presenter.hpp"
#include "render/Renderer.hpp"
#include "sync/DeckFeed.hpp"
#include "sync/LinkClock.hpp"
#include "video/VideoSource.hpp"

namespace reforce::app {

// The offscreen frame is a single texture, so the GPU serializes frames on the
// one queue anyway. Cap CPU run-ahead instead of blocking the main thread.
constexpr int kMaxFramesInFlight = 2;

namespace {

// Copies a Link snapshot into the frame's LINK_* fields; with a session (>= 1
// peer) and driveBeat, BEAT/TEMPO follow Link instead of the audio estimate.
void applyLink(audio::AudioFrame& f, const sync::LinkSnapshot& s, bool driveBeat) {
    f.linkEnabled = s.enabled;
    f.linkListening = s.listening;
    f.linkJoining = s.joining;
    f.linkPeers = s.peers;
    f.linkOn = s.on;
    f.linkBpm = s.bpm;
    f.linkBeat = s.beat;
    f.linkPhase = s.phase;
    f.linkQuantum = s.quantum;
    if (s.on && driveBeat) {
        f.beat = static_cast<float>(sync::linkPulse(s.beat, s.bpm));
        f.tempo = static_cast<float>(s.bpm);
    }
}

}  // namespace

App::App(MTL::Device* device, const AppOptions& options, std::unique_ptr<Director> director,
         std::unique_ptr<AudioDriver> audio)
    : options_(options),
      device_(device->retain()),
      queue_(device->newCommandQueue()),
      renderer_(std::make_unique<render::Renderer>(device, options.frameWidth * uint32_t(std::max(1, options.supersample)),
                                                   options.frameHeight * uint32_t(std::max(1, options.supersample)))),
      presenter_(std::make_unique<render::Presenter>(device, *renderer_)),
      syphon_(std::make_unique<platform::SyphonPublisher>(device, options.syphonName)),
      director_(std::move(director)),
      audio_(std::move(audio)),
      clock_(options.fps, options.maxCatchUpSteps),
      inFlight_(std::make_shared<std::atomic<int>>(0)),
      gpu_(std::make_shared<GpuStats>()),
      stats_(std::make_shared<perf::FrameStats>(8192)) {
    if (options_.supersample > 1) {
        downsampler_ = std::make_unique<render::Downsampler>(device, options.frameWidth, options.frameHeight,
                                                             options.supersample);
        std::fprintf(stderr, "[reforce] supersample %dx: rendering %ux%u, output %ux%u\n", options.supersample,
                     renderer_->width(), renderer_->height(), options.frameWidth, options.frameHeight);
    }
    if (audio_ && !options_.fixedStep) audio_->startControl();
    if (options_.link && !options_.fixedStep) {
        link_ = std::make_unique<sync::LinkClock>(options_.linkQuantum, 120.0, options_.linkFollow);
        link_->setEnabled(true);
        std::fprintf(stderr, "[reforce] Ableton Link on (%s, quantum %.0f, offset %.0f ms)\n",
                     options_.linkFollow ? "follow: joins once another Link app runs" : "joins at launch",
                     options_.linkQuantum, options_.linkOffset * 1000.0);
    }
    if (options_.deckFeed && !options_.fixedStep && director_) {
        deckFeed_ = std::make_unique<sync::DeckFeed>(options_.deckFeedPort, options_.deckFeedMidi,
                                                     sync::loadControllerMaps(options_.controllerDirs));
        director_->visualizer().setDeckFeed(deckFeed_.get());
    }
    // Video inputs never subscribe to this server (Syphon feedback guard).
    video::setOwnSyphonServer(options.syphonName, syphon_->uuid());
    // Render-thread log lines go through the logger thread from here on.
    logx::setAsync(true);
    std::fprintf(stderr, "[reforce] device: %s; frame %ux%u (IOSurface id %u); Syphon server \"%s\": %s\n",
                 device->name()->utf8String(), renderer_->width(), renderer_->height(),
                 IOSurfaceGetID(renderer_->ioSurface()), options.syphonName,
                 syphon_->valid() ? "running" : "FAILED");
}

App::~App() {
    logx::setAsync(false);
    if (director_) director_->visualizer().setDeckFeed(nullptr);
    director_.reset();
    deckFeed_.reset();
    link_.reset();
    audio_.reset();
    syphon_.reset();
    presenter_.reset();
    renderer_.reset();
    if (windowFrame_) windowFrame_->release();
    if (syphonFrame_) syphonFrame_->release();
    queue_->release();
    device_->release();
}

void App::onFrame(const platform::FrameInfo& frame) {
    using Clock = std::chrono::steady_clock;
    const auto f0 = Clock::now();
    auto msSince = [](Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); };
    if (deferredAnswer_ && deferredAnswer_->load() != 0) {
        if (deferredAnswer_->load() == 1) openAudioInput(deferredInput_);
        else rlog("[reforce] no microphone permission: running silent\n");
        deferredAnswer_.reset();
    }
    if (startTime_ < 0) startTime_ = lastStats_ = frame.hostTime;
    const double wallT = frame.hostTime - startTime_;
    if (frame.source == platform::FrameSource::Timer) perf::tag(perf::TagTimer);
    const double intervalMs = lastFrameHost_ < 0 ? 0.0 : (frame.hostTime - lastFrameHost_) * 1000.0;
    lastFrameHost_ = frame.hostTime;
    if (!statsWarm_ && wallT >= options_.perfWarmup) {
        stats_->finalize(0);
        stats_->reset(perf::renderAllocs().load());
        statsWarm_ = true;
    }

    (frame.source == platform::FrameSource::DisplayLink ? framesLink_ : framesTimer_)++;
    (frame.source == platform::FrameSource::DisplayLink ? totalLink_ : totalTimer_)++;
    if (fpsWindowStart_ < 0) fpsWindowStart_ = frame.hostTime;
    ++fpsWindowFrames_;
    if (frame.hostTime - fpsWindowStart_ >= 1.0) {
        displayFps_.store(double(fpsWindowFrames_) / (frame.hostTime - fpsWindowStart_), std::memory_order_relaxed);
        fpsWindowStart_ = frame.hostTime;
        fpsWindowFrames_ = 0;
    }
    windowVisible_ = frame.windowVisible;
    if (inFlight_->load() >= kMaxFramesInFlight) {
        // GPU behind: this display frame shows nothing new.
        framesSkipped_++;
        recordFrame(frame, 0, msSince(f0), 0, 0, 0, 0, intervalMs, wallT, false);
        logStats(frame.hostTime);
        return;
    }
    if (options_.stressInterval > 0) stress(wallT);

    MTL::CommandBuffer* cmd = queue_->commandBuffer();
    cmd->setLabel(NS::String::string("reforce frame", NS::UTF8StringEncoding));

    const auto c0 = Clock::now();
    // Fixed simulation tick (G-Force is per frame at TargetFrameRate): whole
    // 1/fps steps from an accumulator, independent of the display refresh.
    // Deterministic mode: exactly one step per display frame.
    const double realDt = lastHost_ < 0 ? clock_.stepSeconds() : frame.hostTime - lastHost_;
    lastHost_ = frame.hostTime;
    // Locked to the frame callbacks' cadence (vsync): even steps (Phase 5D).
    const double cbPeriod = frame.callbackRate > 0 ? 1.0 / frame.callbackRate : frame.refreshPeriod;
    const int steps = options_.fixedStep ? 1 : clock_.advanceLocked(realDt, cbPeriod);
    if (steps > 1) perf::tag(perf::TagCatchUp);
    double simMs = 0, colorizeMs = 0;
    bool splitVideo = false, syphonAlpha = false, backdrop = false, windowVisuals = false;
    // The window's own frame: with the color video layer when Syphon gets the plain visualizer, or the visuals
    // alone (reforce.Window.VisualsOnly); created on first use.
    auto windowFrame = [&]() -> MTL::Texture* {
        if (!windowFrame_) {
            MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
                MTL::PixelFormatBGRA8Unorm, renderer_->width(), renderer_->height(), false);
            td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            td->setStorageMode(MTL::StorageModePrivate);
            perf::countRenderAlloc();  // once, when the option is first used
            windowFrame_ = render::makeTexture(device_, td);
        }
        return windowFrame_;
    };
    if (director_) {
        if (frameIndex_ == 0 && options_.seedNoise) director_->visualizer().seedNoise(cmd);
        const double dt = 1.0 / options_.fps;
        for (int i = 0; i < steps; ++i) {
            // Scene time = simulation time (tracks real seconds, probe #17).
            const double t = double(simSteps_) * dt;
            const audio::AudioFrame* af = nullptr;
            if (audio_) af = options_.fixedStep ? &audio_->step(options_.fps) : &audio_->update(dt);
            if (link_ && af) {
                // Earlier catch-up steps sample the session correspondingly earlier.
                const double ahead = options_.linkOffset + director_->visualizer().beatOffsetMs() / 1000.0 -
                                     double(steps - 1 - i) * dt;
                linkFrame_ = *af;
                applyLink(linkFrame_, link_->sample(ahead), options_.linkDriveBeat);
                af = &linkFrame_;
            }
            director_->step(cmd, t, af);
            ++simSteps_;
            ++stepsSinceStats_;
        }
        simMs = msSince(c0);
        // Present/colorize/Syphon at display rate (re-colorizes the latest sim frame).
        // The color video layer is composited here too; with Syphon set to
        // the plain visualizer the window gets its own composite below.
        const auto k0 = Clock::now();
        const Visualizer& vis = director_->visualizer();
        splitVideo = vis.videoLayerActive() && !vis.videoToSyphon();
        // Syphon alpha (reforce.Syphon.Alpha): the frame's alpha tracks what stays opaque; the window ignores it.
        // A camera backdrop (docs/backdrop.md) shows through where that alpha would be transparent, mixed in under the
        // dancers: the frame (window and Syphon) is then opaque.
        backdrop = vis.backdropActive();
        syphonAlpha = vis.syphonAlpha() && !backdrop;
        // Sync test flash: one opaque frame for the window and Syphon alike.
        const bool flash = vis.beatFlashOn();
        if (flash) splitVideo = false, syphonAlpha = false;
        // Window shows the visuals only (for screen-mirrored room lights): without a split video layer, a copy taken
        // from this pass before the backdrop, dancers and text; with one, its own pass below.
        windowVisuals = vis.windowVisualsOnly() && frame.layer && !flash;
        MTL::Texture* visualsCopy = windowVisuals && !splitVideo ? windowFrame() : nullptr;
        director_->colorize(cmd, renderer_->frameTexture(), !splitVideo, syphonAlpha, backdrop, visualsCopy);
        if (flash) {  // over everything: a clear pass in the flash's gray
            const double g = vis.beatFlashLevel();
            MTL::RenderPassDescriptor* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
            MTL::RenderPassColorAttachmentDescriptor* att = rp->colorAttachments()->object(0);
            att->setTexture(renderer_->frameTexture());
            att->setLoadAction(MTL::LoadActionClear);
            att->setStoreAction(MTL::StoreActionStore);
            att->setClearColor(MTL::ClearColor(g, g, g, 1.0));
            render::makeRenderEncoder(cmd, rp)->endEncoding();
        }
        colorizeMs = msSince(k0);
    } else {
        renderer_->encodeTestPattern(cmd, static_cast<float>(wallT));
    }
    frameIndex_++;
    cpuMsSum_ += std::chrono::duration<double, std::milli>(Clock::now() - c0).count();
    cpuFrames_++;
    const auto s0 = Clock::now();
    MTL::Texture* published = renderer_->frameTexture();
    if (syphonAlpha) {
        if (!syphonFrame_) {
            MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
                MTL::PixelFormatBGRA8Unorm, renderer_->width(), renderer_->height(), false);
            td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            td->setStorageMode(MTL::StorageModePrivate);
            perf::countRenderAlloc();  // once, when the option is first used
            syphonFrame_ = render::makeTexture(device_, td);
        }
        if (syphonFrame_) {
            director_->visualizer().encodeSyphonAlpha(cmd, published, syphonFrame_);
            published = syphonFrame_;
        }
    }
    // Supersampling: what leaves reforce is averaged down to the output size (color first, then the average).
    if (downsampler_) published = downsampler_->encode(cmd, published, 0);
    syphon_->publish(published, cmd);

    // The window present runs on its own thread (it may block in nextDrawable,
    // e.g. while the window is occluded); here we only hand it the frame.
    if (frame.layer) {
        MTL::Texture* shown = renderer_->frameTexture();
        if (splitVideo) {
            if (MTL::Texture* w = windowFrame()) {
                director_->colorize(cmd, w, true, false, backdrop && !windowVisuals, nullptr, !windowVisuals);
                shown = w;
            }
        } else if (windowVisuals && windowFrame_) {
            shown = windowFrame_;  // filled by the main pass above
        }
        if (downsampler_) shown = downsampler_->encode(cmd, shown, 1);
        presenter_->setLayer(frame.layer);
        presenter_->encodeCapture(cmd, shown);
    }

    inFlight_->fetch_add(1);
    auto inFlight = inFlight_;
    auto gpu = gpu_;
    auto stats = stats_;
    const std::uint64_t recIndex = stats_->reserve();
    cmd->addCompletedHandler([inFlight, gpu, stats, recIndex](MTL::CommandBuffer* cb) {
        const double ms = (cb->GPUEndTime() - cb->GPUStartTime()) * 1000.0;
        if (ms > 0 && ms < 1000) {
            gpu->sumMs.store(gpu->sumMs.load() + ms);
            gpu->count.fetch_add(1);
        }
        stats->setGpu(recIndex, ms > 0 && ms < 10000 ? ms : 0.0);
        if (cb->status() == MTL::CommandBufferStatusError && cb->error()) {
            std::fprintf(stderr, "[reforce] GPU error: %s\n",
                         cb->error()->localizedDescription()->utf8String());
        }
        inFlight->fetch_sub(1);
    });
    cmd->commit();
    const double submitMs = msSince(s0);
    recordFrame(frame, steps, msSince(f0), simMs, colorizeMs, submitMs, -1, intervalMs, wallT, true);

    logStats(frame.hostTime);
}

perf::PerfSummary App::perfSummary() const {
    std::lock_guard<std::mutex> lock(summaryMutex_);
    return summary_;
}

void App::recordFrame(const platform::FrameInfo& frame, int steps, double cpuMs, double simMs, double colorizeMs,
                      double submitMs, double gpuMs, double intervalMs, double wallT, bool reserved) {
    (void)reserved;
    // Cadence: what one frame callback should do. The callbacks come at
    // `callbackRate` (pinned display link / timer), else every refresh.
    const double period = frame.refreshPeriod > 0 ? frame.refreshPeriod : 1.0 / 60.0;
    const double cb = frame.callbackRate > 0 ? 1.0 / frame.callbackRate : period;
    perf::Cadence c;
    c.budgetMs = cb * 1000.0;
    c.periodMs = period * 1000.0;
    c.stepsPerVsync = options_.fixedStep ? 1.0 : options_.fps * cb;
    stats_->setCadence(c);
    perf::FrameRecord r;
    r.hostTime = wallT;
    r.intervalMs = intervalMs;
    r.cpuMs = cpuMs;
    r.simMs = simMs;
    r.colorizeMs = colorizeMs;
    r.submitMs = submitMs;
    r.gpuMs = float(gpuMs);
    r.steps = steps;
    r.vsyncs = intervalMs > 0 ? std::max(1, int(std::lround(intervalMs / c.budgetMs))) : 1;
    r.tags = perf::frameTags().exchange(0, std::memory_order_relaxed);
    stats_->add(r);
    stats_->finalize(8, [this](const perf::FrameRecord& h) {
        ++hitchesSinceStats_;
        if (h.hostTime - hitchLogWindow_ >= 10.0) {
            hitchLogWindow_ = h.hostTime;
            hitchLogCount_ = 0;
        }
        if (hitchLogCount_++ < 20) rlog("[reforce] hitch: %s\n", perf::FrameStats::describe(h).c_str());
    });
    if (wallT - lastSummary_ >= 0.5) {
        std::unique_lock<std::mutex> lock(summaryMutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            summary_ = stats_->summary(perf::renderAllocs().load());
            lastSummary_ = wallT;
        }
    }
}

void App::stress(double now) {
    // --stress: every ~interval s switch all three morph layers (random blend
    // mode, quick or slideshow duration, so running transitions get
    // interrupted mid-way), take a Union now and then, spawn particles,
    // sprites and solids (one every other round), and kill them every few rounds.
    if (!director_) return;
    if (nextStress_ < 0) nextStress_ = now + options_.stressInterval;
    if (now < nextStress_) return;
    std::uniform_real_distribution<double> u(0.0, 1.0);
    nextStress_ = now + options_.stressInterval * (0.75 + 0.5 * u(stressRng_));
    ++stressTick_;
    perf::tag(perf::TagStress);
    auto& q = director_->commands();
    for (engine::Layer l : engine::kMorphLayers) {
        const auto& modes = engine::blendModeNames(l);
        if (!modes.empty() && u(stressRng_) < 0.7)
            q.push(engine::Command::blendMode(l, modes[std::size_t(u(stressRng_) * double(modes.size())) % modes.size()]));
        if (l == engine::Layer::FlowField && u(stressRng_) < 0.3) q.push(engine::Command::next(engine::Layer::Union));
        q.push(engine::Command::next(l, u(stressRng_) < 0.5));
    }
    q.push(engine::Command::next(engine::Layer::Particle));
    q.push(engine::Command::next(engine::Layer::Sprite));
    if (stressTick_ % 2 == 1) q.push(engine::Command::next(engine::Layer::Object));  // all slots full: oldest makes way
    if (stressTick_ % 4 == 0) {
        q.push(engine::Command::kill(engine::Layer::Particle));
        q.push(engine::Command::kill(engine::Layer::Sprite));
    }
    if (stressTick_ % 6 == 0) q.push(engine::Command::kill(engine::Layer::Object));
    // Phase 5F: video start/stop on the color layer (crossfades interrupted
    // now and then), movie sprites, blend changes.
    const auto& videos = director_->videoFiles();
    if (!videos.empty()) {
        auto pick = [&]() -> const engine::SceneEntry& {
            return videos[std::size_t(u(stressRng_) * double(videos.size())) % videos.size()];
        };
        if (stressTick_ % 5 == 4) {
            q.push(engine::Command::videoStop(engine::VideoTarget::Layer));
        } else if (u(stressRng_) < 0.6) {
            q.push(engine::Command::videoStart(engine::VideoTarget::Layer, "file:" + pick().path.string()));
        }
        if (u(stressRng_) < 0.3)
            q.push(engine::Command::videoSet(
                "blend", engine::videoBlendName(engine::VideoBlend(int(u(stressRng_) * 4.0) % 4))));
        if (stressTick_ % 3 == 1)
            q.push(engine::Command::videoStart(engine::VideoTarget::Sprite, "file:" + pick().path.string()));
        if (stressTick_ % 7 == 6) q.push(engine::Command::videoStop(engine::VideoTarget::Sprite));
    }
}

bool App::windowVisualsOnly() { return director_ && director_->snapshot().video.windowVisuals; }

int App::lightsMenuState() {
    if (!director_) return -1;
    const engine::VideoState v = director_->snapshot().video;
    return !v.lightsAvailable ? -1 : v.lightsOn ? 1 : 0;
}

std::vector<platform::MenuItem> App::videoMenu() {
    using platform::MenuItem;
    std::vector<MenuItem> items;
    if (!director_) return items;
    const engine::VideoState v = director_->snapshot().video;
    const auto files = director_->videoFiles();
    const auto cameras = video::listCameras();
    const auto servers = video::listSyphonServers();
    const video::CameraAccess camAccess = video::systemCameraGate().access();
    auto sources = [&](const std::string& prefix, bool layer) {
        std::vector<MenuItem> out;
        std::size_t n = 0;
        for (const auto& f : files) {
            if (++n > 200) break;
            MenuItem m{prefix + "file:" + f.path.string(), f.id};
            m.checked = layer && v.layerSpec == "file:" + f.path.string();
            out.push_back(std::move(m));
        }
        if (files.empty()) {
            MenuItem none{"", "No video files (Sprite package, reforce.VideoDir)"};
            none.enabled = false;
            out.push_back(std::move(none));
        }
        bool first = true;
        if (!cameras.empty() && camAccess != video::CameraAccess::Granted &&
            camAccess != video::CameraAccess::NotDetermined) {
            // Why the cameras show nothing, and the way out.
            const bool denied = camAccess == video::CameraAccess::Denied;
            MenuItem m{denied ? "opensettings:camera" : "",
                       denied ? "Camera Access Denied: Open Privacy Settings…"
                       : camAccess == video::CameraAccess::Requesting
                           ? "Camera Access: Waiting for Your Answer…"
                           : "Camera Unavailable: Reinstall reforce.app (no usage description)"};
            m.enabled = denied;
            m.separatorBefore = true;
            first = false;
            out.push_back(std::move(m));
        }
        for (const auto& c : cameras) {
            MenuItem m{prefix + "camera:" + c.id, "Camera: " + c.name};
            m.checked = layer && v.layerSpec == "camera:" + c.id;
            m.separatorBefore = first;
            first = false;
            out.push_back(std::move(m));
        }
        first = true;
        for (const auto& srv : servers) {
            if (srv.self) continue;  // never our own output (feedback loop)
            const std::string name = video::syphonDisplayName(srv);
            MenuItem m{prefix + "syphon:" + name, "Syphon: " + name};
            m.checked = layer && v.layerSpec == "syphon:" + name;
            m.separatorBefore = first;
            first = false;
            out.push_back(std::move(m));
        }
        return out;
    };
    MenuItem layer{"", "Color Layer"};
    layer.children = sources("video:layer:", true);
    items.push_back(std::move(layer));
    MenuItem sprite{"", "Video Sprite"};
    sprite.children = sources("video:sprite:", false);
    items.push_back(std::move(sprite));
    MenuItem stopLayer{"videostop:layer", "Stop Color Layer"};
    stopLayer.enabled = v.layerActive || !v.layerPending.empty();
    stopLayer.separatorBefore = true;
    items.push_back(std::move(stopLayer));
    MenuItem stopSprites{"videostop:sprite", "Stop Video Sprites"};
    stopSprites.enabled = !v.sprites.empty();
    items.push_back(std::move(stopSprites));
    auto choice = [](const char* title, const char* key, const std::string& cur,
                     std::initializer_list<const char*> values) {
        MenuItem m{"", title};
        for (const char* val : values) {
            MenuItem c{std::string("videoset:") + key + ":" + val, val};
            c.checked = cur == val;
            m.children.push_back(std::move(c));
        }
        return m;
    };
    MenuItem blend = choice("Blend", "blend", v.blend, {"normal", "add", "screen", "multiply"});
    blend.separatorBefore = true;
    items.push_back(std::move(blend));
    items.push_back(choice("Order", "order", v.order, {"over", "under"}));
    items.push_back(choice("Fit", "fit", v.fit, {"cover", "contain", "stretch"}));
    items.push_back(choice("Key", "key", v.key, {"none", "luma", "chroma"}));
    MenuItem opacity{"", "Opacity"};
    for (int pct : {100, 75, 50, 25}) {
        MenuItem c{"videoset:opacity:" + std::to_string(pct / 100.0), std::to_string(pct) + "%"};
        c.checked = std::fabs(v.opacity * 100.0 - pct) < 0.5;
        opacity.children.push_back(std::move(c));
    }
    items.push_back(std::move(opacity));
    MenuItem syphon{std::string("videoset:syphon:") + (v.toSyphon ? "off" : "on"), "Video in Syphon Output"};
    syphon.checked = v.toSyphon;
    items.push_back(std::move(syphon));
    return items;
}

void App::selectVideoMenu(const std::string& id) {
    if (id == "opensettings:camera") {
        video::openCameraPrivacySettings();
        return;
    }
    if (!director_) return;
    std::string err;
    if (auto c = engine::parseCommand(id, &err)) director_->commands().push(std::move(*c));
    else rlog("[reforce] video menu: %s\n", err.c_str());
}

void App::logStats(double now) {
    if (options_.statsInterval <= 0 || now - lastStats_ < options_.statsInterval) return;
    const double dt = now - lastStats_;
    const render::Presenter::Stats ps = presenter_->takeStats();
    rlog(
                 "[reforce] t=%.1fs: %.1f fps (display link %llu, timer %llu, skipped %llu), "
                 "presented %llu (no drawable %llu, max wait %.1f ms), window %s, syphon clients: %s\n",
                 now - startTime_, double(framesLink_ + framesTimer_ - framesSkipped_) / dt,
                 (unsigned long long)framesLink_, (unsigned long long)framesTimer_,
                 (unsigned long long)framesSkipped_, (unsigned long long)ps.presented,
                 (unsigned long long)ps.noDrawable, ps.maxAcquireMs, windowVisible_ ? "visible" : "hidden",
                 syphon_->hasClients() ? "yes" : "no");
    if (director_) {
        const int gn = gpu_->count.exchange(0);
        const double gms = gpu_->sumMs.exchange(0);
        const auto& tm = director_->visualizer().lastTimings();
        rlog(
                     "[reforce]   sim %.1f steps/s, cpu %.2f ms/frame (last step: wave %.2f, geom %.2f, color %.2f, "
                     "encode %.2f; %zu segments), gpu %.2f ms/frame; audio: %s\n",
                     double(stepsSinceStats_) / dt, cpuFrames_ ? cpuMsSum_ / double(cpuFrames_) : 0.0, tm.waveEvalMs,
                     tm.geometryMs, tm.colorMs, tm.encodeMs, tm.segments, gn ? gms / gn : 0.0,
                     audio_ ? audio_->cachedDescription().c_str() : "none");
        const perf::PerfSummary ps2 = stats_->summary(perf::renderAllocs().load());
        rlog("[reforce]   frames: work p50 %.2f / p99 %.2f / max %.2f ms, hitches %llu in this interval (%llu since "
             "warm-up), render-thread Metal allocations %llu\n",
             ps2.workP50, ps2.workP99, ps2.workMax, (unsigned long long)hitchesSinceStats_,
             (unsigned long long)ps2.hitches, (unsigned long long)ps2.renderAllocs);
        rlog("[reforce]   beat: %s\n", director_->visualizer().beatSummary().c_str());
        hitchesSinceStats_ = 0;
        stepsSinceStats_ = 0;
        cpuMsSum_ = 0;
        cpuFrames_ = 0;
    }
    lastStats_ = now;
    framesLink_ = framesTimer_ = framesSkipped_ = 0;
}

bool App::onKey(const platform::KeyEvent& key) {
    if (!director_) return false;
    auto cmds = hotkeyCommands(key, [this](engine::Layer l) { return director_->visualizer().blendMode(l); });
    for (auto& c : cmds) director_->commands().push(std::move(c));
    return !cmds.empty();
}

std::vector<platform::AudioInputItem> App::audioInputs() {
    std::vector<platform::AudioInputItem> items;
    if (!audio_ || audio_->hasFile()) return items;  // --audio-file: no live input to choose
    const std::string cur = audio_->deviceName(), curUid = audio_->deviceUid();
    const auto devices = audio::listInputDevices();
    std::string defaultTitle = "Default Input";
    for (const auto& d : devices)
        if (d.isDefaultInput) defaultTitle += " (" + d.name + ")";
    items.push_back({"default", defaultTitle, audio_->followsDefault(), false});
    const bool system = audio_->isSystemAudio();
    if (audio::systemAudioSupported()) items.push_back({audio::kSystemAudioId, audio::kSystemAudioName, system, false});
    bool first = true;
    for (const auto& d : devices) {
        const bool on = audio_->hasDevice() && !audio_->followsDefault() && !system && (d.uid == curUid || d.name == cur);
        items.push_back({d.name, d.name, on, first});
        first = false;
    }
    items.push_back({"none", "No Audio", !audio_->hasDevice(), true});
    return items;
}

void App::selectAudioInput(const std::string& id) {
    if (!audio_ || audio_->hasFile()) return;
    deferredAnswer_.reset();  // an explicit choice replaces a pending one
    openAudioInput(id);
    if (audioInputChanged_) audioInputChanged_(id);
}

void App::openAudioInput(const std::string& id) {
    if (!audio_ || audio_->hasFile()) return;
    // Opened/closed on the audio control thread (never the render thread);
    // it logs the result.
    if (id == "none") audio_->requestClose();
    else audio_->requestDevice(id == "default" ? std::string() : id);
}

void App::onShutdown() {
    presenter_->stop();  // may wait for a blocked nextDrawable (<= ~1 s)
    // Drain the GPU: command buffers on one queue complete in order.
    MTL::CommandBuffer* fence = queue_->commandBuffer();
    fence->commit();
    fence->waitUntilCompleted();
    // Room lights: the panels go back to what they showed before (blocks up to a few seconds when they don't answer).
    if (director_) director_->visualizer().shutdownLights();
    if (options_.perfReport > 0) {
        stats_->finalize(0);
        const perf::PerfSummary s = stats_->summary(perf::renderAllocs().load());
        std::printf("==== reforce perf report: render %ux%u, sim %.0f Hz, %s, frames after %.0f s warm-up "
                    "(display link %llu, timer %llu in total)\n%s",
                    renderer_->width(), renderer_->height(), options_.fps,
                    options_.stressInterval > 0 ? "stress" : "normal", options_.perfWarmup,
                    (unsigned long long)totalLink_, (unsigned long long)totalTimer_,
                    perf::FrameStats::report(s).c_str());
        std::fflush(stdout);
    }
    std::fprintf(stderr, "[reforce] shutting down\n");
}

}  // namespace reforce::app

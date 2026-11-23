// Visualizer: 3D solids (PLAN 5L) and the WaveShape tile they wear.
#include "app/Visualizer.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include <pthread.h>

#include "audio/Analyzer.hpp"
#include "engine/DanceClips.hpp"
#include "engine/MediaOptions.hpp"
#include "engine/Log.hpp"
#include "engine/Perf.hpp"
#include "lights/LightsOutput.hpp"
#include "render/ImageFiles.hpp"
#include "render/LightsCapture.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"

#include "app/VisualizerInternal.hpp"

namespace reforce::app {
using engine::Layer;
using namespace visdetail;

// The WaveShape tile (PLAN 5L.3): the morph output again, in the scene's own units. The tile shows the config's
// square [-1, 1]^2 in authored coordinates (normalized x times X_EXTENTS), centred: with Aspc = 0 that is the whole
// frame (unstretched: a circle the config draws is round on the faces, as authored), with Aspc = 1 the square on
// the short axis (the long axis beyond +-1 falls outside the tile, but faces only show the tile's inscribed circle
// anyway). Line widths: the frame's widths scaled by the content scale tile / frame height (the tile's 2 units vs
// the frame's) times kTileWidthBoost, at least kTileMinWidth tile px, so lines stay legible on faces that show the
// tile minified and survive the mip filtering.
void Visualizer::appendTileSegments(const engine::MorphFrame& frame, std::vector<engine::LineSegment>& out) const {
    constexpr double kTileMinWidth = 3.0;
    constexpr double kTileWidthBoost = 2.0;
    const double T = double(solids_->tileSize());
    const double W = double(opts_.width), H = double(opts_.height);
    const double ex = wsExtents_.first > 0 ? wsExtents_.first : 1.0;
    const double ey = wsExtents_.second > 0 ? wsExtents_.second : 1.0;
    const engine::FrameMapping map{T, T, 1.0 / ex, 1.0 / ey};
    const std::size_t first = out.size();
    engine::appendSegments(frame, map, lineStyle_, out);
    // appendSegments sized the widths for a T x T frame; rescale to frame width x T / H.
    const double atTile = engine::lineWidthPixels(1.0, lineStyle_, T, T);
    const double atFrame = engine::lineWidthPixels(1.0, lineStyle_, W, H);
    const double k = (atTile > 1e-9 ? atFrame / atTile * T / H : T / H) * kTileWidthBoost;
    const float minR = float(0.5 * kTileMinWidth);
    for (std::size_t i = first; i < out.size(); ++i) out[i].radius = std::max(float(out[i].radius * k), minR);
}

int Visualizer::freeSolidSlot() const {
    if (!solids_) return -1;
    for (int slot = 0; slot < solids_->maxLive(); ++slot) {
        bool used = false;
        for (const auto& s : solidTests_) used = used || s.slot == slot;
        for (const auto& s : liveSolids_) used = used || s.slot == slot;
        if (!used) return slot;
    }
    return -1;
}

bool Visualizer::startSolid(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label, double t,
                            const FadeSpec& fade) {
    if (!cfg) return false;
    if (!solids_) {
        rlog("[reforce] Solid \"%s\": 3D solids are unavailable\n", label.c_str());
        return false;
    }
    std::string written;
    const auto mesh = engine::solidMesh(*cfg, &written);
    if (!mesh) {
        rlog("[reforce] Solid \"%s\": unknown Mesh \"%s\" (tetrahedron, cube, octahedron, dodecahedron, icosahedron, "
             "sphere, torus, prism)\n", label.c_str(), written.c_str());
        return false;
    }
    makeRoomForObject(t, fade.fade, label);
    int slot = freeSolidSlot();
    if (slot < 0) {
        if (liveSolids_.empty()) {
            rlog("[reforce] Solid \"%s\": no free solid slot (reforce.Object.MaxLive)\n", label.c_str());
            return false;
        }
        // Every target is taken (several quick starts in a row): the oldest fading one goes at once.
        auto it = std::find_if(liveSolids_.begin(), liveSolids_.end(), [&](const auto& s) { return s.run->fadingOut(t); });
        if (it == liveSolids_.end()) it = liveSolids_.begin();
        rlog("[reforce] Solid \"%s\" removed for \"%s\" (no free render target)\n", it->run->name().c_str(),
             label.c_str());
        slot = it->slot;
        liveSolids_.erase(it);
    }
    perf::tag(perf::TagSolidStart);
    scene::Environment env = environment(t);
    env.seed = opts_.seed + 0x1000 * ++loads_;
    auto run = std::make_unique<engine::SolidRun>(std::move(cfg), env, label, *mesh, t, fade.fade, fade.defaultHold,
                                                  fade.fn);
    run->setHold(engine::holdForBars(run->bars(), fade.fade, syncInput(), phrase_.state().bars,
                                     phrase_.state().quantum));  // PBar
    const auto& e = run->envelope();
    rlog("[reforce] Solid \"%s\" starts: %s, Text=%d, slot %d, fade %.2f s, hold %.2f s\n", label.c_str(),
         engine::meshKindName(*mesh), run->text(), slot, e.fade, e.hold);
    liveSolids_.push_back({std::move(run), slot});
    return true;
}

void Visualizer::killSolids(double t, double fade) {
    if (liveSolids_.empty()) return;
    perf::tag(perf::TagKill);
    rlog("[reforce] %zu solid(s) killed%s\n", liveSolids_.size(), fade > 0 ? ": fading out" : "");
    if (!(fade > 0)) {
        liveSolids_.clear();
        return;
    }
    for (auto& s : liveSolids_) s.run->fadeOutFrom(t, fade);
}

std::vector<std::string> Visualizer::solidNames() const {
    std::vector<std::string> n;
    for (const auto& s : liveSolids_) n.push_back(s.run->name());
    return n;
}

void Visualizer::addSolidQuads(const render::SolidQuad& sq, const engine::SpriteDrawSpec& ds,
                               std::vector<render::SpriteQuad>& feedback,
                               std::vector<render::SpriteQuad>& display, bool over) const {
    engine::SpriteLayerPlan plan = engine::spriteLayerPlan(ds, spriteLayering_, spriteTrails_, 0.0);
    if (over) {
        // Over = 1: Pen into the feedback, EPen onto the display, each a premultiplied over with the solid's
        // coverage as alpha (the sprite pass's alpha-blend pipeline), so its intensities land exactly.
        const bool beforeLines = plan.feedbackBeforeLines;
        plan = engine::SpriteLayerPlan{};
        plan.feedbackBeforeLines = beforeLines;
        const engine::SpriteLayerDraw d{engine::SpriteOp::Blend, engine::SpriteBlend::AlphaBlend, 0.0, 0.0};
        if (ds.hasPen) {
            plan.feedback[plan.feedbackCount++] = d;
            plan.feedback[plan.feedbackCount - 1].pen = ds.pen;
        }
        if (ds.hasEPen && ds.epen > 0.0) {
            plan.display = d;
            plan.display.pen = ds.epen;
            plan.hasDisplay = true;
        }
    }
    auto quad = [&](const engine::SpriteLayerDraw& w) {
        render::SpriteQuad r;
        r.texture = sq.texture;
        r.cx = sq.cx;
        r.cy = sq.cy;
        r.w = sq.w;
        r.h = sq.h;
        r.u0 = sq.u0;
        r.v0 = sq.v0;
        r.u1 = sq.u1;
        r.v1 = sq.v1;
        r.opacity = float(ds.opacity);
        r.pen = float(w.pen);
        r.blend = int(w.blend);
        r.op = int(w.op);
        r.mix = float(w.mix);
        return r;
    };
    for (int i = 0; i < plan.feedbackCount; ++i) feedback.push_back(quad(plan.feedback[i]));
    if (plan.hasDisplay) display.push_back(quad(plan.display));
}

// Each live solid (scene files, then the --solid-test hook) is drawn into its target slot here (encoded on `cmd`
// before the step's sprite pass) and composited as sprite quads after this step's sprites, with the sprite layering
// plan of its Pen / EPen / Flag; several solids stack like sprites (gforce / trails: the oldest on top). No solid
// live: nothing is encoded.
void Visualizer::stepSolids(MTL::CommandBuffer* cmd, double t, const audio::AudioFrame* audio) {
    if (!solids_) return;
    opaqueSolids_.clear();
    colourSolids_.clear();
    glassDraws_.clear();
    for (auto it = liveSolids_.begin(); it != liveSolids_.end();) {
        if (it->run->expired(t)) {
            rlog("[reforce] Solid \"%s\" ended\n", it->run->name().c_str());
            it = liveSolids_.erase(it);
        } else {
            ++it;
        }
    }
    if (liveSolids_.empty() && solidTests_.empty()) return;
    // Face sources (PLAN 5L.3), once per step and only when a live solid uses them.
    bool wave = false, frame = false;
    bool rubik = false, fifteen = false;
    for (const auto& s : solidTests_) {
        const bool r = engine::isPuzzleMesh(s.spec.draw.mesh);
        rubik = rubik || s.spec.draw.mesh == engine::MeshKind::Rubik;
        fifteen = fifteen || s.spec.draw.mesh == engine::MeshKind::Fifteen;
        wave = wave || (!r && s.spec.draw.text == render::SolidText::Wave);
        frame = frame || (!r && s.spec.draw.text == render::SolidText::Frame);
    }
    for (const auto& s : liveSolids_) {
        const bool r = engine::isPuzzleMesh(s.run->mesh());
        rubik = rubik || s.run->mesh() == engine::MeshKind::Rubik;
        fifteen = fifteen || s.run->mesh() == engine::MeshKind::Fifteen;
        wave = wave || (!r && s.run->text() == int(render::SolidText::Wave));
        frame = frame || (!r && s.run->text() == int(render::SolidText::Frame));
    }
    // Puzzle colours (rubik stickers, fifteen rows): six intensities that read as six colours under the current
    // ColorMap (engine/StickerColors).
    std::array<float, 6> stickers{};
    if (rubik) {
        stickerPicker_.update(palette_, lut_, t);
        stickers = stickerPicker_.levels(t);
        const int sw = stickerPicker_.switches();
        if (sw != stickerLogged_ && stickerPicker_.score() >= 2.0f) {  // (not the blank map at startup)
            stickerLogged_ = sw;
            const auto to = stickerPicker_.levels(t + 3600.0);  // the target (past any ease)
            rlog("[reforce] Rubik colours: %.2f %.2f %.2f %.2f %.2f %.2f (closest pair %.1f CIELAB)\n", to[0], to[1],
                 to[2], to[3], to[4], to[5], stickerPicker_.score());
        }
    }
    // The 15 puzzle: four row colours, light with dark numbers (or dark with light numbers on a map without good light
    // colours), engine::TilePicker; they go into the sticker slots its rows use (render::kFifteenRowColour).
    std::array<float, 6> tiles = stickers;
    if (fifteen) {
        tilePicker_.update(palette_, lut_, t);
        const auto rows = tilePicker_.levels(t);
        for (int r = 0; r < 4; ++r) tiles[std::size_t(render::kFifteenRowColour[r])] = rows[std::size_t(r)];
        const int sw = tilePicker_.switches();
        if (sw != tileLogged_ && tilePicker_.score() >= 2.0f) {
            tileLogged_ = sw;
            const auto to = tilePicker_.levels(t + 3600.0);
            rlog("[reforce] Fifteen colours: %.2f %.2f %.2f %.2f, %s tiles (closest pair %.1f CIELAB)\n", to[0], to[1],
                 to[2], to[3], tilePicker_.light() ? "light" : "dark", tilePicker_.score());
        }
    }
    if (wave) {
        tileSegments_.clear();
        if (wsOutValid_) appendTileSegments(wsOut_, tileSegments_);
        solids_->encodeWaveTile(cmd, tileSegments_.data(), tileSegments_.size());
    }
    if (frame) solids_->encodeFrameTile(cmd);
    solidFeedback_.clear();
    solidDisplay_.clear();
    // Puzzles (rubik, fifteen): also drawn in colour over the colorized frame (encodeColorize, solid_colour_fs).
    auto colourQuad = [&](const render::SolidQuad& sq, double opacity, bool opaque, engine::MeshKind mesh) {
        render::SpriteQuad o;
        o.texture = sq.texture;
        o.cx = sq.cx;
        o.cy = sq.cy;
        o.w = sq.w;
        o.h = sq.h;
        o.u0 = sq.u0;
        o.v0 = sq.v0;
        o.u1 = sq.u1;
        o.v1 = sq.v1;
        o.opacity = float(std::clamp(opacity, 0.0, 1.0));
        o.opaque = opaque;
        o.puzzle = mesh == engine::MeshKind::Fifteen ? 1 : 0;
        o.numeral = o.puzzle == 1 ? tilePicker_.numeral(t) : 0.0f;
        colourSolids_.push_back(o);
    };
    // A glass 15 puzzle (Glas = 1): not drawn into the visuals; colorize draws it over the frame (encodeGlass).
    auto glassDraw = [&](int slot, const engine::SolidPose& pose, bool perspective, const engine::fifteen::Frame& f,
                         double frost, double tint, double bend, double opacity, bool opaque) {
        render::GlassDraw g;
        g.slot = slot;
        g.pose = pose;
        g.perspective = perspective;
        g.fifteen = f;
        // One glass for every button: a light colour of the ColorMap (the first light row colour) halfway to white;
        // etched near-white numbers.
        const auto c = engine::colourAt(palette_, lut_, tiles[std::size_t(render::kFifteenRowColour[0])]);
        for (int r = 0; r < 4; ++r) {
            for (int k = 0; k < 3; ++k) {
                g.tint[std::size_t(r)][std::size_t(k)] = 0.5f * (c[std::size_t(k)] + 1.0f);
                g.numeral[std::size_t(r)][std::size_t(k)] = 0.95f;
            }
        }
        g.frost = float(frost);
        g.tintAmount = float(tint);
        g.bend = float(bend);
        g.opacity = float(std::clamp(opacity, 0.0, 1.0));
        g.opaque = opaque;
        glassDraws_.push_back(g);
    };
    for (auto& s : solidTests_) {
        if (s.start < 0) s.start = t;
        render::SolidDraw d = s.spec.draw;
        if (d.mesh == engine::MeshKind::Rubik) d.stickers = stickers;
        if (d.mesh == engine::MeshKind::Fifteen) d.stickers = tiles;
        const double age = t - s.start;
        d.pose.rotX += s.spec.spin[0] * age;
        d.pose.rotY += s.spec.spin[1] * age;
        d.pose.rotZ += s.spec.spin[2] * age;
        if (d.mesh == engine::MeshKind::Fifteen && s.spec.glass) {
            glassDraw(s.slot, d.pose, d.perspective, d.fifteen, 8.0, 0.15, 1.0, s.spec.opacity, false);
            continue;
        }
        const render::SolidQuad sq = solids_->encode(cmd, s.slot, d);
        if (!s.logged) {
            s.logged = true;
            const auto ref = render::solidReference(solids_->mesh(d.mesh), d, int(opts_.width), int(opts_.height));
            rlog("[reforce] solid test \"%s\": %s, slot %d, msaa %d, extent %.2f,%.2f - %.2f,%.2f px, hull area %.1f "
                 "px2\n",
                 s.spec.text.c_str(), engine::meshKindName(d.mesh), s.slot, solids_->msaa(), ref.x0, ref.y0, ref.x1,
                 ref.y1, ref.area);
            for (const auto& f : ref.faces)
                rlog("[reforce] solid test face: %.1f,%.1f lum %.3f facing %.3f\n", f.x, f.y, f.lum, f.facing);
        }
        if (!sq.visible || !(s.spec.opacity > 0)) continue;
        engine::SpriteDrawSpec ds;
        ds.hasPen = s.spec.pen >= 0;
        ds.hasEPen = s.spec.epen >= 0;
        ds.pen = std::clamp(s.spec.pen, 0.0, 1.0);
        ds.epen = std::clamp(s.spec.epen, 0.0, 1.0);
        ds.opacity = std::clamp(s.spec.opacity, 0.0, 1.0);
        ds.blend = engine::spriteBlendMode(s.spec.flags);
        addSolidQuads(sq, ds, solidFeedback_, solidDisplay_);
        if (engine::isPuzzleMesh(d.mesh)) colourQuad(sq, ds.opacity, false, d.mesh);
    }
    if (!liveSolids_.empty()) {
        const engine::SpriteAudio a = spriteAudio(audio);
        for (auto& s : liveSolids_) {
            const engine::SolidSpec sp = s.run->evaluate(t, a);
            if (!(sp.opacity > 0)) continue;
            render::SolidDraw d;
            d.mesh = sp.mesh;
            d.pose = sp.pose;
            d.perspective = sp.perspective;
            d.text = render::SolidText(std::clamp(sp.text, 0, 2));
            d.shading.fill = float(sp.fill);
            d.shading.shade = float(sp.shade);
            d.shading.edge = float(sp.edge);
            if (sp.mesh == engine::MeshKind::Rubik) {
                const double seed = std::isfinite(sp.seed) ? std::fabs(std::floor(sp.seed)) : 0.0;
                d.rubik = engine::rubik::frameAt(uint64_t(std::min(seed, 1e15)), sp.moves, sp.turn);
                d.stickers = stickers;
                d.body = 0.0f;
            } else if (sp.mesh == engine::MeshKind::Fifteen) {
                const double seed = std::isfinite(sp.seed) ? std::fabs(std::floor(sp.seed)) : 0.0;
                d.fifteen = engine::fifteen::frameAt(uint64_t(std::min(seed, 1e15)), sp.moves, sp.turn, sp.shuffle);
                if (sp.glass) {
                    glassDraw(s.slot, d.pose, d.perspective, d.fifteen, sp.frost, sp.tint, sp.bend, sp.opacity,
                              !sp.colorKey);
                    continue;
                }
                d.stickers = tiles;
                d.body = 0.0f;
                d.shading.shade = float(sp.shade);
            }
            const render::SolidQuad sq = solids_->encode(cmd, s.slot, d);
            if (!sq.visible) continue;
            engine::SpriteDrawSpec ds;
            ds.hasPen = sp.hasPen;
            ds.hasEPen = sp.hasEPen;
            ds.pen = sp.pen;
            ds.epen = sp.epen;
            ds.opacity = std::clamp(sp.opacity, 0.0, 1.0);
            ds.blend = engine::spriteBlendMode(sp.flags);
            addSolidQuads(sq, ds, solidFeedback_, solidDisplay_, sp.over);
            if (engine::isPuzzleMesh(sp.mesh)) colourQuad(sq, ds.opacity, !sp.colorKey, sp.mesh);
            if (!sp.colorKey) {  // opaque in the Syphon alpha / over the backdrop (encodeColorize)
                render::SpriteQuad o;
                o.texture = sq.texture;
                o.cx = sq.cx;
                o.cy = sq.cy;
                o.w = sq.w;
                o.h = sq.h;
                o.u0 = sq.u0;
                o.v0 = sq.v0;
                o.u1 = sq.u1;
                o.v1 = sq.v1;
                o.opacity = float(ds.opacity);
                o.opaque = true;
                opaqueSolids_.push_back(o);
            }
        }
    }
    // Solids go after this step's sprites (on top of them), in start order or oldest on top like the sprites.
    if (engine::spriteOldestOnTop(spriteLayering_)) {
        feedbackQuads_.insert(feedbackQuads_.end(), solidFeedback_.rbegin(), solidFeedback_.rend());
        displayQuads_.insert(displayQuads_.end(), solidDisplay_.rbegin(), solidDisplay_.rend());
    } else {
        feedbackQuads_.insert(feedbackQuads_.end(), solidFeedback_.begin(), solidFeedback_.end());
        displayQuads_.insert(displayQuads_.end(), solidDisplay_.begin(), solidDisplay_.end());
    }
}

}  // namespace reforce::app

// Visualizer: fractal zooms (PLAN 5L.14, docs/3d-design.md "Fractal zoom"): a live fractal solid raymarched into its
// solid slot (render/FractalPipeline) and composited like any solid.
#include "app/Visualizer.hpp"

#include <algorithm>

#include "app/VisualizerInternal.hpp"

namespace reforce::app {

void Visualizer::stepFractal(MTL::CommandBuffer* cmd, LiveSolid& s, double t, const engine::SpriteAudio& a) {
    if (!fractals_) return;
    const engine::SolidSpec sp = s.run->evaluate(t, a);
    const engine::FractalSettings& fs = s.run->fractal();
    if (!(sp.opacity > 0) || !fs.path) return;
    render::FractalDraw d;
    d.kind = fs.kind;
    d.g = fs.path->g;
    d.cam = sp.fractalCam;
    d.slot = s.slot;
    d.res = float(fs.res);
    d.detail = float(fs.detail);
    d.maxSteps = fs.steps;
    d.fill = float(sp.fill);
    d.shade = float(sp.shade);
    d.glow = float(sp.glow);
    d.fog = float(sp.fog);
    const render::SolidQuad sq = fractals_->encode(cmd, d);
    if (!sq.visible) return;
    engine::SpriteDrawSpec ds;
    ds.hasPen = sp.hasPen;
    ds.hasEPen = sp.hasEPen;
    ds.pen = sp.pen;
    ds.epen = sp.epen;
    ds.opacity = std::clamp(sp.opacity, 0.0, 1.0);
    ds.blend = engine::spriteBlendMode(sp.flags);
    addSolidQuads(sq, ds, solidFeedback_, solidDisplay_, sp.over);
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

}  // namespace reforce::app

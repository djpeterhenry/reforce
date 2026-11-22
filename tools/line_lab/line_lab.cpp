// Line antialiasing lab (Phase 5E): offline quality review and GPU timing of
// the line pass, on synthetic segments (our own test geometry, no scene files).
//
//   reforce-line-lab review OUTDIR   numbers on stdout, magnified PNG crops in OUTDIR
//   reforce-line-lab bench [--quick] GPU time of the line pass per AA method at 1080p and 2160p
//
// Methods: off (--no-aa), analytic (legacy per-segment blend), path (per-path
// max coverage, the default; reforce.ThinLines = smooth), gforce (per-path with
// G-Force's rasterization, reforce.ThinLines = gforce), msaa (4x MSAA baseline).
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../tests/line_harness.hpp"
#include "render/ImageFiles.hpp"

using namespace reforce;
using linetest::Field;
using linetest::Harness;

namespace {

struct Method {
    const char* name;
    bool aa;
    render::LineAa m;
    render::ThinLines thin = render::ThinLines::Smooth;
};
const Method kMethods[] = {{"off", false, render::LineAa::Analytic},
                           {"analytic", true, render::LineAa::Analytic},
                           {"path", true, render::LineAa::PerPath},
                           {"gforce", true, render::LineAa::PerPath, render::ThinLines::GForce},
                           {"msaa", true, render::LineAa::Msaa4}};

render::ScenePipelineOptions options(const Method& m, bool half = false, float decay = 1.0f / 255.0f) {
    render::ScenePipelineOptions o;
    o.antialias = m.aa;
    o.lineAa = m.m;
    o.thinLines = m.thin;
    o.format = half ? render::IntensityFormat::Half16 : render::IntensityFormat::GForce8;
    o.decayPerFrame = decay;
    return o;
}

// ---- CPU reference: per-path max coverage (the intended result) ------------------
float segDist(float px, float py, const engine::LineSegment& s, float& h) {
    const float bax = s.bx - s.ax, bay = s.by - s.ay, bb = bax * bax + bay * bay;
    h = bb > 1e-8f ? std::clamp(((px - s.ax) * bax + (py - s.ay) * bay) / bb, 0.0f, 1.0f) : 0.0f;
    const float dx = px - s.ax - bax * h, dy = py - s.ay - bay * h;
    return std::sqrt(dx * dx + dy * dy);
}

std::vector<float> reference(int w, int h, const std::vector<engine::LineSegment>& segs) {
    std::vector<float> img(std::size_t(w) * h, 0.f);
    std::size_t i = 0;
    while (i < segs.size()) {
        std::size_t j = i;
        while (j < segs.size() && segs[j].path == segs[i].path) ++j;
        std::vector<float> cov(img.size(), 0.f), pen(img.size(), 0.f);
        for (std::size_t k = i; k < j; ++k) {
            const auto& s = segs[k];
            const float r = std::max(s.radius, 0.5f);
            const float rr = std::max(s.radius, 0.f);
            float kk = rr / r;
            if (s.ax == s.bx && s.ay == s.by) {  // dots: G-Force's dot energy (as line_cov_energy in the shader)
                const float e = 2.f * rr + (rr > 1.f ? 4.f * std::sqrt(rr * rr - 1.f) : 0.f);
                kk = rr < 2.f ? std::min(1.f, e / (float(M_PI) * (r * r + 1.f / 12.f))) : 1.f;
            }
            const int x0 = std::max(0, int(std::min(s.ax, s.bx) - r - 2)), x1 = std::min(w - 1, int(std::max(s.ax, s.bx) + r + 2));
            const int y0 = std::max(0, int(std::min(s.ay, s.by) - r - 2)), y1 = std::min(h - 1, int(std::max(s.ay, s.by) + r + 2));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    float hh;
                    const float d = segDist(x + 0.5f, y + 0.5f, s, hh);
                    const float c = kk * std::clamp(r + 0.5f - d, 0.f, 1.f);
                    const std::size_t p = std::size_t(y) * w + x;
                    if (c > 0 && c >= cov[p]) {
                        cov[p] = c;
                        pen[p] = s.penA + (s.penB - s.penA) * hh;
                    }
                }
        }
        for (std::size_t p = 0; p < img.size(); ++p)
            if (cov[p] > 0) img[p] = img[p] + (pen[p] * 255.f - img[p]) * cov[p];
        i = j;
    }
    return img;
}

void savePng(const std::string& path, const std::vector<float>& img, int w, int x0, int y0, int cw, int ch, int zoom) {
    std::vector<std::uint8_t> bgra(std::size_t(cw * zoom) * ch * zoom * 4);
    for (int y = 0; y < ch * zoom; ++y)
        for (int x = 0; x < cw * zoom; ++x) {
            const float v = img[std::size_t(y0 + y / zoom) * w + x0 + x / zoom];
            const auto b = std::uint8_t(std::clamp(std::lround(v), 0L, 255L));
            std::uint8_t* p = &bgra[(std::size_t(y) * cw * zoom + x) * 4];
            p[0] = p[1] = p[2] = b;
            p[3] = 255;
        }
    render::writePngBgra8(path, bgra.data(), cw * zoom, ch * zoom, std::size_t(cw) * zoom * 4);
}

// ---- review ----------------------------------------------------------------------
void reviewJoints(MTL::Device* dev, const std::string& out) {
    const int W = 640, H = 200;
    std::printf("\n## Joints (3 px wide, LWdt-like; V shapes with turn angles 10/45/90/150 deg; dense circle of\n"
                "## 0.6-px segments). Error vs the per-path max-coverage reference, in levels.\n");
    for (int bg = 0; bg < 2; ++bg) {
        std::vector<engine::LineSegment> segs;
        if (bg) linetest::addPolyline(segs, {0, 100, float(W), 100}, 0.8f, 120.f);  // Pen .8 background path
        const float pen = bg ? 0.3f : 0.8f, r = 1.5f;
        const float turns[] = {10, 45, 90, 150};
        for (int k = 0; k < 4; ++k) {
            const float cx = 60.f + 120.f * k + 0.37f, cy = 100.3f, L = 50.f;
            const float a = turns[k] * float(M_PI) / 180.f;
            linetest::addPolyline(segs, {cx - L, cy, cx, cy, cx - L * std::cos(a), cy - L * std::sin(a)}, pen, r);
        }
        {
            std::vector<float> xy;
            for (int i = 0; i <= 400; ++i) {
                const float t = float(i) / 400.f * 2.f * float(M_PI);
                xy.push_back(570.f + 38.f * std::cos(t));
                xy.push_back(100.f + 38.f * std::sin(t));
            }
            linetest::addPolyline(segs, xy, pen, r);
        }
        const auto ref = reference(W, H, segs);
        std::printf("background %s, Pen %.1f:\n", bg ? "Pen .8 path" : "black", pen);
        std::printf("  %-9s %12s %12s %14s %14s\n", "method", "V max|err|", "V px>1 lvl", "circle max|err|",
                    "circle px>1");
        for (const Method& m : kMethods) {
            Harness hs(dev, W, H, options(m), Field::Void);
            hs.step(segs);
            const auto img = hs.read();
            double vMax = 0, cMax = 0;
            int vN = 0, cN = 0;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const double e = std::abs(img[std::size_t(y) * W + x] - ref[std::size_t(y) * W + x]);
                    if (x < 520) { vMax = std::max(vMax, e); vN += e > 1.0; }
                    else { cMax = std::max(cMax, e); cN += e > 1.0; }
                }
            std::printf("  %-9s %12.1f %12d %14.1f %14d\n", m.name, vMax, vN, cMax, cN);
            savePng(out + "/joints_" + (bg ? "bg_" : "black_") + m.name + ".png", img, W, 0, 40, W, 120, 4);
        }
        savePng(out + "/joints_" + (bg ? "bg_" : "black_") + "reference.png", ref, W, 0, 40, W, 120, 4);
    }
}

void reviewThin(MTL::Device* dev, const std::string& out) {
    const int W = 600, H = 160;
    const float widths[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
    std::printf("\n## Thin lines (Pen 1, slope 1/37): per-column energy E = sum/255 (ideal = width in px),\n"
                "## ripple = std/mean of E over columns (aliasing; moves as the line moves = crawl), peak level.\n");
    std::printf("  %-9s", "method");
    for (float wd : widths) std::printf("   w=%-4.2f E/ripple/peak  ", wd);
    std::printf("\n");
    for (const Method& m : kMethods) {
        std::vector<engine::LineSegment> segs;
        for (int k = 0; k < 6; ++k) {
            const float y = 15.f + 25.f * k + 0.2f;
            linetest::addPolyline(segs, {20, y, 580, y + 560.f / 37.f}, 1.0f, 0.5f * widths[k]);
        }
        Harness hs(dev, W, H, options(m), Field::Void);
        hs.step(segs);
        const auto img = hs.read();
        std::printf("  %-9s", m.name);
        for (int k = 0; k < 6; ++k) {
            std::vector<double> e;
            float peak = 0;
            for (int x = 40; x < 560; ++x) {
                const float yc = 15.f + 25.f * k + 0.2f + (x + 0.5f - 20.f) / 37.f;
                double s = 0;
                for (int y = int(yc) - 6; y <= int(yc) + 6; ++y) {
                    s += img[std::size_t(y) * W + x];
                    peak = std::max(peak, img[std::size_t(y) * W + x]);
                }
                e.push_back(s / 255.0);
            }
            double mean = 0, var = 0;
            for (double v : e) mean += v;
            mean /= double(e.size());
            for (double v : e) var += (v - mean) * (v - mean);
            const double sd = std::sqrt(var / double(e.size()));
            std::printf("   %5.2f/%5.1f%%/%3.0f      ", mean, 100.0 * sd / std::max(mean, 1e-9), peak);
        }
        std::printf("\n");
        savePng(out + "/thin_" + m.name + ".png", img, W, 0, 0, 200, H, 4);
    }
}

void reviewDots(MTL::Device* dev, const std::string& out) {
    const int W = 400, H = 120;
    const float radii[] = {0.5f, 1.0f, 2.0f, 4.0f};
    std::printf("\n## Dots (Pen 1) at 16 sub-pixel offsets: area/(pi r^2) mean and spread (max-min) over offsets,\n"
                "## anisotropy = sqrt(lambda_max/lambda_min) of the intensity covariance (1 = round), worst offset.\n");
    std::printf("  %-9s", "method");
    for (float r : radii) std::printf("     r=%-3.1f area/spread/aniso", r);
    std::printf("\n");
    for (const Method& m : kMethods) {
        std::printf("  %-9s", m.name);
        for (int ri = 0; ri < 4; ++ri) {
            const float r = radii[ri];
            std::vector<engine::LineSegment> segs;
            for (int k = 0; k < 16; ++k)
                linetest::addDot(segs, 20.f + 24.f * k + 0.25f * float(k % 4), 60.f + 0.25f * float(k / 4), 1.f, r);
            Harness hs(dev, W, H, options(m), Field::Void);
            hs.step(segs);
            const auto img = hs.read();
            double amin = 1e9, amax = 0, asum = 0, aniso = 1;
            for (int k = 0; k < 16; ++k) {
                const double cx = segs[k].ax, cy = segs[k].ay;
                double s = 0, mx = 0, my = 0, sxx = 0, syy = 0, sxy = 0;
                for (int y = int(cy) - 8; y <= int(cy) + 8; ++y)
                    for (int x = int(cx) - 8; x <= int(cx) + 8; ++x) {
                        const double v = img[std::size_t(y) * W + x] / 255.0;
                        s += v;
                        mx += v * (x + 0.5);
                        my += v * (y + 0.5);
                    }
                mx /= s;
                my /= s;
                for (int y = int(cy) - 8; y <= int(cy) + 8; ++y)
                    for (int x = int(cx) - 8; x <= int(cx) + 8; ++x) {
                        const double v = img[std::size_t(y) * W + x] / 255.0, dx = x + 0.5 - mx, dy = y + 0.5 - my;
                        sxx += v * dx * dx;
                        syy += v * dy * dy;
                        sxy += v * dx * dy;
                    }
                const double tr = sxx + syy, det = sxx * syy - sxy * sxy;
                const double d = std::sqrt(std::max(tr * tr / 4 - det, 0.0));
                const double l1 = tr / 2 + d, l2 = std::max(tr / 2 - d, 1e-12);
                aniso = std::max(aniso, std::sqrt(l1 / l2));
                const double a = s / (M_PI * r * r);
                amin = std::min(amin, a);
                amax = std::max(amax, a);
                asum += a;
            }
            std::printf("     %5.2f/%5.2f/%5.3f        ", asum / 16, amax - amin, aniso);
            if (ri == 1 || ri == 2) savePng(out + "/dots_r" + std::to_string(int(r)) + "_" + m.name + ".png", img, W, 0, 40, 200, 40, 8);
        }
        std::printf("\n");
    }
}

// G-Force's dot energy (probe #32): column spans of half-height sqrt(r^2 - dx^2) at a snapped center.
double gforceDotEnergy(double r) {
    double e = 0;
    for (int dx = -int(r); dx <= int(r); ++dx)
        if (dx * dx < r * r) e += 2.0 * std::sqrt(r * r - dx * dx);
    return e;
}

void reviewDotEnergy(MTL::Device* dev) {
    const int W = 420, H = 60;
    // LWdt .05 .1 .2 .3 .4 .5 1 at 1260 px (2.838 px per LWdt): r = 1.419 LWdt
    const float lw[] = {0.05f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 1.0f};
    std::printf("\n## Small dots (Pen 1) at 16 sub-pixel offsets, LWdt at 1260 px: energy (sum/255, px^2) mean and\n"
                "## spread over offsets, mean peak level. G-Force (probe #32): energy = column-span area, peak 255 min(1, w).\n");
    std::printf("  %-9s", "method");
    for (float l : lw) std::printf("   LWdt %-4.2f E/spread/peak", l);
    std::printf("\n  %-9s", "G-Force");
    for (float l : lw) {
        const double r = 0.5 * l * (1260 - 68) / 420.0;
        std::printf("   %5.3f/%5.3f/%4.0f        ", gforceDotEnergy(r), 0.0, 255 * std::min(1.0, 2 * r));
    }
    std::printf("\n");
    for (const Method& m : kMethods) {
        std::printf("  %-9s", m.name);
        for (float l : lw) {
            const float r = 0.5f * l * (1260 - 68) / 420.f;
            std::vector<engine::LineSegment> segs;
            for (int k = 0; k < 16; ++k)
                linetest::addDot(segs, 12.f + 25.f * k + 0.25f * float(k % 4), 30.f + 0.25f * float(k / 4), 1.f, r);
            Harness hs(dev, W, H, options(m), Field::Void);
            hs.step(segs);
            const auto img = hs.read();
            double emin = 1e9, emax = 0, esum = 0, psum = 0;
            for (int k = 0; k < 16; ++k) {
                double e = 0, pk = 0;
                for (int y = int(segs[k].ay) - 5; y <= int(segs[k].ay) + 5; ++y)
                    for (int x = int(segs[k].ax) - 5; x <= int(segs[k].ax) + 5; ++x) {
                        const double v = img[std::size_t(y) * W + x];
                        e += v / 255.0;
                        pk = std::max(pk, v);
                    }
                emin = std::min(emin, e);
                emax = std::max(emax, e);
                esum += e;
                psum += pk;
            }
            std::printf("   %5.3f/%5.3f/%4.0f        ", esum / 16, emax - emin, psum / 16);
        }
        std::printf("\n");
    }
}

void reviewFlow(MTL::Device* dev, const std::string& out) {
    const int W = 480, H = 270;
    std::printf("\n## Flow + decay: a static 3-px polyline drawn every frame for 300 frames under a slow zoom (1.004)\n"
                "## + rotate (0.003 rad) field with 1/255 decay. 8-bit buffer vs 16-bit float (hq) buffer; levels.\n");
    std::printf("  %-9s %10s %10s %14s %16s\n", "method", "rms(8-hq)", "max|8-hq|", "f2f mean|d| 8", "f2f mean|d| hq");
    for (const Method& m : kMethods) {
        if (!m.aa && false) continue;
        std::vector<engine::LineSegment> segs;
        std::vector<float> xy;
        for (int i = 0; i <= 60; ++i) {
            const float t = float(i) / 60.f;
            xy.push_back(140.f + 200.f * t);
            xy.push_back(135.f + 40.f * std::sin(t * 6.f));
        }
        linetest::addPolyline(segs, xy, 1.0f, 1.5f);
        std::vector<float> img[2], prev[2];
        for (int f = 0; f < 2; ++f) {
            Harness hs(dev, W, H, options(m, f == 1), Field::ZoomRotate);
            for (int i = 0; i < 299; ++i) hs.step(segs);
            prev[f] = hs.read();
            hs.step(segs);
            img[f] = hs.read();
        }
        double se = 0, mx = 0, d8 = 0, dh = 0;
        int n = 0;
        for (std::size_t p = 0; p < img[0].size(); ++p) {
            if (img[1][p] < 0.5f && img[0][p] < 0.5f) continue;
            const double e = img[0][p] - img[1][p];
            se += e * e;
            mx = std::max(mx, std::abs(e));
            d8 += std::abs(img[0][p] - prev[0][p]);
            dh += std::abs(img[1][p] - prev[1][p]);
            ++n;
        }
        std::printf("  %-9s %10.2f %10.1f %14.3f %16.3f\n", m.name, std::sqrt(se / std::max(n, 1)), mx, d8 / std::max(n, 1),
                    dh / std::max(n, 1));
        savePng(out + "/flow_8bit_" + m.name + ".png", img[0], W, 120, 60, 240, 150, 3);
        if (std::strcmp(m.name, "path") == 0) savePng(out + "/flow_hq_path.png", img[1], W, 120, 60, 240, 150, 3);
    }
}

// ---- bench -----------------------------------------------------------------------
struct Scene {
    const char* name;
    std::vector<engine::LineSegment> segs;
};

std::vector<Scene> benchScenes(int W, int H) {
    const float px = float(H - 68) / 420.f;  // px per LWdt (AutoLineScale)
    std::vector<Scene> s;
    auto curve = [&](std::vector<engine::LineSegment>& out, int paths, int segsPerPath, float lwdt, int seed) {
        for (int p = 0; p < paths; ++p) {
            std::vector<float> xy;
            const float ph = float(p * 7 + seed), fa = 3.f + float(p % 5), fb = 2.f + float(p % 3);
            const float cx = W * (0.5f + 0.3f * std::sin(ph)), cy = H * (0.5f + 0.3f * std::cos(ph * 1.3f));
            const float rad = paths > 50 ? 0.04f * H : 0.42f * H;
            for (int i = 0; i <= segsPerPath; ++i) {
                const float t = float(i) / float(segsPerPath) * 2.f * float(M_PI);
                xy.push_back(cx + rad * std::sin(fa * t + ph) * (paths > 50 ? 1.f : float(W) / H * 0.9f));
                xy.push_back(cy + rad * std::sin(fb * t));
            }
            linetest::addPolyline(out, xy, 0.4f + 0.6f * float(p % 7) / 6.f, 0.5f * lwdt * px);
        }
    };
    Scene a{"6.4k segs, 8 long paths, LWdt 1", {}};
    curve(a.segs, 8, 800, 1.f, 1);
    s.push_back(std::move(a));
    Scene b{"6.4k segs, 8 paths, LWdt 4", {}};
    curve(b.segs, 8, 800, 4.f, 2);
    s.push_back(std::move(b));
    Scene c{"2000 paths x 4 segs, LWdt 1.5", {}};
    curve(c.segs, 2000, 4, 1.5f, 3);
    s.push_back(std::move(c));
    Scene d{"6000 dots, LWdt 2", {}};
    for (int i = 0; i < 6000; ++i)
        linetest::addDot(d.segs, float((i * 7919) % W) + 0.3f, float((i * 104729 / 7) % H) + 0.6f, 0.9f, px);
    s.push_back(std::move(d));
    Scene e{"16k segs, 64 paths, LWdt 2", {}};
    curve(e.segs, 64, 256, 2.f, 4);
    s.push_back(std::move(e));
    return s;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void bench(MTL::Device* dev, bool quick) {
    const int sizes[2][2] = {{1920, 1080}, {3840, 2160}};
    const int K = quick ? 10 : 60, reps = quick ? 3 : 7;
    std::printf("Line pass GPU time per step (ms) = median over %d runs of [GPU time of %d steps / %d] minus the\n"
                "same with no segments and AA off (flow pass only). Field: identity, 8-bit buffer.\n",
                reps, K, K);
    for (const auto& sz : sizes) {
        const int W = sz[0], H = sz[1];
        const auto scenes = benchScenes(W, H);
        // baseline: flow only
        double base;
        {
            Harness hs(dev, W, H, options(kMethods[0]), Field::Identity);
            hs.step({}, K);
            std::vector<double> t;
            for (int r = 0; r < reps; ++r) t.push_back(hs.step({}, K) / K);
            base = median(t);
        }
        std::printf("\n%dx%d (flow pass alone: %.3f ms)\n  %-32s", W, H, base, "scene");
        for (const Method& m : kMethods) std::printf(" %9s", m.name);
        std::printf("\n");
        for (const Scene& sc : scenes) {
            std::printf("  %-32s", sc.name);
            for (const Method& m : kMethods) {
                Harness hs(dev, W, H, options(m), Field::Identity);
                hs.step(sc.segs, K);  // warm-up
                std::vector<double> t;
                for (int r = 0; r < reps; ++r) t.push_back(hs.step(sc.segs, K) / K);
                std::printf(" %9.3f", median(t) - base);
            }
            std::printf("\n");
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    MTL::Device* dev = MTL::CreateSystemDefaultDevice();
    if (!dev) {
        std::fprintf(stderr, "no Metal device\n");
        return 1;
    }
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "review" && argc > 2) {
        const std::string out = argv[2];
        std::filesystem::create_directories(out);
        reviewJoints(dev, out);
        reviewThin(dev, out);
        reviewDots(dev, out);
        reviewDotEnergy(dev);
        reviewFlow(dev, out);
    } else if (cmd == "bench") {
        bench(dev, argc > 2 && std::strcmp(argv[2], "--quick") == 0);
    } else {
        std::fprintf(stderr, "usage: reforce-line-lab review OUTDIR | bench [--quick]\n");
        return 2;
    }
    dev->release();
    pool->release();
    return 0;
}

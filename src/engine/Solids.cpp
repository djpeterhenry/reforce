#include "engine/Solids.hpp"

#include "engine/Log.hpp"
#include "engine/ModelFile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace reforce::engine {

std::optional<MeshKind> solidMesh(const scene::CompiledConfig& cfg, std::string* written) {
    std::string name;
    if (const scene::CompiledKey* k = cfg.find("MESH")) name = k->text;
    const auto b = name.find_first_not_of(" \t\r\n");
    const auto e = name.find_last_not_of(" \t\r\n");
    name = b == std::string::npos ? std::string() : name.substr(b, e - b + 1);
    if (written) *written = name;
    if (name.empty()) return MeshKind::Icosahedron;
    return parseMeshKind(name);
}

SolidRun::SolidRun(std::shared_ptr<const scene::CompiledConfig> cfg, const scene::Environment& env,
                   std::string name, MeshKind mesh, double startTime, double fade, double defaultHold,
                   std::shared_ptr<const TransitionFunction> fn)
    : cfg_(std::move(cfg)), name_(std::move(name)), mesh_(mesh), start_(startTime) {
    scene::Environment e = env;
    e.loadTime = startTime;
    inst_ = std::make_unique<scene::SolidInstance>(*cfg_, e);
    double hold = inst_->duration();
    if (!(std::isfinite(hold) && hold >= 0)) hold = defaultHold;
    env_.fade = std::max(0.0, fade);
    env_.hold = std::min(hold, 24.0 * 3600.0);
    env_.fn = std::move(fn);
    if (mesh_ == MeshKind::Soma) {
        const std::string figs = inst_->literal("FIGS");
        soma_ = std::make_unique<soma::Player>(soma::parseFigures(figs.empty() ? "cube" : figs, &somaUnknown_));
        soma::Timing tm;
        tm.len = inst_->loaded("LEN", tm.len);
        tm.hold = inst_->loaded("HOLD", tm.hold);
        tm.each = inst_->loaded("EACH", tm.each);
        somaTiming_ = soma::clampTiming(tm);
    }
    if (mesh_ == MeshKind::Labyrinth) {
        auto whole = [&](const char* key, double fallback, int lo, int hi) {
            const double v = inst_->loaded(key, fallback);
            return std::isfinite(v) ? std::clamp(int(std::lround(std::clamp(v, -1e6, 1e6))), lo, hi) : int(fallback);
        };
        grid_ = whole("GRID", 8, labyrinth::kMinGrid, labyrinth::kMaxGrid);
        beats_ = whole("BEATS", 32, labyrinth::kMinBeats, labyrinth::kMaxBeats);
        traps_ = whole("TRAPS", 3, 0, labyrinth::kMaxTraps);
        labyrinth_ = std::make_unique<labyrinth::Player>(beats_);
    }
    if (mesh_ == MeshKind::Hanoi) {
        auto num = [&](const char* key, double fallback) {
            const double v = inst_->loaded(key, fallback);
            return std::isfinite(v) ? std::clamp(v, -1e6, 1e6) : fallback;
        };
        hanoi::Settings hs;
        hs.discs = int(std::lround(num("DISC", 6)));
        hs.each = num("EACH", 1);
        hs.reset = num("RSET", 0) >= 0.5 ? 1 : 0;
        const hanoi::Settings c = hanoi::clampSettings(hs);
        // No Beats: one move slot for the reset (6 discs: 64 beats).
        hs.beats = num("BEATS", (hanoi::moveCount(c.discs) + 1) * c.each);
        hanoi_ = std::make_unique<hanoi::Plan>(hs);
    }
    if (mesh_ == MeshKind::Model) {
        model_.file = inst_->literal("FILE");
        const double look = inst_->loaded("LOOK", 0.0);
        model_.look = std::isfinite(look) && look >= 0.5 ? 1 : 0;
        if (const std::string n = inst_->literal("FRONT"); !n.empty()) {
            if (auto a = parseModelAxis(n)) model_.front = *a;
            else model_.error += "unknown Front \"" + n + "\"; ";
        }
        if (const std::string u = inst_->literal("UP"); !u.empty()) {
            if (auto a = parseModelAxis(u)) model_.up = *a;
            else model_.error += "unknown Up \"" + u + "\"; ";
        }
        if (std::string m = inst_->literal("MOVE"); !m.empty()) {
            std::transform(m.begin(), m.end(), m.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
            model_.move = m;
        }
        model_.freeCamera = model_.move == "free";
        // The scene's own keyframes (K1, K2, ...) win over the Move preset.
        model_.path = cameraPathFrom([&](const char* key) { return inst_->literal(key); }, &model_.error);
        model_.ownPath = !model_.path.empty();
        const double mdur = inst_->loaded("MDUR", 24.0);
        if (std::isfinite(mdur) && mdur > 0.1) model_.moveSeconds = mdur;
        model_.moveBars = inst_->loaded("MBAR", std::nan(""));
        model_.loop = inst_->loaded("LOOP", 1.0) != 0.0;
        model_.trails = inst_->defines("PEN");
    }
    if (mesh_ == MeshKind::Rain) {
        auto num = [&](const char* key, double fb, double lo, double hi) {
            const double v = inst_->loaded(key, fb);
            return std::isfinite(v) ? std::clamp(v, lo, hi) : fb;
        };
        const std::string mode = inst_->literal("RAIN");
        rainSettings_.mode = rain::parseMode(mode).value_or(rain::Mode::Window);
        const bool floor = rainSettings_.mode == rain::Mode::Floor;
        rainSettings_.slide = num("SLID", 17.0, 2.0, 200.0);
        rainSettings_.contact = num("BEAD", floor ? 55.0 : 62.0, 20.0, 85.0);
        rainSettings_.wipe = num("WIPE", 1.0, 0.0, 1.0) >= 0.5;
        rainSettings_.maxDrops = int(num("MAX", 700.0, 16.0, 4000.0));
        // The seed: Seed when the scene sets it (evaluated once), else the object's own.
        double seed = double(env.seed % 1000003u);
        if (inst_->defines("SEED")) {
            const double v = inst_->frameKey("SEED", seed);
            if (std::isfinite(v)) seed = std::fabs(std::floor(std::clamp(v, -1e15, 1e15)));
        }
        rainSettings_.seed = std::uint64_t(seed);
    }
    if (mesh_ == MeshKind::Bubbles) {
        auto num = [&](const char* key, double fb, double lo, double hi) {
            const double v = inst_->loaded(key, fb);
            return std::isfinite(v) ? std::clamp(v, lo, hi) : fb;
        };
        bubbleSettings_.source = bubbles::parseSource(inst_->literal("SRC")).value_or(bubbles::Source::Wand);
        bubbleSettings_.maxBubbles = int(num("MAX", 40.0, 1.0, double(bubbles::kMaxBubbles)));
        bubbleSettings_.cluster = num("CLST", 0.5, 0.0, 1.0);
        // The seed: Seed when the scene sets it (evaluated once), else the object's own.
        double seed = double(env.seed % 1000003u);
        if (inst_->defines("SEED")) {
            const double v = inst_->frameKey("SEED", seed);
            if (std::isfinite(v)) seed = std::fabs(std::floor(std::clamp(v, -1e15, 1e15)));
        }
        bubbleSettings_.seed = std::uint64_t(seed);
    }
    if (mesh_ == MeshKind::Shatter) {
        auto num = [&](const char* key, double fb, double lo, double hi) {
            const double v = inst_->loaded(key, fb);
            return std::isfinite(v) ? std::clamp(v, lo, hi) : fb;
        };
        shatter::Config& c = shatterConfig_;
        c.impacts = int(std::lround(num("IMP", 1.0, 1.0, double(shatter::kMaxImpacts))));
        c.cycle = num("CYCL", 64.0, 16.0, 256.0);
        c.hit = num("HIT", 4.0, 0.0, 256.0);
        c.spokes = num("SPK", 9.0, 5.0, 12.0);
        c.rings = num("RNG", 0.5, 0.0, 1.0);
        c.fork = num("FORK", 0.6, 0.0, 1.0);
        c.cracked = num("CRK", 1.0, 0.0, 1.0);
        {
            const double v = inst_->frameKey("SIZE", 1.0);  // (a per-frame key for the rain: evaluated once here)
            c.size = std::isfinite(v) ? std::clamp(v, 0.3, 3.0) : 1.0;
        }
        c.arrive = num("ARRV", 0.0, 0.0, 1.0) >= 0.5 ? 1 : 0;
        shatterConfig_ = shatter::clampConfig(c);
        // The seed: Seed when the scene sets it (evaluated once), else the object's own.
        double seed = double(env.seed % 1000003u);
        if (inst_->defines("SEED")) {
            const double v = inst_->frameKey("SEED", seed);
            if (std::isfinite(v)) seed = std::fabs(std::floor(std::clamp(v, -1e15, 1e15)));
        }
        shatterSeed_ = std::uint64_t(seed);
    }
    if (mesh_ == MeshKind::Fractal) {
        auto num = [&](const char* key, double fb, double lo, double hi) {
            const double v = inst_->loaded(key, fb);
            return std::isfinite(v) ? std::clamp(v, lo, hi) : fb;
        };
        const std::string frac = inst_->literal("FRAC"), path = inst_->literal("PATH");
        if (const auto k = parseFractalKind(frac)) {
            fractal_.kind = *k;
            fractal_.path = findFractalPath(*k, path);
            if (!fractal_.path)
                fractal_.error = "unknown Path \"" + path + "\" (" + fractalPathNames(*k) + ")";
        } else {
            fractal_.error = "unknown Frac \"" + frac + "\" (sierpinski, menger)";
        }
        FractalTiming& tm = fractal_.timing;
        tm.barsPerStep = num("ZBAR", tm.barsPerStep, 0.25, 64.0);
        tm.pulse = num("PULS", tm.pulse, 0.0, 1.0);
        tm.drop = num("DROP", tm.drop, 0.0, 16.0);
        tm.dropBeats = num("DRPB", tm.dropBeats, 0.25, 64.0);
        tm.beatStep = num("STPB", tm.beatStep, 0.0, 1.0);
        fractalClock_ = FractalClock(tm);
        fractal_.res = num("RES", fractal_.res, 0.25, 1.0);
        fractal_.detail = num("DETL", fractal_.detail, 0.25, 16.0);
        fractal_.steps = int(std::lround(num("STPS", fractal_.steps, 8.0, 400.0)));
    }
}

void SolidRun::setCameraPath(CameraPath p) {
    if (model_.ownPath) return;
    // The scene's own options win over the preset's.
    auto own = [&](const char* key) { return !inst_->literal(key).empty(); };
    const CameraPath scene = model_.path;
    if (own("EASE")) p.ease = scene.ease;
    if (own("STEP")) p.step = scene.step;
    if (own("RAND")) p.random = scene.random;
    if (own("PUSH")) p.push = scene.push;
    model_.path = std::move(p);
}

SolidSpec SolidRun::evaluate(double t, const SpriteAudio& audio) {
    barHold_.follow(env_, t - start_, audio.sync);  // PBar: the end stays on the live clock's lines
    scene::SolidFrameInput in;
    in.t = t;
    in.startTime = start_;
    in.endTime = endTime();
    in.dt = in.endTime > start_ ? std::clamp((t - start_) / (in.endTime - start_), 0.0, 1.0) : 0.0;
    in.fader = env_(t - start_);
    in.bass = audio.bass;
    in.level = audio.level;
    in.energy = audio.energy;
    in.mid = audio.mid;
    in.treble = audio.treble;
    in.beat = audio.beat;
    in.tempo = audio.tempo;
    in.linkOn = audio.linkOn;
    in.linkBpm = audio.linkBpm;
    in.linkBeat = audio.linkBeat;
    in.linkPhase = audio.linkPhase;
    in.sync = audio.sync;
    const scene::SolidFrame f = inst_->evaluateFrame(in);
    auto finite = [](double v, double fb) { return std::isfinite(v) ? v : fb; };
    SolidSpec s;
    s.mesh = mesh_;
    s.pose.x = finite(f.x, 0.0);
    s.pose.y = finite(f.y, 0.0);
    s.pose.z = finite(f.z, 0.0);
    s.pose.scale = std::max(0.0, finite(f.scale, 0.5));
    s.pose.rotX = finite(f.rotX, 0.0);
    s.pose.rotY = finite(f.rotY, 0.0);
    s.pose.rotZ = finite(f.rotZ, 0.0);
    s.perspective = inst_->perspective();
    s.text = inst_->text();
    s.fill = std::clamp(finite(f.fill, 0.15), 0.0, 1.0);
    s.shade = std::clamp(finite(f.shade, 0.6), 0.0, 1.0);
    s.edge = std::clamp(finite(f.edge, 0.8), 0.0, 1.0);
    s.hasPen = f.hasPen;
    s.hasEPen = f.hasEPen;
    s.pen = std::clamp(finite(f.pen, 0.0), 0.0, 1.0);
    s.epen = std::clamp(finite(f.epen, 0.0), 0.0, 1.0);
    s.opacity = in.fader;
    s.flags = inst_->flags();
    s.colorKey = inst_->colorKey();
    s.over = inst_->over();
    s.moves = inst_->moves();
    s.turn = finite(f.turn, 2.0 * s.moves);
    s.seed = finite(f.seed, 0.0);
    s.shuffle = finite(f.shuffle, 1.0);
    s.glass = inst_->glass();
    s.frost = std::clamp(finite(f.frost, 12.0), 0.0, 60.0);
    s.tint = std::clamp(finite(f.tint, 0.35), 0.0, 1.0);
    s.bend = std::clamp(finite(f.bend, 1.0), 0.0, 4.0);
    if (mesh_ == MeshKind::Soma && soma_) {
        // No Turn: the figure complete. Seed / Turn / Expl / Tens are made safe by the player (NaN, huge values).
        const double turn = inst_->hasTurn() ? f.turn : somaTiming_.len;
        s.soma = soma_->frame(f.seed, turn, somaTiming_, inst_->frameKey("EXPL", 0.0), inst_->frameKey("TENS", 0.0),
                              {s.pose.rotY, s.pose.rotX});
    }
    if (mesh_ == MeshKind::Rain) {
        const bool floor = rainSettings_.mode == rain::Mode::Floor;
        auto key = [&](const char* k, double fb, double lo, double hi) {
            const double v = inst_->frameKey(k, fb);
            return std::isfinite(v) ? std::clamp(v, lo, hi) : fb;
        };
        rain::Controls c;
        c.density = key("DENS", 4.0, 0.0, 40.0);
        c.size = key("SIZE", 11.0, 0.5, 60.0);
        c.drizzle = key("DRZ", 1.0, 0.0, 40.0);
        c.cluster = key("CLUS", 1.0, 0.0, 8.0);
        c.wet = key("WET", 0.65, 0.0, 1.0);
        c.flow = key("FLOW", 1.0, 0.0, 4.0);
        rain::Look& l = s.rainLook;
        l.mode = rainSettings_.mode;
        l.depth = float(key("DPTH", floor ? 0.25 : 0.5, 0.0, 4.0));
        l.fog = float(key("FOG", floor ? 0.0 : 0.8, 0.0, 1.0));
        l.refog = float(key("REFG", 25.0, 0.5, 600.0));
        l.frost = float(key("FROS", 16.0, 0.0, 60.0));
        l.spec = float(key("SPEC", 1.0, 0.0, 8.0));
        l.rim = float(key("RIM", 1.0, 0.0, 4.0));
        l.wet = float(c.wet);
        l.speed = float(key("SPD", 0.3, 0.02, 2.0));
        l.decay = float(key("DCAY", 1.6, 0.05, 30.0));
        l.pudl = float(key("PUDL", 1.0, 0.0, 3.0));
        l.opacity = float(std::clamp(in.fader, 0.0, 1.0));
        l.opaque = !s.colorKey;
        rain::Clock clk;
        clk.on = audio.sync.on > 0.5;
        clk.beat = audio.sync.beat;
        clk.bar = audio.sync.bar;
        clk.phrase = audio.sync.phrase;
        clk.build = audio.sync.build;
        clk.drop = audio.sync.drop;
        s.rainReset = !rain_;
        if (!rain_) rain_ = std::make_shared<rain::Sim>(rainSettings_, aspect_, start_);
        rain_->step(t, c, clk);
        s.rain = rain_;
    }
    if (mesh_ == MeshKind::Bubbles) {
        auto key = [&](const char* k, double fb, double lo, double hi) {
            const double v = inst_->frameKey(k, fb);
            return std::isfinite(v) ? std::clamp(v, lo, hi) : fb;
        };
        bubbles::Controls c;
        c.blow = key("BLOW", 6.0, 0.0, 64.0);
        c.size = key("SIZE", 55.0, 4.0, 400.0);
        c.pop = key("POP", 1.0, 0.0, 16.0);
        c.life = key("LIFE", 32.0, 1.0, 4096.0);
        c.air = key("AIR", 1.0, 0.0, 8.0);
        c.rise = key("RISE", 1.0, -4.0, 8.0);
        c.wind = key("WIND", 0.0, 0.0, 4.0);
        bubbles::Look& l = s.bubbleLook;
        l.film = float(key("FILM", 520.0, 0.0, 3000.0));
        l.iris = float(key("IRIS", 1.0, 0.0, 3.0));
        l.swirl = float(key("SWRL", 1.0, 0.0, 3.0));
        l.drain = float(key("DRN", 1.0, 0.0, 4.0));
        l.bend = float(key("BEND", 1.0, 0.0, 4.0));
        l.spec = float(key("SPEC", 1.0, 0.0, 4.0));
        l.rim = float(key("RIM", 1.0, 0.0, 3.0));
        l.depth = float(key("DPTH", 0.3, 0.0, 4.0));
        l.tint = float(key("TINT", 0.2, 0.0, 1.0));
        l.opacity = float(std::clamp(in.fader, 0.0, 1.0));
        l.opaque = !s.colorKey;
        bubbles::Clock clk;
        clk.on = audio.sync.on > 0.5;
        clk.beat = audio.sync.beat;
        clk.bar = audio.sync.bar;
        clk.phrase = audio.sync.phrase;
        clk.build = audio.sync.build;
        clk.drop = audio.sync.drop;
        if (!bubbles_) bubbles_ = std::make_shared<bubbles::Sim>(bubbleSettings_, aspect_, start_);
        bubbles_->step(t, c, clk);
        s.bubbles = bubbles_;
    }
    if (mesh_ == MeshKind::Shatter) {
        auto key = [&](const char* k, double fb, double lo, double hi) {
            const double v = inst_->frameKey(k, fb);
            return std::isfinite(v) ? std::clamp(v, lo, hi) : fb;
        };
        shatter::Controls ctl;
        ctl.burst = key("BRST", 1.0, 0.0, 1.0);
        ctl.gravity = key("GRAV", 1.0, 0.0, 4.0);
        ctl.tilt = key("TILT", 1.0, 0.0, 4.0);
        shatter::Look& l = s.shatterLook;
        l.depth = float(key("DPTH", 0.35, 0.0, 4.0));
        l.spec = float(key("SPEC", 1.0, 0.0, 8.0));
        l.line = float(key("LINE", 1.0, 0.0, 4.0));
        l.sparkle = float(key("SPRK", 1.0, 0.0, 8.0));
        l.sheen = float(key("GLS", 1.0, 0.0, 8.0));
        l.thick = float(key("THCK", 1.0, 0.0, 4.0));
        l.opacity = float(std::clamp(in.fader, 0.0, 1.0));
        l.opaque = !s.colorKey;
        shatter::Clock clk;
        clk.on = audio.sync.on > 0.5;
        clk.bar = audio.sync.bar;
        clk.build = audio.sync.build;
        clk.drop = audio.sync.drop;
        if (!shatter_) shatter_ = std::make_shared<shatter::Player>(shatterSeed_, shatterConfig_, aspect_, start_);
        shatter_->step(t, clk, ctl);
        s.shatter = shatter_;
    }
    if (mesh_ == MeshKind::Knight) {
        s.turn = finite(f.turn, 64.0);
        if (!inst_->hasTurn()) s.turn = 64.0;
        s.symmetric = inst_->loaded("SYM", 1.0) != 0.0;
        const double brd = inst_->loaded("BRD", 1.0);
        s.board = std::isfinite(brd) ? int(std::clamp(std::round(brd), 0.0, 2.0)) : 1;
        s.orbit = finite(inst_->frameKey("ORBT", 0.0), 0.0);
        s.follow = std::clamp(finite(inst_->frameKey("FOLW", 0.15), 0.15), 0.0, 1.0);
        s.flash = std::clamp(finite(inst_->frameKey("FLSH", 0.0), 0.0), 0.0, 1.0);
        s.hover = std::clamp(finite(inst_->frameKey("HOVR", 0.0), 0.0), 0.0, 1.0);
    }
    if (labyrinth_) {
        const double seed = std::fabs(std::floor(finite(f.seed, 0.0)));
        s.labyrinth = labyrinth_->at(uint64_t(std::min(seed, 1e15)), grid_, beats_, s.turn, traps_);
        if (s.labyrinth.catchingUp && !catchUpLogged_) {  // started mid-run: the plan catches up over a few frames
            catchUpLogged_ = true;
            rlog("[reforce] Solid \"%s\": labyrinth plan catching up to beat %.2f (board level, ball resting)\n",
                 name_.c_str(), s.turn);
        }
        s.floor = std::clamp(finite(inst_->frameKey("FLOOR", 0.55), 0.55), 0.0, 1.0);
        s.tiltGain = std::clamp(finite(inst_->frameKey("TILT", 1.0), 1.0), 0.0, 4.0);
    }
    if (hanoi_) {
        // No Turn: solved. The style of a move (a drop's higher arc, its flips) is read when the move starts, so a value
        // changing in flight never jumps the disc.
        const double turn = inst_->hasTurn() ? finite(f.turn, hanoi_->settings().beats) : hanoi_->settings().beats;
        const double seed = finite(f.seed, 0.0);
        const int64_t run = hanoi::Plan::runOf(seed);
        const int move = hanoi_->moveAt(turn);
        if (run != hanoiRun_ || move != hanoiMove_) {
            hanoiRun_ = run;
            hanoiMove_ = move;
            hanoiStyle_ = hanoi::makeStyle(inst_->frameKey("LIFT", 1.0), inst_->frameKey("FLIP", 0.0));
        }
        hanoi::Controls hc;
        hc.hover = finite(inst_->frameKey("HOVR", 0.0), 0.0);
        hc.style = hanoiStyle_;
        s.turn = turn;
        s.hanoi = hanoi_->frameAt(seed, turn, hc);
        auto key = [&](const char* k, double fb, double lo, double hi) {
            const double v = inst_->frameKey(k, fb);
            return std::isfinite(v) ? std::clamp(v, lo, hi) : fb;
        };
        s.frost = key("FROS", 6.0, 0.0, 60.0);
        s.tint = key("TINT", 0.5, 0.0, 1.0);
        s.bend = key("BEND", 1.0, 0.0, 4.0);
        s.depth = key("DPTH", 0.5, 0.0, 4.0);
        s.spec = key("SPEC", 1.0, 0.0, 8.0);
        s.mono = inst_->defines("MONO") ? key("MONO", -1.0, -1.0, 1.0) : -1.0;
    }
    if (mesh_ == MeshKind::Jigsaw) {
        auto loadedInt = [&](const char* key, double fallback, int lo, int hi) {
            const double v = inst_->loaded(key, fallback);
            return std::isfinite(v) ? int(std::clamp(std::round(v), double(lo), double(hi))) : int(fallback);
        };
        s.jigsaw.pieces = loadedInt("PCS", 32, 2, jigsaw::kMaxPieces);
        s.jigsaw.order = jigsaw::Order(loadedInt("ORDR", 0, 0, 3));
        s.jigsaw.cycle = loadedInt("CYCL", 32, 4, 256);
        s.jigsaw.brk = loadedInt("BRK", 4, 0, 254);
        s.gap = loadedInt("GAP", 0, 0, 3);
        s.jigsawIn.turn = inst_->hasTurn() ? f.turn : std::nan("");  // no Turn: whole
        s.jigsawIn.kick = std::clamp(finite(inst_->frameKey("KICK", 0.0), 0.0), 0.0, 1.0);
        s.jigsawIn.shake = std::clamp(finite(inst_->frameKey("SHAK", 0.0), 0.0), 0.0, 1.0);
        const double dim = inst_->frameKey("DIM", std::nan(""));
        s.dim = std::isfinite(dim) ? std::clamp(dim, 0.0, 1.0) : std::nan("");
        s.frost = std::clamp(finite(inst_->frameKey("FROS", 10.0), 10.0), 0.0, 60.0);
    }
    if (mesh_ == MeshKind::Model) {
        const double age = std::max(0.0, t - start_);
        double p = age / model_.moveSeconds;
        p = model_.loop ? p - std::floor(p) : std::min(p, 1.0);
        CameraMoveParams cp;
        cp.dist = std::max(0.05, finite(inst_->frameKey("DIST", 1.0), 1.0));
        cp.height = finite(inst_->frameKey("HITE", 0.0), 0.0);
        cp.side = finite(inst_->frameKey("SIDE", 1.0), 1.0) < 0 ? -1.0 : 1.0;
        cp.roll = finite(inst_->frameKey("ROLL", 0.0), 0.0);
        cp.fov = finite(inst_->frameKey("FOV", 0.0), 0.0);
        cp.seed = s.seed;
        cp.cutBeats = std::max(0.25, finite(inst_->frameKey("CUTB", 4.0), 4.0));
        cp.beat = audio.sync.on > 0 ? audio.sync.beat : age * 2.0;  // no clock: 120 BPM from the start
        ModelShot shot = pathShot(model_.freeCamera ? CameraPath{} : model_.path, p, cp);
        const char* cam[3] = {"CAMX", "CAMY", "CAMZ"};
        const char* aim[3] = {"AIMX", "AIMY", "AIMZ"};
        for (int k = 0; k < 3; ++k) {
            const std::size_t j = std::size_t(k);
            // Free: CamX..AimZ are the camera; other moves: offsets added to the path.
            const double c = finite(inst_->frameKey(cam[k], model_.freeCamera ? shot.eye[j] : 0.0), 0.0);
            const double a = finite(inst_->frameKey(aim[k], model_.freeCamera ? shot.aim[j] : 0.0), 0.0);
            shot.eye[j] = model_.freeCamera ? c : shot.eye[j] + c;
            shot.aim[j] = model_.freeCamera ? a : shot.aim[j] + a;
        }
        s.shot = shot;
        s.spin = finite(inst_->frameKey("SPIN", 0.0), 0.0) * age;
        s.bank = finite(inst_->frameKey("BANK", 0.0), 0.0);
        s.glow = std::clamp(finite(inst_->frameKey("GLOW", 1.0), 1.0), 0.0, 20.0);
        s.bloom = std::clamp(finite(inst_->frameKey("BLOM", 0.6), 0.6), 0.0, 4.0);
        s.exposure = std::clamp(finite(inst_->frameKey("EXPO", 1.0), 1.0), 0.0, 16.0);
        s.tint = std::clamp(finite(inst_->frameKey("TINT", 0.0), 0.0), 0.0, 1.0);
        s.light = {finite(inst_->frameKey("LTX", -0.45), -0.45), finite(inst_->frameKey("LTY", 0.6), 0.6),
                   finite(inst_->frameKey("LTZ", 0.65), 0.65)};
    }
    if (mesh_ == MeshKind::Fractal && fractal_.path) {
        // The zoom from the beat clock (engine::FractalClock), or the scene's own Zoom (steps; its fraction counts).
        const FractalPath& path = *fractal_.path;
        const double dt = std::isfinite(lastT_) ? t - lastT_ : 0.0;
        lastT_ = t;
        // Hovr is a per-frame key (the knight's tour shares the name), so it is read here.
        if (inst_->defines("HOVR")) fractalClock_.setHover(finite(inst_->frameKey("HOVR", 0.75), 0.75));
        const FractalBeat fb = fractalClock_.step(audio.sync.beat, audio.sync.bar, audio.sync.on > 0,
                                                  t - start_, audio.sync.build, audio.sync.drop, dt);
        double zoom = fb.zoom;
        if (inst_->defines("ZOOM")) {
            const double z = inst_->frameKey("ZOOM", 0.0);
            if (std::isfinite(z) && std::fabs(z) < 1e15) zoom = z - std::floor(z);
        }
        // The sway: by default a slow orbit about the path's axis and a look around, stepping on the beats.
        constexpr double kTau = 6.283185307179586;
        const double b = fb.beats;
        FractalSway sw;
        sw.orbit = finite(inst_->frameKey("ORBT", path.orbit * std::sin(kTau * b / 32.0) + path.spin * b), 0.0);
        sw.lookX = finite(inst_->frameKey("LKX", path.look * std::sin(kTau * b / 64.0)), 0.0);
        sw.lookY = finite(inst_->frameKey("LKY", 0.5 * path.look * std::cos(kTau * b / 32.0)), 0.0);
        sw.roll = finite(inst_->frameKey("ROLL", 0.0), 0.0);
        sw.fov = finite(inst_->frameKey("FOV", 0.0), 0.0);
        s.fractalCam = fractalCamera(path, zoom, sw);
        s.zoom = zoom;
        s.fog = std::clamp(finite(inst_->frameKey("FOG", path.fog), path.fog), 0.2, 50.0);
        s.glow = std::clamp(finite(inst_->frameKey("GLOW", 0.6), 0.6), 0.0, 4.0) * fb.pulse;
        if (!inst_->defines("FILL")) s.fill = 0.06;
        if (!inst_->defines("SHAD")) s.shade = 0.85;
    }
    return s;
}

}  // namespace reforce::engine

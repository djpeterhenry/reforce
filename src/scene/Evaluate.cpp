#include "scene/Evaluate.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "expr/Builtins.hpp"

namespace reforce::scene {

using expr::Value;
using expr::Var;

// ----------------------------------------------------------------- base

ConfigInstance::ConfigInstance(const CompiledConfig& cfg, const Environment& env) : cfg_(&cfg), env_(env) {
    cfg.prepare(ctx_);
    ctx_.rng.reseed(env.seed);
    ctx_.set(Var::WIDTH, env.width);
    ctx_.set(Var::HEIGHT, env.height);
    ctx_.set(Var::NUM_FFT_BINS, env.numFftBins);
    ctx_.set(Var::NUM_SAMPLE_BINS, env.numSampleBins);
    ctx_.set(Var::NUM, 1);
    ctx_.set(Var::NUM_S_STEPS, env.numSampleBins);
    ctx_.set(Var::T, env.loadTime);
    ctx_.set(Var::X_EXTENTS, 1);
    ctx_.set(Var::Y_EXTENTS, 1);
}

const Value& ConfigInstance::evalKey(int i) { return evalKeyIn(ctx_, i); }

const Value& ConfigInstance::evalKeyIn(expr::EvalContext& c, int i) const {
    const CompiledKey& k = cfg_->keys[std::size_t(i)];
    Value& slot = c.keys[std::size_t(i)];
    slot = expr::run(k.prog, c);
    if (k.family >= 0 && k.tier != Tier::Ignored) c.slot(k.family, k.index) = slot;
    return slot;
}

void ConfigInstance::evalFamily(int fam, int lane, int laneCount) {
    const auto& order = cfg_->families[std::size_t(fam)];
    for (std::size_t idx = 0; idx < order.size(); ++idx) {
        const int k = order[idx];
        if (k < 0) continue;
        Value v = expr::run(cfg_->keys[std::size_t(k)].prog, ctx_);
        if (laneCount > 1) v = expr::extractLane(v, lane, laneCount);
        ctx_.keys[std::size_t(k)] = v;
        ctx_.slot(fam, int(idx)) = std::move(v);
    }
}

void ConfigInstance::sizeStepStore(int fam, int stepCount) {
    const auto& order = cfg_->families[std::size_t(fam)];
    if (stepStore_.size() != order.size()) stepStore_.assign(order.size(), {});
    for (auto& v : stepStore_)
        if (int(v.size()) != stepCount) v.assign(std::size_t(stepCount), Value());
}

void ConfigInstance::evalStepFamily(int fam, int step, int stepCount) {
    sizeStepStore(fam, stepCount);
    evalStepFamilyIn(ctx_, fam, step, stepCount);
}

void ConfigInstance::evalStepFamilyIn(expr::EvalContext& c, int fam, int step, int stepCount) {
    const auto& order = cfg_->families[std::size_t(fam)];
    for (std::size_t idx = 0; idx < order.size(); ++idx) c.slot(fam, int(idx)) = stepStore_[idx][std::size_t(step)];
    for (std::size_t idx = 0; idx < order.size(); ++idx) {
        const int k = order[idx];
        if (k < 0) continue;
        Value v;
        double d = 0.0;
        if (stepCount > 1 && !slice_.empty() && slice_[std::size_t(k)] && runSliced(c, k, step, stepCount, d)) {
            v.s_ = d;
        } else {
            v = expr::run(cfg_->keys[std::size_t(k)].prog, c);
            if (stepCount > 1) v = expr::extractLane(v, step, stepCount);
        }
        c.keys[std::size_t(k)] = v;
        stepStore_[idx][std::size_t(step)] = v;
        c.slot(fam, int(idx)) = std::move(v);
    }
}

double ConfigInstance::evalScalar(int keyIndex, double fallback, int lane, int laneCount) {
    return evalScalarIn(ctx_, keyIndex, fallback, lane, laneCount);
}

namespace {

constexpr int kSliceStack = 64;

bool isBinaryOp(expr::Op op) { return op >= expr::Op::Add && op <= expr::Op::Shr; }
int rowBuiltin() {
    static const int row = expr::findBuiltin("row");
    return row;
}

// Programs runSliced can evaluate: scalar constants, loads, unary/binary
// operators, scalar fast-path builtins and the elementwise audio lookups.
// Everything else (matrix literals, row/col/vector/select/..., min/max of
// one argument, the RNG) needs whole values.
bool sliceable(const expr::Program& p) {
    if (p.usesRandom || p.code.empty() || p.maxStack + 1 > kSliceStack) return false;
    static const int mag = expr::findBuiltin("mag"), fft = expr::findBuiltin("fft");
    for (const auto& in : p.code) {
        switch (in.op) {
            case expr::Op::PushConst:
                if (!p.consts[std::size_t(in.arg)].isScalar()) return false;
                break;
            case expr::Op::LoadA:
            case expr::Op::LoadB:
            case expr::Op::LoadC:
            case expr::Op::LoadD:
            case expr::Op::LoadKey:
            case expr::Op::LoadVar:
            case expr::Op::Neg:
            case expr::Op::Not:
            case expr::Op::CastInt:
            case expr::Op::CallS1:
            case expr::Op::CallS2: break;
            case expr::Op::Call:
                if (!(in.argc == 1 && (in.arg == mag || in.arg == fft)) && !(in.argc == 2 && in.arg == rowBuiltin()))
                    return false;
                break;
            default:
                if (!isBinaryOp(in.op)) return false;
                break;
        }
    }
    return true;
}

}  // namespace

// Column `lane` of program `key` evaluated on doubles; false (nothing written)
// when an input is neither a scalar nor a 1 x laneCount row (or an R x
// laneCount matrix that only row() reads). Mirrors the scalar paths of
// expr::run: an elementwise result's column `lane` is the same operation
// applied to the inputs' column `lane`, and row(M, r)'s is M(r, lane).
bool ConfigInstance::runSliced(expr::EvalContext& c, int key, int lane, int laneCount, double& out) const {
    using expr::Op;
    const expr::Program& p = cfg_->keys[std::size_t(key)].prog;
    // A slot is a double, or (m != nullptr) a borrowed R x laneCount matrix
    // waiting for row(); any other use of a matrix slot gives up.
    struct Slot {
        double v;
        const expr::Mat* m;
        bool lane;  // v varies with the lane (comes from a row); row() needs a plain scalar index
    };
    Slot st[kSliceStack];
    Slot* sp = st;
    auto load = [&](const Value& v) {
        if (!v.m_) {
            *sp++ = {v.s_, nullptr, false};
            return true;
        }
        const expr::Mat& m = *v.m_;
        if (m.cols != laneCount) return false;
        *sp++ = m.rows == 1 ? Slot{m.d[std::size_t(lane)], nullptr, true} : Slot{0.0, &m, true};
        return true;
    };
    for (const expr::Instr& in : p.code) {
        // Only row() takes a matrix slot (its first argument).
        if (in.op != Op::Call || in.arg != rowBuiltin()) {
            const int consumed = in.op == Op::Neg || in.op == Op::Not || in.op == Op::CastInt || in.op == Op::CallS1 ||
                                         in.op == Op::Call
                                     ? 1
                                 : (in.op == Op::CallS2 || isBinaryOp(in.op)) ? 2
                                                                               : 0;
            for (int i = 1; i <= consumed; ++i)
                if (sp[-i].m) return false;
        }
        switch (in.op) {
            case Op::PushConst: *sp++ = {p.consts[std::size_t(in.arg)].s_, nullptr, false}; break;
            case Op::LoadA:
            case Op::LoadB:
            case Op::LoadC:
            case Op::LoadD: {
                const auto& fam = c.vars[std::size_t(int(in.op) - int(Op::LoadA))];
                if (std::size_t(in.arg) < fam.size()) {
                    if (!load(fam[std::size_t(in.arg)])) return false;
                } else {
                    *sp++ = {0.0, nullptr, false};
                }
                break;
            }
            case Op::LoadKey:
                if (std::size_t(in.arg) < c.keys.size()) {
                    if (!load(c.keys[std::size_t(in.arg)])) return false;
                } else {
                    *sp++ = {0.0, nullptr, false};
                }
                break;
            case Op::LoadVar:
                if (!load(c.builtins[std::size_t(in.arg)])) return false;
                break;
            case Op::Neg: sp[-1].v = -sp[-1].v; break;
            case Op::Not: sp[-1].v = sp[-1].v == 0.0 ? 1.0 : 0.0; break;
            case Op::CastInt: sp[-1].v = std::trunc(sp[-1].v); break;
            case Op::CallS1: sp[-1].v = expr::builtinAt(in.arg).s1(sp[-1].v); break;
            case Op::CallS2:
                sp[-2].v = expr::builtinAt(in.arg).s2(sp[-2].v, sp[-1].v);
                sp[-2].lane = sp[-2].lane || sp[-1].lane;
                --sp;
                break;
            case Op::Call: {
                if (in.arg == rowBuiltin()) {  // row(x, n), n scalar
                    // A row of indices makes row() return several rows: give up.
                    if (sp[-1].m || sp[-1].lane) return false;
                    if (const expr::Mat* m = sp[-2].m) {
                        // indexArg (BuiltinsMatrix.cpp): trunc, negative from the end, clamped.
                        const double n = sp[-1].v;
                        int r = 0;
                        if (std::isfinite(n)) {
                            long i = long(std::trunc(n));
                            if (i < 0) i += m->rows;
                            if (i < 0) i = 0;
                            if (i >= m->rows) i = m->rows - 1;
                            r = int(i);
                        }
                        sp[-2] = {m->at(r, lane), nullptr, true};
                    }
                    // A scalar or 1 x laneCount x: row() returns x itself (one row).
                    --sp;
                    break;
                }
                Value a(sp[-1].v);  // mag / fft of one scalar
                sp[-1].v = expr::builtinAt(in.arg).fn(c, &a, 1).scalar();
                break;
            }
            case Op::Add: sp[-2].v += sp[-1].v; break;
            case Op::Sub: sp[-2].v -= sp[-1].v; break;
            case Op::Mul: sp[-2].v *= sp[-1].v; break;
            case Op::Div: sp[-2].v /= sp[-1].v; break;
            default: sp[-2].v = expr::scalarBinary(in.op, sp[-2].v, sp[-1].v); break;
        }
        if (isBinaryOp(in.op)) {
            sp[-2].lane = sp[-2].lane || sp[-1].lane;
            --sp;
        }
    }
    if (st[0].m) return false;  // a whole matrix as the result
    out = st[0].v;
    return true;
}

double ConfigInstance::evalScalarIn(expr::EvalContext& c, int keyIndex, double fallback, int lane,
                                    int laneCount) const {
    if (keyIndex < 0 || !cfg_->keys[std::size_t(keyIndex)].prog.defined) return fallback;
    return expr::laneScalar(evalKeyIn(c, keyIndex), lane, laneCount);
}

void ConfigInstance::runLoadTiers() {
    // Pre: Aspc first, then NUM/Stps/PDur/ConB/named constants in dependency order.
    for (int i : cfg_->preOrder) {
        const CompiledKey& k = cfg_->keys[std::size_t(i)];
        const double v = evalKey(i).scalar();
        if (!k.prog.defined) continue;
        if (k.key == "ASPC") {
            aspc_ = v != 0.0;
            if (aspc_ && env_.width > 0 && env_.height > 0) {
                // Spec §5 (assumed): the shorter axis spans +-1. A FlowField's
                // grid (edge pixels on the frame edges, #26) has square steps,
                // 2/(short-1) on both axes, so its long axis spans
                // (long-1)/(short-1): "Nebula" (1/wrap(232 r)) pins this to
                // ~0.01 px; W/H decorrelates its texture away from the center.
                const bool grid = pixelGridExtents_ && std::min(env_.width, env_.height) > 1.0;
                const double off = grid ? 1.0 : 0.0;
                if (env_.width >= env_.height) {
                    ctx_.set(Var::X_EXTENTS, (env_.width - off) / (env_.height - off));
                    ctx_.set(Var::Y_EXTENTS, 1.0);
                } else {
                    ctx_.set(Var::X_EXTENTS, 1.0);
                    ctx_.set(Var::Y_EXTENTS, (env_.height - off) / (env_.width - off));
                }
            }
        } else if (k.key == "NUM") {
            // Non-integer part truncated [SP]. GUESS: values < 1 (and NaN) give 1.
            const double n = std::isfinite(v) ? std::trunc(v) : 1.0;
            ctx_.set(Var::NUM, std::clamp(n, 1.0, double(env_.maxInstances)));
        } else if (k.key == "STPS") {
            // 0 or omitted gives NUM_SAMPLE_BINS [RC]. GUESS: negatives too.
            const double n = std::isfinite(v) ? std::trunc(v) : 0.0;
            ctx_.set(Var::NUM_S_STEPS, n >= 1.0 ? std::min(n, double(env_.maxSteps)) : double(env_.numSampleBins));
        }
    }
    if (on_pre_done_) on_pre_done_();
    evalFamily(expr::FamA);
    for (int i : cfg_->postLoadOrder) evalKey(i);
}

// ----------------------------------------------------------------- WaveShape

WaveShapeInstance::WaveShapeInstance(const CompiledConfig& cfg, const Environment& env) : ConfigInstance(cfg, env) {
    // A and B tiers see `s` as the whole step vector (VectorC, spec §4).
    on_pre_done_ = [this] {
        numSteps_ = std::max(1, int(ctx_.get(Var::NUM_S_STEPS)));
        numInstances_ = std::max(1, int(ctx_.get(Var::NUM)));
        std::vector<double> s(static_cast<std::size_t>(numSteps_));
        for (int k = 0; k < numSteps_; ++k) s[std::size_t(k)] = stepParam(k);
        sVector_ = Value::row(s);
        ctx_.set(Var::S, sVector_);
        ctx_.set(Var::ID, 0);
    };
    runLoadTiers();

    const int conb = cfg.indexOf("CONB");
    connected_ = conb >= 0 && cfg.keys[std::size_t(conb)].prog.defined && ctx_.keys[std::size_t(conb)].scalar() != 0.0;
    const int pdur = cfg.indexOf("PDUR");
    duration_ = (pdur >= 0 && cfg.keys[std::size_t(pdur)].prog.defined) ? ctx_.keys[std::size_t(pdur)].scalar()
                                                                         : env.particleDuration;
    pen_ = cfg.indexOf("PEN");
    lwdt_ = cfg.indexOf("LWDT");

    // Paths: X#k / Y#k pairs; a path exists when either coordinate is defined.
    std::vector<int> ks;
    for (const auto& k : cfg.keys) {
        if (k.tier != Tier::StepOutput || (k.key[0] != 'X' && k.key[0] != 'Y') || k.key.size() < 2) continue;
        if (!k.prog.defined) continue;
        const int n = std::atoi(k.key.c_str() + 1);
        if (std::find(ks.begin(), ks.end(), n) == ks.end()) ks.push_back(n);
    }
    std::sort(ks.begin(), ks.end());
    for (int n : ks) {
        pathIdx_.push_back(n);
        pathKeys_.emplace_back(cfg.indexOf("X" + std::to_string(n)), cfg.indexOf("Y" + std::to_string(n)));
    }
    penPerPath_ = pen_ >= 0 && cfg.keys[std::size_t(pen_)].prog.usesVar(Var::PATH_ID);
    widthPerPath_ = lwdt_ >= 0 && cfg.keys[std::size_t(lwdt_)].prog.usesVar(Var::PATH_ID);
    analyzeParallel();
    analyzeSlicing();
}

// Lane slicing (see ConfigInstance::slice_): C keys always qualify when their
// program does (the step store keeps the extracted column either way); output
// keys keep the whole row in their key slot when evaluated normally, so they
// qualify only when no program reads them.
void WaveShapeInstance::analyzeSlicing() {
    const CompiledConfig& cfg = *cfg_;
    slice_.assign(cfg.keys.size(), 0);
    std::vector<char> referenced(cfg.keys.size(), 0);
    for (const auto& k : cfg.keys)
        for (int r : k.prog.keyRefs)
            if (r >= 0 && std::size_t(r) < referenced.size()) referenced[std::size_t(r)] = 1;
    for (int k : cfg.families[expr::FamC])
        if (k >= 0 && sliceable(cfg.keys[std::size_t(k)].prog)) slice_[std::size_t(k)] = 1;
    auto output = [&](int k) {
        if (k >= 0 && cfg.keys[std::size_t(k)].family < 0 && !referenced[std::size_t(k)] &&
            sliceable(cfg.keys[std::size_t(k)].prog))
            slice_[std::size_t(k)] = 1;
    };
    output(pen_);
    output(lwdt_);
    for (const auto& [xk, yk] : pathKeys_) {
        output(xk);
        output(yk);
    }
}

// An output key's value at step k (evalScalarIn, lane-sliced when allowed).
double WaveShapeInstance::outputIn(expr::EvalContext& c, int keyIndex, double fallback, int k, int N) const {
    if (keyIndex < 0 || !cfg_->keys[std::size_t(keyIndex)].prog.defined) return fallback;
    double d = 0.0;
    if (N > 1 && !slice_.empty() && slice_[std::size_t(keyIndex)] && runSliced(c, keyIndex, k, N, d)) {
        c.keys[std::size_t(keyIndex)].setScalar(d);
        return d;
    }
    return evalScalarIn(c, keyIndex, fallback, k, N);
}

double WaveShapeInstance::stepParam(int k) const {
    // Spec §10.5: inclusive i/(N-1) assumed (vectorLR is inclusive).
    if (numSteps_ <= 1) return 0.0;
    return env_.sInclusive ? double(k) / double(numSteps_ - 1) : double(k) / double(numSteps_);
}

void WaveShapeInstance::evaluateFrame(const WaveShapeFrameInput& in, WaveShapeFrame& out) {
    ctx_.set(Var::T, in.t);
    ctx_.set(Var::BASS, in.bass);
    ctx_.set(Var::LEVEL, in.level);
    ctx_.set(Var::ENERGY, in.energy);
    ctx_.set(Var::MID, in.mid);
    ctx_.set(Var::TREBLE, in.treble);
    ctx_.set(Var::BEAT, in.beat);
    ctx_.set(Var::TEMPO, in.tempo);
    ctx_.set(Var::LINK_ON, in.linkOn);
    ctx_.set(Var::LINK_BPM, in.linkBpm);
    ctx_.set(Var::LINK_BEAT, in.linkBeat);
    ctx_.set(Var::LINK_PHASE, in.linkPhase);
    setSyncVars(ctx_, in.sync);
    ctx_.set(Var::START_TIME, in.startTime);
    ctx_.set(Var::END_TIME, in.endTime);
    double dt = 0.0;  // plain WaveShapes: 0 (probe #43)
    if (in.dt >= 0.0) dt = in.dt;
    else if (in.endTime < 1e29 && in.endTime > in.startTime) dt = (in.t - in.startTime) / (in.endTime - in.startTime);
    ctx_.set(Var::DT, dt);
    ctx_.set(Var::FADER, in.fader);
    ctx_.audio = in.audio;
    ctx_.randomLanes = 1;

    const int N = numSteps_;
    const std::size_t P = pathKeys_.size();
    out.connected = connected_;
    out.paths.resize(std::size_t(numInstances_) * P);
    for (int id = 0; id < numInstances_; ++id)
        for (std::size_t p = 0; p < P; ++p) {
            WavePath& wp = out.paths[std::size_t(id) * P + p];
            wp.id = id;
            wp.pathIndex = pathIdx_[p];
            wp.x.resize(std::size_t(N));
            wp.y.resize(std::size_t(N));
            wp.pen.resize(std::size_t(N));
            wp.width.resize(std::size_t(N));
        }
    sizeStepStore(expr::FamC, N);

    const bool canParallel = pool_ && pool_->lanes() > 1 && parallel_ != Parallel::Serial && N > 1;
    if (canParallel && (heavy_ || long(numInstances_) * long(N) * stepRuns_ >= minParallelWork_)) {
        evaluateParallel(out);
        return;
    }
    // Program runs underestimate matrix-heavy steps: a serial frame that took
    // long enough switches this instance to parallel for good.
    const auto t0 = canParallel ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (int id = 0; id < numInstances_; ++id) {
        ctx_.set(Var::ID, id);
        ctx_.set(Var::S, sVector_);
        ctx_.set(Var::PATH_ID, P ? pathIdx_[0] : 0);
        evalFamily(expr::FamB);  // B vars: shared storage across IDs (spec §4)
        for (int k = 0; k < N; ++k) evalStep(ctx_, out, id, k);
    }
    if (canParallel && std::chrono::steady_clock::now() - t0 > std::chrono::microseconds(250)) heavy_ = true;
}

// One s step of one ID on context `c` (the instance's own, or a lane's).
void WaveShapeInstance::evalStep(expr::EvalContext& c, WaveShapeFrame& out, int id, int k) {
    const int N = numSteps_;
    const std::size_t P = pathKeys_.size();
    c.set(Var::S, stepParam(k));
    // PATH_ID as the serial loop leaves it: the first path's before step 0
    // (set with B), the last path's after every step.
    if (P) c.set(Var::PATH_ID, k == 0 ? pathIdx_[0] : pathIdx_[P - 1]);
    evalStepFamilyIn(c, expr::FamC, k, N);
    double pen = 1.0, width = 1.0;
    if (!penPerPath_) pen = outputIn(c, pen_, 1.0, k, N);
    if (!widthPerPath_) width = outputIn(c, lwdt_, 1.0, k, N);
    for (std::size_t p = 0; p < P; ++p) {
        c.set(Var::PATH_ID, pathIdx_[p]);
        WavePath& wp = out.paths[std::size_t(id) * P + p];
        wp.x[std::size_t(k)] = float(outputIn(c, pathKeys_[p].first, 0.0, k, N));
        wp.y[std::size_t(k)] = float(outputIn(c, pathKeys_[p].second, 0.0, k, N));
        if (penPerPath_) pen = outputIn(c, pen_, 1.0, k, N);
        if (widthPerPath_) width = outputIn(c, lwdt_, 1.0, k, N);
        // Pen is clipped at 1 [RC]; negatives clamp to 0 (assumed).
        wp.pen[std::size_t(k)] = float(std::isnan(pen) ? pen : std::clamp(pen, 0.0, 1.0));
        wp.width[std::size_t(k)] = float(width);
    }
}

// Which state can a step observe from another step? (Phase 5D)
//   - C slots: loaded per step from the step store (step k's own history), so
//     steps are independent; the chain of step k across IDs stays in order
//     because one task runs a step range for every ID in turn.
//   - Key slots of step-tier keys (C, X#/Y#, Pen, LWdt): a key read before the
//     step (re)writes it sees the previous step's value -> must stay serial.
//   - The RNG and print(): order-dependent -> serial.
//   - Builtins: S, PATH_ID and ID are set explicitly per step (evalStep).
// B (per ID, serial) may read C slots or step-tier keys, i.e. the last step of
// the previous ID: then each ID is its own dispatch (PerId), else all B values
// are computed first and the whole frame is one dispatch (Frame).
void WaveShapeInstance::analyzeParallel() {
    const CompiledConfig& cfg = *cfg_;
    auto defined = [&](int k) { return k >= 0 && cfg.keys[std::size_t(k)].prog.defined; };
    std::vector<int> firstPos(cfg.keys.size(), -1);
    int pos = 0;
    long runs = 0;
    auto add = [&](int k) {
        if (firstPos[std::size_t(k)] < 0) {
            firstPos[std::size_t(k)] = pos;
            stepKeys_.push_back(k);
        }
        ++pos;
        ++runs;
    };
    for (int k : cfg.families[expr::FamC])
        if (k >= 0) add(k);  // C keys run even when empty
    if (!penPerPath_ && defined(pen_)) add(pen_);
    if (!widthPerPath_ && defined(lwdt_)) add(lwdt_);
    for (const auto& [xk, yk] : pathKeys_) {
        if (defined(xk)) add(xk);
        if (defined(yk)) add(yk);
        if (penPerPath_ && defined(pen_)) add(pen_);
        if (widthPerPath_ && defined(lwdt_)) add(lwdt_);
    }
    stepRuns_ = std::max(1L, runs);
    for (int k : cfg.families[expr::FamB])
        if (k >= 0) bKeys_.push_back(k);

    auto impure = [](const expr::Program& p) {
        if (p.usesRandom) return true;
        static const int print = expr::findBuiltin("print");
        for (const auto& in : p.code)
            if ((in.op == expr::Op::Call || in.op == expr::Op::CallS1 || in.op == expr::Op::CallS2) && in.arg == print)
                return true;
        return false;
    };
    for (int k : stepKeys_) {
        const CompiledKey& ck = cfg.keys[std::size_t(k)];
        if (impure(ck.prog)) return;
        // Output keys that also write a user-var slot: not modelled, stay serial.
        if (ck.family >= 0 && ck.family != expr::FamC) return;
        for (int r : ck.prog.keyRefs) {
            const int rp = r >= 0 && std::size_t(r) < firstPos.size() ? firstPos[std::size_t(r)] : -1;
            if (rp >= 0 && rp >= firstPos[std::size_t(k)]) return;  // read before written this step
        }
    }
    bool bReadsSteps = false;
    for (int k : bKeys_) {
        const expr::Program& p = cfg.keys[std::size_t(k)].prog;
        for (const auto& [fam, idx] : p.userRefs)
            if (fam == expr::FamC) bReadsSteps = true;
        for (int r : p.keyRefs)
            if (r >= 0 && std::size_t(r) < firstPos.size() && firstPos[std::size_t(r)] >= 0) bReadsSteps = true;
    }
    parallel_ = bReadsSteps ? Parallel::PerId : Parallel::Frame;
}

namespace {

// dst = src with dst owning its matrix (storage reused when it can be).
void assignDeep(Value& dst, const Value& src) {
    if (!src.m_) {
        dst.setScalar(src.s_);
        return;
    }
    const expr::Mat& m = *src.m_;
    if (dst.m_ && dst.m_.use_count() == 1 && dst.m_->rows == m.rows && dst.m_->cols == m.cols) {
        std::copy(m.d.begin(), m.d.end(), dst.m_->d.begin());
    } else {
        dst.m_ = std::make_shared<expr::Mat>(m);
    }
    dst.s_ = src.s_;
}

void assignDeep(std::vector<Value>& dst, const std::vector<Value>& src) {
    dst.resize(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) assignDeep(dst[i], src[i]);
}

}  // namespace

// The instance's state into a lane's context, once per dispatch per lane.
// A vars and the load-time keys never change after construction: copied when
// the lane is created only (they can be large). Per dispatch: B (vars, keys,
// per-ID snapshots) and the builtins. C slots are loaded per step, D is unused
// by WaveShapes, step-tier keys are written before they are read.
void WaveShapeInstance::syncLane(Lane& lane, bool created, bool withSnapshots) const {
    expr::EvalContext& c = lane.ctx;
    if (created) {
        for (std::size_t f = 0; f < ctx_.vars.size(); ++f) assignDeep(c.vars[f], ctx_.vars[f]);
        assignDeep(c.keys, ctx_.keys);
    }
    if (!withSnapshots) {  // Frame mode loads B per ID from the snapshots instead
        assignDeep(c.vars[expr::FamB], ctx_.vars[expr::FamB]);
        for (int k : bKeys_) assignDeep(c.keys[std::size_t(k)], ctx_.keys[std::size_t(k)]);
    }
    for (std::size_t i = 0; i < ctx_.builtins.size(); ++i) assignDeep(c.builtins[i], ctx_.builtins[i]);
    c.audio = ctx_.audio;
    c.randomLanes = 1;
    c.deterministicClock = ctx_.deterministicClock;
    if (withSnapshots) {
        if (lane.bSnap.size() < std::size_t(numInstances_)) lane.bSnap.resize(std::size_t(numInstances_));
        for (int id = 0; id < numInstances_; ++id) assignDeep(lane.bSnap[std::size_t(id)], bSnap_[std::size_t(id)]);
    }
}

void WaveShapeInstance::saveFinal(const expr::EvalContext& lane) {
    finalKeys_.resize(stepKeys_.size());
    for (std::size_t i = 0; i < stepKeys_.size(); ++i) finalKeys_[i] = lane.keys[std::size_t(stepKeys_[i])];
    finalC_ = lane.vars[expr::FamC];
}

void WaveShapeInstance::restoreFinal() {
    for (std::size_t i = 0; i < stepKeys_.size(); ++i) ctx_.keys[std::size_t(stepKeys_[i])] = finalKeys_[i];
    ctx_.vars[expr::FamC] = finalC_;
    ctx_.set(Var::S, stepParam(numSteps_ - 1));
    if (!pathIdx_.empty()) ctx_.set(Var::PATH_ID, pathIdx_.back());
}

void WaveShapeInstance::evaluateParallel(WaveShapeFrame& out) {
    const int N = numSteps_;
    const int P = int(pathKeys_.size());
    const int lanes = pool_->lanes();
    if (int(lanes_.size()) != lanes) {  // first use only
        lanes_.clear();
        lanes_.resize(std::size_t(lanes));
        for (Lane& l : lanes_) l.ctx = ctx_;  // stamp 0: the first sync copies everything
    }
    finalKeys_.resize(stepKeys_.size());
    // Contiguous step ranges, a few per lane for balance (cores differ in speed).
    const int tasks = std::min(N, lanes * 4);
    auto range = [N, tasks](int t) { return std::pair<int, int>(int(long(N) * t / tasks), int(long(N) * (t + 1) / tasks)); };
    auto setBase = [&](int id) {
        ctx_.set(Var::ID, id);
        ctx_.set(Var::S, sVector_);
        ctx_.set(Var::PATH_ID, P ? pathIdx_[0] : 0);
    };

    if (parallel_ == Parallel::Frame) {
        const std::size_t nB = ctx_.vars[expr::FamB].size();
        if (bSnap_.size() < std::size_t(numInstances_)) bSnap_.resize(std::size_t(numInstances_));
        for (int id = 0; id < numInstances_; ++id) {
            setBase(id);
            evalFamily(expr::FamB);
            auto& snap = bSnap_[std::size_t(id)];
            snap.resize(nB + bKeys_.size());
            for (std::size_t i = 0; i < nB; ++i) snap[i] = ctx_.vars[expr::FamB][i];
            for (std::size_t i = 0; i < bKeys_.size(); ++i) snap[nB + i] = ctx_.keys[std::size_t(bKeys_[i])];
        }
        const std::uint64_t stamp = ++dispatch_;
        pool_->run(tasks, [&](int t, int laneIdx) {
            Lane& lane = lanes_[std::size_t(laneIdx)];
            expr::EvalContext& c = lane.ctx;
            if (lane.stamp != stamp) {
                syncLane(lane, lane.stamp == 0, true);
                lane.stamp = stamp;
            }
            const auto [k0, k1] = range(t);
            for (int id = 0; id < numInstances_; ++id) {
                const auto& snap = lane.bSnap[std::size_t(id)];
                for (std::size_t i = 0; i < nB; ++i) c.vars[expr::FamB][i] = snap[i];
                for (std::size_t i = 0; i < bKeys_.size(); ++i) c.keys[std::size_t(bKeys_[i])] = snap[nB + i];
                c.set(Var::ID, id);
                for (int k = k0; k < k1; ++k) evalStep(c, out, id, k);
            }
            if (k1 == N) saveFinal(c);
        });
        restoreFinal();
        return;
    }
    for (int id = 0; id < numInstances_; ++id) {
        setBase(id);
        evalFamily(expr::FamB);
        const std::uint64_t stamp = ++dispatch_;
        pool_->run(tasks, [&](int t, int laneIdx) {
            Lane& lane = lanes_[std::size_t(laneIdx)];
            expr::EvalContext& c = lane.ctx;
            if (lane.stamp != stamp) {
                syncLane(lane, lane.stamp == 0, false);
                lane.stamp = stamp;
            }
            const auto [k0, k1] = range(t);
            for (int k = k0; k < k1; ++k) evalStep(c, out, id, k);
            if (k1 == N) saveFinal(c);
        });
        restoreFinal();
    }
}

// ----------------------------------------------------------------- ColorMap

Rgb hsvToRgb(double h, double s, double v) {
    h = (h - std::floor(h)) * 6.0;
    const int sector = std::min(int(h), 5);
    const double f = h - sector;
    const double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    double r = v, g = t, b = p;
    switch (sector) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return {float(r), float(g), float(b)};
}

ColorMapInstance::ColorMapInstance(const CompiledConfig& cfg, const Environment& env) : ConfigInstance(cfg, env) {
    ctx_.set(Var::I, 0.0);
    runLoadTiers();
    h_ = cfg.indexOf("H");
    s_ = cfg.indexOf("S");
    v_ = cfg.indexOf("V");
    r_ = cfg.indexOf("R");
    g_ = cfg.indexOf("G");
    b_ = cfg.indexOf("B");
    const bool anyHsv = cfg.defines("H") || cfg.defines("S") || cfg.defines("V");
    const bool anyRgb = cfg.defines("R") || cfg.defines("G") || cfg.defines("B");
    hsv_ = anyHsv || !anyRgb;
    const int cc = cfg.indexOf("CCAL");
    if (cc >= 0 && cfg.keys[std::size_t(cc)].prog.defined) {
        const double v = ctx_.keys[std::size_t(cc)].scalar();
        if (std::isfinite(v) && v > 0.0) ccal_ = v;
    }
}

void ColorMapInstance::evaluate(double t, std::array<Rgb, kEntries>& out) {
    ctx_.set(Var::T, t);
    ctx_.set(Var::I, 0.0);  // B vars see "only t, never i" [CW]
    ctx_.randomLanes = 1;
    evalFamily(expr::FamB);
    auto clamp01 = [](double x) { return std::isnan(x) ? x : std::clamp(x, 0.0, 1.0); };
    for (int k = 0; k < kEntries; ++k) {
        ctx_.set(Var::I, double(k) / double(kEntries - 1));  // i = k/255 (spec §10.16, assumed)
        evalStepFamily(expr::FamC, k, kEntries);  // per-i VectorC state, like WaveShape C
        if (hsv_) {
            const double h = evalScalar(h_, 0.0), s = evalScalar(s_, 0.0), v = evalScalar(v_, 0.0);
            // H wraps [CW]; S and V clamp to [0,1] (outputs clamp per probe 2026-11).
            if (!std::isfinite(h) || std::isnan(s) || std::isnan(v)) {
                const float nan = float(std::nan(""));
                out[std::size_t(k)] = {nan, nan, nan};
            } else {
                out[std::size_t(k)] = hsvToRgb(h, clamp01(s), clamp01(v));
            }
        } else {
            out[std::size_t(k)] = {float(clamp01(evalScalar(r_, 0.0))), float(clamp01(evalScalar(g_, 0.0))),
                                   float(clamp01(evalScalar(b_, 0.0)))};
        }
    }
}

// ----------------------------------------------------------------- FlowField

FlowFieldInstance::FlowFieldInstance(const CompiledConfig& cfg, const Environment& env) : ConfigInstance(cfg, env) {
    for (Var v : {Var::X, Var::Y, Var::R, Var::Theta}) ctx_.set(v, 0.0);
    pixelGridExtents_ = true;
    ctx_.float32 = env.flowFloat32;
    runLoadTiers();
    srcX_ = cfg.indexOf("SRCX");
    srcY_ = cfg.indexOf("SRCY");
    srcR_ = cfg.indexOf("SRCR");
    srcT_ = cfg.indexOf("SRCT");
    alfa_ = cfg.indexOf("ALFA");
    const bool cart = cfg.defines("SRCX") || cfg.defines("SRCY");
    polar_ = !cart && (cfg.defines("SRCR") || cfg.defines("SRCT"));
}

// FlowField pixel grid: G-Force evaluates pixel column c at x = -1 + 2c/(W-1)
// and row r at y = 1 - 2r/(H-1) (times the extents), i.e. the first and last
// pixels sit exactly on the frame edges (corpus sweep 5B.4: "Bits Between
// Bits", "Digital Staircase", "TriGrid" pin this down; pixel centers or
// corners fail them). Sources map back to pixels with the same rule
// (ScenePipeline sample_prev).
std::vector<double> FlowFieldInstance::rowXs(int w) const {
    std::vector<double> xs(std::size_t(std::max(w, 0)));
    const double ex = xExtent();
    for (int c = 0; c < w; ++c) xs[std::size_t(c)] = w > 1 ? (-1.0 + 2.0 * c / double(w - 1)) * ex : 0.0;
    return xs;
}

double FlowFieldInstance::rowY(int row, int h) const {
    return h > 1 ? (1.0 - 2.0 * row / double(h - 1)) * yExtent() : 0.0;
}

void FlowFieldInstance::bindRow(double y, std::span<const double> xs) {
    const int W = int(xs.size());
    const bool f32 = ctx_.float32;
    // float32 mode: x, y, r and theta computed in single precision.
    auto polar = [f32](double x, double y, double& r, double& th) {
        if (f32) {
            const float xf = float(x), yf = float(y);
            r = std::sqrt(xf * xf + yf * yf);
            th = std::atan2(yf, xf);
        } else {
            r = std::sqrt(x * x + y * y);
            th = std::atan2(y, x);  // theta in -pi..pi from +x
        }
    };
    if (f32) y = double(float(y));
    ctx_.set(Var::Y, y);
    if (W == 1) {
        double r = 0, th = 0;
        polar(xs[0], y, r, th);
        ctx_.set(Var::X, f32 ? double(float(xs[0])) : xs[0]);
        ctx_.set(Var::R, r);
        ctx_.set(Var::Theta, th);
        return;
    }
    Value xv = Value::matrix(1, W), rv = Value::matrix(1, W), tv = Value::matrix(1, W);
    auto& xd = xv.mutableMat().d;
    auto& rd = rv.mutableMat().d;
    auto& td = tv.mutableMat().d;
    for (int i = 0; i < W; ++i) {
        const double x = xs[std::size_t(i)];
        xd[std::size_t(i)] = f32 ? double(float(x)) : x;
        polar(x, y, rd[std::size_t(i)], td[std::size_t(i)]);
    }
    ctx_.set(Var::X, std::move(xv));
    ctx_.set(Var::R, std::move(rv));
    ctx_.set(Var::Theta, std::move(tv));
}

void FlowFieldInstance::evaluateRow(double y, std::span<const double> xs, double* srcX, double* srcY) {
    const int W = int(xs.size());
    if (W == 0) return;
    bindRow(y, xs);
    ctx_.randomLanes = 1;      // C vars: rnd() returns one scalar per row [FL]
    evalFamily(expr::FamC);
    ctx_.randomLanes = W;      // D vars and src keys: per-pixel rnd() [NM]
    evalFamily(expr::FamD);
    auto eval = [&](int idx) -> Value {
        if (idx < 0 || !cfg_->keys[std::size_t(idx)].prog.defined) return Value();
        return evalKey(idx);
    };
    if (polar_) {
        const bool hasR = srcR_ >= 0 && cfg_->keys[std::size_t(srcR_)].prog.defined;
        const bool hasT = srcT_ >= 0 && cfg_->keys[std::size_t(srcT_)].prog.defined;
        const Value R = eval(srcR_), T = eval(srcT_);
        // GUESS: a missing polar coordinate keeps the destination's (r or theta).
        const Value& rr = hasR ? R : ctx_.value(Var::R);
        const Value& tt = hasT ? T : ctx_.value(Var::Theta);
        for (int i = 0; i < W; ++i) {
            const double r = expr::laneScalar(rr, i, W), th = expr::laneScalar(tt, i, W);
            if (ctx_.float32) {
                const float rf = float(r), tf = float(th);
                srcX[i] = rf * std::cos(tf);
                srcY[i] = rf * std::sin(tf);
            } else {
                srcX[i] = r * std::cos(th);
                srcY[i] = r * std::sin(th);
            }
        }
    } else {
        const bool hasX = srcX_ >= 0 && cfg_->keys[std::size_t(srcX_)].prog.defined;
        const bool hasY = srcY_ >= 0 && cfg_->keys[std::size_t(srcY_)].prog.defined;
        const Value X = eval(srcX_), Y = eval(srcY_);
        // GUESS: a missing coordinate is the identity (no flow along that axis).
        for (int i = 0; i < W; ++i) {
            srcX[i] = hasX ? expr::laneScalar(X, i, W) : xs[std::size_t(i)];
            srcY[i] = hasY ? expr::laneScalar(Y, i, W) : y;
        }
    }
    ctx_.randomLanes = 1;
}

void FlowFieldInstance::evaluateAlphaRow(double y, std::span<const double> xs, double* alpha) {
    const int W = int(xs.size());
    if (W == 0) return;
    bindRow(y, xs);
    ctx_.randomLanes = 1;
    evalFamily(expr::FamC);
    ctx_.randomLanes = W;
    evalFamily(expr::FamD);
    if (alfa_ >= 0 && cfg_->keys[std::size_t(alfa_)].prog.defined) {
        const Value a = evalKey(alfa_);
        for (int i = 0; i < W; ++i) alpha[i] = expr::laneScalar(a, i, W);
    } else {
        for (int i = 0; i < W; ++i) alpha[i] = 0.0;
    }
    ctx_.randomLanes = 1;
}

std::array<double, 2> FlowFieldInstance::evaluatePoint(double x, double y) {
    double sx = 0, sy = 0;
    const double xs[1] = {x};
    evaluateRow(y, std::span<const double>(xs, 1), &sx, &sy);
    return {sx, sy};
}

void FlowFieldInstance::evaluateGrid(int w, int h, std::vector<float>& srcXY) {
    srcXY.assign(std::size_t(std::max(w, 0)) * std::size_t(std::max(h, 0)) * 2, 0.0f);
    if (w <= 0 || h <= 0) return;
    const auto xs = rowXs(w);
    std::vector<double> sx(static_cast<std::size_t>(w)), sy(static_cast<std::size_t>(w));
    for (int r = 0; r < h; ++r) {
        evaluateRow(rowY(r, h), xs, sx.data(), sy.data());
        float* dst = srcXY.data() + std::size_t(r) * std::size_t(w) * 2;
        for (int c = 0; c < w; ++c) {
            dst[2 * c] = float(sx[std::size_t(c)]);
            dst[2 * c + 1] = float(sy[std::size_t(c)]);
        }
    }
}

// ----------------------------------------------------------------- Sprite

void setSyncVars(expr::EvalContext& ctx, const SyncInput& s) {
    ctx.set(Var::SYNC_ON, s.on);
    ctx.set(Var::SYNC_BPM, s.bpm);
    ctx.set(Var::SYNC_BEAT, s.beat);
    ctx.set(Var::SYNC_LOCK, s.lock);
    ctx.set(Var::SYNC_BAR, s.bar);
    ctx.set(Var::SYNC_PHRASE, s.phrase);
    ctx.set(Var::SYNC_BUILD, s.build);
    ctx.set(Var::SYNC_DROP, s.drop);
}

SpriteInstance::SpriteInstance(const CompiledConfig& cfg, const Environment& env) : ConfigInstance(cfg, env) {
    runLoadTiers();
    auto loaded = [&](const char* key, double fallback) {
        const int i = cfg.indexOf(key);
        if (i < 0 || !cfg.keys[std::size_t(i)].prog.defined) return fallback;
        return ctx_.keys[std::size_t(i)].scalar();
    };
    x_ = cfg.indexOf("X");
    y_ = cfg.indexOf("Y");
    pen_ = cfg.indexOf("PEN");
    epen_ = cfg.indexOf("EPEN");
    hasPen_ = cfg.defines("PEN");
    hasEPen_ = cfg.defines("EPEN");
    hasFlag_ = cfg.defines("FLAG");
    const double fl = loaded("FLAG", 0.0);
    flags_ = std::isfinite(fl) && fl > 0 ? unsigned(std::min(fl, 4294967295.0)) : 0u;
    invt_ = loaded("INVT", 0.0) != 0.0;
    sped_ = loaded("SPED", 0.0);
    if (!std::isfinite(sped_)) sped_ = 0.0;
    duration_ = loaded("PDUR", std::nan(""));
}

SpriteFrame SpriteInstance::evaluateFrame(double t, double dt, double fader) {
    SpriteFrameInput in;
    in.t = t;
    in.dt = dt;
    in.fader = fader;
    return evaluateFrame(in);
}

SpriteFrame SpriteInstance::evaluateFrame(const SpriteFrameInput& in) {
    ctx_.set(Var::T, in.t);
    ctx_.set(Var::DT, in.dt);
    ctx_.set(Var::FADER, in.fader);
    ctx_.set(Var::XPOS, in.xpos);
    ctx_.set(Var::YPOS, in.ypos);
    ctx_.set(Var::START_TIME, in.startTime);
    ctx_.set(Var::END_TIME, in.endTime);
    ctx_.set(Var::BASS, in.bass);
    ctx_.set(Var::LEVEL, in.level);
    ctx_.set(Var::ENERGY, in.energy);
    ctx_.set(Var::MID, in.mid);
    ctx_.set(Var::TREBLE, in.treble);
    ctx_.set(Var::BEAT, in.beat);
    ctx_.set(Var::TEMPO, in.tempo);
    ctx_.set(Var::LINK_ON, in.linkOn);
    ctx_.set(Var::LINK_BPM, in.linkBpm);
    ctx_.set(Var::LINK_BEAT, in.linkBeat);
    ctx_.set(Var::LINK_PHASE, in.linkPhase);
    setSyncVars(ctx_, in.sync);
    ctx_.randomLanes = 1;
    evalFamily(expr::FamB);
    SpriteFrame f;
    f.x = evalScalar(x_, 0.0);
    f.y = evalScalar(y_, 0.0);
    f.hasPen = hasPen_;
    f.hasEPen = hasEPen_;
    f.pen = evalScalar(pen_, hasPen_ ? 1.0 : 0.0);  // Pen absent: background-only sprite [DS]
    f.epen = evalScalar(epen_, 0.0);
    return f;
}

// ----------------------------------------------------------------- Solid

SolidInstance::SolidInstance(const CompiledConfig& cfg, const Environment& env) : ConfigInstance(cfg, env) {
    runLoadTiers();
    auto loaded = [&](const char* key, double fallback) {
        const int i = cfg.indexOf(key);
        if (i < 0 || !cfg.keys[std::size_t(i)].prog.defined) return fallback;
        return ctx_.keys[std::size_t(i)].scalar();
    };
    if (const CompiledKey* m = cfg.find("MESH")) {
        mesh_ = m->text;
        const auto b = mesh_.find_first_not_of(" \t\r\n");
        const auto e = mesh_.find_last_not_of(" \t\r\n");
        mesh_ = b == std::string::npos ? std::string() : mesh_.substr(b, e - b + 1);
    }
    const double tx = loaded("TEXT", 0.0);
    text_ = std::isfinite(tx) ? std::clamp(int(std::lround(tx)), 0, 2) : 0;
    hasFlag_ = cfg.defines("FLAG");
    const double fl = loaded("FLAG", 0.0);
    flags_ = std::isfinite(fl) && fl > 0 ? unsigned(std::min(fl, 4294967295.0)) : 0u;
    duration_ = loaded("PDUR", std::nan(""));
    pers_ = loaded("PERS", 1.0) != 0.0;
    colorKey_ = loaded("COLORKEY", 1.0) != 0.0;
    over_ = loaded("OVER", 0.0) != 0.0;
    glass_ = loaded("GLAS", 0.0) != 0.0;
    const double mv = loaded("MOVES", 30.0);
    moves_ = std::isfinite(mv) ? std::clamp(int(std::lround(mv)), 1, 64) : 30;
    turn_ = cfg.defines("TURN") ? cfg.indexOf("TURN") : -1;
    seed_ = cfg.indexOf("SEED");
    shuf_ = cfg.indexOf("SHUF");
    fros_ = cfg.indexOf("FROS");
    tint_ = cfg.indexOf("TINT");
    bend_ = cfg.indexOf("BEND");
    x_ = cfg.indexOf("X");
    y_ = cfg.indexOf("Y");
    z_ = cfg.indexOf("Z");
    scal_ = cfg.indexOf("SCAL");
    rx_ = cfg.indexOf("ROTX");
    ry_ = cfg.indexOf("ROTY");
    rz_ = cfg.indexOf("ROTZ");
    fill_ = cfg.indexOf("FILL");
    shad_ = cfg.indexOf("SHAD");
    edge_ = cfg.indexOf("EDGE");
    pen_ = cfg.indexOf("PEN");
    epen_ = cfg.indexOf("EPEN");
    // Like a sprite: no Pen and no EPen = Pen 1; EPen alone = display only.
    hasEPen_ = cfg.defines("EPEN");
    hasPen_ = cfg.defines("PEN") || !hasEPen_;
}

SolidFrame SolidInstance::evaluateFrame(const SolidFrameInput& in) {
    ctx_.set(Var::T, in.t);
    ctx_.set(Var::DT, in.dt);
    ctx_.set(Var::FADER, in.fader);
    ctx_.set(Var::START_TIME, in.startTime);
    ctx_.set(Var::END_TIME, in.endTime);
    ctx_.set(Var::BASS, in.bass);
    ctx_.set(Var::LEVEL, in.level);
    ctx_.set(Var::ENERGY, in.energy);
    ctx_.set(Var::MID, in.mid);
    ctx_.set(Var::TREBLE, in.treble);
    ctx_.set(Var::BEAT, in.beat);
    ctx_.set(Var::TEMPO, in.tempo);
    ctx_.set(Var::LINK_ON, in.linkOn);
    ctx_.set(Var::LINK_BPM, in.linkBpm);
    ctx_.set(Var::LINK_BEAT, in.linkBeat);
    ctx_.set(Var::LINK_PHASE, in.linkPhase);
    setSyncVars(ctx_, in.sync);
    ctx_.randomLanes = 1;
    evalFamily(expr::FamB);
    SolidFrame f;
    f.x = evalScalar(x_, 0.0);
    f.y = evalScalar(y_, 0.0);
    f.z = evalScalar(z_, 0.0);
    f.scale = evalScalar(scal_, 0.5);
    f.rotX = evalScalar(rx_, 0.0);
    f.rotY = evalScalar(ry_, 0.0);
    f.rotZ = evalScalar(rz_, 0.0);
    f.fill = evalScalar(fill_, 0.15);
    f.shade = evalScalar(shad_, 0.6);
    f.edge = evalScalar(edge_, 0.8);
    f.hasPen = hasPen_;
    f.hasEPen = hasEPen_;
    f.pen = evalScalar(pen_, hasPen_ ? 1.0 : 0.0);
    f.epen = evalScalar(epen_, 0.0);
    f.turn = evalScalar(turn_, 2.0 * moves_);  // no Turn: solved (rubik; fifteen clamps to Moves)
    f.seed = evalScalar(seed_, 0.0);
    f.shuffle = evalScalar(shuf_, 1.0);
    f.frost = evalScalar(fros_, 12.0);
    f.tint = evalScalar(tint_, 0.35);
    f.bend = evalScalar(bend_, 1.0);
    return f;
}

// ----------------------------------------------------------------- Organic

OrganicInstance::OrganicInstance(const CompiledConfig& cfg, const Environment& env) : ConfigInstance(cfg, env) {
    runLoadTiers();
    if (const CompiledKey* k = cfg.find("KIND")) {
        const auto b = k->text.find_first_not_of(" \t\r\n");
        const auto e = k->text.find_last_not_of(" \t\r\n");
        kind_ = b == std::string::npos ? std::string() : k->text.substr(b, e - b + 1);
    }
    duration_ = loaded("PDUR", std::nan(""));
    grow_ = cfg.indexOf("GROW");
    trpX_ = cfg.indexOf("TRPX");
    trpY_ = cfg.indexOf("TRPY");
    sway_ = cfg.indexOf("SWAY");
    x_ = cfg.indexOf("X");
    y_ = cfg.indexOf("Y");
    scal_ = cfg.indexOf("SCAL");
    rot_ = cfg.indexOf("ROT");
    pen_ = cfg.indexOf("PEN");
    bud_ = cfg.indexOf("BUD");
    amp_ = cfg.indexOf("AMP");
    strg_ = cfg.indexOf("STRG");
    phas_ = cfg.indexOf("PHAS");
}

std::string OrganicInstance::literal(const char* key) const {
    const CompiledKey* k = cfg_->find(key);
    if (!k) return {};
    const auto b = k->text.find_first_not_of(" \t\r\n");
    const auto e = k->text.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : k->text.substr(b, e - b + 1);
}

double OrganicInstance::loaded(const char* key, double fallback) const {
    const int i = cfg_->indexOf(key);
    if (i < 0 || !cfg_->keys[std::size_t(i)].prog.defined) return fallback;
    return ctx_.keys[std::size_t(i)].scalar();
}

void OrganicInstance::evaluatePoints(int count, std::span<const PointKey> keys,
                                     std::vector<std::vector<double>>& out) {
    std::vector<int> idx;
    for (const PointKey& k : keys) idx.push_back(cfg_->indexOf(k.key));
    out.assign(keys.size(), {});
    for (auto& o : out) o.reserve(std::size_t(std::max(0, count)));
    ctx_.randomLanes = 1;
    for (int i = 0; i < count; ++i) {
        ctx_.set(Var::S, count > 1 ? double(i) / double(count - 1) : 0.0);
        evalFamily(expr::FamC);
        for (std::size_t k = 0; k < keys.size(); ++k) out[k].push_back(evalScalar(idx[k], keys[k].fallback));
    }
}

OrganicFrame OrganicInstance::evaluateFrame(const OrganicFrameInput& in) {
    ctx_.set(Var::T, in.t);
    ctx_.set(Var::DT, in.dt);
    ctx_.set(Var::FADER, in.fader);
    ctx_.set(Var::START_TIME, in.startTime);
    ctx_.set(Var::END_TIME, in.endTime);
    ctx_.set(Var::BASS, in.bass);
    ctx_.set(Var::LEVEL, in.level);
    ctx_.set(Var::ENERGY, in.energy);
    ctx_.set(Var::MID, in.mid);
    ctx_.set(Var::TREBLE, in.treble);
    ctx_.set(Var::BEAT, in.beat);
    ctx_.set(Var::TEMPO, in.tempo);
    ctx_.set(Var::LINK_ON, in.linkOn);
    ctx_.set(Var::LINK_BPM, in.linkBpm);
    ctx_.set(Var::LINK_BEAT, in.linkBeat);
    ctx_.set(Var::LINK_PHASE, in.linkPhase);
    setSyncVars(ctx_, in.sync);
    ctx_.randomLanes = 1;
    evalFamily(expr::FamB);
    OrganicFrame f;
    f.grow = evalScalar(grow_, 12.0);
    f.trpX = evalScalar(trpX_, 0.0);
    f.trpY = evalScalar(trpY_, 0.0);
    f.sway = evalScalar(sway_, 0.15);
    f.x = evalScalar(x_, 0.0);
    f.y = evalScalar(y_, 0.0);
    f.scale = evalScalar(scal_, 1.0);
    f.rot = evalScalar(rot_, 0.0);
    f.pen = evalScalar(pen_, 1.0);
    f.bud = evalScalar(bud_, 0.0);
    f.amp = evalScalar(amp_, std::nan(""));
    f.strg = evalScalar(strg_, std::nan(""));
    f.phas = evalScalar(phas_, std::nan(""));
    return f;
}

}  // namespace reforce::scene

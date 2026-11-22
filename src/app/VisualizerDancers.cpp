// Visualizer: dancers (PLAN 5P), stream guests and name tags.
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

// ---------------------------------------------------------------- dancers (PLAN 5P)

bool Visualizer::startDancer(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label, double t,
                             const FadeSpec& fade) {
    if (!cfg) return false;
    engine::DancerSpec spec = engine::dancerSpec(*cfg, label);
    for (const auto& w : spec.warnings) rlog("[reforce] Dancer \"%s\": %s\n", label.c_str(), w.c_str());
    // hold 0: never leaves by itself (Duration ∞ wins over the character's PDur).
    const double hold = fade.forever ? 0.0 : std::isnan(spec.duration) ? fade.defaultHold : spec.duration;
    return startDancerSpec(std::move(spec), t, hold);
}

bool Visualizer::dancerFileAllowed(const std::filesystem::path& path) {
    if (danceRandom_ == "all") return true;
    const std::string key = path.string();
    auto it = dancerFileStyle_.find(key);
    if (it == dancerFileStyle_.end()) {
        int style = -1;
        if (auto cfg = compileFile(path, scene::SceneType::Dancer)) {
            const engine::DancerSpec spec = engine::dancerSpec(*cfg, path.stem().string());
            if (spec.random) style = int(spec.style);
        }
        it = dancerFileStyle_.emplace(key, style).first;
    }
    return it->second < 0 || engine::randomStyleAllowed(danceRandom_, engine::dance::Style(it->second));
}

// The most dancers on stage at once, guests and characters together: the renderer's budget (DancerPipeline's
// shadows: 3 a dancer for 32, plus a few waving goodbye).
static constexpr int kStageCap = 32;

bool Visualizer::startDancerSpec(engine::DancerSpec spec, double t, double hold) {
    if (!dancers_) {
        rlog("[reforce] Dancer \"%s\": dancers are unavailable\n", spec.name.c_str());
        return false;
    }
    perf::tag(perf::TagDancerStart);
    // Max dancers (reforce.Dancer.MaxLive) counts the characters only: stream guests come and go regardless of it
    // (Peter, 2026-11-21). A new character sends the oldest character off; a guest sends nobody off, short of the
    // stage's hard limit (kStageCap, the renderer's budget), where characters make way first, then the oldest guest.
    const int maxLive = std::clamp(danceMaxLive_, 1, kStageCap);
    const bool guest = !spec.guestId.empty();
    auto count = [&](bool charactersOnly) {
        return int(std::count_if(liveDancers_.begin(), liveDancers_.end(), [&](const auto& d) {
            return !d->leaving() && (!charactersOnly || d->spec().guestId.empty());
        }));
    };
    if (!guest)
        for (auto& d : liveDancers_) {
            if (count(true) < maxLive) break;
            if (d->leaving() || !d->spec().guestId.empty()) continue;
            rlog("[reforce] Dancer \"%s\" makes way for \"%s\" (reforce.Dancer.MaxLive %d)\n", d->name().c_str(),
                 spec.name.c_str(), maxLive);
            d->leave(t, stage_.beat, 1.0);
        }
    for (int pass = 0; pass < 2; ++pass)
        for (auto& d : liveDancers_) {
            if (count(false) < kStageCap) break;
            if (d->leaving() || (pass == 0 && !d->spec().guestId.empty())) continue;
            rlog("[reforce] Dancer \"%s\" makes way for \"%s\" (the stage holds %d)\n", d->name().c_str(),
                 spec.name.c_str(), kStageCap);
            d->leave(t, stage_.beat, 1.0);
        }
    std::uint32_t seed = std::uint32_t(opts_.seed * 2654435761ULL) ^ (dancerSeed_++ * 0x9e3779b9U);
    seed ^= seed >> 15;
    if (spec.random) {
        // A new character each time, dressed for the style (keeps the file's Snap / Moves).
        engine::DancerSpec made = engine::generateDancer(spec.style, seed);
        rlog("[reforce] Dancer \"%s\": a new character, %s\n", spec.name.c_str(), made.name.c_str());
        made.snap = spec.snap;
        made.likes = spec.likes;
        made.tag = spec.tag;
        made.guestId = spec.guestId;
        spec = std::move(made);
    }
    if (hold > 0)
        rlog("[reforce] Dancer \"%s\" starts: %s, hold %.0f s\n", spec.name.c_str(),
             engine::dance::styleName(spec.style), hold);
    else
        rlog("[reforce] Dancer \"%s\" starts: %s, stays until cleared\n", spec.name.c_str(),
             engine::dance::styleName(spec.style));
    liveDancers_.push_back(std::make_unique<engine::DancerRun>(std::move(spec), seed, t, stage_.beat, hold));
    return true;
}

void Visualizer::killDancers(double t, double fade) {
    if (liveDancers_.empty()) return;
    perf::tag(perf::TagKill);
    rlog("[reforce] %zu dancer(s) killed%s\n", liveDancers_.size(), fade > 0 ? ": waving goodbye" : "");
    if (fade <= 0) {
        liveDancers_.clear();
        stage_.bodyCount = 0;
        return;
    }
    for (auto& d : liveDancers_) d->leave(t, stage_.beat, fade);
}

std::vector<std::string> Visualizer::dancerNames() const {
    std::vector<std::string> n;
    for (const auto& d : liveDancers_)
        if (!d->leaving()) n.push_back(d->name());
    return n;
}

engine::dance::MusicState Visualizer::danceMusic(double t, const audio::AudioFrame* audio) {
    const scene::SyncInput s = syncInput();
    engine::dance::MusicState m;
    if (s.bpm > 0) {
        m.beat = s.beat;
        m.bar = s.bar != 0 ? s.bar : s.beat / 4;
        m.phrase = s.phrase != 0 ? s.phrase : m.bar / 16;
        m.locked = s.on > 0.5;
    } else {  // no beat clock: a gentle 110 BPM groove
        m.beat = t * 110.0 / 60.0;
        m.bar = m.beat / 4;
        m.phrase = m.bar / 16;
        m.locked = false;
    }
    m.build = s.build;
    m.drop = s.drop;
    const double bass = audio ? std::clamp(audio->bass * 0.5, 0.0, 1.0) : 0.0;
    danceBass_ += (bass - danceBass_) * 0.08;
    m.bass = danceBass_;
    return m;
}

void Visualizer::setGuestsBlocked(const std::string& list) {
    guestsBlocked_.clear();
    std::size_t at = 0;
    while (at <= list.size()) {
        const std::size_t comma = std::min(list.find(',', at), list.size());
        if (comma > at) guestsBlocked_.push_back(list.substr(at, comma - at));
        at = comma + 1;
    }
    // Blocked guests on stage now wave goodbye.
    for (auto& d : liveDancers_)
        if (!d->leaving() && !d->spec().guestId.empty() &&
            std::find(guestsBlocked_.begin(), guestsBlocked_.end(), d->spec().guestId) != guestsBlocked_.end()) {
            rlog("[reforce] guest \"%s\" blocked: leaves\n", d->name().c_str());
            d->leave(std::max(0.0, lastDanceT_), stage_.beat, 2.0);
        }
}

bool Visualizer::callGuest(const std::string& idOrLabel, double t) {
    if (!guestBook_) return false;
    const web::SavedGuest* g = guestBook_->find(idOrLabel);
    if (!g) return false;
    if (std::find(guestsBlocked_.begin(), guestsBlocked_.end(), g->id) != guestsBlocked_.end()) return false;
    const bool here = std::any_of(liveDancers_.begin(), liveDancers_.end(), [&](const auto& d) {
        return !d->leaving() && d->spec().guestId == g->id;
    });
    if (here) {
        rlog("[reforce] past guest \"%s\" is on stage already\n", g->name.c_str());
        return true;
    }
    const auto text = guestBook_->sceneText(g->id);
    if (!text) return false;
    const scene::CompiledConfig cfg = scene::compileConfig(scene::parseSceneText(*text), scene::SceneType::Dancer);
    engine::DancerSpec spec = engine::dancerSpec(cfg, g->name);
    spec.guestId = g->id;
    spec.random = false;
    guestNames_[g->id] = g->name;
    rlog("[reforce] past guest \"%s\" brought on (stays until cleared)\n", g->name.c_str());
    return startDancerSpec(std::move(spec), t, 0.0);
}

void Visualizer::startGuests(double t) {
    if (!guestFeed_) return;
    web::GuestFeed::Batch batch = guestFeed_->take();
    for (const web::GuestArrival& a : batch.arrivals) {
        if (!guestsOn_) continue;
        guestNames_[a.id] = a.name;
        if (std::find(guestsBlocked_.begin(), guestsBlocked_.end(), a.id) != guestsBlocked_.end()) {
            rlog("[reforce] guest \"%s\" checked in (blocked: not shown)\n", a.name.c_str());
            continue;
        }
        if (guestBook_) {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch()).count();
            const std::string err = guestBook_->save(a.id, a.name, a.sceneText, std::int64_t(now));
            if (!err.empty()) rlog("[reforce] guest \"%s\" not saved: %s\n", a.name.c_str(), err.c_str());
        }
        const bool here = std::any_of(liveDancers_.begin(), liveDancers_.end(), [&](const auto& d) {
            return !d->leaving() && d->spec().guestId == a.id;
        });
        if (here) {
            rlog("[reforce] guest \"%s\" checked in again (already dancing)\n", a.name.c_str());
            continue;
        }
        const scene::SceneFile file = scene::parseSceneText(a.sceneText);
        const scene::CompiledConfig cfg = scene::compileConfig(file, scene::SceneType::Dancer);
        engine::DancerSpec spec = engine::dancerSpec(cfg, a.name);
        for (const auto& w : spec.warnings) rlog("[reforce] guest \"%s\": %s\n", a.name.c_str(), w.c_str());
        spec.guestId = a.id;
        spec.random = false;
        rlog("[reforce] guest \"%s\" is here\n", a.name.c_str());
        startDancerSpec(std::move(spec), t, guestsMinutes_ * 60.0);
    }
    // Messages: a speech bubble over the guest's dancer (who arrives in this batch or is on stage already).
    for (const web::GuestMessage& m : batch.messages) {
        if (!guestsOn_) continue;
        if (std::find(guestsBlocked_.begin(), guestsBlocked_.end(), m.id) != guestsBlocked_.end()) continue;
        auto run = std::find_if(liveDancers_.begin(), liveDancers_.end(),
                                [&](const auto& d) { return !d->leaving() && d->spec().guestId == m.id; });
        if (run == liveDancers_.end()) {
            rlog("[reforce] guest \"%s\" says (not on stage): %s\n", m.name.c_str(), m.text.c_str());
            continue;
        }
        rlog("[reforce] guest \"%s\" says: %s\n", m.name.c_str(), m.text.c_str());
        const engine::DancerRun* r = run->get();
        guestBubbles_.erase(std::remove_if(guestBubbles_.begin(), guestBubbles_.end(),
                                           [&](const GuestBubble& b) { return b.run == r; }),
                            guestBubbles_.end());
        guestBubbles_.push_back({r, m.text, t});
    }
}

void Visualizer::stepDancers(double t, const audio::AudioFrame* audio) {
    if (!dancers_) return;
    startGuests(t);
    if (!dancerTestsPending_.empty()) {
        int i = 0;
        for (const auto& s0 : dancerTestsPending_) {
            // "random" / "random:STYLE": a generated character.
            const bool random = s0.rfind("random", 0) == 0;
            const std::string s = random ? (s0.size() > 7 ? s0.substr(7) : std::string()) : s0;
            if (random) {
                const auto st = engine::dance::parseStyle(s);
                engine::DancerSpec spec;
                spec.name = "random";
                spec.random = true;
                spec.style = st.value_or(engine::dance::Style(i % engine::dance::kStyleCount));
                startDancerSpec(std::move(spec), t, 0);
                ++i;
                continue;
            }
            // "STYLE": a generated character (seeded by its place in the list: the same each run; no named characters
            // since 2026-11-21); "STYLE@HAIR" (e.g. pop@afro) with that hairstyle (and no hat); "STYLE@HAIR@HAT"
            // (pop@bob@cap) with that hat; "STYLE@HAIR@HAT@BEARD" (pop@short@none@full) with that beard.
            const auto at = s.find('@');
            const auto at2 = at == std::string::npos ? std::string::npos : s.find('@', at + 1);
            // "STYLE#SEED..." (house#12@short): that generated character instead of the list position's.
            const std::string head0 = s.substr(0, at);
            const auto hash = head0.find('#');
            const std::uint32_t seed = hash == std::string::npos ? std::uint32_t(i)
                                                                 : std::uint32_t(std::strtoul(head0.c_str() + hash + 1, nullptr, 10));
            const auto st = engine::dance::parseStyle(head0.substr(0, hash));
            if (!st) rlog("[reforce] --dancer-test: unknown style \"%s\" (a style per dancer: house, pop, lock, break, "
                          "disco, stomp, hiphop, jazz, ballet)\n", s.c_str());
            engine::DancerSpec spec = engine::generateDancer(
                st.value_or(engine::dance::Style(i % engine::dance::kStyleCount)), seed);
            if (at != std::string::npos) {
                const std::string hair = s.substr(at + 1, at2 == std::string::npos ? std::string::npos : at2 - at - 1);
                if (const auto h = engine::dance::parseHairStyle(hair)) {
                    spec.look.hairStyle = *h;
                    spec.look.hat = engine::dance::Hat::None;
                } else {
                    rlog("[reforce] --dancer-test: unknown hairstyle \"%s\"\n", hair.c_str());
                }
                if (at2 != std::string::npos) {
                    const auto at3 = s.find('@', at2 + 1);
                    const std::string hatName = s.substr(at2 + 1, at3 == std::string::npos ? std::string::npos : at3 - at2 - 1);
                    if (const auto hat = engine::dance::parseHat(hatName)) spec.look.hat = *hat;
                    else rlog("[reforce] --dancer-test: unknown hat \"%s\"\n", hatName.c_str());
                    if (at3 != std::string::npos) {  // "STYLE@HAIR@HAT@BEARD" (pop@short@none@full)
                        if (const auto b = engine::dance::parseBeard(s.substr(at3 + 1))) spec.look.beard = *b;
                        else rlog("[reforce] --dancer-test: unknown beard \"%s\"\n", s.substr(at3 + 1).c_str());
                    }
                }
            }
            startDancerSpec(std::move(spec), t, 0);
            ++i;
        }
        dancerTestsPending_.clear();
    }
    const double dt = lastDanceT_ < 0 ? 1.0 / 60.0 : std::clamp(t - lastDanceT_, 0.0, 0.1);
    lastDanceT_ = t;
    // Max dancers lowered: the oldest characters wave goodbye until it fits (guests don't count against it).
    {
        int onStage = int(std::count_if(liveDancers_.begin(), liveDancers_.end(),
                                        [](const auto& d) { return !d->leaving() && d->spec().guestId.empty(); }));
        for (auto& d : liveDancers_) {
            if (onStage <= std::clamp(danceMaxLive_, 1, kStageCap)) break;
            if (d->leaving() || !d->spec().guestId.empty()) continue;
            rlog("[reforce] Dancer \"%s\" leaves (max dancers %d)\n", d->name().c_str(), danceMaxLive_);
            d->leave(t, stage_.beat, 2.0);
            --onStage;
        }
    }
    for (auto it = liveDancers_.begin(); it != liveDancers_.end();) {
        if ((*it)->expired(t)) {
            rlog("[reforce] Dancer \"%s\" left the stage\n", (*it)->name().c_str());
            it = liveDancers_.erase(it);
        } else {
            ++it;
        }
    }
    const double aspect = double(opts_.width) / double(opts_.height);
    {
        const int onStage = int(std::count_if(liveDancers_.begin(), liveDancers_.end(), [](const auto& d) { return !d->leaving(); }));
        const float want = onStage > 14 ? std::max(0.6f, std::sqrt(14.0f / float(onStage))) : 1.0f;
        danceZoom_ += (want - danceZoom_) * float(std::min(1.0, dt * 1.2));
    }
    // Pulled back for a crowd: the floor reaches further back too (zoom .5: about twice as deep).
    // ... and looks down more (zoom .5: from a little above head height to about a balcony), so the rows further back
    // rise up the frame instead of all standing along the bottom.
    stage_.cam = engine::stageCamera(aspect, danceHeight_ * danceZoom_,
                                     engine::kFloorZMin - (1.0f / danceZoom_ - 1.0f) * (engine::kFloorZMax - engine::kFloorZMin),
                                     0.29 + 0.6 * (1.0 - danceZoom_));
    stage_.zoom = danceZoom_;
    const float floorTarget = danceFloor_ && !liveDancers_.empty() ? 1.0f : 0.0f;
    stage_.floor += (floorTarget - stage_.floor) * float(std::min(1.0, dt * 2.5));
    if (stage_.floor < 0.002f && floorTarget == 0) stage_.floor = 0;
    danceShow_ = danceHidden_ ? std::max(0.0f, danceShow_ - float(dt / 0.5)) : std::min(1.0f, danceShow_ + float(dt / 0.5));
    stage_.opacity = danceShow_ * danceShow_ * (3 - 2 * danceShow_);
    stage_.lighting = float(danceLighting_);
    stage_.reflection = float(danceReflection_);
    stage_.outline = float(danceOutline_);
    const int n = int(liveDancers_.size());
    stage_.bodyCount = 0;
    if (n == 0) {
        nameTags_.clear();
        stage_.shadows = 0;
        if (stage_.floor <= 0) return;
        stage_.beat = danceMusic(t, audio).beat;
        return;
    }
    const engine::dance::MusicState m = danceMusic(t, audio);
    // Who is on stage (leaving dancers keep their spot and face the audience while they wave goodbye).
    std::vector<int> act;
    for (int k = 0; k < n; ++k)
        if (!liveDancers_[std::size_t(k)]->leaving()) act.push_back(k);
    // Social mode (PLAN 5P.6): always mingle (Peter, 2026-11-21: "make mingle the only social mode I do"; the
    // formations and crew sync modes are still in engine/Dancers and its tests, not used here).
    const long segment = long(std::floor(m.bar / 8.0));
    const engine::SocialMode mode =
        engine::pickSocialMode("mingle", int(act.size()), segment, std::uint32_t(opts_.seed) ^ 0x5eed);
    if (mode != socialMode_ && !act.empty())
        rlog("[reforce] dancers: %s\n", engine::socialModeName(mode));
    socialMode_ = mode;
    std::vector<engine::StageSpot> spots;
    if (mode == engine::SocialMode::Mingle) {
        // Everyone on their own clock (engine::Mingle keeps who is with whom).
        std::vector<engine::MinglePerson> people;
        for (const int k : act) {
            const auto& d = liveDancers_[std::size_t(k)];
            // The id is the dancer's seed: unique within a run (startDancerSpec derives it from a counter, one-to-one)
            // and the same in every run, so --deterministic renders repeat (a memory address did not).
            people.push_back({std::uint64_t(d->seed()), d->x(), d->z(), d->facing(),
                              d->placed()});
        }
        spots = mingle_.plan(people, stage_.cam, m.beat, dt, std::uint32_t(opts_.seed) ^ 0x3141u);
    } else {
        mingle_.clear();
        spots = engine::planStage(int(act.size()), stage_.cam, mode, m.bar, std::uint32_t(opts_.seed) ^ 0x6d11u);
    }
    const float energy = m.locked ? 1.0f : 0.7f;  // unlocked: smaller, gentler moves
    // Nobody walks through anybody (engine::avoidEachOther / keepApart): steer before the step, separate after.
    std::vector<engine::DancerRun*> crowd;
    for (auto& d : liveDancers_) crowd.push_back(d.get());
    engine::avoidEachOther(crowd);
    for (std::size_t a = 0; a < act.size(); ++a) {
        auto& run = *liveDancers_[std::size_t(act[a])];
        const engine::StageSpot& sp = spots[a];
        run.setSlot(sp.x, sp.z);
        engine::DanceDirection dir;
        if (mode == engine::SocialMode::Show) {
            dir.facing = sp.facing;
        } else {
            if (sp.leader >= 0 && sp.leader != int(a))
                dir.leader = &liveDancers_[std::size_t(act[std::size_t(sp.leader)])]->choreographer();
            dir.mirror = sp.mirror;
            dir.react = sp.react;
            dir.facing = sp.facing;
            dir.look = sp.look;
            if (*sp.role) {
                dir.role = sp.role;
                if (sp.other >= 0) dir.role += " " + liveDancers_[std::size_t(act[std::size_t(sp.other)])]->name();
            }
        }
        run.setHuman(float(danceHuman_));
        run.choreographer().setMocap(float(danceMocap_));
        run.choreographer().tryMove(danceTry_);
        run.step(t, dt, m, energy * std::clamp(sp.effort, 0.3f, 1.0f), dir);
    }
    for (int k = 0; k < n; ++k)
        if (liveDancers_[std::size_t(k)]->leaving()) {
            liveDancers_[std::size_t(k)]->setHuman(float(danceHuman_));
            liveDancers_[std::size_t(k)]->step(t, dt, m, energy);
        }
    engine::keepApart(crowd);
    if (int(stage_.bodies.size()) < n) stage_.bodies.resize(std::size_t(n));
    for (int k = 0; k < n; ++k) {
        auto& body = stage_.bodies[std::size_t(k)];
        body.parts.clear();
        liveDancers_[std::size_t(k)]->build(body);
    }
    stage_.bodyCount = n;
    stepNameTags(dt);
    stage_.shadows = 1;
    stage_.beat = m.beat;
    const auto twoBars = std::uint32_t(std::max(0.0, std::floor(m.bar / 2)));
    stage_.pattern = int((twoBars * 2654435761U) >> 29) & 3;
}

void Visualizer::stepNameTags(double dt) {
    // After the bodies are built: stage_.bodies[k] is liveDancers_[k].
    for (NameTag& g : nameTags_) g.seen = false;
    // Bubbles end after kBubbleSeconds, or with their dancer.
    const double now = std::max(0.0, lastDanceT_);
    guestBubbles_.erase(std::remove_if(guestBubbles_.begin(), guestBubbles_.end(),
                                       [&](const GuestBubble& b) {
                                           return now - b.start > kBubbleSeconds ||
                                                  std::none_of(liveDancers_.begin(), liveDancers_.end(),
                                                               [&](const auto& d) { return d.get() == b.run; });
                                       }),
                        guestBubbles_.end());
    {
        for (std::size_t k = 0; k < liveDancers_.size(); ++k) {
            const engine::DancerRun& d = *liveDancers_[k];
            // Name tags (Peter, 2026-11-21): stream guests always (their file sets Name), generated dancers never.
            const std::string& text = d.spec().tag;
            auto bub = std::find_if(guestBubbles_.begin(), guestBubbles_.end(), [&](const GuestBubble& b) { return b.run == &d; });
            if (text.empty() && bub == guestBubbles_.end()) continue;
            auto it = std::find_if(nameTags_.begin(), nameTags_.end(), [&](const NameTag& g) { return g.run == &d; });
            if (it == nameTags_.end()) {
                NameTag g;
                g.run = &d;
                nameTags_.push_back(g);
                it = nameTags_.end() - 1;
            }
            NameTag& g = *it;
            g.seen = true;
            g.text = text;
            g.bubble = bub != guestBubbles_.end() ? bub->text : std::string();
            g.bubbleAge = bub != guestBubbles_.end() ? now - bub->start : 0.0;
            g.scale = d.scale();
            const engine::dance::BodyOut& body = stage_.bodies[k];
            // Just above the top of the head, hair or hat, at the dancer's full size (the pop scales the tag).
            const float k1 = std::max(d.scale(), 0.05f);
            const float top = (body.headTop - body.rootFloor.y) / k1 + 0.07f;
            if (!g.placed) g.headY = top;
            g.headY += (top - g.headY) * float(1.0 - std::exp(-dt / 0.6));
            g.x = body.rootFloor.x;
            g.z = body.rootFloor.z;
            g.y = body.rootFloor.y + g.headY * std::min(k1, 1.0f);
            g.placed = true;
        }
    }
    nameTags_.erase(std::remove_if(nameTags_.begin(), nameTags_.end(), [](const NameTag& g) { return !g.seen; }),
                    nameTags_.end());
}

void Visualizer::encodeNameTags(MTL::CommandBuffer* cmd, MTL::Texture* output) {
    if (!tags_ || !output) return;
    const double now = sync::DeckFeed::now();
    const double dt = tagsLastT_ < 0 ? 0 : std::clamp(now - tagsLastT_, 0.0, 0.25);
    tagsLastT_ = now;
    const double W = double(output->width()), H = double(output->height());
    const double s = H / 1080.0;
    // One text slot per name: a name keeps its slot while it is on stage (laid out once).
    std::vector<int> slotOf(nameTags_.size(), -1);
    std::vector<bool> used(std::size_t(tags_->slots()), false);
    for (std::size_t i = 0; i < nameTags_.size(); ++i)
        for (int sl = 0; sl < tags_->slots() && !nameTags_[i].text.empty(); ++sl)
            if (!used[std::size_t(sl)] && tagSlotText_[std::size_t(sl)] == nameTags_[i].text) {
                slotOf[i] = sl;
                used[std::size_t(sl)] = true;
                break;
            }
    for (std::size_t i = 0; i < nameTags_.size(); ++i) {
        if (slotOf[i] >= 0 || nameTags_[i].text.empty()) continue;
        for (int sl = 0; sl < tags_->slots(); ++sl)
            if (!used[std::size_t(sl)]) {
                slotOf[i] = sl;
                used[std::size_t(sl)] = true;
                break;
            }
    }
    std::vector<render::TextOverlay::Draw> draws;
    std::vector<double> topOf(nameTags_.size(), -1);  // where each bubble's tail ends: the top of the tag (or the head)
    for (std::size_t i = 0; i < nameTags_.size(); ++i) {
        const NameTag& g = nameTags_[i];
        const int sl = slotOf[i];
        if (g.scale <= 0.02f) continue;
        const engine::dance::V3 head = engine::projectStage(stage_.cam, {g.x, g.y, g.z});
        if (std::isfinite(head.y)) topOf[i] = head.y * H;
        if (sl < 0) continue;
        {
            // A little smaller when the camera has pulled back for a crowd, in whole pixels: set() is a no-op while
            // the text and size stay the same, so a tag is laid out again only when either changes.
            render::TextBlockSpec spec;
            spec.lines = {{g.text, std::round(34 * s * (0.6 + 0.4 * stage_.zoom)), true, 0, 0}};
            spec.maxWidth = 420 * s;
            tags_->set(sl, spec);
            tagSlotText_[std::size_t(sl)] = g.text;
        }
        const double w = tags_->width(sl), h = tags_->height(sl);
        if (w <= 0) continue;  // still being laid out
        const engine::dance::V3 p = engine::projectStage(stage_.cam, {g.x, g.y, g.z});
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) continue;
        render::TextOverlay::Draw d;
        d.slot = sl;
        d.x = p.x * W - w / 2;
        d.y = std::max(4 * s, p.y * H - h);
        d.scale = std::clamp(double(g.scale), 0.0, 1.2);
        d.opacity = std::clamp(double(g.scale) * 1.5, 0.0, 1.0) * stage_.opacity;
        draws.push_back(d);
        topOf[i] = d.y;
    }
    // Always a soft dark shadow behind the white letters (strongest over bright pictures).
    render::TextOverlay::Shadow shadow;
    shadow.strength = 0.9;
    shadow.from = 0.0;
    tags_->encode(cmd, output, draws.data(), int(draws.size()), shadow, dt);

    // Speech bubbles, above the tags: one slot per message text while it shows.
    if (!bubbles_) return;
    std::vector<int> bslot(nameTags_.size(), -1);
    std::vector<bool> bused(std::size_t(bubbles_->slots()), false);
    for (int pass = 0; pass < 2; ++pass)
        for (std::size_t i = 0; i < nameTags_.size(); ++i) {
            if (nameTags_[i].bubble.empty() || bslot[i] >= 0) continue;
            for (int sl = 0; sl < bubbles_->slots(); ++sl)
                if (!bused[std::size_t(sl)] && (pass == 1 || bubbleSlotText_[std::size_t(sl)] == nameTags_[i].bubble)) {
                    bslot[i] = sl;
                    bused[std::size_t(sl)] = true;
                    break;
                }
        }
    std::vector<render::TextOverlay::Draw> bdraws;
    for (std::size_t i = 0; i < nameTags_.size(); ++i) {
        const NameTag& g = nameTags_[i];
        const int sl = bslot[i];
        if (sl < 0 || topOf[i] < 0 || g.scale <= 0.02f) continue;
        render::TextBlockSpec spec;
        spec.lines = {{g.bubble, std::round(30 * s * (0.6 + 0.4 * stage_.zoom)), true, 0, 0}};
        spec.maxWidth = std::round(380 * s);
        spec.bubble = true;
        bubbles_->set(sl, spec);
        bubbleSlotText_[std::size_t(sl)] = g.bubble;
        const double w = bubbles_->width(sl), h = bubbles_->height(sl);
        if (w <= 0) continue;
        const engine::dance::V3 head = engine::projectStage(stage_.cam, {g.x, g.y, g.z});
        if (!std::isfinite(head.x)) continue;
        const double age = g.bubbleAge;
        render::TextOverlay::Draw d;
        d.slot = sl;
        d.image = true;
        d.x = std::clamp(head.x * W - w / 2, 6 * s, std::max(6 * s, W - w - 6 * s));
        d.y = std::max(4 * s, topOf[i] - h - 2 * s);
        // Pops in (a little overshoot), fades out over the last second.
        const double in = std::clamp(age / 0.3, 0.0, 1.0);
        const double c1 = 1.70158, c3 = c1 + 1, k = in - 1;
        d.scale = std::clamp(double(g.scale), 0.0, 1.0) * (0.4 + 0.6 * (1 + c3 * k * k * k + c1 * k * k));
        d.opacity = std::min(std::clamp(age / 0.15, 0.0, 1.0), std::clamp(kBubbleSeconds - age, 0.0, 1.0)) *
                    std::clamp(double(g.scale) * 1.5, 0.0, 1.0) * stage_.opacity;
        bdraws.push_back(d);
    }
    render::TextOverlay::Shadow none;
    bubbles_->encode(cmd, output, bdraws.data(), int(bdraws.size()), none, dt);
}

}  // namespace reforce::app

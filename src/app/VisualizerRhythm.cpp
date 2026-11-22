// Visualizer: beat clock, phrase lines from deck starts, kick zoom, beat settings, the now-playing overlay.
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

void Visualizer::stepBeatClock(double t, const audio::AudioFrame* audio) {
    beatClockDt_ = beatClockT_ < 0 ? 0.0 : std::max(0.0, t - beatClockT_);
    beatClockT_ = t;
    sync::BeatClockConfig cfg = beatClock_.config();
    if (cfg.source != opts_.prefs.beatClockSource || cfg.audioOffsetMs != opts_.prefs.beatClockAudioOffsetMs ||
        cfg.steady != opts_.prefs.beatClockSteadyTempo || cfg.outputOffsetMs != beatOffsetMs()) {
        cfg.outputOffsetMs = beatOffsetMs();
        cfg.source = opts_.prefs.beatClockSource;
        cfg.audioOffsetMs = opts_.prefs.beatClockAudioOffsetMs;
        cfg.steady = opts_.prefs.beatClockSteadyTempo;
        beatClock_.setConfig(cfg);
    }
    sync::BeatClockInput in;
    if (audio) {
        in.linkOn = audio->linkOn;
        in.linkBpm = audio->linkBpm;
        in.linkBeat = audio->linkBeat;
        in.linkQuantum = audio->linkQuantum;
        // With a Link session BEAT/TEMPO may follow Link (reforce.Link.DriveBeat); the clock uses Link then anyway.
        in.audioTempo = audio->tempo;
        in.audioBeat = audio->beat;
        in.audioStrength = audio->beatStrength;
        in.audioLowShare = audio->beatLowShare;
        in.audioKick = audio->kickPresent;
        in.audioKickness = audio->beatKickness;
        in.audioBeatness = audio->beatBeatness;
        in.kickPhase = audio->kickPhase;
        in.kickPhaseLock = audio->kickPhaseLock;
        in.foldPhase = audio->beatFoldPhase;
        in.foldConf = audio->beatFoldConf;
        in.filterPhase = audio->beatFilterPhase;
        in.filterConf = audio->beatFilterConf;
        lastAudioTempo_ = audio->tempo;
        lastKick_ = audio->kickPresent;
        const bool changed = (lastAudioTempo_ > 0) != (loggedAudioTempo_ > 0) ||
                             (lastAudioTempo_ > 0 && std::abs(lastAudioTempo_ / loggedAudioTempo_ - 1.0) > 0.03);
        if (changed) {
            rlog("[reforce] audio tempo %.1f -> %.1f BPM%s\n", loggedAudioTempo_, lastAudioTempo_,
                 lastKick_ ? "" : " (no kick)");
            loggedAudioTempo_ = lastAudioTempo_;
        }
    }
    const sync::BeatSource before = beatClock_.state().source;
    const sync::BeatClockState& s = beatClock_.update(beatClockDt_, in);
    if (lightsOut_ && lights_.enabled && lights_.pulse > 0) lightsOut_->setBeat(s.beat, s.bpm, s.confident);
    if (s.source != before)
        rlog("[reforce] beat clock: %s%s\n", sync::beatSourceName(s.source),
             s.source == sync::BeatSource::None ? "" : (" at " + std::to_string(int(std::lround(s.bpm))) + " BPM").c_str());
    // Phrases (PLAN 5O.1): loudness = the mean of LEVEL and BASS (a drop is mostly bass).
    const std::string howBefore = phrase_.state().how;
    const double originBefore = phrase_.state().origin;
    const sync::PhraseState& ps = phrase_.update(s, audio ? 0.5 * (audio->level + audio->bass) : 0.0);
    if (ps.how == "auto" && (howBefore != "auto" || ps.origin != originBefore))
        rlog("[reforce] phrase: lines found at beat %.0f of %d (confidence %.2f)\n", ps.origin,
             int(std::lround(ps.bars * ps.quantum)), ps.confidence);
    stepDeckStarts(s);
    // Drops (PLAN 5O.5): a drop most likely starts a phrase.
    drop_.update(beatClockDt_, s, audio ? audio->bass : 0.0);
    if (drop_.dropped()) {
        dropPending_ = true;
        phrase_.hint(drop_.state().lastDropBeat, 2.0);
        rlog("[reforce] drop #%d at %.1f s (beat %.1f)\n", drop_.state().drops, beatClockT_, drop_.state().lastDropBeat);
    }
    stepKick(audio);
}

void Visualizer::stepDeckStarts(const sync::BeatClockState& s) {
    // Deck starts (PLAN 5O.8): the song just brought in started on a phrase line.
    if (!deckFeed_) return;
    deckFeed_->drain(deckStarts_);
    // A jump moves the master deck's bar phase and rkbx_link then moves Link's timeline to match; that arrives
    // after the jump's frame, so a start is placed only once Link has settled.
    const double now = sync::DeckFeed::now();
    {
        std::array<sync::NowPlayingDeck, sync::NowPlaying::kDecks> decks;
        for (int i = 0; i < sync::NowPlaying::kDecks; ++i) {
            decks[std::size_t(i)].track = deckFeed_->track(i);
            decks[std::size_t(i)].level = deckStarts_.level(i);
        }
        const sync::NowPlayingText before = nowPlaying_.text();
        nowPlaying_.update(now, decks, deckFeed_->masterDeck(), deckStarts_.fadersSeen());
        const sync::NowPlayingText& t = nowPlaying_.text();
        if (!(t == before)) {
            if (t.show != before.show || t.artist != before.artist || t.title != before.title)
                rlog("[reforce] now playing: %s\n", t.show ? ("deck " + std::to_string(t.deck + 1) + " " + t.artist +
                                                             " - " + t.title + (t.year.empty() ? "" : " (" + t.year + ")"))
                                                                .c_str()
                                                       : "(none)");
            if (t.vocal != before.vocal || t.vocalTitle != before.vocalTitle)
                rlog("[reforce] now playing vocal: %s\n",
                     t.vocal ? ("deck " + std::to_string(t.vocalDeck + 1) + " " + t.vocalArtist + " - " + t.vocalTitle).c_str()
                             : "(none)");
        }
    }
    for (const sync::DeckCommit& c : deckStarts_.take()) deckPending_.emplace_back(now + 0.4, c);
    std::vector<sync::DeckCommit> due;
    for (auto it = deckPending_.begin(); it != deckPending_.end();) {
        if (it->first <= now) {
            due.push_back(it->second);
            it = deckPending_.erase(it);
        } else {
            ++it;
        }
    }
    for (const sync::DeckCommit& c : due) {
        if (!deckStartsOn_ || !phrase_.state().on) continue;
        // Beats since the start as of now (the commit is as of its last rkbx frame).
        const double elapsed = c.elapsed + std::max(0.0, sync::DeckFeed::now() - c.at) * c.bpm / 60.0;
        const double before = phrase_.state().origin;
        phrase_.startAt(s.beat - elapsed, c.deck);
        rlog("[reforce] phrase: deck %d %s, %.1f beats ago -> phrase line at beat %.0f of %d (was %.0f, %s)\n",
             c.deck + 1, c.why.c_str(), elapsed, phrase_.state().origin,
             int(std::lround(phrase_.state().bars * phrase_.state().quantum)), before,
             sync::beatSourceName(s.source));
    }
}

void Visualizer::stepKick(const audio::AudioFrame* audio) {
    // Kick zoom (PLAN 5O.3): a pulse exp(-age / decay) after every beat of the beat clock when it is confident
    // (exactly on the grid, with the beat offset), else after every detected onset (BEAT).
    const sync::BeatClockState& s = beatClock_.state();
    const double decay = std::max(0.02, kickDecayMs_ / 1000.0);
    double pulse = 0;
    if (s.confident && s.bpm > 0) {
        const double age = (s.beat - std::floor(s.beat)) * 60.0 / s.bpm;
        pulse = std::exp(-age / decay);
    } else if (audio && audio->beat > 0) {
        pulse = std::pow(std::clamp(double(audio->beat), 0.0, 1.0), 0.12 / decay);  // BEAT decays over 0.12 s
    }
    kick_ = pulse;
    const double zoom = kickZoom_ * pulse;
    const double rot = kickRotateDeg_ * pulse * 3.141592653589793 / 180.0;
    pipeline_->setKick(float(zoom), float(rot));
}

std::string Visualizer::setBeatParam(const std::string& key, const std::string& value) {
    if (auto e = engine::validateBeatSetting(key, value); !e.empty()) return e;
    const double num = std::strtod(value.c_str(), nullptr);
    sync::PhraseConfig pc = phrase_.config();
    if (key == "phrasenow") {
        if (!phrase_.state().on) return "phrasenow: no beat clock";
        phrase_.startNow();
        rlog("[reforce] phrase: starts now (beat %.0f of the phrase)\n", phrase_.state().origin);
    } else if (key == "phrasenudge") {
        phrase_.nudge(int(std::lround(num)));
        rlog("[reforce] phrase: moved %+d bar(s)\n", int(std::lround(num)));
    } else if (key == "phrasebars") {
        pc.bars = int(std::lround(num));
        phrase_.setConfig(pc);
    } else if (key == "phrasedecks") {
        deckStartsOn_ = value == "on" || value == "1";
    } else if (key == "phraseauto") {
        pc.autoAlign = value == "on" || value == "1";
        phrase_.setConfig(pc);
    } else if (key == "kickzoom") {
        kickZoom_ = num;
    } else if (key == "kickrotate") {
        kickRotateDeg_ = num;
    } else if (key == "kickdecay") {
        kickDecayMs_ = num;
    } else if (key == "nowplaying") {
        nowPlayingOn_ = value == "on" || value == "1";
    } else if (key == "nowplayingsize") {
        nowPlayingSize_ = num;
    } else if (key == "nowplayingcorner") {
        nowPlayingCorner_ = value;
    } else if (key == "nowplayingshadow") {
        npShadow_.strength = num;
    } else if (key == "nowplayingshadowfrom") {
        npShadow_.from = num;
    } else if (key == "nowplayingswitch") {
        sync::NowPlayingConfig nc = nowPlaying_.config();
        nc.switchSeconds = num;
        nowPlaying_.setConfig(nc);
    } else if (key == "dancefloor") {
        danceFloor_ = value == "on" || value == "1";
    } else if (key == "guests") {
        guestsOn_ = value == "on" || value == "1";
        if (guestFeed_) guestFeed_->configure(guestsOn_, guestsUrl_, 5.0, guestsKey_);
        rlog("[reforce] stream guests %s\n", guestsOn_ ? (guestFeed_ ? "on" : "on (but not polling: offline / test instance)") : "off");
    } else if (key == "dancerleave") {
        for (auto& d : liveDancers_)
            if (d->seed() == std::uint32_t(num) && !d->leaving()) {
                rlog("[reforce] Dancer \"%s\" sent off\n", d->name().c_str());
                d->leave(std::max(0.0, lastDanceT_), stage_.beat, 2.0);
            }
    } else if (key == "guestsblocked") {
        setGuestsBlocked(value == "none" ? std::string() : value);
    } else if (key == "dancesay") {  // NAME=TEXT: a speech bubble over that dancer, like a guest's message
        const auto eq = value.find('=');
        const std::string who = value.substr(0, eq), text = value.substr(eq + 1);
        for (auto& d : liveDancers_)
            if (!d->leaving() && d->name() == who) {
                const engine::DancerRun* r = d.get();
                guestBubbles_.erase(std::remove_if(guestBubbles_.begin(), guestBubbles_.end(),
                                                   [&](const GuestBubble& b) { return b.run == r; }),
                                    guestBubbles_.end());
                guestBubbles_.push_back({r, text, std::max(0.0, lastDanceT_)});
                break;
            }
    } else if (key == "guestcall") {  // ID or the saved name: a past guest on stage until cleared
        if (!callGuest(value, std::max(0.0, lastDanceT_))) return "guestcall: no past guest \"" + value + "\" here";
    } else if (key == "dancehide") {
        danceHidden_ = value == "on" || value == "1";
        rlog("[reforce] dancers %s\n", danceHidden_ ? "hidden (still dancing behind the scenes)" : "shown");
    } else if (key == "guestminutes") {
        guestsMinutes_ = num;
    } else if (key == "dancerandom") {
        danceRandom_ = engine::normalizeRandomStyles(value).value_or("all");
    } else if (key == "dancelight") {
        danceLighting_ = num;
    } else if (key == "dancereflect") {
        danceReflection_ = num;
    } else if (key == "danceoutline") {
        danceOutline_ = num;
    } else if (key == "danceheight") {
        danceHeight_ = num;
    } else if (key == "dancehuman") {
        danceHuman_ = num;
    } else if (key == "dancemocap") {
        danceMocap_ = num;
    } else if (key == "dancetry") {
        const int clip = value == "off" ? -1 : engine::dance::findClip(value);
        if (value != "off" && clip < 0) return "dancetry: no mocap move \"" + value + "\"";
        danceTry_ = clip < 0 ? -1 : engine::dance::clipInfo(clip).move;
        rlog("[reforce] dancers %s\n", clip < 0 ? "back to their own moves" : ("trying " + engine::dance::clipInfo(clip).label).c_str());
    } else if (key == "dancemax") {
        danceMaxLive_ = std::clamp(int(std::lround(num)), 1, 32);
    }
    return {};
}

void Visualizer::rhythmState(engine::RhythmState& r) const {
    const sync::PhraseState& p = phrase_.state();
    r.phraseOn = p.on;
    r.bars = p.bars;
    r.quantum = p.quantum;
    r.phrase = p.phrase;
    r.barInPhrase = p.barInPhrase;
    r.bpm = beatClock_.state().bpm;
    r.how = p.how;
    r.deck = p.deck;
    r.deckStarts = deckStartsOn_;
    if (deckFeed_) {
        const bool rkbx = deckFeed_->rkbxActive();
        const std::string midi = deckFeed_->midiSources();
        r.decks = rkbx ? deckStarts_.summary() : std::string();
        r.deckInputs = std::string(rkbx ? "rkbx_link ✓" : "no rkbx_link") + " · " +
                       (midi.empty() ? std::string("no controller MIDI") : "MIDI " + midi);
    }
    r.confidence = p.confidence;
    r.autoAlign = phrase_.config().autoAlign;
    r.kickZoom = kickZoom_;
    r.danceFloor = danceFloor_;
    r.danceRandom = danceRandom_;
    r.danceHidden = danceHidden_;
    r.guests = guestsOn_;
    r.guestMinutes = guestsMinutes_;
    r.guestStatus = !guestsOn_            ? std::string()
                    : guestsUrl_.empty()  ? "no guest service set (reforce.Guests.Url)"
                    : guestFeed_          ? guestFeed_->status()
                                          : "not here (offline / test copy)";
    r.guestArrivals = guestFeed_ ? guestFeed_->arrivals() : 0;
    r.danceMode = liveDancers_.empty() ? "" : engine::socialModeName(socialMode_);
    r.danceLighting = danceLighting_;
    r.danceReflection = danceReflection_;
    r.danceOutline = danceOutline_;
    r.danceHeight = danceHeight_;
    r.danceHuman = danceHuman_;
    r.danceMocap = danceMocap_;
    r.danceTry.clear();
    if (danceTry_ >= 0)
        for (int i = 0; i < engine::dance::clipCount(); ++i)
            if (engine::dance::clipInfo(i).move == danceTry_) r.danceTry = engine::dance::clipInfo(i).id;
    r.danceMax = danceMaxLive_;
    r.danceRuns.clear();
    for (const auto& d : liveDancers_)
        r.danceRuns.push_back({d->seed(), d->name(), d->spec().guestId, d->leaving()});
    r.guestBook.clear();
    if (guestBook_)
        for (const auto& g : guestBook_->guests())
            if (std::find(guestsBlocked_.begin(), guestsBlocked_.end(), g.id) == guestsBlocked_.end())
                r.guestBook.push_back({g.id, g.label, g.style, double(g.seen)});
    r.guestsBlocked.clear();
    for (const auto& id : guestsBlocked_) {
        auto it = guestNames_.find(id);
        r.guestsBlocked.emplace_back(id, it == guestNames_.end() ? std::string() : it->second);
    }
    r.dancing.clear();
    for (const auto& d : liveDancers_)
        r.dancing.push_back(d->name() + " (" + engine::dance::styleName(d->spec().style) + ")" +
                            (d->leaving() ? ": leaving" : ": " + d->describe(stage_.beat)));
    r.kickRotate = kickRotateDeg_;
    r.kickDecayMs = kickDecayMs_;
    r.nowPlaying = nowPlayingOn_;
    r.nowPlayingSize = nowPlayingSize_;
    r.nowPlayingCorner = nowPlayingCorner_;
    r.nowPlayingSwitch = nowPlaying_.config().switchSeconds;
    r.nowPlayingShadow = npShadow_.strength;
    r.nowPlayingShadowFrom = npShadow_.from;
    {
        const sync::NowPlayingText& t = nowPlaying_.text();
        std::string line;
        if (t.show) {
            line = "deck " + std::to_string(t.deck + 1) + " · " + t.artist + " – " + t.title;
            if (!t.year.empty()) line += " · " + t.year;
        }
        if (t.vocal)
            line += std::string(line.empty() ? "" : "  +  ") + "vocal: deck " + std::to_string(t.vocalDeck + 1) + " · " +
                    t.vocalArtist + " – " + t.vocalTitle;
        r.nowPlayingLine = line;
    }
    r.kick = kick_;
    r.breakdown = drop_.state().breakdown;
    r.build = drop_.state().build;
    r.drop = drop_.state().drop;
    r.drops = drop_.state().drops;
}

std::string Visualizer::beatSummary() const {
    const sync::BeatClockState& s = beatClock_.state();
    char b[160];
    std::snprintf(b, sizeof b, "%s %.1f BPM %s (lock %.2f), %s, TEMPO %.1f, offset %+.0f ms",
                  sync::beatSourceName(s.source), s.bpm, s.confident ? "locked" : "not locked", s.lock,
                  lastKick_ ? "kick" : "no kick", lastAudioTempo_, beatOffsetMs());
    return b;
}

scene::SyncInput Visualizer::syncInput() const {
    const sync::BeatClockState& s = beatClock_.state();
    scene::SyncInput in;
    if (s.source == sync::BeatSource::None) return in;
    in.on = s.confident ? 1.0 : 0.0;
    in.bpm = s.bpm;
    in.beat = s.beat;
    in.lock = s.lock;
    in.bar = phrase_.state().bar;
    in.phrase = phrase_.state().phrase;
    in.build = drop_.state().build;
    in.drop = drop_.state().drop;
    return in;
}

void Visualizer::encodeNowPlaying(MTL::CommandBuffer* cmd, MTL::Texture* output) {
    // history_server's /now page, at 1920 x 1080: 48 px type, artist 1.2 em bold, title 1.1 em, year 0.9 em, all
    // uppercase, 20 px from the corner; a vocal above it, 40 px further in and 48 px up. Fades in over 0.5 s.
    // A soft dark shadow shows only where the picture behind the text is bright (TextOverlay::Shadow).
    if (!text_ || !output) return;
    const double now = sync::DeckFeed::now();
    const double dt = npLastT_ < 0 ? 0 : std::clamp(now - npLastT_, 0.0, 0.25);
    npLastT_ = now;
    const double H = double(output->height()), W = double(output->width());
    const double s = H / 1080.0 * nowPlayingSize_ / 100.0;
    const double em = 48 * s, pad = 20 * s;
    const sync::NowPlayingText& t = nowPlaying_.text();
    const bool live = nowPlayingOn_ && deckFeed_ != nullptr;
    auto spec = [&](std::vector<render::TextLine> lines) {
        render::TextBlockSpec b;
        b.lines = std::move(lines);
        b.maxWidth = std::max(100.0, W - 2 * pad - 40 * s);
        b.uppercase = true;
        return b;
    };
    auto step = [&](TextFade& f, int slot, bool on, const render::TextBlockSpec& want) {
        if (on && !(f.on && f.spec == want)) {
            f.alpha = 0;  // new text: in from nothing (history_server swaps, then fades in)
            f.spec = want;
            text_->set(slot, want);
        }
        f.on = on;
        const double rate = dt / 0.5;
        f.alpha = on ? std::min(1.0, f.alpha + rate) : std::max(0.0, f.alpha - rate);
    };
    step(npMain_, 0, live && t.show,
         spec({{t.artist, 1.2 * em, true, 0, 0}, {t.title, 1.1 * em, false, 4 * s, 4 * s},
               {t.year, 0.9 * em, false, 4 * s, 4 * s}}));
    step(npVocal_, 1, live && t.vocal,
         spec({{t.vocalArtist, 1.2 * em, true, 2 * s, 2 * s}, {t.vocalTitle, 1.1 * em, false, 2 * s, 2 * s}}));
    if (npMain_.alpha <= 0 && npVocal_.alpha <= 0) return;
    const bool right = nowPlayingCorner_ == "br" || nowPlayingCorner_ == "tr";
    const bool top = nowPlayingCorner_ == "tl" || nowPlayingCorner_ == "tr";
    const double mw = text_->width(0), mh = npMain_.alpha > 0 ? text_->height(0) : 0;
    const double vw = text_->width(1), vh = text_->height(1);
    render::TextOverlay::Draw d[2];
    d[0].slot = 0;
    d[0].opacity = npMain_.alpha;
    d[0].x = right ? W - pad - mw : pad;
    d[1].slot = 1;
    d[1].opacity = npVocal_.alpha;
    d[1].x = right ? W - pad - 40 * s - vw : pad + 40 * s;
    if (top) {  // vocal below the main block
        d[0].y = pad;
        d[1].y = pad + (mh > 0 ? mh + 48 * s : 0);
    } else {    // vocal above it
        d[0].y = H - pad - mh;
        d[1].y = H - pad - (mh > 0 ? mh + 48 * s : 0) - vh;
    }
    text_->encode(cmd, output, d, 2, npShadow_, dt);
}

}  // namespace reforce::app

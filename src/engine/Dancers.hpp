#pragma once
// reforce extension (PLAN 5P, docs/dancers-design.md): live dancers on the stage. Pure C++ (no Metal).
//
//   - StageCamera: the fixed perspective camera looking slightly down at the stage line near the bottom of the frame.
//   - stageSlots: where n dancers stand (spread across the frame, alternate ones a step back when there are 3+).
//   - DancerRun: one live dancer: its look, skeleton and choreographer, the entrance pop (a hop in, scale with an
//     overshoot, waving hello), gliding to its slot, the lifetime, and the exit (a goodbye wave, then shrinking away).
//   - DancerSpec / dancerSpec(): a character from a reforce.Dancer.package scene file.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/DanceClips.hpp"
#include "engine/DanceMoves.hpp"
#include "engine/DancerRig.hpp"
#include "engine/Meshes.hpp"

namespace reforce::scene {
class CompiledConfig;
}

namespace reforce::engine {

inline constexpr float kFloorZMin = -0.75f, kFloorZMax = 0.3f;  // where dancers may stand (stage z), by default

struct StageCamera {
    Mat4 viewProj;
    dance::V3 eye;
    double fovY = 28.0;    // degrees
    double distance = 0;   // eye to the stage line (z = 0)
    double halfWidth = 0;  // half the visible width at z = 0 (stage units)
    float proj11 = 1;      // projection y scale (for pixel-sized outlines)
    // The back of where dancers may stand (stage z): further back when the camera pulls back for a crowd, so a
    // big crowd spreads over more rows. The front is always kFloorZMax.
    float floorBack = kFloorZMin;
};
// `height`: dancer height as a fraction of the frame height (reforce.Dancer.Height).
// `floorBack`: StageCamera::floorBack. `eyeHeight`: the eye's height over its distance (0.29: looking slightly down;
// higher looks down more, so rows further back rise up the frame: a crowd).
StageCamera stageCamera(double aspect, double height = 0.42, float floorBack = kFloorZMin, double eyeHeight = 0.29);
// How much wider the visible floor is at stage depth z than at z = 0 (perspective: further back shows more width).
inline float floorWiden(const StageCamera& cam, float z) { return std::max(0.5f, (cam.eye.z - z) / std::max(cam.eye.z, 0.1f)); }
// Screen position (0..1, origin top-left) of a stage point; z = depth 0..1 (NaN behind the camera).
dance::V3 projectStage(const StageCamera& cam, dance::V3 p);
// Stage spots (x, z) for n dancers.
std::vector<std::pair<float, float>> stageSlots(int n, const StageCamera& cam);

// Where the stage puts a dancer this step and what it asks of them (planned by engine::Mingle, below). Mingle is the
// only social mode: the crew-wide formations (show, partners, call & response, face to face, cypher; pickSocialMode,
// planStage, scatterSpots) were removed 2026-11-27.
struct StageSpot {
    float x = 0, z = 0;
    float facing = std::nanf("");  // NaN = the audience
    float look = 0;                // extra head turn
    int leader = -1;               // index of the dancer whose routines to dance (-1 = own): partners
    bool mirror = false;
    bool react = false;            // watching `other` (DanceDirection::react)
    int other = -1;                // who this dancer is with / watching (-1 = nobody)
    const char* role = "";         // "dancing", "showing off", "with", "watching", "cheering", "wandering"...
    float effort = 1;              // size of the moves (< 1: taking it easy)
    float floorRoom = 1e30f;       // DanceDirection::floorRoom
};

// ---- mingle (Peter, 2026-11-20): "something more natural in feel where people are just milling around", not the
// whole crew switching from one formation to the next in sync. No crew-wide plan: every dancer runs their own small
// state machine on their own clock (changes land on a beat, but each on their own: after 5 beats, 11 bars...):
//   dancing on their spot | showing off (others may come over to watch and hype them) | wandering to a new spot |
//   taking a break near the edge (smaller moves, looking around) | walking over to someone free or to a group |
//   in a group: a pair (partners as mirror images, face to face, or call & response swapping on the pair's own
//   2-bar clock) or 3-4 in a horseshoe open to the audience (dancing to each other, now and then one solos and the
//   rest cheer). Members leave a group one at a time, each when their own time is up.
// One personality for everyone (Mingle::kPersonality; Peter, 2026-11-27: no hidden different personalities):
// sociability (how often they join someone, how long they stay), restlessness (how often they move on), showiness.
// What varies is each dancer's own clock and their seeded choices (what next, where to wander, how long). Glances:
// now and then a dancer looks at someone nearby (more often at someone showing off, walking past, or coming over to
// them) for a second or two.
//
// Stream guests choose how social they are (Peter, 2026-11-27; docs/guests.md "Social"):
//   Mingle  the same personality as everyone else;
//   Solo    dance on their own: never walk over to anyone or join a group, and nobody comes to join them (someone
//           who picked them to dance with dances near them, watching);
//   Crew    always with others: join the biggest group with room (anywhere on the floor), or walk over to someone,
//           and stay;
// and may pick someone to dance with (`with`): they walk over to that dancer (or the group they are in, which may
// then grow past its usual 4, up to 6) and stay with them, following them when they move on. The one picked
// stays a little longer in a group with whoever picked them.
enum class MingleVibe : std::uint8_t { Mingle, Solo, Crew };
std::optional<MingleVibe> parseMingleVibe(std::string_view s);
struct MinglePerson {
    std::uint64_t id = 0;  // stable while on stage
    float x = 0, z = 0;    // where they are now
    float facing = 0;      // body turn now
    bool placed = false;   // not yet on the floor: the first spot places them
    MingleVibe vibe = MingleVibe::Mingle;
    std::uint64_t with = 0;  // wants to dance with this person (their id; 0 = nobody in particular)
    // Busting a move (DancerRun::bustMove, asked or on): dances where they are; the others they were dancing with
    // make room and watch them (an arc open to the audience, dancing as watchers do), nobody new joins them.
    bool bustMove = false;
    // Floor work (DancerRun::floorInfo; 2026-11-27, docs/dancers-design.md "Floor moves held still, room to go
    // down"): 0 standing, 1 a floor move (or a bust) waiting for room, 2 on the floor; `reach` how far it reaches
    // (stage units, this dancer's size). Mingle holds a dancer going down or on the floor where they are (no new
    // plans, nobody walks over to them), clears `reach` round them (neighbors step back), turns them at the start so
    // the reach points into open space (mostly to the audience), and says when there is room (StageSpot::floorRoom).
    int floor = 0;
    dance::FloorReach reach = {};
    // Their dance style (dance::Style; -1 unknown). A pair dancing as partners (one mirroring the other's routine)
    // only when both dance the same style; otherwise face to face, each their own moves (Peter, 2026-11-27).
    int style = -1;
};
class Mingle {
public:
    // Spots for `people` (same order) at the music's `beat`; `dt` seconds since the last call. `seed` seeds the
    // choices of dancers new to the floor (with their id).
    std::vector<StageSpot> plan(const std::vector<MinglePerson>& people, const StageCamera& cam, double beat,
                                double dt, std::uint32_t seed = 1);
    void clear();
    // For tests: the size of the group `id` is in (0: not in one).
    int groupSize(std::uint64_t id) const;
    // For tests: whether `a` and `b` are in the same group; who `id` is watching (0: nobody).
    bool together(std::uint64_t a, std::uint64_t b) const;
    std::uint64_t watching(std::uint64_t id) const;
    // The personality (0..1 each): the same for every dancer, guests and generated ones alike (Peter, 2026-11-27:
    // "just a single generally decent personality"); about the middle of the seeded spread (0..1 each) dancers had
    // before, set so the crowd's time alone, in groups and showing off stays as it was (docs/dancers-design.md).
    struct Traits {
        float sociable, restless, showy;
    };
    static constexpr Traits kPersonality{0.45f, 0.5f, 0.6f};
    // For tests: the personality of `id` on the floor (all zero: not on it).
    Traits traits(std::uint64_t id) const;

private:
    enum class Act { Solo, Show, Wander, Rest, Seek, Watch, Group };
    enum class Kind { Partners, Facing, Call, Circle };
    struct Agent {
        std::uint64_t id = 0;
        std::uint32_t rng = 1;
        Act act = Act::Solo;
        double until = 0;           // (clock) when this activity ends; in a group: when this one leaves it
        float sx = 0, sz = 0;       // own spot (alone, wandering, resting, watching, walking over)
        float tilt = 0;             // turn towards the audience when alone
        std::uint64_t target = 0;   // walking over to / watching this person
        int targetGroup = 0;        // walking over to join this group
        int group = 0;              // in this group
        std::uint64_t approachedBy = 0;
        // Glances (seconds).
        std::uint64_t glanceAt = 0;
        double glanceUntil = 0, nextGlance = 0;
        float look = 0;
        // This frame.
        float x = 0, z = 0, facing = 0;
        bool placed = false, seen = false;
        float outX = 0, outZ = 0;   // the spot handed out last
        // Chosen by a stream guest (MinglePerson).
        MingleVibe vibe = MingleVibe::Mingle;
        std::uint64_t with = 0;
        bool bustMove = false;
        int style = -1;      // (MinglePerson::style)
        float nearSide = 0;  // dancing near a Solo dancer: the side they came from (+1 / -1), kept
        // Following their place in a group (or near someone) with their own lag, `react` seconds (Peter, 2026-11-27:
        // groups shifted in unison).
        float react = 0.6f;
        bool held = false;
        float hx = 0, hz = 0;
        // Floor work (MinglePerson::floor): where they went down and the turn they were given; the room kept round
        // them (eased: neighbors drift back as they get up).
        int floor = 0;
        dance::FloorReach reach;
        float ax = 0, az = 0, floorFacing = 0, room = 0, waitT = 0, wantT = 0;
        bool anchored = false;
        bool clearNow = false;
    };
    struct Group {
        int id = 0;
        std::vector<std::uint64_t> members;  // in their places, left to right round the horseshoe
        double start = 0;                    // (clock) the group's own beat zero: turns, kind changes
        Kind kind = Kind::Facing;
        float cx = 0, cz = 0, tilt = 0, phase = 0;
        std::uint32_t rng = 1;
        long kindBar = 0;                    // the group's 8-bar block the kind was last chosen in
        std::uint64_t floorId = 0;           // a member going down / on the floor: the rest make room and watch
    };
    Agent* find(std::uint64_t id);
    Group* findGroup(int id);
    void decide(Agent& a, const StageCamera& cam, double beat);
    // `with` / Solo / Crew (MingleVibe): true when it set what `a` does now.
    bool keepWith(Agent& a, const StageCamera& cam, double beat);
    float sociable(const Agent& a) const;
    void enter(Agent& a, Act act, double bars, double beat);
    void leaveGroup(Agent& a, double beat);
    void layout(Group& g);
    std::pair<float, float> pickSpot(Agent& a, const StageCamera& cam, bool edge);
    // Floor work: whether `a` may go down now (on their own spot, showing off, a horseshoe's soloist, busting a move).
    bool mayGoDown(const Agent& a) const;
    float pickFloorFacing(const Agent& a) const;
    float clearPad(const Agent& a) const;
    float roomHalfWidth(const StageCamera& cam) const;

    std::vector<Agent> agents_;
    std::vector<Group> groups_;
    double clock_ = 0, lastBeat_ = 0, t_ = 0;
    bool started_ = false;
    int nextGroup_ = 1;
};

// A character (scene file keys, docs/dancers-design.md).
struct DancerSpec {
    std::string name;
    std::string tag;             // Name: the name tag shown over the dancer (a stream guest's name); "" = none
    std::string guestId;         // a stream guest (web/Guests.hpp): their id; "" = a character
    dance::Look look;
    dance::Style style = dance::Style::Pop;
    float snap = -1;             // Snap (< 0: the style's)
    std::vector<int> likes;      // Moves (move indices)
    double duration = std::nan("");  // PDur
    bool random = false;         // Random=1: a new character each time it starts (generateDancer), dressed for Style
    bool feminine = false;       // generateDancer: feminine (else masculine; tests and statistics)
    std::vector<std::string> warnings;
};
// Reads a compiled Dancer config's keys (unknown values keep the defaults and add a warning). In DancerScene.cpp
// (the scene compiler is not part of the web preview build).
DancerSpec dancerSpec(const scene::CompiledConfig& cfg, const std::string& name);
// The same from any key lookup: `lookup("HAIRSTYLE")` gives the value written for HairStyle ("" when absent). The
// web preview (web/dancer-preview) reads a guest's keys with this.
DancerSpec dancerSpecFromKeys(const std::function<std::string(const char*)>& lookup, const std::string& name);
// A new character dressed for `style` (Peter, 2026-11-19: "a bunch more variety"): a first name, body proportions,
// a matched color scheme, the style's wardrobe (outfits, hats, prints, fabrics), sunglasses, glow sticks and a
// face. The same seed gives the same character.
DancerSpec generateDancer(dance::Style style, std::uint32_t seed);
// reforce.Dancer.RandomStyles: which styles the generated ("Random STYLE") characters may be picked from when the
// next dancer is chosen: "all", "none" or a comma list of styles ("disco,rave"). Named characters are not affected.
// normalizeRandomStyles: the canonical form ("all", "none" or the styles in their order, comma separated), or
// nothing when a name isn't a style.
std::optional<std::string> normalizeRandomStyles(std::string_view value);
bool randomStyleAllowed(std::string_view list, dance::Style style);

// Room to go down (docs/dancers-design.md "Floor moves held still, room to go down"): how far a neighbor's body
// center must be from a floor dancer at (0, 0) turned `facing`, towards (dx, dz): the move's reach that way plus
// `pad` (a neighbor's body and air). Mingle clears with kFloorClear, keepApart holds kFloorKeep.
inline constexpr float kFloorClear = 0.4f, kFloorKeep = 0.3f;
float floorNeed(const dance::FloorReach& r, float facing, float dx, float dz, float pad);
// Partners (a pair dancing the first one's routine mirrored): when the leader goes down, the follower makes room and
// watches, dancing as watchers do (false); true: the follower mirrors the floor move too (as before 2026-11-27).
inline constexpr bool kFollowerMirrorsFloor = false;
// The floor-move fixes one by one, for measurements only (tests/test_engine_floorslide.cpp): the spot and turn held
// on the floor, the humanising faded out there, planted contacts held. All on in the app.
struct FloorFixes {
    bool hold = true, fade = true, pins = true;
};
inline FloorFixes floorFixes;

// What the stage (engine::Mingle) asks of a dancer this step.
struct DanceDirection {
    dance::Choreographer* leader = nullptr;  // dance the leader's routines instead of this dancer's own (partners)
    bool mirror = false;                      // the leader's routines mirrored (partners)
    bool react = false;                       // watch someone else dance (watchPose)
    float facing = std::nanf("");             // body turn, radians (+ = towards +x); NaN = the audience
    float look = 0;                           // extra head turn towards whoever is being watched
    std::string role;                         // for the panel: "with Dex", "solo", "cheering Nia"...
    // Room to go down (Choreographer::setFloorRoom: the widest floor move reach that fits round the dancer now,
    // stage units; < 0 not now). Infinite: no say (as before).
    float floorRoom = 1e30f;
};

class DancerRun;

// Dancers don't walk through each other (Peter, 2026-11-19). No path planning: on an open floor with a dozen people
// local steering is enough, as in crowd simulation.
//   avoidEachOther (before the step): anticipation. For each pair that would come closer than two body radii within
//     kAvoidHorizon seconds at their current velocities, both get a push away from where they would meet, stronger
//     the sooner it is; head-on, both keep to their right so they pass instead of stalling. The one walking does most
//     of the avoiding; someone dancing on their spot gives way a little.
//   keepApart (after the step): the backstop. Any pair closer than kMinApart is pushed apart (half each, or all onto
//     the one walking), so bodies never overlap even when someone pops in on top of a passer-by.
// (Peter, 2026-11-27: arms went into neighbors; they reach about 0.48 from the body's axis. Was 0.21 / 0.38.)
inline constexpr float kBodyRadius = 0.25f;  // stage units (a dancer is about 1 tall)
inline constexpr float kMinApart = 0.46f;
inline constexpr float kAvoidHorizon = 2.0f;  // seconds
void avoidEachOther(const std::vector<DancerRun*>& runs);
void keepApart(const std::vector<DancerRun*>& runs);

class DancerRun {
public:
    // `hold`: seconds on stage before leaving by itself (<= 0: until killed). `beat`: the music's beat now.
    DancerRun(DancerSpec spec, std::uint32_t seed, double t, double beat, double hold);

    const std::string& name() const { return spec_.name; }
    const DancerSpec& spec() const { return spec_; }
    // Size now: 0 .. 1 with the entrance pop's overshoot; shrinks to 0 as it leaves. Head radius (character units).
    float scale() const { return scale_; }
    float headRadius() const { return sk_.headR; }
    std::uint32_t seed() const { return seed_; }
    double startTime() const { return start_; }
    dance::Choreographer& choreographer() { return choreo_; }
    // Bust a move (Peter, 2026-11-27; docs/dancers-design.md "Bust a move"): two whole break routines with floor work
    // from the next 8-bar point (Choreographer::bustMove), whatever their style; the stage doesn't send them over to
    // anyone meanwhile (MinglePerson::bustMove). inBustMove: asked or on at the music's `beat`.
    void bustMove() { choreo_.bustMove(32); }
    bool inBustMove(double beat) const { return choreo_.bustMoveAt(beat, true); }
    // Where it stands at the music's `beat` (Choreographer::bustPhase, on this dancer's own beat; `beats` as there).
    dance::Choreographer::BustPhase bustPhase(double beat, double* beats = nullptr) const {
        return choreo_.bustPhase(danceBeat(beat), beats);
    }

    // Where to stand. The first call places the dancer there; later ones glide.
    void setSlot(float x, float z);
    // Crowd (avoidEachOther / keepApart): position and walking velocity on the floor (stage units), an extra
    // steering acceleration for the next step, and a direct correction of the position.
    float z() const { return z_; }
    float vx() const { return vx_; }
    float vz() const { return vz_; }
    bool placed() const { return placed_; }
    void steer(float ax, float az) {
        avoidX_ += ax;
        avoidZ_ += az;
    }
    void nudge(float dx, float dz) {
        x_ += dx;
        z_ += dz;
    }
    // A new look (a stream guest changed their dancer while on stage): the body, clothes and name change in place;
    // a new dance style gets that style's moves. Keeps the seed (personality, place in the mingle) and guestId.
    void restyle(DancerSpec spec);
    // Stay `hold` seconds from `t` (a guest checking in again); <= 0 leaves the hold as it is.
    void stayFor(double t, double hold) {
        if (hold > 0 && hold_ > 0) hold_ = std::max(hold_, t - start_ + hold);
    }
    // Stay exactly `hold` more seconds from `t` (a guest who picked another stay on the page: shorter too); <= 0, or a
    // dancer staying until cleared, as it is.
    void setStay(double t, double hold) {
        if (hold > 0 && hold_ > 0) hold_ = std::max(0.0, t - start_ + hold);
    }
    // A stay the guest picked: `hold` seconds from `t`, or <= 0 until cleared (also for one brought on by hand).
    void pickStay(double t, double hold) { hold_ = hold > 0 ? std::max(1e-3, t - start_ + hold) : 0.0; }
    // When the stay runs out (scene time), or < 0: staying until cleared (or leaving already).
    double leavesAt() const { return hold_ > 0 && !leaving() ? start_ + hold_ : -1.0; }
    // Leave: a goodbye wave, then shrink away over `fade` seconds in all (at least 1.6 s).
    void leave(double t, double beat, double fade);
    bool leaving() const { return exitStart_ >= 0; }
    bool expired(double t) const { return leaving() && t >= exitStart_ + exitLen_; }

    // One step, as directed (DanceDirection; the own choreographer always does hello and goodbye). A change of
    // source (own / leader / reacting) blends over 0.35 s; turning to a new facing takes about half a second.
    void step(double t, double dt, const dance::MusicState& m, float energy, const DanceDirection& dir = {});
    float facing() const { return facing_; }
    // The body as last stepped (appends to out.parts; sets the face, head and feet).
    void build(dance::BodyOut& out) const;
    void stepFeet(double beat, double dt, float moving);
    // Floor work for the stage (MinglePerson::floor, reach): going down, on the floor, how far it reaches.
    void floorInfo(MinglePerson& p, double beat) const;
    // On the floor (0..1): the spot and the turn are held, the crowd doesn't push them (avoidEachOther, keepApart);
    // the reach of the move playing (stage units).
    float anchored() const { return floorHold_; }
    // Going down soon (a floor move or bust waiting for room, the stage holding their place): the crowd keeps out of
    // the move's reach as from someone on the floor (avoidEachOther, keepApart).
    bool clearing() const { return clearing_; }
    const dance::FloorReach& floorReachNow() const { return reachNow_; }
    std::string describe(double beat) const;
    float x() const { return x_; }
    // Humanising (PLAN 5P.8, reforce.Dancer.Human): 0 = machine-exact .. 1 = as designed .. 2 = sloppy. Each dancer
    // has a seeded personality (laid back or eager on the beat, bigger arms or smaller steps, a slouch, a head tilt,
    // one hand livelier than the other, punchy or laid back within the beat) plus drifts (timing, effort, where the
    // head looks, breathing); the arms and head move a little after the core, and the hands and head follow through
    // a little behind the body. The feet are left alone (no sliding).
    void setHuman(float amount) { humanAmt_ = std::clamp(amount, 0.0f, 2.0f); }
    // The beat this dancer's hits land on (the music's beat plus its timing lean, a slow drift and a little
    // beat-to-beat variation).
    double humanBeat(double beat) const;
    // The clock the moves run on: humanBeat with this dancer's feel within each beat (punch or lay back; the hits
    // themselves stay on humanBeat). Never runs backwards.
    double danceBeat(double beat) const;

private:
    DancerSpec spec_;
    dance::Skeleton sk_;
    dance::Choreographer choreo_;
    std::uint32_t seed_;
    double start_, hold_;
    double exitStart_ = -1, exitLen_ = 0;
    bool placed_ = false;
    float x_ = 0, z_ = 0, vx_ = 0, vz_ = 0, tx_ = 0, tz_ = 0;
    float avoidX_ = 0, avoidZ_ = 0;  // steering from the crowd for the next step
    dance::DancePose pose_;
    float scale_ = 0, lift_ = 0;
    bool usedLeader_ = false;
    std::string desc_;
    std::string sourceKey_;
    // Inertial blending (a change of source keeps its motion): the offset from the old pose and its velocity decay
    // to zero with a critically damped spring.
    float inertOff_[dance::kPoseFloats] = {}, inertVel_[dance::kPoseFloats] = {};
    float lastOut_[dance::kPoseFloats] = {}, outVel_[dance::kPoseFloats] = {};
    bool haveOut_ = false;
    // Footsteps while wandering: each foot planted on the floor (stage x, z) or swinging to its next spot.
    struct Foot {
        float x = 0, z = 0;
        bool swinging = false;
        double swingBeat = 0;
        float fromX = 0, fromZ = 0, lift = 0;
    };
    Foot feet_[2];
    // Floor work (2026-11-27, "Floor moves held still"): how much the spot and turn are held; the reach playing; each
    // planted contact (hands, feet, knees, the head: DancePose pin*) held where it came down, on the stage; the body's
    // shift that holds the knees and head (and hands out of reach).
    float floorHold_ = 0;
    bool clearing_ = false;
    dance::FloorReach reachNow_;
    struct Pin {
        bool on = false, gaveUp = false;
        float x = 0, z = 0, amt = 0;
    };
    Pin pins_[7];
    float rootX_ = 0, rootZ_ = 0;
    float legRatio_ = 1;
    void pinContacts(double dt);
    float walk_ = 0;  // 0 dancing in place .. 1 walking (feet planted / stepping)
    bool feetPlaced_ = false;
    double lastStepBeat_ = -1e9;
    int steps_ = 0;

public:
    float walking() const { return walk_; }
    // The pose as last stepped (character space; measurements: tests/test_engine_floorslide.cpp).
    const dance::DancePose& poseNow() const { return pose_; }
    int stepsTaken() const { return steps_; }
    // Planted contacts held this step (tests): how many, and the body's shift for them (stage units).
    int pinsHeld() const {
        int n = 0;
        for (const Pin& p : pins_) n += p.on && p.amt > 0.5f;
        return n;
    }
    float pinShift() const { return std::hypot(rootX_, rootZ_); }
    // Feet on the floor (stage x, z), for tests.
    std::pair<float, float> footSpot(int i) const { return {feet_[i].x, feet_[i].z}; }
    bool footSwinging(int i) const { return feet_[i].swinging; }

private:
    float facing_ = 0;
    bool facingSet_ = false;
    // Humanising: the personality (seeded once) and the follow-through springs on the hands and head.
    struct Personality {
        float lean = 0;                // beats ahead (+) / behind (-) the music
        float armGain = 1, legGain = 1, hipGain = 1, headGain = 1;
        float asym = 0;                // + the left hand moves bigger, - the right
        float slouch = 0, tilt = 0, nod = 0, weight = 0, stance = 1;
        float breath = 4;              // seconds per breath
        float handWn = 30, headWn = 16;  // follow-through stiffness (rad/s)
        // Timing (beats): the arms and head trail the core (each hand its own lag); the feel within a beat
        // (+ snaps through the hit and hangs between beats, - lands softly and hurries between).
        float armLag = 0, armLagAsym = 0, headLag = 0, punch = 0;
    };
    Personality pers_;
    float humanAmt_ = 1;
    float rest_[dance::kPoseFloats] = {};
    float follow_[dance::kPoseFloats] = {}, followVel_[dance::kPoseFloats] = {};
    bool followSet_ = false;
    void humanize(float* pose, double beat, double t) const;
    // Hair in motion: a point behind each side of the head (where hanging hair ends) on a bouncy spring, pulled
    // along by the head and partly down by gravity; its lag behind the head is the pose's hairSwing.
    struct HairTip {
        dance::V3 x{}, v{}, target{};
        bool set = false;
    };
    HairTip hair_[2];
    void stepHair(double dt);
    // Hair physics (2026-11-27, reforce.Dancer.Hair; docs/dancers-design.md "Hair physics"): each hanging piece a
    // short chain of points (stage space) on the head, bent towards its rest shape, under gravity, kept out of the
    // body; the loose volumes' lag (a shift and a turn behind the head, on springs).
    struct HairSim {
        bool defsSet = false, set = false;
        int chains = 0;
        dance::HairChainDef def[dance::kHairChains];
        float shrink[dance::kHairChains][dance::kHairColliders] = {};  // a collider shrunk for a chain (as designed)
        dance::V3 x[dance::kHairChains][dance::kHairChainPts] = {}, xp[dance::kHairChains][dance::kHairChainPts] = {};
        // The last step's head frame, placement, scale and body (stage space), to sub-step between.
        dance::Frame head;
        dance::V3 place{};
        float k = 1;
        dance::HairCollider col[dance::kHairColliders];
        // Volume lag: a point following the head center, and a frame following its turn (stage space).
        dance::V3 jp{}, jv{}, lw{};
        dance::Frame lag;
    };
    HairSim sim_;
    dance::HairState hairOut_;
    // The look's hair and hat round the head, held off the floor in floor work (made on the first floor move).
    dance::HeadExtent headExt_;
    bool headExtSet_ = false;
    float hairAmt_ = 1;  // reforce.Dancer.Hair: 0 the classic swing, 1 hair physics as designed, 2 loose
    bool longHair_ = true;  // reforce.Dancer.LongHair: long hair bends (three columns) / the rigid tube turning
    void stepHairSim(double dt);

public:
    // Hair swing (head frame, per unit length) as last stepped, for tests.
    dance::V3 hairSwing(int side) const { return pose_.hairSwing[side & 1]; }
    // reforce.Dancer.Hair: 0 = the classic swing (two springs, each hanging piece swung whole), above 0 hair physics
    // (chains, collisions, loose volumes): 1 as designed, 2 loose (softer shapes, more lag).
    void setHairMotion(float amount) { hairAmt_ = std::clamp(amount, 0.0f, 2.0f); }
    bool hairPhysics() const { return hairAmt_ > 0.001f; }
    // reforce.Dancer.LongHair (with hair physics): 1 = long hair bends along three columns (back, left, right; the
    // tube drawn bent), 0 = the tube turns whole towards one chain (as before 2026-11-27). A change starts the hair
    // again from rest.
    void setLongHair(bool bend) {
        if (bend == longHair_) return;
        longHair_ = bend;
        sim_.defsSet = sim_.set = false;
    }
    bool longHair() const { return longHair_; }
    // The hair sim's output as last stepped (character space; tests), and the body it collides with.
    const dance::HairState& hairState() const { return hairOut_; }
    void hairBody(dance::HairCollider out[dance::kHairColliders]) const;
    float hairShrink(int chain, int collider) const { return sim_.shrink[chain][collider]; }
    const dance::HairChainDef& hairChain(int chain) const { return sim_.def[chain]; }
    // The head frame as last stepped (character space).
    dance::Frame headNow() const { return dance::headFrame(sk_, pose_); }
    // Tests: step only the hair with this pose (full size, standing where the dancer stands).
    void stepHairWithPose(const dance::DancePose& p, double dt) {
        pose_ = p;
        scale_ = 1;
        stepHair(dt);
        stepHairSim(dt);
    }

private:
};

}  // namespace reforce::engine

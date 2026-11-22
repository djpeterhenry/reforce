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
#include <vector>

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

// ---- social mode (PLAN 5P.6): how the crew is arranged for an 8-bar segment.
enum class SocialMode {
    Show,     // everyone faces the audience (the crew sync setting applies)
    Partner,  // pairs face each other and dance the same routine as mirror images
    Call,     // pairs face each other; one dances, the partner watches and hypes them; swap every 2 bars
    Side,     // pairs face each other, each with their own moves
    Cypher,   // an arc facing the middle; one dancer solos in front, the others cheer; next soloist every 2 bars
    Mingle,   // everyone on their own (engine::Mingle): pairs and small groups form and break up, no crew-wide change
};
const char* socialModeName(SocialMode m);
// The mode for an 8-bar segment: `setting` = auto (social about 3/4 of the time, never two show segments in a row) |
// always (any social mode) | partners | call | facing | cypher (that one; a cypher needs 3) | off. Show for fewer than
// two dancers. Deterministic in (segment, seed). `mingle`: always Mingle (planned by engine::Mingle, not planStage).
SocialMode pickSocialMode(const std::string& setting, int dancers, long segment, std::uint32_t seed);

struct StageSpot {
    float x = 0, z = 0;
    float facing = std::nanf("");  // NaN = the audience
    float look = 0;                // extra head turn
    int leader = -1;               // index of the dancer whose routines to dance (-1 = own)
    bool mirror = false;
    bool react = false;            // watching `other` (claps, points, nods)
    int other = -1;                // who this dancer is with / watching (-1 = nobody)
    const char* role = "";         // "with", "showing", "watching", "solo", "cheering", "facing"
    float effort = 1;              // size of the moves (< 1: taking it easy)
};
// Spots for n dancers in `mode` at running bar `bar` (whose turn it is changes every 2 bars). Pairs, with an odd count
// the last three as a trio. Milling (Peter, 2026-11-18: "more like people milling around"): dancers (and pairs) stand
// at scattered spots on the floor, not in a line; each wanders to a new spot every 3-5 bars on its own schedule, the
// spots drift a little all the time, and pairs stand at a random tilt. Spots never overlap (>= minDist apart).
std::vector<StageSpot> planStage(int n, const StageCamera& cam, SocialMode mode, double bar, std::uint32_t seed = 1);
// Scattered spots for n people (or groups) at bar `bar`, at least `minDist` apart, inside the floor area.
// `people`: how many dancers share the floor (sizes the room; default n). Distance in depth counts half (two dancers
// one behind the other still overlap on screen).
std::vector<std::pair<float, float>> scatterSpots(int n, const StageCamera& cam, double bar, std::uint32_t seed,
                                                  float minDist, int people = -1);

// ---- mingle (Peter, 2026-11-20): "something more natural in feel where people are just milling around", not the
// whole crew switching from one formation to the next in sync. No crew-wide plan: every dancer runs their own small
// state machine on their own clock (changes land on a beat, but each on their own: after 5 beats, 11 bars...):
//   dancing on their spot | showing off (others may come over to watch and hype them) | wandering to a new spot |
//   taking a break near the edge (smaller moves, looking around) | walking over to someone free or to a group |
//   in a group: a pair (partners as mirror images, face to face, or call & response swapping on the pair's own
//   2-bar clock) or 3-4 in a horseshoe open to the audience (dancing to each other, now and then one solos and the
//   rest cheer). Members leave a group one at a time, each when their own time is up.
// Personality per dancer (seeded): sociability (how often they join someone, how long they stay), restlessness
// (how often they move on), showiness. Glances: now and then a dancer looks at someone nearby (more often at
// someone showing off, walking past, or coming over to them) for a second or two.
struct MinglePerson {
    std::uint64_t id = 0;  // stable while on stage
    float x = 0, z = 0;    // where they are now
    float facing = 0;      // body turn now
    bool placed = false;   // not yet on the floor: the first spot places them
};
class Mingle {
public:
    // Spots for `people` (same order) at the music's `beat`; `dt` seconds since the last call. `seed` sets the
    // personality of dancers new to the floor.
    std::vector<StageSpot> plan(const std::vector<MinglePerson>& people, const StageCamera& cam, double beat,
                                double dt, std::uint32_t seed = 1);
    void clear();
    // For tests: the size of the group `id` is in (0: not in one) and its personality.
    int groupSize(std::uint64_t id) const;
    struct Traits {
        float sociable = 0, restless = 0, showy = 0;
    };
    Traits traits(std::uint64_t id) const;

private:
    enum class Act { Solo, Show, Wander, Rest, Seek, Watch, Group };
    enum class Kind { Partners, Facing, Call, Circle };
    struct Agent {
        std::uint64_t id = 0;
        std::uint32_t rng = 1;
        Traits tr;
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
    };
    struct Group {
        int id = 0;
        std::vector<std::uint64_t> members;  // in their places, left to right round the horseshoe
        double start = 0;                    // (clock) the group's own beat zero: turns, kind changes
        Kind kind = Kind::Facing;
        float cx = 0, cz = 0, tilt = 0, phase = 0;
        std::uint32_t rng = 1;
        long kindBar = 0;                    // the group's 8-bar block the kind was last chosen in
    };
    Agent* find(std::uint64_t id);
    Group* findGroup(int id);
    void decide(Agent& a, const StageCamera& cam, double beat);
    void enter(Agent& a, Act act, double bars, double beat);
    void leaveGroup(Agent& a, double beat);
    void layout(Group& g);
    std::pair<float, float> pickSpot(Agent& a, const StageCamera& cam, bool edge);
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
// a matched colour scheme, the style's wardrobe (outfits, hats, prints, fabrics), sunglasses, glow sticks and a
// face. The same seed gives the same character.
DancerSpec generateDancer(dance::Style style, std::uint32_t seed);
// reforce.Dancer.RandomStyles: which styles the generated ("Random STYLE") characters may be picked from when the
// next dancer is chosen: "all", "none" or a comma list of styles ("disco,rave"). Named characters are not affected.
// normalizeRandomStyles: the canonical form ("all", "none" or the styles in their order, comma separated), or
// nothing when a name isn't a style.
std::optional<std::string> normalizeRandomStyles(std::string_view value);
bool randomStyleAllowed(std::string_view list, dance::Style style);

// What the stage asks of a dancer this step (crew sync modes, social mode).
struct DanceDirection {
    dance::Choreographer* leader = nullptr;  // dance the leader's routines instead of this dancer's own
    double delay = 0;                         // beats of lag behind the leader (canon)
    bool mirror = false;                      // the leader's routines mirrored (partner dance, mirror crews)
    bool react = false;                       // watch someone else dance: claps, points, nods (social mode)
    float facing = std::nanf("");             // body turn, radians (+ = towards +x); NaN = the audience
    float look = 0;                           // extra head turn towards whoever is being watched
    std::string role;                         // for the panel: "with Dex", "solo", "cheering Nia"...
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
inline constexpr float kBodyRadius = 0.21f;  // stage units (a dancer is about 1 tall)
inline constexpr float kMinApart = 0.38f;
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
    float walk_ = 0;  // 0 dancing in place .. 1 walking (feet planted / stepping)
    bool feetPlaced_ = false;
    double lastStepBeat_ = -1e9;
    int steps_ = 0;

public:
    float walking() const { return walk_; }
    int stepsTaken() const { return steps_; }
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

public:
    // Hair swing (head frame, per unit length) as last stepped, for tests.
    dance::V3 hairSwing(int side) const { return pose_.hairSwing[side & 1]; }

private:
};

}  // namespace reforce::engine

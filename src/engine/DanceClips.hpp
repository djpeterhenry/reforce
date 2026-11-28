#pragma once
// reforce extension (docs/dancers-design.md "Mocap moves"): dance moves recorded from real dancers. Pure C++.
//
// tools/aist_import turns AIST++ (Google, CC BY 4.0: 3D keypoints of dancers in 10 street and jazz styles, cut to
// music at known tempos) into a moves file: 4-bar basic moves and 16-bar routines (some 8) as loops of DancePose-like
// channels that don't depend on body size
// (lengths over the source dancer's arm or leg). loadClips() reads it and adds every loop to moveLibrary() as a Full
// move (MoveDef::clip); clipPose() turns a loop at a beat phase into a pose for a character's own skeleton, so the
// rig's IK places the hands and feet, the bend hints pick where elbows and knees point, and moves mix, mirror and
// humanise like the procedural ones.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/DanceMoves.hpp"
#include "engine/DancerRig.hpp"

namespace reforce::engine::dance {

// How far a move with floor work reaches from the dancer's spot (2026-11-27, docs/dancers-design.md "Floor moves held
// still, room to go down"): the support of the body (capsule ends plus radii; the standard body, character units) over
// its whole loop, horizontally, in 8 directions in the frame the stage turns (the move's own travel and turns
// included): dir[0] the front (+z, the way the dancer faces), dir[2] the left (+x), dir[4] the back, dir[6] the right.
// `max`: the farthest point any way. headFirst: it goes down head first towards the front (the face-down basics 10):
// never to be turned away from the audience.
struct FloorReach {
    float dir[8] = {};
    float max = 0;
    bool headFirst = false;
    // The reach towards `angle` (radians from the front, + towards the left), between the two nearest directions.
    float toward(float angle) const;
    FloorReach scaled(float k) const;
};

struct ClipInfo {
    std::string id;     // "aist:gHO_sBM_cAll_d19_mHO0_ch01@8" (source clip @ first beat)
    std::string label;  // "house basic 1 · dancer 19"
    std::string genre;  // "HO" (AIST genre code)
    bool routine = false;  // from an advanced routine (sFM); else a basic move (sBM)
    int bpm = 120;         // the source music's tempo
    int beats = 8;
    float motion = 0;      // how much it moves (average speed of the hands and feet, arm / leg lengths per beat)
    int move = -1;         // index in moveLibrary()
    // Break floor work (docs/dancers-design.md "Break floor work", content/Moves/aist_floor.moves): 0 standing, else
    // the most daring thing in it.
    int tier = 0;
    // From the floor moves' file (floor moves, and break routines with some floor work in them: standing, tier 0).
    bool floorWork = false;
    // Floor work: how far it reaches (zeros for standing moves).
    FloorReach reach;
    // The fixed turn given to the whole clip at load (radians, + to the left), so it faces the audience as much as it
    // can: the most samples within 90 degrees of the camera (Peter, 2026-11-27: busting dancers spent a lot of time
    // facing the back; some routines were recorded facing away most of the time).
    float yawOffset = 0;
    float awayBefore = 0, awayAfter = 0;  // share of samples facing more than 90 degrees from the camera
};

// Floor: low work on the hands, knees and hips (Peter, 2026-11-27: "footwork" and "power" read as the same thing).
// Flips (goes upside down) are no longer in the file (Peter, 2026-11-27: not good enough yet); a file's flips read
// as such and are never picked, its tier 2 (freezes: none in AIST++) reads as floor.
enum ClipTier : int { kStanding = 0, kFloorTier = 1, kFlips = 3 };
const char* clipTierName(int tier);  // "standing", "floor", "flips"
// How often the choreographer picks floor moves: each move's weight against a standing move of the style (0: never;
// 1: as often). reforce.Dancer.FloorMoves, default kDefaultFloorWeight (Peter, 2026-11-27: break dancers mix them in
// by themselves, as ordinary break moves: no special weighting, no say of the music). Trying a move (the panel) plays any. Watching someone never uses floor moves.
inline constexpr float kDefaultFloorWeight = 1.0f;
void setFloorWeights(float floor);
float clipWeight(int clip);

// AIST genre codes in the order of our styles (HO house, PO pop, LO lock, BR break, WA waack: disco, KR krump: stomp,
// MH middle hip-hop: party hip-hop, LH LA hip-hop, JS street jazz, JB ballet jazz) and their names here.
const std::vector<std::string_view>& clipGenres();
std::string_view clipGenreName(std::string_view code);
// The style a genre's moves belong to (the choreographer picks them for dancers of that style), and as a style bit.
std::optional<Style> clipGenreStyle(std::string_view code);
std::uint32_t clipGenreStyles(std::string_view code);

// Reads a moves file and adds its moves to moveLibrary() (replacing any loaded before, or after them with `append`:
// the floor moves' own file; call before dancers start, the library is not locked). Returns "" or what was wrong
// (nothing is added then).
std::string loadClips(const std::string& path, bool append = false);
// The same from bytes in memory (the web preview fetches its moves file); `path` only names it in errors.
std::string loadClipsFromMemory(const unsigned char* data, std::size_t size, const std::string& path = "moves",
                                bool append = false);
int clipCount();
const ClipInfo& clipInfo(int clip);
int findClip(std::string_view id);  // by id or label (-1: none)
// Floor work: the source dancer's floor clearance at beat phase b (character units for `sk`, as clipPose) of the left
// and right hand, the left and right knee and the head (out[0..4]); false (out untouched) for a standing move or a
// file without them. Tests.
bool clipContacts(int clip, float b, const Skeleton& sk, float out[5]);
// The widest reach of the routines a bust plays (ClipInfo::floorWork, break, 16 bars), each direction's widest.
const FloorReach& bustMoveReach();

// The pose of `clip` at beat phase b (0 .. beats, wraps) for skeleton `sk`. Writes the body, hips, chest, head, hands,
// feet and bend hints; leaves the face as it was.
void clipPose(int clip, float b, const Skeleton& sk, DancePose& p);

}  // namespace reforce::engine::dance

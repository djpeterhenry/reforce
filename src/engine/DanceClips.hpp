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

struct ClipInfo {
    std::string id;     // "aist:gHO_sBM_cAll_d19_mHO0_ch01@8" (source clip @ first beat)
    std::string label;  // "house basic 1 · dancer 19"
    std::string genre;  // "HO" (AIST genre code)
    bool routine = false;  // from an advanced routine (sFM); else a basic move (sBM)
    int bpm = 120;         // the source music's tempo
    int beats = 8;
    float motion = 0;      // how much it moves (average speed of the hands and feet, arm / leg lengths per beat)
    int move = -1;         // index in moveLibrary()
};

// AIST genre codes in the order of our styles (HO house, PO pop, LO lock, BR break, WA waack: disco, KR krump: stomp,
// MH middle hip-hop: party hip-hop, LH LA hip-hop, JS street jazz, JB ballet jazz) and their names here.
const std::vector<std::string_view>& clipGenres();
std::string_view clipGenreName(std::string_view code);
// The style a genre's moves belong to (the choreographer picks them for dancers of that style), and as a style bit.
std::optional<Style> clipGenreStyle(std::string_view code);
std::uint32_t clipGenreStyles(std::string_view code);

// Reads a moves file and adds its moves to moveLibrary() (replacing any loaded before; call before dancers start,
// the library is not locked). Returns "" or what was wrong (nothing is added then).
std::string loadClips(const std::string& path);
// The same from bytes in memory (the web preview fetches its moves file); `path` only names it in errors.
std::string loadClipsFromMemory(const unsigned char* data, std::size_t size, const std::string& path = "moves");
int clipCount();
const ClipInfo& clipInfo(int clip);
int findClip(std::string_view id);  // by id or label (-1: none)

// The pose of `clip` at beat phase b (0 .. beats, wraps) for skeleton `sk`. Writes the body, hips, chest, head, hands,
// feet and bend hints; leaves the face as it was.
void clipPose(int clip, float b, const Skeleton& sk, DancePose& p);

}  // namespace reforce::engine::dance

#include "expr/Vars.hpp"

#include <array>
#include <cctype>

namespace reforce::expr {

namespace {
constexpr std::array<const char*, kVarCount> kNames = {
    "T", "S", "I", "X", "Y", "R", "THETA", "ID", "NUM", "NUM_S_STEPS", "BASS", "LEVEL", "ENERGY",
    "END_TIME", "START_TIME", "DT", "X_EXTENTS", "Y_EXTENTS", "WIDTH", "HEIGHT", "PI", "PATH_ID",
    "FADER", "NUM_FFT_BINS", "NUM_SAMPLE_BINS", "XPOS", "YPOS", "MID", "TREBLE", "BEAT", "TEMPO",
    "LINK_ON", "LINK_BPM", "LINK_BEAT", "LINK_PHASE", "SYNC_ON", "SYNC_BPM", "SYNC_BEAT", "SYNC_LOCK",
    "SYNC_BAR", "SYNC_PHRASE", "SYNC_BUILD", "SYNC_DROP",
};
}  // namespace

const char* varName(Var v) { return kNames[std::size_t(v)]; }

std::string toUpper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

std::string toLower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::optional<Var> lookupVar(std::string_view name) {
    const std::string up = toUpper(name);
    for (int i = 0; i < kVarCount; ++i)
        if (up == kNames[std::size_t(i)]) return Var(i);
    return std::nullopt;
}

bool parseUserVar(std::string_view name, int& family, int& index) {
    if (name.size() < 2) return false;
    const char f = char(std::toupper(static_cast<unsigned char>(name[0])));
    if (f < 'A' || f > 'D') return false;
    int idx = 0;
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
        idx = idx * 10 + (name[i] - '0');
        if (idx > kMaxUserVarIndex) return false;
    }
    family = f - 'A';
    index = idx;
    return true;
}

}  // namespace reforce::expr

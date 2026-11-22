#pragma once
// Tolerant G-Force scene file loader (scene-format §2):
// bytes -> (normalized key, raw key, value, quoted?, line) entries + comment tags.
//
// Accepts: // and /* */ comments; CR, LF, CRLF and mixed line endings;
// multi-line quoted values; whitespace- or comma-separated pairs (also several
// on one line, trailing commas); spaces around '='; a quoted value on the line
// after '='; empty values; duplicate keys (last wins, warning); non-UTF-8
// bytes (handled as opaque Latin-1 bytes; only a warning outside comments);
// stray text outside comments (warning). Keys are matched case-insensitively.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "expr/Diagnostics.hpp"

namespace reforce::scene {

struct SceneEntry {
    std::string key;       // normalized (upper-case) key
    std::string rawKey;    // spelling in the file
    std::string value;     // quoted: text between the quotes; unquoted: the token
    bool quoted = false;
    int line = 0;          // line of the key
    int endLine = 0;       // last line of the value (multi-line strings)
    bool overridden = false;  // a later entry with the same key wins
};

struct SceneFile {
    std::string path;                 // informational
    std::vector<SceneEntry> entries;  // file order, including overridden duplicates
    std::vector<std::string> tags;    // "#tag" words found in comments, without '#', lower-case, unique
    bool particleOnly = false;        // #waveshape-particle-only
    bool particleAlso = false;        // #waveshape-particle-also
    bool hadBom = false;
    bool nonUtf8 = false;             // file is not valid UTF-8 (read as Latin-1)
    expr::Diagnostics diags;

    // Last (winning) entry for a key, case-insensitive.
    const SceneEntry* find(std::string_view key) const;
    // Winning entries only, in file order.
    std::vector<const SceneEntry*> effectiveEntries() const;

    std::map<std::string, std::size_t> index;  // normalized key -> winning entry index
};

SceneFile parseSceneText(std::string_view bytes, std::string path = {});
std::optional<SceneFile> loadSceneFile(const std::filesystem::path& path);

}  // namespace reforce::scene

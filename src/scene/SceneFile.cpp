#include "scene/SceneFile.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>

#include "expr/Vars.hpp"

namespace reforce::scene {

const SceneEntry* SceneFile::find(std::string_view key) const {
    auto it = index.find(expr::toUpper(key));
    return it == index.end() ? nullptr : &entries[it->second];
}

std::vector<const SceneEntry*> SceneFile::effectiveEntries() const {
    std::vector<const SceneEntry*> out;
    for (const auto& e : entries)
        if (!e.overridden) out.push_back(&e);
    return out;
}

namespace {

bool isUtf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        int extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
        if (extra < 0) return false;
        if (extra > 0 && i + std::size_t(extra) >= s.size()) return false;
        for (int k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(s[i + std::size_t(k)]) >> 6) != 0x2) return false;
        i += std::size_t(extra) + 1;
    }
    return true;
}

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string snippet(std::string_view s, std::size_t n = 60) {
    std::string out;
    for (char c : s) {
        if (c == '\n' || c == '\r') break;
        out += (c == '\t') ? ' ' : c;
        if (out.size() >= n) {
            out += "...";
            break;
        }
    }
    return out;
}

class Lexer {
public:
    Lexer(std::string_view text, SceneFile& out) : s_(text), f_(out) {}

    void run() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\r' || c == '\n') {
                newline();
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\f' || c == '\v') {
                ++i_;
                continue;
            }
            // Legacy separators: "AVRG=11, DCAY=.7" and C-initializer trailing commas.
            if (c == ',' || c == ';') {
                ++i_;
                continue;
            }
            if (c == '/' && peek(1) == '/') {
                lineComment();
                continue;
            }
            if (c == '/' && peek(1) == '*') {
                blockComment();
                continue;
            }
            if (isIdentStart(c) && tryEntry()) continue;
            strayText();
        }
    }

private:
    char peek(std::size_t k) const { return i_ + k < s_.size() ? s_[i_ + k] : '\0'; }

    // Consumes one line break (CRLF, CR or LF).
    void newline() {
        if (s_[i_] == '\r' && peek(1) == '\n') ++i_;
        ++i_;
        ++line_;
    }

    void scanTags(std::string_view body) {
        for (std::size_t k = 0; k < body.size(); ++k) {
            if (body[k] != '#') continue;
            if (k > 0 && (isIdentChar(body[k - 1]) || body[k - 1] == '&')) continue;
            std::size_t j = k + 1;
            if (j >= body.size() || !std::isalpha(static_cast<unsigned char>(body[j]))) continue;
            while (j < body.size() && (isIdentChar(body[j]) || body[j] == '-')) ++j;
            std::string tag = expr::toLower(body.substr(k + 1, j - k - 1));
            if (tag == "waveshape-particle-only") f_.particleOnly = true;
            if (tag == "waveshape-particle-also") f_.particleAlso = true;
            if (std::find(f_.tags.begin(), f_.tags.end(), tag) == f_.tags.end()) f_.tags.push_back(std::move(tag));
            k = j - 1;
        }
    }

    void lineComment() {
        const std::size_t start = i_ + 2;
        while (i_ < s_.size() && s_[i_] != '\n' && s_[i_] != '\r') ++i_;
        scanTags(s_.substr(start, i_ - start));
    }

    void blockComment() {
        const int startLine = line_;
        i_ += 2;
        const std::size_t start = i_;
        while (i_ < s_.size() && !(s_[i_] == '*' && peek(1) == '/')) {
            if (s_[i_] == '\r' || s_[i_] == '\n') newline();
            else ++i_;
        }
        const std::string_view body = s_.substr(start, i_ - start);
        if (i_ >= s_.size()) {
            f_.diags.warn("unterminated-comment", "/* comment runs to end of file", startLine);
        } else {
            i_ += 2;
        }
        if (body.find("/*") != std::string_view::npos)
            f_.diags.warn("nested-comment", "'/*' inside a /* comment (comments do not nest)", startLine);
        scanTags(body);
    }

    void strayText() {
        // Skip to end of line or the start of a comment, whichever is first.
        const std::size_t start = i_;
        while (i_ < s_.size() && s_[i_] != '\n' && s_[i_] != '\r' &&
               !(s_[i_] == '/' && (peek(1) == '/' || peek(1) == '*')))
            ++i_;
        if (i_ == start) ++i_;
        if (line_ != lastStrayLine_) {
            f_.diags.warn("stray-text", "text outside a KEY=VALUE pair ignored: '" + snippet(s_.substr(start, i_ - start)) + "'",
                          line_);
            lastStrayLine_ = line_;
        }
    }

    bool tryEntry() {
        std::size_t j = i_;
        while (j < s_.size() && isIdentChar(s_[j])) ++j;
        std::size_t k = j;
        while (k < s_.size() && (s_[k] == ' ' || s_[k] == '\t')) ++k;
        if (k >= s_.size() || s_[k] != '=') return false;

        SceneEntry e;
        e.rawKey = std::string(s_.substr(i_, j - i_));
        e.key = expr::toUpper(e.rawKey);
        e.line = line_;
        i_ = k + 1;
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t')) ++i_;

        // Value on the next line(s)? Only accepted when it is a quoted string.
        if (i_ < s_.size() && (s_[i_] == '\r' || s_[i_] == '\n')) {
            std::size_t q = i_;
            int lines = 0;
            while (q < s_.size() && (s_[q] == ' ' || s_[q] == '\t' || s_[q] == '\r' || s_[q] == '\n')) {
                if (s_[q] == '\n' || (s_[q] == '\r' && (q + 1 >= s_.size() || s_[q + 1] != '\n'))) ++lines;
                ++q;
            }
            if (q < s_.size() && s_[q] == '"') {
                f_.diags.warn("value-on-next-line", "quoted value for " + e.rawKey + " starts on a later line", e.line,
                              e.rawKey);
                i_ = q;
                line_ += lines;
            }
        }

        if (i_ < s_.size() && s_[i_] == '"') {
            const std::size_t start = ++i_;
            while (i_ < s_.size() && s_[i_] != '"') {
                if (s_[i_] == '\r' || s_[i_] == '\n') newline();
                else ++i_;
            }
            e.quoted = true;
            e.value = std::string(s_.substr(start, i_ - start));
            if (i_ >= s_.size()) {
                f_.diags.error("unterminated-string", "quoted value for " + e.rawKey + " is not closed", e.line, e.rawKey);
            } else {
                ++i_;  // closing quote
            }
            for (char ch : e.value)
                if (static_cast<unsigned char>(ch) >= 0x80) {
                    f_.diags.warn("non-ascii-value", "non-ASCII byte inside the value of " + e.rawKey, e.line, e.rawKey);
                    break;
                }
        } else {
            // Unquoted token: up to whitespace, a separator, a quote or a comment.
            const std::size_t start = i_;
            while (i_ < s_.size()) {
                const char c = s_[i_];
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ';' || c == '"') break;
                if (c == '/' && (peek(1) == '/' || peek(1) == '*')) break;
                ++i_;
            }
            e.value = std::string(s_.substr(start, i_ - start));
            if (e.value.empty())
                f_.diags.warn("empty-value", e.rawKey + "= has no value (treated as undefined)", e.line, e.rawKey);
        }
        e.endLine = line_;
        add(std::move(e));
        return true;
    }

    void add(SceneEntry e) {
        auto it = f_.index.find(e.key);
        if (it != f_.index.end()) {
            SceneEntry& prev = f_.entries[it->second];
            prev.overridden = true;
            f_.diags.warn("duplicate-key",
                          "duplicate key " + e.rawKey + " (first at line " + std::to_string(prev.line) + "); last wins",
                          e.line, e.rawKey);
        }
        f_.index[e.key] = f_.entries.size();
        f_.entries.push_back(std::move(e));
    }

    std::string_view s_;
    SceneFile& f_;
    std::size_t i_ = 0;
    int line_ = 1;
    int lastStrayLine_ = 0;
};

}  // namespace

SceneFile parseSceneText(std::string_view bytes, std::string path) {
    SceneFile f;
    f.path = std::move(path);
    if (bytes.size() >= 3 && bytes.substr(0, 3) == "\xEF\xBB\xBF") {
        f.hadBom = true;
        bytes.remove_prefix(3);
    }
    f.nonUtf8 = !isUtf8(bytes);
    if (bytes.find('\0') != std::string_view::npos) f.diags.warn("nul-byte", "file contains NUL bytes");
    Lexer(bytes, f).run();
    return f;
}

std::optional<SceneFile> loadSceneFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parseSceneText(bytes, path.string());
}

}  // namespace reforce::scene

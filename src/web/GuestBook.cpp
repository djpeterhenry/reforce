#include "web/GuestBook.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace reforce::web {

namespace fs = std::filesystem;

namespace {

// key="value" (or key=value) lines of a scene file: the value of `key` (case-insensitive), "" when absent.
std::string valueOf(const std::string& text, const char* key) {
    std::istringstream in(text);
    std::string line;
    const std::string k = key;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq != k.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < eq; ++i)
            same = same && std::tolower(static_cast<unsigned char>(line[i])) == std::tolower(static_cast<unsigned char>(k[i]));
        if (!same) continue;
        std::string v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        return v;
    }
    return {};
}

// The text without the lines of `keys` (case-insensitive).
std::string without(const std::string& text, std::initializer_list<const char*> keys) {
    std::istringstream in(text);
    std::string line, out;
    while (std::getline(in, line)) {
        bool drop = false;
        for (const char* k : keys) {
            const std::size_t n = std::char_traits<char>::length(k);
            if (line.size() > n && line[n] == '=') {
                bool same = true;
                for (std::size_t i = 0; i < n; ++i)
                    same = same && std::tolower(static_cast<unsigned char>(line[i])) == std::tolower(static_cast<unsigned char>(k[i]));
                drop = drop || same;
            }
        }
        if (!drop) out += line + "\n";
    }
    return out;
}

std::optional<std::string> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

}  // namespace

std::string guestFileStem(const std::string& name) {
    std::string s;
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '\'' || c >= 0x80) s.push_back(char(c));
    }
    while (!s.empty() && (s.front() == ' ' || s.front() == '.')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    if (s.size() > 40) {
        // Cut at 40 bytes, at the start of a UTF-8 character (never half of one).
        std::size_t cut = 40;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
        s.resize(cut);
        while (!s.empty() && s.back() == ' ') s.pop_back();
    }
    return s.empty() ? std::string("Guest") : s;
}

GuestBook::GuestBook(fs::path dir) : dir_(std::move(dir)) { load(); }

void GuestBook::load() {
    guests_.clear();
    std::error_code ec;
    if (!fs::is_directory(dir_, ec)) return;
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
        if (e.path().extension() != ".txt") continue;
        const auto text = readFile(e.path());
        if (!text) continue;
        SavedGuest g;
        g.id = valueOf(*text, "Guest");
        if (g.id.empty()) continue;  // not one of ours
        g.name = valueOf(*text, "Name");
        g.style = valueOf(*text, "Style");
        g.label = e.path().stem().string();
        try {
            g.seen = std::stoll(valueOf(*text, "Seen"));
        } catch (...) {
            g.seen = 0;
        }
        guests_.push_back(std::move(g));
    }
    std::sort(guests_.begin(), guests_.end(), [](const SavedGuest& a, const SavedGuest& b) { return a.seen > b.seen; });
}

std::string GuestBook::freeLabel(const std::string& name, const std::string& id) const {
    const std::string stem = guestFileStem(name);
    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    for (int n = 1;; ++n) {
        const std::string label = n == 1 ? stem : stem + " " + std::to_string(n);
        const bool taken = std::any_of(guests_.begin(), guests_.end(), [&](const SavedGuest& g) {
            return g.id != id && lower(g.label) == lower(label);  // (the Mac's file names ignore case)
        });
        std::error_code ec;
        const bool stray = !taken && fs::exists(dir_ / (label + ".txt"), ec) &&
                           std::none_of(guests_.begin(), guests_.end(), [&](const SavedGuest& g) {
                               return g.id == id && lower(g.label) == lower(label);
                           });
        if (!taken && !stray) return label;
    }
}

std::string GuestBook::save(const std::string& id, const std::string& name, const std::string& sceneText,
                            std::int64_t seen) {
    if (id.empty()) return "a guest without an id";
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) return "can't make " + dir_.string() + ": " + ec.message();
    auto it = std::find_if(guests_.begin(), guests_.end(), [&](const SavedGuest& g) { return g.id == id; });
    SavedGuest g = it != guests_.end() ? *it : SavedGuest{};
    const std::string oldLabel = g.label;
    g.id = id;
    g.name = name;
    g.style = valueOf(sceneText, "Style");
    g.seen = std::max(seen, g.seen);
    // A new guest, or a new name: the file of their (free) name.
    if (oldLabel.empty() || guestFileStem(name) != guestFileStem(it->name)) g.label = freeLabel(name, id);
    // Guest= and Seen= are ours; quotes in the id are dropped (it goes inside Guest="...").
    std::string safeId;
    for (char c : id)
        if (c != '"' && c != '\n' && c != '\r') safeId.push_back(c);
    std::string text = without(sceneText, {"Guest", "Seen"});
    text += "Guest=\"" + safeId + "\"\nSeen=\"" + std::to_string(g.seen) + "\"\n";
    // Written next to it and renamed over it, so a crash never leaves half a file.
    const fs::path path = pathOf(g), tmp = dir_ / (g.label + ".txt.tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return "can't write " + tmp.string();
        f << text;
        if (!f) return "can't write " + tmp.string();
    }
    fs::rename(tmp, path, ec);
    if (ec) return "can't write " + path.string() + ": " + ec.message();
    if (!oldLabel.empty() && oldLabel != g.label) fs::remove(dir_ / (oldLabel + ".txt"), ec);
    if (it != guests_.end()) *it = g;
    else guests_.push_back(g);
    std::sort(guests_.begin(), guests_.end(), [](const SavedGuest& a, const SavedGuest& b) { return a.seen > b.seen; });
    return {};
}

const SavedGuest* GuestBook::find(const std::string& idOrLabel) const {
    for (const auto& g : guests_)
        if (g.id == idOrLabel) return &g;
    for (const auto& g : guests_)
        if (g.label == idOrLabel) return &g;
    return nullptr;
}

std::optional<std::string> GuestBook::sceneText(const std::string& id) const {
    for (const auto& g : guests_)
        if (g.id == id) return readFile(pathOf(g));
    return std::nullopt;
}

}  // namespace reforce::web

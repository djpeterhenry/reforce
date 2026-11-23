#include "engine/Themes.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace reforce::engine {
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n' || c == '\r') {
            if (!trim(cur).empty()) out.push_back(lower(trim(cur)));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!trim(cur).empty()) out.push_back(lower(trim(cur)));
    return out;
}

// Splits a CategoryExpr on the literal two-character "\t" (and real tabs).
std::vector<std::string> exprTerms(const std::string& e) {
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < e.size(); ++i) {
        if (e[i] == '\t' || (e[i] == '\\' && i + 1 < e.size() && e[i + 1] == 't')) {
            if (e[i] == '\\') ++i;
            if (!trim(cur).empty()) out.push_back(trim(cur));
            cur.clear();
        } else {
            cur.push_back(e[i]);
        }
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool regexMatch(const std::string& pattern, const std::string& name) {
    if (pattern.empty()) return false;
    try {
        return std::regex_match(name, std::regex(pattern, std::regex::ECMAScript | std::regex::icase));
    } catch (const std::regex_error&) {
        return false;
    }
}

}  // namespace

std::map<std::string, std::string> parseKeyValueFile(const std::string& text) {
    std::map<std::string, std::string> kv;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        // key up to '=' on this line
        std::size_t eol = text.find_first_of("\r\n", i);
        if (eol == std::string::npos) eol = n;
        const std::size_t eq = text.find('=', i);
        if (eq == std::string::npos || eq > eol) {
            i = eol + 1;
            continue;
        }
        std::string key = trim(text.substr(i, eq - i));
        if (key.size() >= 2 && key.front() == '"' && key.back() == '"') key = key.substr(1, key.size() - 2);
        std::size_t v = eq + 1;
        while (v < n && (text[v] == ' ' || text[v] == '\t')) ++v;
        std::string value;
        if (v < n && text[v] == '"') {
            // Quoted, may span lines; \" and \\ escapes, backslash-t kept literal.
            ++v;
            while (v < n && text[v] != '"') {
                if (text[v] == '\\' && v + 1 < n && (text[v + 1] == '"' || text[v + 1] == '\\')) ++v;
                value.push_back(text[v++]);
            }
            i = v + 1;
        } else {
            std::size_t e = text.find_first_of("\r\n", v);
            if (e == std::string::npos) e = n;
            value = trim(text.substr(v, e - v));
            i = e + 1;
        }
        if (!key.empty()) kv[lower(key)] = value;
    }
    return kv;
}

bool Category::matches(const std::string& configName, const std::string& id) const {
    const std::string n = lower(configName);
    const bool inc = std::find(includeList.begin(), includeList.end(), n) != includeList.end() ||
                     regexMatch(includePattern, configName) || (!id.empty() && regexMatch(includeIdPattern, id));
    if (!inc) return false;
    const bool exc = std::find(excludeList.begin(), excludeList.end(), n) != excludeList.end() ||
                     regexMatch(excludePattern, configName);
    return !exc;
}

void ThemeLibrary::addTheme(const std::string& text) {
    const auto kv = parseKeyValueFile(text);
    Theme t;
    auto it = kv.find("themename");
    if (it == kv.end() || it->second.empty()) return;
    t.name = it->second;
    t.baseName = t.name;
    if ((it = kv.find("about")) != kv.end()) t.about = it->second;
    const std::string suffix = ".categoryexpr";
    for (const auto& [k, v] : kv)
        if (k.size() > suffix.size() && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0)
            t.categoryExpr[k.substr(0, k.size() - suffix.size())] = v;
    if ((it = kv.find("object.slideshow.interval.duration")) != kv.end()) {
        char* end = nullptr;
        const double v = std::strtod(it->second.c_str(), &end);
        if (end != it->second.c_str() && v >= 0 && v < 1e6) t.objectInterval = v;
    }
    if ((it = kv.find("dancer.slideshow.interval.duration")) != kv.end()) {
        char* end = nullptr;
        const double v = std::strtod(it->second.c_str(), &end);
        if (end != it->second.c_str() && v >= 0 && v < 1e6) t.dancerInterval = v;
    }
    themes_.push_back(std::move(t));
}

void ThemeLibrary::merge(const ThemeLibrary& other) {
    for (const auto& t : other.themes_)
        if (!findListed(t.name)) themes_.push_back(t);
    for (const auto& [type, cats] : other.categories_)
        for (const auto& c : cats)
            if (!category(type, c.title)) categories_[type].push_back(c);
}

void ThemeLibrary::addCategory(const std::string& typeName, const std::string& text) {
    const auto kv = parseKeyValueFile(text);
    auto get = [&](const char* k) {
        auto it = kv.find(k);
        return it == kv.end() ? std::string() : it->second;
    };
    Category c;
    c.title = get("categorytitle");
    if (c.title.empty()) return;
    c.includeList = lines(get("include.list"));
    c.excludeList = lines(get("exclude.list"));
    c.includePattern = trim(get("include.pattern"));
    c.excludePattern = trim(get("exclude.pattern"));
    c.includeIdPattern = trim(get("include.id"));
    categories_[lower(typeName)].push_back(std::move(c));
}

ThemeLibrary ThemeLibrary::load(const fs::path& scenesDir) {
    ThemeLibrary lib;
    std::error_code ec;
    std::string text;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(scenesDir / "Themes", ec))
        if (e.is_regular_file(ec) && lower(e.path().extension().string()) == ".txt") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files)
        if (readFile(f, text)) lib.addTheme(text);
    for (const auto& typeDir : fs::directory_iterator(scenesDir / "Categories", ec)) {
        if (!typeDir.is_directory(ec)) continue;
        files.clear();
        for (const auto& e : fs::directory_iterator(typeDir.path(), ec))
            if (e.is_regular_file(ec) && lower(e.path().extension().string()) == ".txt") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files)
            if (readFile(f, text)) lib.addCategory(typeDir.path().filename().string(), text);
    }
    return lib;
}

const Theme* ThemeLibrary::findListed(const std::string& name) const {
    for (const Theme& t : themes_)
        if (lower(t.name) == lower(name)) return &t;
    return nullptr;
}

ThemeLibrary ThemeLibrary::loadAll(const fs::path& scenesDir, const fs::path& contentDir) {
    ThemeLibrary lib = contentDir.empty() ? ThemeLibrary{} : load(contentDir);
    if (!scenesDir.empty()) {
        ThemeLibrary gforce = load(scenesDir);
        gforce.prefixNames(kGForceThemePrefix);
        lib.merge(gforce);
    }
    return lib;
}

void ThemeLibrary::prefixNames(const std::string& prefix) {
    for (Theme& t : themes_) t.name = prefix + t.baseName;
}

const Theme* ThemeLibrary::find(const std::string& name) const {
    const std::string n = lower(trim(name));
    if (n.empty() || n == "(standard)") return nullptr;
    for (const Theme& t : themes_)
        if (lower(t.name) == n) return &t;
    for (const Theme& t : themes_)  // the bare title (settings saved before the prefix, G-Force's own prefs)
        if (lower(t.baseName) == n) return &t;
    return nullptr;
}

const Category* ThemeLibrary::category(const std::string& typeName, const std::string& title) const {
    auto it = categories_.find(lower(typeName));
    if (it == categories_.end()) return nullptr;
    for (const Category& c : it->second)
        if (lower(c.title) == lower(title)) return &c;
    return nullptr;
}

std::vector<std::string> ThemeLibrary::filter(const Theme* theme, const std::string& typeName,
                                              const std::vector<std::string>& names,
                                              std::vector<std::string>* warnings,
                                              const std::vector<std::string>* ids) const {
    if (!theme) return names;
    auto it = theme->categoryExpr.find(lower(typeName));
    if (it == theme->categoryExpr.end()) return names;
    const auto terms = exprTerms(it->second);
    std::vector<std::pair<bool, std::string>> plus, minus;
    for (const std::string& term : terms) {
        if (term.size() < 2 || (term[0] != '+' && term[0] != '-')) continue;
        (term[0] == '+' ? plus : minus).push_back({true, trim(term.substr(1))});
    }
    auto inCategory = [&](const std::string& title, const std::string& name, const std::string& id) {
        if (const Category* c = category(typeName, title)) return c->matches(name, id);
        if (lower(title) == "(suggested)") return true;  // built in for this type: everything
        return false;
    };
    if (warnings) {
        for (const auto* list : {&plus, &minus})
            for (const auto& [_, title] : *list)
                if (!category(typeName, title) && lower(title) != "(suggested)")
                    warnings->push_back(typeName + " category \"" + title + "\" not found");
    }
    std::vector<std::string> out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string& name = names[i];
        const std::string id = ids && i < ids->size() ? (*ids)[i] : std::string();
        bool in = plus.empty();
        for (const auto& [_, title] : plus)
            if (!in && inCategory(title, name, id)) in = true;
        if (!in) continue;
        bool out_ = false;
        for (const auto& [_, title] : minus)
            if (!out_ && inCategory(title, name, id)) out_ = true;
        if (!out_) out.push_back(name);
    }
    return out;
}

}  // namespace reforce::engine

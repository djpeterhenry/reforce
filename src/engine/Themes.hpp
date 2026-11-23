#pragma once
// Themes and Categories (scene-format §8): user data files next to the packages.
//
//   <scenes>/Themes/*.txt                 ThemeName, <Type>.CategoryExpr = "+A\t-B ..."
//   <scenes>/Categories/<Type>/*.txt      CategoryTitle, Include/Exclude .List/.Pattern
//
// `Key = Value` lines; values are integers or double-quoted strings that may
// span lines. Inside strings the two characters backslash-t are a literal
// separator (CategoryExpr terms).
//
// Theme filter for one type (inferred semantics): union of the `+` categories
// minus the union of the `-` categories; no `+` term = start from everything;
// a type without a CategoryExpr is unrestricted. "(Suggested)" without a
// category file for the type is built in and means every config.
// The startup theme "(Standard)" (G-Force's default) means no filtering.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace reforce::engine {

// Parses G-Force's Theme/Category `Key = Value` format (keys lower-cased).
std::map<std::string, std::string> parseKeyValueFile(const std::string& text);

struct Category {
    std::string title;
    std::vector<std::string> includeList, excludeList;  // lower-case config names
    std::string includePattern, excludePattern;          // regex (ECMAScript), "" = none
    bool matches(const std::string& configName) const;   // name = file stem
};

struct Theme {
    std::string name;      // as listed: "g-force: Infernal" for G-Force's own themes (loadAll)
    std::string baseName;  // the theme's own title ("Infernal"); find() accepts either
    std::string about;
    std::map<std::string, std::string> categoryExpr;  // lower-case type name -> expression
    // reforce extension: `Object.Slideshow.Interval.Duration = SEC` in a theme starts objects (solids and organics)
    // every SEC seconds while the theme is on (objects are not in the default slideshow); unset = the setting's
    // interval.
    std::optional<double> objectInterval;
    // The same for Dancers (PLAN 5P): `Dancer.Slideshow.Interval.Duration = SEC`.
    std::optional<double> dancerInterval;
};

inline constexpr const char* kGForceThemePrefix = "g-force: ";  // how G-Force's own themes are listed

class ThemeLibrary {
public:
    // Reads <scenesDir>/Themes and <scenesDir>/Categories (missing dirs: empty).
    static ThemeLibrary load(const std::filesystem::path& scenesDir);
    // reforce's own themes (contentDir) first, then G-Force's (scenesDir), listed as "g-force: NAME" (Peter,
    // 2026-11-19). Either dir may be empty.
    static ThemeLibrary loadAll(const std::filesystem::path& scenesDir, const std::filesystem::path& contentDir);
    // Lists every theme as PREFIX + its title.
    void prefixNames(const std::string& prefix);

    void addTheme(const std::string& text);
    void addCategory(const std::string& typeName, const std::string& text);
    // Adds `other`'s themes (a name already here keeps the existing one) and categories (reforce's own
    // content/ next to G-Force's scenes/).
    void merge(const ThemeLibrary& other);

    const std::vector<Theme>& themes() const { return themes_; }
    // Case-insensitive; nullptr for "(Standard)", "" and unknown names.
    const Theme* find(const std::string& name) const;
    const Category* category(const std::string& typeName, const std::string& title) const;

    // Names (file stems) of `names` that `theme` admits for `typeName`
    // ("WaveShape", "FlowField", ...). theme == nullptr: all of them.
    // `warnings` collects unknown categories.
    std::vector<std::string> filter(const Theme* theme, const std::string& typeName,
                                    const std::vector<std::string>& names,
                                    std::vector<std::string>* warnings = nullptr) const;

private:
    const Theme* findListed(const std::string& name) const;  // by listed name only
    std::vector<Theme> themes_;
    std::map<std::string, std::vector<Category>> categories_;  // lower-case type -> categories
};

}  // namespace reforce::engine

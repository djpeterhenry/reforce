#pragma once
// reforce extension: past stream guests (docs/dancers-design.md "Past guests"). Pure C++ (std::filesystem).
//
// Every guest who checks in has their latest dancer saved as a scene file in a folder next to settings.json
// (Guests/), so Peter can bring them back by hand, as long as he likes, after their check-in has run out (Peter,
// 2026-11-21: "keep guests last sign-on available as something I could launch manually and indefinitely").
//
// A file is the guest's scene text (web/Guests sceneText: their look, Style, Name) plus Guest="<id>" (the service's
// id: names need not be unique, so the id is what tells guests apart) and Seen="<ms since 1970>". It is named after the
// guest ("Robin.txt"); a second guest with the same name gets "Robin 2.txt". A guest who changes their name moves to a
// file of the new name.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace reforce::web {

struct SavedGuest {
    std::string id;     // the guest service's id
    std::string name;   // their name tag
    std::string label;  // the file's name: the name, numbered when names repeat ("Robin 2")
    std::string style;  // their dance style as saved (Style=)
    std::int64_t seen = 0;  // ms since 1970 of the last check-in
};

class GuestBook {
public:
    // Reads the folder (created on the first save).
    explicit GuestBook(std::filesystem::path dir);

    // Saves or updates a guest: `sceneText` without Guest= / Seen= (they are added). Returns "" or what went wrong.
    std::string save(const std::string& id, const std::string& name, const std::string& sceneText, std::int64_t seen);
    // Newest first.
    const std::vector<SavedGuest>& guests() const { return guests_; }
    const SavedGuest* find(const std::string& idOrLabel) const;
    // The saved scene file's text (nullopt: no such guest, or the file is gone).
    std::optional<std::string> sceneText(const std::string& id) const;
    const std::filesystem::path& dir() const { return dir_; }

private:
    void load();
    std::filesystem::path pathOf(const SavedGuest& g) const { return dir_ / (g.label + ".txt"); }
    std::string freeLabel(const std::string& name, const std::string& id) const;

    std::filesystem::path dir_;
    std::vector<SavedGuest> guests_;
};

// A file name from a guest's name: letters, digits, spaces and - _ . ' kept (the rest dropped), trimmed, at most 40
// characters; "Guest" when nothing is left.
std::string guestFileStem(const std::string& name);

}  // namespace reforce::web

#include "engine/Scheduler.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "engine/Transition.hpp"

namespace reforce::engine {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string baseName(const std::string& name) {
    const auto slash = name.find_last_of('/');
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

}  // namespace

double SchedRng::normal() {
    if (spare_) {
        const double v = *spare_;
        spare_.reset();
        return v;
    }
    double u1 = uniform();
    while (u1 <= 1e-300) u1 = uniform();
    const double u2 = uniform();
    const double r = std::sqrt(-2.0 * std::log(u1));
    spare_ = r * std::sin(2.0 * 3.14159265358979323846 * u2);
    return r * std::cos(2.0 * 3.14159265358979323846 * u2);
}

bool excludedFromSlideshow(const std::string& name) {
    const std::string b = baseName(name);
    return !b.empty() && b.front() == '(';
}

// ---------------------------------------------------------------- Playlist

void Playlist::setItems(std::vector<std::string> names) {
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    names.erase(std::unique(names.begin(), names.end()), names.end());
    items_ = std::move(names);
    order_.clear();
    pos_ = 0;
}

void Playlist::reshuffle(SchedRng& rng) {
    order_ = items_;
    for (std::size_t i = order_.size(); i > 1; --i) std::swap(order_[i - 1], order_[rng.index(i)]);
    // No immediate repeat across the cycle boundary.
    if (order_.size() > 1 && lower(order_.front()) == lower(current_)) std::swap(order_.front(), order_.back());
    pos_ = 0;
}

std::string Playlist::peek(SchedRng& rng) {
    if (items_.empty()) return {};
    for (int guard = 0; guard < 3; ++guard) {
        if (pos_ >= order_.size()) reshuffle(rng);
        // Skip the current config (it may have been chosen by hand meanwhile).
        while (pos_ < order_.size() && items_.size() > 1 && lower(order_[pos_]) == lower(current_)) ++pos_;
        if (pos_ < order_.size()) return order_[pos_];
    }
    return items_.front();
}

std::string Playlist::next(SchedRng& rng) {
    std::string n = peek(rng);
    if (!n.empty()) ++pos_;
    return n;
}

std::string Playlist::previous() {
    while (!history_.empty()) {
        std::string h = history_.back();
        history_.pop_back();
        if (lower(h) != lower(current_)) {
            current_.clear();  // so noteCurrent(h) does not push the one we leave back as "previous"
            return h;
        }
    }
    if (items_.empty()) return {};
    const std::string cur = lower(current_);
    for (std::size_t i = 0; i < items_.size(); ++i)
        if (lower(items_[i]) >= cur) return items_[(i + items_.size() - 1) % items_.size()];
    return items_.back();
}

void Playlist::noteCurrent(const std::string& name) {
    if (!current_.empty() && lower(current_) != lower(name)) {
        history_.push_back(current_);
        if (history_.size() > 64) history_.erase(history_.begin());
    }
    current_ = name;
}

// ---------------------------------------------------------------- LayerSlideshow

double LayerSlideshow::sampleInterval(SchedRng& rng) const {
    return sampleWithDeviation(interval_, intervalDev_, rng.normal(), 0.1);
}

double LayerSlideshow::sampleTransitionDuration(SchedRng& rng) const {
    if (duration_ <= 0) return 0;
    return sampleWithDeviation(duration_, durationDev_, rng.normal(), 0.0);
}

void LayerSlideshow::arm(double now, SchedRng& rng) {
    armed_ = true;
    dueAt_ = now + sampleInterval(rng);
}

}  // namespace reforce::engine

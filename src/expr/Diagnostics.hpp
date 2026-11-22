#pragma once
// Per-file diagnostics shared by the scene loader and the expression compiler.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace reforce::expr {

enum class Severity { Warning, Error };

struct Diagnostic {
    Severity severity = Severity::Warning;
    int line = 0;          // 1-based source line (0 = unknown / whole file)
    std::string key;       // scene key the diagnostic belongs to (may be empty)
    std::string kind;      // short stable category, e.g. "duplicate-key", "unknown-identifier"
    std::string message;   // human-readable detail
};

class Diagnostics {
public:
    void warn(std::string kind, std::string message, int line = 0, std::string key = {}) {
        add(Severity::Warning, std::move(kind), std::move(message), line, std::move(key));
    }
    void error(std::string kind, std::string message, int line = 0, std::string key = {}) {
        add(Severity::Error, std::move(kind), std::move(message), line, std::move(key));
    }
    void add(Severity sev, std::string kind, std::string message, int line, std::string key) {
        items_.push_back({sev, line, std::move(key), std::move(kind), std::move(message)});
    }
    void append(const Diagnostics& other) {
        items_.insert(items_.end(), other.items_.begin(), other.items_.end());
    }

    const std::vector<Diagnostic>& items() const { return items_; }
    std::size_t errorCount() const { return count(Severity::Error); }
    std::size_t warningCount() const { return count(Severity::Warning); }
    bool hasKind(const std::string& kind) const {
        for (const auto& d : items_)
            if (d.kind == kind) return true;
        return false;
    }
    std::size_t countKind(const std::string& kind) const {
        std::size_t n = 0;
        for (const auto& d : items_)
            if (d.kind == kind) ++n;
        return n;
    }
    bool empty() const { return items_.empty(); }
    void clear() { items_.clear(); }

private:
    std::size_t count(Severity s) const {
        std::size_t n = 0;
        for (const auto& d : items_)
            if (d.severity == s) ++n;
        return n;
    }
    std::vector<Diagnostic> items_;
};

}  // namespace reforce::expr

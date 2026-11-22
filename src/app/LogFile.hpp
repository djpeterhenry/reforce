#pragma once
// Log file for Dock/Finder launches (Phase 5A): everything the app writes to
// stderr (fprintf and the platform layer's messages) is copied, with a
// timestamp per line, to ~/Library/Logs/reforce/reforce.log. When stderr is a
// terminal it keeps going there too.
//
// Rotation: the log is rotated when it reaches `maxBytes` (at startup and
// while running): reforce.log -> reforce.1.log -> ... -> reforce.<keep>.log,
// the oldest dropped.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace reforce::app {

// ~/Library/Logs/reforce/reforce.log
std::filesystem::path defaultLogPath();

// Rotates `path` if it is at least `maxBytes` long; keeps `keep` old files.
// Returns true if it rotated. Pure filesystem logic (unit-tested).
bool rotateLogIfNeeded(const std::filesystem::path& path, std::uintmax_t maxBytes, int keep);

// Formats one log line with a local-time prefix ("2026-11-11 14:03:07.123 ").
std::string timestampPrefix();

class LogTee {
public:
    static constexpr std::uintmax_t kDefaultMaxBytes = 4u << 20;  // 4 MB
    static constexpr int kDefaultKeep = 3;

    // Redirects fd 2 into a pipe read by a thread that appends to `path`
    // (rotating) and, if `echo` is set, writes to the original stderr as well.
    // Returns null (stderr untouched) if the file or the pipe cannot be opened.
    static std::unique_ptr<LogTee> start(const std::filesystem::path& path, bool echo,
                                         std::uintmax_t maxBytes = kDefaultMaxBytes, int keep = kDefaultKeep);
    // Restores stderr and flushes everything still in the pipe.
    ~LogTee();

    LogTee(const LogTee&) = delete;
    LogTee& operator=(const LogTee&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    LogTee() = default;
    void run();

    std::filesystem::path path_;
    std::uintmax_t maxBytes_ = kDefaultMaxBytes;
    int keep_ = kDefaultKeep;
    bool echo_ = false;
    int savedStderr_ = -1;  // the original fd 2
    int readFd_ = -1;
    struct Thread;
    std::unique_ptr<Thread> thread_;
};

}  // namespace reforce::app

#include "app/LogFile.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>

namespace reforce::app {

namespace fs = std::filesystem;

fs::path defaultLogPath() {
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : "") / "Library" / "Logs" / "reforce" / "reforce.log";
}

namespace {

fs::path numbered(const fs::path& path, int n) {
    // reforce.log -> reforce.<n>.log
    fs::path p = path;
    p.replace_filename(path.stem().string() + "." + std::to_string(n) + path.extension().string());
    return p;
}

}  // namespace

bool rotateLogIfNeeded(const fs::path& path, std::uintmax_t maxBytes, int keep) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size < maxBytes) return false;
    if (keep < 1) {
        fs::remove(path, ec);
        return true;
    }
    fs::remove(numbered(path, keep), ec);
    for (int i = keep - 1; i >= 1; --i) {
        const fs::path from = numbered(path, i);
        if (fs::exists(from, ec)) fs::rename(from, numbered(path, i + 1), ec);
    }
    fs::rename(path, numbered(path, 1), ec);
    return !ec;
}

std::string timestampPrefix() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const int ms = int(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[40];
    const std::size_t n = std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    std::snprintf(buf + n, sizeof buf - n, ".%03d ", ms);
    return buf;
}

struct LogTee::Thread {
    std::thread t;
};

std::unique_ptr<LogTee> LogTee::start(const fs::path& path, bool echo, std::uintmax_t maxBytes, int keep) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    rotateLogIfNeeded(path, maxBytes, keep);
    {
        // Probe that the file is writable before touching stderr.
        std::FILE* f = std::fopen(path.c_str(), "a");
        if (!f) return nullptr;
        std::fclose(f);
    }
    int fds[2];
    if (::pipe(fds) != 0) return nullptr;
    std::unique_ptr<LogTee> tee(new LogTee());
    tee->path_ = path;
    tee->maxBytes_ = maxBytes;
    tee->keep_ = keep;
    tee->echo_ = echo;
    std::fflush(stderr);
    tee->savedStderr_ = ::dup(STDERR_FILENO);
    ::fcntl(tee->savedStderr_, F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    tee->readFd_ = fds[0];
    ::dup2(fds[1], STDERR_FILENO);
    ::close(fds[1]);
    tee->thread_ = std::make_unique<Thread>();
    tee->thread_->t = std::thread([t = tee.get()] { t->run(); });
    return tee;
}

void LogTee::run() {
    std::FILE* out = std::fopen(path_.c_str(), "a");
    std::string line;
    char buf[4096];
    auto flushLine = [&](bool force) {
        if (line.empty() || (!force && line.back() != '\n')) return;
        if (!out) out = std::fopen(path_.c_str(), "a");
        if (out) {
            const std::string pre = timestampPrefix();
            std::fwrite(pre.data(), 1, pre.size(), out);
            std::fwrite(line.data(), 1, line.size(), out);
            if (line.back() != '\n') std::fputc('\n', out);
            std::fflush(out);
            if (std::ftell(out) >= long(maxBytes_)) {
                std::fclose(out);
                out = nullptr;
                rotateLogIfNeeded(path_, maxBytes_, keep_);
            }
        }
        line.clear();
    };
    for (;;) {
        const ssize_t n = ::read(readFd_, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (echo_ && savedStderr_ >= 0) {
            ssize_t off = 0;
            while (off < n) {
                const ssize_t w = ::write(savedStderr_, buf + off, std::size_t(n - off));
                if (w <= 0) break;
                off += w;
            }
        }
        for (ssize_t i = 0; i < n; ++i) {
            line.push_back(buf[i]);
            if (buf[i] == '\n') flushLine(false);
        }
    }
    flushLine(true);
    if (out) std::fclose(out);
}

LogTee::~LogTee() {
    std::fflush(stderr);
    // Point fd 2 back at the original stderr: that drops the last reference
    // to the pipe's write end, so the reader sees EOF after draining it.
    if (savedStderr_ >= 0) ::dup2(savedStderr_, STDERR_FILENO);
    if (thread_ && thread_->t.joinable()) thread_->t.join();
    if (readFd_ >= 0) ::close(readFd_);
    if (savedStderr_ >= 0) ::close(savedStderr_);
}

}  // namespace reforce::app

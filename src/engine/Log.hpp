#pragma once
// Render-thread logging (Phase 5D): rlog() formats into a stack buffer and,
// once async mode is on (windowed app), hands the line to a logger thread
// through a preallocated ring, so the render thread never does I/O (stderr
// may be a pipe to the log tee or a slow terminal) and never allocates for a
// log line. Without async mode (offline renders, tests) it writes to stderr
// directly, keeping the output order of the deterministic runs.

#include <cstdint>

namespace reforce::logx {

// Starts/stops the logger thread. Stopping flushes what is queued.
void setAsync(bool on);
bool async();
// Lines dropped because the ring was full (async mode).
std::uint64_t dropped();

}  // namespace reforce::logx

namespace reforce {
// printf-style; a trailing newline is the caller's (like fprintf).
void rlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
}  // namespace reforce

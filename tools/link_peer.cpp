// reforce-link-peer: a minimal Ableton Link peer for testing reforce's Link
// support without rekordbox. Joins the session (proposing --bpm), prints the
// session state every --every seconds for --seconds seconds.
//
//   reforce-link-peer [--bpm 124] [--seconds 30] [--every 0.5] [--quiet]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "sync/LinkClock.hpp"

int main(int argc, char** argv) {
    double bpm = 124.0, seconds = 30.0, every = 0.5;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const bool v = i + 1 < argc;
        if (!std::strcmp(argv[i], "--bpm") && v) bpm = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--seconds") && v) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--every") && v) every = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else {
            std::fprintf(stderr, "usage: reforce-link-peer [--bpm B] [--seconds S] [--every E] [--quiet]\n");
            return 2;
        }
    }
    reforce::sync::LinkClock link(4.0, bpm);
    link.setEnabled(true);
    link.proposeTempo(bpm);
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        const auto s = link.sample(0.0);
        if (!quiet)
            std::printf("peers %d  bpm %.2f  beat %.3f  phase %.3f\n", s.peers, s.on ? s.bpm : bpm, s.beat, s.phase);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::duration<double>(every));
    }
    return 0;
}

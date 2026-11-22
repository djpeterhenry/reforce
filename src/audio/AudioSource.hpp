#pragma once
// Common interface for audio sources that feed the Analyzer through an SPSC
// ring buffer of mono float samples.
//
// Ownership/threading: a source is created, opened, serviced and closed by one
// owner thread (the app's render or main thread). The source's producer (the
// CoreAudio IO thread, a file feeder thread, or the owner itself in
// deterministic mode) writes ring(); the render thread reads it via
// Analyzer::consume(). state() and sampleRate() are safe from any thread.

#include <cstdint>
#include <string>

#include "audio/SpscRingBuffer.hpp"

namespace reforce::audio {

enum class SourceState : int {
    Stopped,        // not opened / closed
    Running,        // producing samples
    Reconfiguring,  // device format changed; service() will restart it
    Disconnected,   // device missing or died; service() reconnects when it returns
    Finished,       // file source reached the end (no loop)
    Error,          // see lastError()
};

const char* sourceStateName(SourceState s);

class AudioSource {
public:
    virtual ~AudioSource() = default;
    virtual SpscRingBuffer& ring() = 0;
    virtual double sampleRate() const = 0;   // of the samples in ring()
    virtual SourceState state() const = 0;
    virtual std::string description() const = 0;
    // Housekeeping (reconnects, restarts). Call from the owner thread, e.g.
    // once per frame; cheap when nothing changed.
    virtual void service() {}
};

}  // namespace reforce::audio

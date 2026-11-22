#pragma once
// Publishes a Metal texture to other apps (e.g. OBS's "Syphon Client" source)
// through a SyphonMetalServer. Implemented in src/platform/mac/SyphonPublisher.mm.

#include <memory>
#include <string>

namespace MTL { class Device; class Texture; class CommandBuffer; }

namespace reforce::platform {

class SyphonPublisher {
public:
    SyphonPublisher(MTL::Device* device, const std::string& serverName);
    ~SyphonPublisher();
    SyphonPublisher(const SyphonPublisher&) = delete;
    SyphonPublisher& operator=(const SyphonPublisher&) = delete;

    bool valid() const;
    bool hasClients() const;
    // The server's Syphon UUID (video inputs refuse it: feedback loop, Phase 5F).
    std::string uuid() const;

    // Encodes a copy of `texture` (BGRA8Unorm, top-left origin) into Syphon's
    // IOSurface on `commandBuffer`; the frame is announced to clients when the
    // command buffer completes. The caller commits the command buffer.
    void publish(MTL::Texture* texture, MTL::CommandBuffer* commandBuffer);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace reforce::platform

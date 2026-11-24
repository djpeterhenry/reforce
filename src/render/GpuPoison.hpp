#pragma once
// GPU poison mode (REFORCE_GPU_POISON=1): makes reads of undefined GPU memory show up the same way on every GPU.
//
// Metal leaves a new texture or buffer, and an attachment loaded with LoadActionDontCare, undefined. Apple Silicon
// often happens to hand back harmless leftovers, so code that reads them works here and breaks elsewhere (the Now
// Playing shadow failed only on CI's virtual GPU). With the variable set:
//   - every new texture is cleared to poison, every new buffer filled with 0xFF bytes (NaN as float, -1 as int);
//   - every DontCare color load becomes a clear to poison;
//   - poison is NaN in float formats, magenta (1, 0, 1, 1) in the others.
// Without the variable these helpers are plain newTexture / newBuffer / renderCommandEncoder calls.
//
// NaN checks: after poison::startChecks(), every float texture rendered into is remembered, and poison::check()
// (called once the GPU is idle) reads them back and reports each that holds NaN: a pass read poison (or made a NaN
// itself). The offline renderer checks after every frame, the render tests after every test case.
//
// All texture, buffer and render pass creation in src/ goes through these (tests/gpu_poison_lint.cmake).

#include <IOSurface/IOSurfaceRef.h>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <string>
#include <vector>

namespace reforce::render {

MTL::Texture* makeTexture(MTL::Device* device, MTL::TextureDescriptor* td);
MTL::Texture* makeTexture(MTL::Device* device, MTL::TextureDescriptor* td, IOSurfaceRef surface, NS::UInteger plane);
// Uninitialized buffer (newBuffer with initial bytes is defined and needs no wrapper).
MTL::Buffer* makeBuffer(MTL::Device* device, NS::UInteger length, MTL::ResourceOptions options);
MTL::RenderCommandEncoder* makeRenderEncoder(MTL::CommandBuffer* cmd, MTL::RenderPassDescriptor* pass);

namespace poison {

bool enabled();  // REFORCE_GPU_POISON set and not "0" (read once)

// Start remembering float render targets for check(). No-op unless enabled().
void startChecks();

// Reads back the float textures rendered into since the last call (the caller makes sure their GPU work has
// completed) and returns one line per texture holding NaN. Empty when nothing is wrong or checks are off.
std::vector<std::string> check();

}  // namespace poison
}  // namespace reforce::render

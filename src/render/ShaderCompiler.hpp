#pragma once
// Runtime Metal shader compilation (MTL::Device::newLibrary from source).
// Scene expressions will be turned into MSL source and compiled through here;
// the skeleton's own shaders use it too, so there is no offline .metal step.

#include <stdexcept>
#include <string>

namespace MTL { class Device; class Library; class Function; }

namespace reforce::render {

struct ShaderCompileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Returns a library the caller owns (release() it). Throws ShaderCompileError
// with the compiler log on failure.
MTL::Library* compileLibrary(MTL::Device* device, const std::string& source,
                             const std::string& label);

// Returns a function the caller owns. Throws ShaderCompileError if missing.
MTL::Function* loadFunction(MTL::Library* library, const char* name);

}  // namespace reforce::render

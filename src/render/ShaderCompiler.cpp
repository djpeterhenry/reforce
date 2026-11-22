#include "render/ShaderCompiler.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

namespace reforce::render {

MTL::Library* compileLibrary(MTL::Device* device, const std::string& source,
                             const std::string& label) {
    NS::SharedPtr<MTL::CompileOptions> options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
    options->setFastMathEnabled(true);
    options->setLanguageVersion(MTL::LanguageVersion3_0);

    NS::Error* error = nullptr;
    NS::String* src = NS::String::string(source.c_str(), NS::UTF8StringEncoding);
    MTL::Library* library = device->newLibrary(src, options.get(), &error);
    if (!library) {
        std::string log = error ? error->localizedDescription()->utf8String() : "unknown error";
        throw ShaderCompileError("Metal compile failed for '" + label + "':\n" + log);
    }
    library->setLabel(NS::String::string(label.c_str(), NS::UTF8StringEncoding));
    return library;
}

MTL::Function* loadFunction(MTL::Library* library, const char* name) {
    MTL::Function* fn = library->newFunction(NS::String::string(name, NS::UTF8StringEncoding));
    if (!fn) throw ShaderCompileError(std::string("Metal function not found: ") + name);
    return fn;
}

}  // namespace reforce::render

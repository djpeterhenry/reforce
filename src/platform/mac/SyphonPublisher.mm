#include "platform/SyphonPublisher.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <Syphon/SyphonMetalServer.h>
#import <Syphon/SyphonServerDirectory.h>

#include <cstdio>

namespace reforce::platform {

struct SyphonPublisher::Impl {
    SyphonMetalServer* server = nil;
};

SyphonPublisher::SyphonPublisher(MTL::Device* device, const std::string& serverName)
    : impl_(std::make_unique<Impl>()) {
    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)static_cast<void*>(device);
        NSString* name = [NSString stringWithUTF8String:serverName.c_str()];
        impl_->server = [[SyphonMetalServer alloc] initWithName:name device:dev options:nil];
        if (!impl_->server) {
            std::fprintf(stderr, "[reforce] Failed to create SyphonMetalServer \"%s\"\n", name.UTF8String);
        }
    }
}

SyphonPublisher::~SyphonPublisher() {
    @autoreleasepool {
        [impl_->server stop];
        impl_->server = nil;
    }
}

bool SyphonPublisher::valid() const { return impl_->server != nil; }

std::string SyphonPublisher::uuid() const {
    if (!impl_->server) return {};
    @autoreleasepool {
        id u = impl_->server.serverDescription[SyphonServerDescriptionUUIDKey];
        return [u isKindOfClass:[NSString class]] ? std::string([(NSString*)u UTF8String]) : std::string();
    }
}

bool SyphonPublisher::hasClients() const { return impl_->server && impl_->server.hasClients; }

void SyphonPublisher::publish(MTL::Texture* texture, MTL::CommandBuffer* commandBuffer) {
    if (!impl_->server || !texture || !commandBuffer) return;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)static_cast<void*>(texture);
    id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)static_cast<void*>(commandBuffer);
    // Syphon surfaces use the OpenGL convention (first row = bottom), which is
    // what GL clients such as OBS's Syphon source read. Our Metal texture has its
    // first row at the top, and flipped:NO takes Syphon's straight blit, which
    // shows up upside down in those clients. flipped:YES draws the frame
    // vertically flipped into the shared IOSurface (one small render pass).
    [impl_->server publishFrameTexture:tex
                       onCommandBuffer:cmd
                           imageRegion:NSMakeRect(0, 0, tex.width, tex.height)
                               flipped:YES];
}

}  // namespace reforce::platform

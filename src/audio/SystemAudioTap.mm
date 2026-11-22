#include "audio/SystemAudioTap.hpp"

#import <CoreAudio/AudioHardware.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>

#include <unistd.h>

#include <cstdio>

namespace reforce::audio {

namespace {

constexpr const char* kAggregateUidPrefix = "reforce.system-audio.";

std::string statusText(const char* what, OSStatus st) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s: OSStatus %d", what, static_cast<int>(st));
    return buf;
}

// reforce's own process object, to leave it out of the tap (0 if CoreAudio has none yet).
AudioObjectID ownProcessObject() {
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyTranslatePIDToProcessObject,
                                    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    pid_t pid = getpid();
    AudioObjectID obj = kAudioObjectUnknown;
    UInt32 size = sizeof obj;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, sizeof pid, &pid, &size, &obj) != noErr)
        return kAudioObjectUnknown;
    return obj;
}

}  // namespace

bool systemAudioSupported() {
    if (@available(macOS 14.2, *)) return true;
    return false;
}

bool isSystemAudioAggregateUid(const std::string& uid) { return uid.rfind(kAggregateUidPrefix, 0) == 0; }

bool SystemAudioTap::create(std::string& error) {
    destroy();
    if (@available(macOS 14.2, *)) {
        @autoreleasepool {
            NSMutableArray<NSNumber*>* exclude = [NSMutableArray array];
            if (const AudioObjectID me = ownProcessObject(); me != kAudioObjectUnknown) [exclude addObject:@(me)];
            CATapDescription* desc = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:exclude];
            desc.name = @"reforce System Audio";
            desc.privateTap = YES;
            desc.muteBehavior = CATapUnmuted;
            AudioObjectID tap = kAudioObjectUnknown;
            OSStatus st = AudioHardwareCreateProcessTap(desc, &tap);
            if (st != noErr || tap == kAudioObjectUnknown) {
                error = statusText("creating the system audio tap", st);
                return false;
            }
            tap_ = tap;

            NSString* tapUid = desc.UUID.UUIDString;
            NSString* aggUid = [NSString stringWithFormat:@"%s%@", kAggregateUidPrefix, NSUUID.UUID.UUIDString];
            NSDictionary* agg = @{
                @kAudioAggregateDeviceNameKey: @"reforce System Audio",
                @kAudioAggregateDeviceUIDKey: aggUid,
                @kAudioAggregateDeviceIsPrivateKey: @YES,
                @kAudioAggregateDeviceIsStackedKey: @NO,
                @kAudioAggregateDeviceTapAutoStartKey: @YES,
                @kAudioAggregateDeviceTapListKey: @[ @{
                    @kAudioSubTapUIDKey: tapUid,
                    @kAudioSubTapDriftCompensationKey: @YES,
                } ],
            };
            AudioObjectID dev = kAudioObjectUnknown;
            st = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)agg, &dev);
            if (st != noErr || dev == kAudioObjectUnknown) {
                error = statusText("creating the system audio device", st);
                destroy();
                return false;
            }
            aggregate_ = dev;
            aggregateUid_ = aggUid.UTF8String;
            return true;
        }
    }
    error = "System Audio needs macOS 14.2 or later";
    return false;
}

void SystemAudioTap::destroy() {
    if (@available(macOS 14.2, *)) {
        if (aggregate_ != kAudioObjectUnknown) AudioHardwareDestroyAggregateDevice(aggregate_);
        if (tap_ != kAudioObjectUnknown) AudioHardwareDestroyProcessTap(tap_);
    }
    aggregate_ = tap_ = kAudioObjectUnknown;
    aggregateUid_.clear();
}

}  // namespace reforce::audio

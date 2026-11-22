// Cocoa shell: NSApplication + one NSWindow whose content view is backed by a
// CAMetalLayer. Drives FrameHandler::onFrame from the view's CADisplayLink and,
// when that is paused (window occluded/minimized), from a fallback timer so
// that the offscreen render + Syphon output keep running.
//
// Phase 5D: the display link and the fallback timer run on a dedicated render
// thread (its own run loop), so main-thread stalls (menus, window resizes,
// full-screen Space switches, AppKit work) never delay a frame. The link is
// pinned to the simulation rate when the screen refreshes at a whole multiple
// of it (60 Hz sim on a 120 Hz ProMotion screen: 60 callbacks/s, one step
// each), and frames carry the link's ideal target timestamps, so the step
// cadence is exact. The timer only takes over after the link has been quiet
// for a while (paused: window occluded/minimized, display asleep) and paces
// itself at the simulation rate.

#include "platform/Platform.hpp"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>

using reforce::platform::DebugOptions;
using reforce::platform::FrameHandler;
using reforce::platform::FrameInfo;
using reforce::platform::FrameSource;
using reforce::platform::KeyEvent;
using reforce::platform::WindowConfig;
using reforce::platform::WindowKey;
using reforce::platform::AudioInputItem;
using reforce::platform::MenuItem;

// Platform messages go to stderr like the rest of the app (the log file tees
// stderr; NSLog would only reach the unified log when not on a terminal).
static void RFLog(NSString* format, ...) NS_FORMAT_FUNCTION(1, 2);
static void RFLog(NSString* format, ...) {
    va_list args;
    va_start(args, format);
    NSString* s = [[NSString alloc] initWithFormat:format arguments:args];
    va_end(args);
    std::fprintf(stderr, "%s\n", s.UTF8String);
}

static NSString* const kRFFullScreenKey = @"RFWindowFullScreen";

// ---------------------------------------------------------------------------
// RFMetalView
// ---------------------------------------------------------------------------

@interface RFMetalView : NSView
@property(nonatomic, readonly) CAMetalLayer* metalLayer;
@property(nonatomic, assign) FrameHandler* handler;
@end

@implementation RFMetalView {
    id<MTLDevice> _device;
}

- (instancetype)initWithFrame:(NSRect)frame device:(id<MTLDevice>)device {
    self = [super initWithFrame:frame];
    if (self) {
        _device = device;
        self.wantsLayer = YES;  // layer-backed, layer comes from makeBackingLayer
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    }
    return self;
}

- (CALayer*)makeBackingLayer {
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = _device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.opaque = YES;  // the frame's alpha is for Syphon (reforce.Syphon.Alpha), never the window
    layer.maximumDrawableCount = 3;
    layer.displaySyncEnabled = YES;
    layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    return layer;
}

- (CAMetalLayer*)metalLayer { return (CAMetalLayer*)self.layer; }

- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }
// The normal arrow over the picture: the stream is sent over Syphon, so viewers never see the cursor (Peter,
// 2026-11-21; a blank cursor over the view since 2026-11-17 is gone).
// Double-click in the picture enters full screen (Peter, 2026-11-12). In full
// screen a double-click does nothing: leave with Esc or Cmd+Ctrl+F.
- (void)mouseDown:(NSEvent*)event {
    if (event.clickCount == 2 && self.window && !(self.window.styleMask & NSWindowStyleMaskFullScreen)) {
        RFLog(@"[reforce] double-click: entering full screen");
        [self.window toggleFullScreen:nil];
        return;
    }
    [super mouseDown:event];
}

- (void)updateDrawableSize {
    CGFloat scale = self.window ? self.window.backingScaleFactor
                                : NSScreen.mainScreen.backingScaleFactor;
    NSSize size = self.bounds.size;
    self.metalLayer.contentsScale = scale;
    self.metalLayer.drawableSize = CGSizeMake(MAX(1.0, size.width * scale), MAX(1.0, size.height * scale));
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self updateDrawableSize];
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self updateDrawableSize];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self updateDrawableSize];
}

static KeyEvent keyEventFrom(NSEvent* event) {
    NSEventModifierFlags mods = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    KeyEvent key;
    key.chars = event.charactersIgnoringModifiers.lowercaseString.UTF8String ?: "";
    key.shift = (mods & NSEventModifierFlagShift) != 0;
    key.command = (mods & NSEventModifierFlagCommand) != 0;
    key.control = (mods & NSEventModifierFlagControl) != 0;
    key.option = (mods & NSEventModifierFlagOption) != 0;
    key.keyCode = event.keyCode;
    return key;
}

// Every key goes through reforce::platform::routeWindowKey (unit-tested).
// Returns YES when handled.
- (BOOL)routeKey:(NSEvent*)event keyEquivalent:(BOOL)equivalent {
    const KeyEvent key = keyEventFrom(event);
    const BOOL fullscreen = (self.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    switch (reforce::platform::routeWindowKey(key, fullscreen, equivalent)) {
        case WindowKey::Unhandled: return NO;
        case WindowKey::Consumed: return YES;
        case WindowKey::ToggleFullscreen:
        case WindowKey::ExitFullscreen: [self.window toggleFullScreen:nil]; return YES;
        case WindowKey::App: return self.handler && self.handler->onKey(key);
    }
    return NO;
}

- (BOOL)performKeyEquivalent:(NSEvent*)event {
    // Cmd+Ctrl+F (also in the View menu, which handles it when we are not first responder).
    if (self.window.firstResponder == self && [self routeKey:event keyEquivalent:YES]) return YES;
    return [super performKeyEquivalent:event];
}

- (void)keyDown:(NSEvent*)event {
    if ([self routeKey:event keyEquivalent:NO]) return;
    [super keyDown:event];
}

@end

// ---------------------------------------------------------------------------
// RFAppDelegate
// ---------------------------------------------------------------------------

@interface RFAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate>
- (instancetype)initWithConfig:(const WindowConfig&)config
                        device:(id<MTLDevice>)device
                       handler:(FrameHandler*)handler
                         debug:(const DebugOptions&)debug;
- (void)closeWindow;
@end

@implementation RFAppDelegate {
    WindowConfig _config;
    DebugOptions _debug;
    FrameHandler* _handler;
    id<MTLDevice> _device;
    NSWindow* _window;
    RFMetalView* _view;
    CADisplayLink* _displayLink;  // render thread
    NSTimer* _fallbackTimer;      // render thread
    id<NSObject> _activity;
    CFTimeInterval _lastFrameTime;      // render thread
    CFTimeInterval _lastLinkTime;       // render thread: last display link callback
    double _pinnedRate;                 // render thread: link's preferred rate (0 = every refresh)
    NSThread* _renderThread;
    dispatch_semaphore_t _renderThreadDone;
    std::atomic<bool> _renderStop;
    CAMetalLayer* _layer;               // set once on main before the render thread starts
    std::atomic<bool> _visible;         // updated on main from window notifications
    std::atomic<double> _screenMaxFps;  // updated on main
    BOOL _shuttingDown;
    NSMenu* _audioMenu;
    NSMenu* _videoMenu;
}

- (instancetype)initWithConfig:(const WindowConfig&)config
                        device:(id<MTLDevice>)device
                       handler:(FrameHandler*)handler
                         debug:(const DebugOptions&)debug {
    self = [super init];
    if (self) {
        _config = config;
        _debug = debug;
        _handler = handler;
        _device = device;
    }
    return self;
}

static NSMenuItem* RFAddItem(NSMenu* menu, NSString* title, SEL action, NSString* key,
                             NSEventModifierFlags mods, id target) {
    NSMenuItem* item = [menu addItemWithTitle:title action:action keyEquivalent:key];
    item.keyEquivalentModifierMask = mods;
    item.target = target;
    return item;
}

- (void)buildMenu {
    NSString* appName = @"reforce";
    const NSEventModifierFlags cmd = NSEventModifierFlagCommand;
    NSMenu* bar = [[NSMenu alloc] init];

    // App menu.
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:appName];
    RFAddItem(appMenu, [@"About " stringByAppendingString:appName], @selector(showAbout:), @"", 0, self);
    [appMenu addItem:[NSMenuItem separatorItem]];
    RFAddItem(appMenu, [@"Hide " stringByAppendingString:appName], @selector(hide:), @"h", cmd, nil);
    RFAddItem(appMenu, @"Hide Others", @selector(hideOtherApplications:), @"h",
              cmd | NSEventModifierFlagOption, nil);
    RFAddItem(appMenu, @"Show All", @selector(unhideAllApplications:), @"", 0, nil);
    [appMenu addItem:[NSMenuItem separatorItem]];
    RFAddItem(appMenu, [@"Quit " stringByAppendingString:appName], @selector(terminate:), @"q", cmd, nil);
    appItem.submenu = appMenu;
    [bar addItem:appItem];

    // View.
    NSMenuItem* viewItem = [[NSMenuItem alloc] init];
    NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    RFAddItem(viewMenu, @"Toggle Full Screen", @selector(toggleFullScreen:), @"f",
              cmd | NSEventModifierFlagControl, nil);
    RFAddItem(viewMenu, @"Open Control Panel\u2026", @selector(openControlPanel:), @"k", cmd, self);
    [viewMenu addItem:[NSMenuItem separatorItem]];
    RFAddItem(viewMenu, @"Window Shows Visuals Only (for Lights)", @selector(toggleWindowVisualsOnly:), @"", 0, self);
    RFAddItem(viewMenu, @"Room Lights (Nanoleaf)", @selector(toggleLights:), @"", 0, self);
    viewItem.submenu = viewMenu;
    [bar addItem:viewItem];

    // Scene: the same key events the window's hotkeys send (FrameHandler::onKey).
    // Shown with their (unmodified) keys; Shift+key still reaches the view.
    NSMenuItem* sceneItem = [[NSMenuItem alloc] init];
    NSMenu* sceneMenu = [[NSMenu alloc] initWithTitle:@"Scene"];
    struct { NSString* title; NSString* key; } const scene[] = {
        {@"Next WaveShape", @"w"}, {@"Next ColorMap", @"c"}, {@"Next FlowField", @"f"},
        {nil, nil},
        {@"Hold / Release All", @"h"},
        {nil, nil},
        {@"Start Particle", @"p"}, {@"Start Sprite", @"s"},
        {nil, nil},
        {@"Look A/B (G-Force exact / Done right)", @"b"},
    };
    for (const auto& e : scene) {
        if (!e.title) {
            [sceneMenu addItem:[NSMenuItem separatorItem]];
            continue;
        }
        NSMenuItem* item = RFAddItem(sceneMenu, e.title, @selector(sceneKey:), e.key, 0, self);
        item.representedObject = e.key;
    }
    sceneItem.submenu = sceneMenu;
    [bar addItem:sceneItem];

    // Audio: input devices, rebuilt each time the menu opens.
    NSMenuItem* audioItem = [[NSMenuItem alloc] init];
    _audioMenu = [[NSMenu alloc] initWithTitle:@"Audio"];
    _audioMenu.delegate = self;
    _audioMenu.autoenablesItems = NO;
    audioItem.submenu = _audioMenu;
    [bar addItem:audioItem];

    // Video (Phase 5F): sources and colour-layer settings, rebuilt each time it opens.
    NSMenuItem* videoItem = [[NSMenuItem alloc] init];
    _videoMenu = [[NSMenu alloc] initWithTitle:@"Video"];
    _videoMenu.delegate = self;
    _videoMenu.autoenablesItems = NO;
    videoItem.submenu = _videoMenu;
    [bar addItem:videoItem];

    NSMenuItem* windowItem = [[NSMenuItem alloc] init];
    NSMenu* windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    RFAddItem(windowMenu, @"Minimize", @selector(performMiniaturize:), @"m", cmd, nil);
    windowItem.submenu = windowMenu;
    [bar addItem:windowItem];

    NSApp.mainMenu = bar;
    NSApp.windowsMenu = windowMenu;
}

// --- menu actions -------------------------------------------------------------

- (void)showAbout:(id)sender {
    NSString* git = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"";
    NSDictionary* opts = @{
        NSAboutPanelOptionApplicationName : @"reforce",
        NSAboutPanelOptionVersion : git,
        NSAboutPanelOptionCredits : [[NSAttributedString alloc]
            initWithString:@"A music visualizer that plays G-Force scene files.\n"
                           @"Independent project; not affiliated with SoundSpectrum."],
    };
    [NSApp orderFrontStandardAboutPanelWithOptions:opts];
    [NSApp activate];
}

- (void)openControlPanel:(id)sender {
    if (_config.controlPanelUrl.empty()) return;
    NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:_config.controlPanelUrl.c_str()]];
    if (url) [NSWorkspace.sharedWorkspace openURL:url];
}

- (void)sceneKey:(NSMenuItem*)sender {
    KeyEvent key;
    key.chars = ((NSString*)sender.representedObject).UTF8String ?: "";
    if (_handler) _handler->onKey(key);
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(openControlPanel:)) return !_config.controlPanelUrl.empty();
    if (item.action == @selector(toggleLights:)) {
        const int st = _handler ? _handler->lightsMenuState() : -1;
        item.state = st == 1 ? NSControlStateValueOn : NSControlStateValueOff;
        return st >= 0;
    }
    if (item.action == @selector(toggleWindowVisualsOnly:)) {
        item.state = _handler && _handler->windowVisualsOnly() ? NSControlStateValueOn : NSControlStateValueOff;
        return _handler != nullptr;
    }
    return YES;
}

// View > Room Lights (reforce.Lights.Enabled, docs/lights.md): the Nanoleaf panels follow the visuals.
- (void)toggleLights:(NSMenuItem*)sender {
    if (!_handler) return;
    const int st = _handler->lightsMenuState();
    if (st >= 0) _handler->selectVideoMenu(st == 1 ? "videoset:lights:off" : "videoset:lights:on");
}

// View > Window Shows Visuals Only (reforce.Window.VisualsOnly): no backdrop, dancers or text in the window, for
// screen-mirrored room lights; Syphon keeps everything.
- (void)toggleWindowVisualsOnly:(NSMenuItem*)sender {
    if (!_handler) return;
    _handler->selectVideoMenu(_handler->windowVisualsOnly() ? "videoset:windowvisuals:off" : "videoset:windowvisuals:on");
}

- (void)fillVideoMenu:(NSMenu*)menu items:(const std::vector<MenuItem>&)items {
    for (const MenuItem& it : items) {
        if (it.separatorBefore) [menu addItem:[NSMenuItem separatorItem]];
        NSMenuItem* m = [menu addItemWithTitle:[NSString stringWithUTF8String:it.title.c_str()]
                                        action:it.id.empty() ? nil : @selector(selectVideoItem:)
                                 keyEquivalent:@""];
        m.target = self;
        m.enabled = it.enabled && (!it.id.empty() || !it.children.empty());
        m.state = it.checked ? NSControlStateValueOn : NSControlStateValueOff;
        if (!it.id.empty()) m.representedObject = [NSString stringWithUTF8String:it.id.c_str()];
        if (!it.children.empty()) {
            NSMenu* sub = [[NSMenu alloc] initWithTitle:m.title];
            sub.autoenablesItems = NO;
            [self fillVideoMenu:sub items:it.children];
            m.submenu = sub;
        }
    }
}

- (void)selectVideoItem:(NSMenuItem*)sender {
    NSString* id_ = sender.representedObject;
    if (_handler && id_) _handler->selectVideoMenu(id_.UTF8String);
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
    if (menu == _videoMenu) {
        [menu removeAllItems];
        const std::vector<MenuItem> items = _handler ? _handler->videoMenu() : std::vector<MenuItem>{};
        [self fillVideoMenu:menu items:items];
        return;
    }
    if (menu != _audioMenu) return;
    [menu removeAllItems];
    const std::vector<AudioInputItem> items = _handler ? _handler->audioInputs() : std::vector<AudioInputItem>{};
    if (items.empty()) {
        NSMenuItem* none = [menu addItemWithTitle:@"No live input (audio file or --no-audio)" action:nil
                                    keyEquivalent:@""];
        none.enabled = NO;
        return;
    }
    for (const AudioInputItem& it : items) {
        if (it.separatorBefore) [menu addItem:[NSMenuItem separatorItem]];
        NSMenuItem* m = [menu addItemWithTitle:[NSString stringWithUTF8String:it.title.c_str()]
                                        action:@selector(selectAudioInput:)
                                 keyEquivalent:@""];
        m.target = self;
        m.enabled = YES;
        m.state = it.checked ? NSControlStateValueOn : NSControlStateValueOff;
        m.representedObject = [NSString stringWithUTF8String:it.id.c_str()];
    }
}

- (void)selectAudioInput:(NSMenuItem*)sender {
    NSString* id_ = sender.representedObject;
    if (_handler && id_) _handler->selectAudioInput(id_.UTF8String);
}

- (void)applicationWillFinishLaunching:(NSNotification*)note {
    // Quit Apple event (Dock "Quit", `tell application "reforce" to quit`):
    // terminate the same way Cmd+Q does. AppKit's own handler would report
    // "User canceled" to the sender, because applicationShouldTerminate
    // cancels AppKit's exit() to unwind through main() instead.
    [NSAppleEventManager.sharedAppleEventManager setEventHandler:self
                                                     andSelector:@selector(handleQuitEvent:withReplyEvent:)
                                                   forEventClass:kCoreEventClass
                                                      andEventID:kAEQuitApplication];
    // SIGTERM / SIGINT / SIGHUP (`kill`, Ctrl-C in a terminal): the same clean quit, so what reforce drives outside
    // itself is put back (room lights, docs/lights.md). A second signal while quitting exits at once.
    static dispatch_source_t sources[3];
    const int sigs[3] = {SIGTERM, SIGINT, SIGHUP};
    for (int i = 0; i < 3; ++i) {
        ::signal(sigs[i], SIG_IGN);
        sources[i] = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, uintptr_t(sigs[i]), 0, dispatch_get_main_queue());
        __weak __typeof(self) weakSelf = self;
        dispatch_source_set_event_handler(sources[i], ^{
            __typeof(self) strongSelf = weakSelf;
            if (!strongSelf || strongSelf->_shuttingDown) _exit(1);
            RFLog(@"[reforce] signal received: quitting");
            [NSApp terminate:nil];
        });
        dispatch_resume(sources[i]);
    }
}

- (void)handleQuitEvent:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)reply {
    [NSApp terminate:nil];
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    [self buildMenu];

    NSRect rect = NSMakeRect(0, 0, _config.width, _config.height);
    _window = [[NSWindow alloc]
        initWithContentRect:rect
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = [NSString stringWithUTF8String:_config.title.c_str()];
    _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    _window.backgroundColor = NSColor.blackColor;
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    _view = [[RFMetalView alloc] initWithFrame:rect device:_device];
    _view.handler = _handler;
    _window.contentView = _view;
    [_window makeFirstResponder:_view];
    // Frame (size, position, screen) persists in the bundle's defaults.
    NSString* autosave = _config.background ? @"" : [NSString stringWithUTF8String:_config.autosaveName.c_str()];
    BOOL restored = autosave.length && [_window setFrameUsingName:autosave];
    // Never come back screen-sized (a frame saved while full screen by older builds).
    if (restored) {
        const NSRect vis = (_window.screen ?: NSScreen.mainScreen).visibleFrame;
        if (_window.frame.size.width >= vis.size.width && _window.frame.size.height >= vis.size.height * 0.95) {
            [_window setContentSize:NSMakeSize(_config.width, _config.height)];
            restored = NO;
        }
    }
    if (!restored) [_window center];
    if (autosave.length) [_window setFrameAutosaveName:autosave];
    if (_config.background) {
        [_window orderBack:nil];  // behind everything; never key, never brought forward
    } else {
        [_window makeKeyAndOrderFront:nil];
        // reforce always opens windowed (Peter, 2026-11-12): full screen is never
        // restored from the last session, only entered by hand (Cmd+Ctrl+F) or
        // with the explicit reforce.Window.StartFullScreen = 1.
        [NSUserDefaults.standardUserDefaults removeObjectForKey:kRFFullScreenKey];
    }
    if (_config.startFullScreen && !_config.background) {
        RFLog(@"[reforce] window: entering full screen (reforce.Window.StartFullScreen)");
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self->_window && !(self->_window.styleMask & NSWindowStyleMaskFullScreen))
                [self->_window toggleFullScreen:nil];
        });
    }

    // Keep rendering at full rate when occluded/minimized: opt out of App Nap
    // and timer coalescing for the lifetime of the app.
    _activity = [NSProcessInfo.processInfo
        beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                          reason:@"Real-time rendering and Syphon output"];

    _layer = _view.metalLayer;
    [self updateScreenState];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(screenChanged:)
                                               name:NSWindowDidChangeScreenNotification
                                             object:_window];
    // The display link comes from the view (it follows the view's screen) and
    // is scheduled on the render thread's run loop.
    _displayLink = [_view displayLinkWithTarget:self selector:@selector(displayLinkFired:)];
    _renderThreadDone = dispatch_semaphore_create(0);
    _renderStop = false;
    _renderThread = [[NSThread alloc] initWithTarget:self selector:@selector(renderThreadMain:) object:nil];
    _renderThread.name = @"reforce.render";
    _renderThread.qualityOfService = NSQualityOfServiceUserInteractive;
    [_renderThread start];

    if (!_config.background) [NSApp activate];
    [self scheduleDebugActions];

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        RFLog(@"[reforce] window: %@, on active space: %d, screen %.0fx%.0f @%.0fx",
              [self windowVisible] ? @"visible" : @"not visible", (int)self->_window.isOnActiveSpace,
              self->_window.screen.frame.size.width, self->_window.screen.frame.size.height,
              self->_window.backingScaleFactor);
    });
}

- (void)scheduleDebugActions {
    __weak RFAppDelegate* weakSelf = self;
    auto after = [](double seconds, dispatch_block_t block) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), block);
    };
    if (_debug.minimizeAfter > 0) {
        after(_debug.minimizeAfter, ^{
            RFLog(@"[reforce] debug: miniaturizing window");
            RFAppDelegate* s = weakSelf;
            if (s) [s->_window miniaturize:nil];
        });
    }
    if (_debug.fullscreenAfter > 0) {
        after(_debug.fullscreenAfter, ^{
            RFLog(@"[reforce] debug: toggling fullscreen");
            RFAppDelegate* s = weakSelf;
            if (s) [s->_window toggleFullScreen:nil];
        });
    }
    if (_debug.quitAfter > 0) {
        after(_debug.quitAfter, ^{
            RFLog(@"[reforce] debug: quitting");
            [NSApp terminate:nil];
        });
    }
}

- (BOOL)windowVisible {
    return !_window.isMiniaturized && (_window.occlusionState & NSWindowOcclusionStateVisible) != 0;
}

// --- render thread ------------------------------------------------------------

- (void)renderThreadMain:(id)unused {
    @autoreleasepool {
        NSRunLoop* loop = NSRunLoop.currentRunLoop;
        _pinnedRate = 0;
        _lastLinkTime = CACurrentMediaTime();  // give the link a moment before the timer steps in
        [self applyPinnedRate];
        [_displayLink addToRunLoop:loop forMode:NSRunLoopCommonModes];
        // Fallback: fires at the simulation rate (a repeating timer keeps its
        // schedule, so no drift) and renders only while the link is quiet.
        _fallbackTimer = [NSTimer timerWithTimeInterval:1.0 / (_config.targetFrameRate > 0 ? _config.targetFrameRate : 60.0)
                                                 target:self
                                               selector:@selector(fallbackFired:)
                                               userInfo:nil
                                                repeats:YES];
        _fallbackTimer.tolerance = 0;
        [loop addTimer:_fallbackTimer forMode:NSRunLoopCommonModes];
        while (!_renderStop.load()) {
            @autoreleasepool {
                [loop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.25]];
            }
        }
        [_displayLink invalidate];
        _displayLink = nil;
        [_fallbackTimer invalidate];
        _fallbackTimer = nil;
    }
    dispatch_semaphore_signal(_renderThreadDone);
}

- (void)stopRenderThread {
    if (!_renderThread) return;
    _renderStop = true;
    // Wake the run loop so it notices.
    [self performSelector:@selector(noop) onThread:_renderThread withObject:nil waitUntilDone:NO];
    dispatch_semaphore_wait(_renderThreadDone, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    _renderThread = nil;
}

- (void)noop {
}

// Main thread: screen properties the render thread reads.
- (void)updateScreenState {
    NSScreen* screen = _window.screen ?: NSScreen.mainScreen;
    const NSInteger fps = screen.maximumFramesPerSecond;
    _screenMaxFps = fps > 0 ? double(fps) : 60.0;
    _visible = [self windowVisible];
}

- (void)screenChanged:(NSNotification*)note {
    [self updateScreenState];
    RFLog(@"[reforce] window moved to a screen with max %.0f Hz", _screenMaxFps.load());
    if (_renderThread) [self performSelector:@selector(applyPinnedRate) onThread:_renderThread withObject:nil waitUntilDone:NO];
}

// Render thread: pin the display link to the simulation rate when the screen
// refreshes at a whole multiple of it.
- (void)applyPinnedRate {
    if (!_displayLink) return;
    const double maxFps = _screenMaxFps.load();
    const double sim = _config.targetFrameRate;
    double rate = 0;
    if (sim > 0 && maxFps > 0) {
        const double m = maxFps / sim;
        if (m >= 1 && std::abs(m - std::round(m)) < 0.01) rate = sim;
    }
    if (rate > 0) {
        _displayLink.preferredFrameRateRange = CAFrameRateRangeMake(float(rate), float(rate), float(rate));
    } else {
        _displayLink.preferredFrameRateRange = CAFrameRateRangeDefault;
    }
    _pinnedRate = rate;
    RFLog(@"[reforce] display link: screen max %.0f Hz, %s", maxFps,
          rate > 0 ? [NSString stringWithFormat:@"pinned to %.0f Hz (simulation rate)", rate].UTF8String
                   : "every refresh (simulation rate is not a whole fraction of it)");
}

- (void)renderFrom:(FrameSource)source hostTime:(double)host period:(double)period rate:(double)rate {
    if (_shuttingDown || _renderStop.load()) return;
    @autoreleasepool {
        _lastFrameTime = CACurrentMediaTime();
        FrameInfo info;
        info.hostTime = host;
        info.layer = (CA::MetalLayer*)(__bridge void*)_layer;
        info.windowVisible = _visible.load();
        info.source = source;
        info.refreshPeriod = period;
        info.callbackRate = rate;
        _handler->onFrame(info);
    }
}

- (void)displayLinkFired:(CADisplayLink*)link {
    _lastLinkTime = CACurrentMediaTime();
    const double maxFps = _screenMaxFps.load();
    const double period = 1.0 / (maxFps > 0 ? maxFps : 60.0);
    // Ideal (vsync-aligned) time of this frame: no callback jitter.
    const double host = link.targetTimestamp > 0 ? link.targetTimestamp : _lastLinkTime;
    double rate = _pinnedRate;
    if (rate <= 0) {
        const double d = link.targetTimestamp - link.timestamp;
        rate = d > 0 ? 1.0 / d : 1.0 / period;
    }
    [self renderFrom:FrameSource::DisplayLink hostTime:host period:period rate:rate];
}

- (void)fallbackFired:(NSTimer*)timer {
    // Only when the display link has gone quiet (it pauses while the window is
    // not on screen or the display sleeps): a late link callback must not be
    // doubled by a timer frame.
    const double now = CACurrentMediaTime();
    if (now - _lastLinkTime < 0.25) return;
    const double sim = _config.targetFrameRate > 0 ? _config.targetFrameRate : 60.0;
    if (now - _lastFrameTime < 0.5 / sim) return;  // never two frames in one period
    [self renderFrom:FrameSource::Timer hostTime:now period:1.0 / sim rate:sim];
}

// --- window state logging ---------------------------------------------------

- (void)windowDidChangeOcclusionState:(NSNotification*)note {
    _visible = [self windowVisible];
    RFLog(@"[reforce] window %@ (occlusionState=0x%lx)",
          [self windowVisible] ? @"visible" : @"occluded", (unsigned long)_window.occlusionState);
}
- (void)windowDidMiniaturize:(NSNotification*)note {
    _visible = [self windowVisible];
    RFLog(@"[reforce] window minimized");
}
- (void)windowDidDeminiaturize:(NSNotification*)note {
    _visible = [self windowVisible];
    RFLog(@"[reforce] window restored");
}
- (void)windowDidEnterFullScreen:(NSNotification*)note {
    RFLog(@"[reforce] entered fullscreen");
}
- (void)windowDidExitFullScreen:(NSNotification*)note {
    RFLog(@"[reforce] exited fullscreen");
}

// --- shutdown -----------------------------------------------------------------

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app { return YES; }

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)app {
    // Return control to runApplication() (and from there to main()) instead
    // of letting AppKit call exit(), so C++ objects are destroyed normally.
    if (_shuttingDown) return NSTerminateCancel;
    RFLog(@"[reforce] quit requested");
    if (_window) {
        // The windowed frame only: a full-screen frame would reopen screen-sized.
        if (!(_window.styleMask & NSWindowStyleMaskFullScreen)) [_window saveFrameUsingName:_window.frameAutosaveName];
    }
    [NSUserDefaults.standardUserDefaults synchronize];
    _shuttingDown = YES;
    [self stopRenderThread];
    if (_activity) {
        [NSProcessInfo.processInfo endActivity:_activity];
        _activity = nil;
    }
    [NSApp stop:nil];
    // stop: only takes effect after the next event is processed.
    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                       location:NSZeroPoint
                                  modifierFlags:0
                                      timestamp:0
                                   windowNumber:0
                                        context:nil
                                        subtype:0
                                          data1:0
                                          data2:0];
    [NSApp postEvent:wake atStart:NO];
    return NSTerminateCancel;
}

- (void)closeWindow {
    _window.delegate = nil;
    [_window orderOut:nil];
    _window = nil;
    _view = nil;
}

@end

// ---------------------------------------------------------------------------

namespace reforce::platform {

std::string bundleResourcesDir() {
    @autoreleasepool {
        NSBundle* b = NSBundle.mainBundle;
        if (![b.bundlePath.pathExtension isEqualToString:@"app"] || !b.resourcePath) return {};
        return b.resourcePath.UTF8String;
    }
}

bool activateRunningInstance(bool allowMultiple) {
    @autoreleasepool {
        NSString* bundleId = NSBundle.mainBundle.bundleIdentifier;
        if (!bundleId.length) return false;
        NSArray<NSRunningApplication*>* apps = [NSRunningApplication runningApplicationsWithBundleIdentifier:bundleId];
        std::vector<RunningInstance> running;
        for (NSRunningApplication* a in apps) running.push_back({(int)a.processIdentifier, (bool)a.terminated});
        const int pid = instanceToActivate((int)getpid(), running, allowMultiple);
        if (!pid) return false;
        NSRunningApplication* other = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
        std::fprintf(stderr, "[reforce] already running (pid %d); activating it and exiting\n", pid);
        if (!other) return true;
        [other activateWithOptions:NSApplicationActivateAllWindows];
        // A background process may not be allowed to activate another app
        // (cooperative activation); asking LaunchServices to open the running
        // app's bundle brings it to the front reliably.
        if (other.bundleURL) {
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            NSWorkspaceOpenConfiguration* cfg = [NSWorkspaceOpenConfiguration configuration];
            cfg.activates = YES;
            [NSWorkspace.sharedWorkspace openApplicationAtURL:other.bundleURL
                                                configuration:cfg
                                            completionHandler:^(NSRunningApplication*, NSError*) {
                                                dispatch_semaphore_signal(done);
                                            }];
            dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
        }
        return true;
    }
}

int runApplication(const WindowConfig& config, MTL::Device* device, FrameHandler& handler,
                   const DebugOptions& debug) {
    @autoreleasepool {
        // We provide our own "Toggle Full Screen" item; stop AppKit adding another.
        [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"NSFullScreenMenuItemEverywhere"];
        NSWindow.allowsAutomaticWindowTabbing = NO;  // no "Show Tab Bar" items in View

        NSApplication* app = [NSApplication sharedApplication];
        // --background: an accessory app (no Dock icon, no menu bar) that never takes focus from a live instance.
        app.activationPolicy =
            config.background ? NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular;
        id<MTLDevice> dev = (__bridge id<MTLDevice>)static_cast<void*>(device);
        RFAppDelegate* delegate = [[RFAppDelegate alloc] initWithConfig:config
                                                                 device:dev
                                                                handler:&handler
                                                                  debug:debug];
        app.delegate = delegate;
        [app run];

        handler.onShutdown();
        [delegate closeWindow];
        app.delegate = nil;
    }
    return 0;
}

}  // namespace reforce::platform

// The virtual display on macOS: a private CGVirtualDisplay, captured with
// ScreenCaptureKit like any other display.
//
// ## Private API, held at arm's length
//
// CGVirtualDisplay, CGVirtualDisplayDescriptor, CGVirtualDisplayMode and
// CGVirtualDisplaySettings are undocumented CoreGraphics classes. Their shape
// here is the one DeskPad, Chromium's test harness and BetterDisplay use, and
// the one the decklink-display spike proved on macOS 26.4.
//
// They are reached through NSClassFromString and protocols, never by naming
// the classes in code. Naming them would put `_OBJC_CLASS_$_CGVirtualDisplay`
// in the binary's imports, and on a macOS without the class dyld would refuse
// to launch Frame Ferret at all — every node lost for the sake of one. Looked
// up by name, a missing class is a sentence on this node and nothing else.
//
// ## What was measured before this was written (decklink-display spike)
//
// - The display gets its own CGDirectDisplayID, an NSScreen, a name in System
//   Settings and system_profiler, and an arrangement position.
// - Fractional rates are folded: a 59.94 mode runs at 60.0000 (60.03 Hz over
//   60 s). 50 is exact. planVirtualDisplay() asks for the whole rate and warns.
// - macOS may pick another listed mode as active, so exactly one is listed and
//   the active one is checked after the display is published.
// - Capture loses frames when the machine is loaded (WindowServer skips
//   composites), whatever the capture API. Under light load it is clean.

#include "sources/virtual_display.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <cmath>
#include <cstdio>

#include "app/main_loop.h"
#include "sources/screen_capture.h"

// ---- the private classes' shape, as protocols so nothing links to them ----

@protocol FerretVirtualDisplayDescriptor <NSObject>
@property(retain, nonatomic) dispatch_queue_t queue;
@property(retain, nonatomic) NSString* name;
@property(nonatomic) unsigned int maxPixelsHigh;
@property(nonatomic) unsigned int maxPixelsWide;
@property(nonatomic) CGSize sizeInMillimeters;
@property(nonatomic) unsigned int serialNum;
@property(nonatomic) unsigned int productID;
@property(nonatomic) unsigned int vendorID;
@property(copy, nonatomic) void (^terminationHandler)(id, id);
@end

@protocol FerretVirtualDisplayMode <NSObject>
- (instancetype)initWithWidth:(unsigned int)width
                       height:(unsigned int)height
                  refreshRate:(double)refreshRate;
@end

@protocol FerretVirtualDisplaySettings <NSObject>
@property(retain, nonatomic) NSArray* modes;
@property(nonatomic) unsigned int hiDPI;
@end

@protocol FerretVirtualDisplay <NSObject>
@property(readonly, nonatomic) unsigned int displayID;
- (instancetype)initWithDescriptor:(id)descriptor;
- (BOOL)applySettings:(id)settings;
@end

namespace ferret {
namespace {

// 'SW' and 'FF'. Any values do; fixed ones keep the display recognisable as
// the same product across runs, which is what macOS keys its memory on.
constexpr unsigned int kVendorId = 0x5357;
constexpr unsigned int kProductId = 0x4646;

bool displayIsActive(CGDirectDisplayID id) {
  CGDirectDisplayID list[32];
  uint32_t count = 0;
  if (CGGetActiveDisplayList(32, list, &count) != kCGErrorSuccess) return false;
  for (uint32_t i = 0; i < count; ++i) {
    if (list[i] == id) return true;
  }
  return false;
}

/// The active mode, as "1920x1080 @ 60.00 Hz", for messages.
std::string describeMode(CGDirectDisplayID id) {
  CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
  if (!mode) return "no mode";
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%zux%zu @ %.2f Hz",
                CGDisplayModeGetPixelWidth(mode),
                CGDisplayModeGetPixelHeight(mode),
                CGDisplayModeGetRefreshRate(mode));
  CGDisplayModeRelease(mode);
  return buf;
}

bool modeMatches(CGDisplayModeRef mode, const VirtualDisplayPlan& plan) {
  return mode &&
         static_cast<int>(CGDisplayModeGetPixelWidth(mode)) == plan.width &&
         static_cast<int>(CGDisplayModeGetPixelHeight(mode)) == plan.height &&
         std::fabs(CGDisplayModeGetRefreshRate(mode) - plan.refreshHz) < 0.5;
}

/// Makes the planned mode the active one if macOS chose another.
bool ensureMode(CGDirectDisplayID id, const VirtualDisplayPlan& plan,
                std::string& error) {
  CGDisplayModeRef active = CGDisplayCopyDisplayMode(id);
  const bool already = modeMatches(active, plan);
  if (active) CGDisplayModeRelease(active);
  if (already) return true;

  CFArrayRef modes = CGDisplayCopyAllDisplayModes(id, nullptr);
  CGDisplayModeRef wanted = nullptr;
  for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); ++i) {
    auto mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
    if (modeMatches(mode, plan)) {
      wanted = mode;
      break;
    }
  }
  bool ok = false;
  if (wanted) {
    CGDisplayConfigRef config = nullptr;
    if (CGBeginDisplayConfiguration(&config) == kCGErrorSuccess) {
      CGConfigureDisplayWithDisplayMode(config, id, wanted, nullptr);
      ok = CGCompleteDisplayConfiguration(config, kCGConfigureForSession) ==
           kCGErrorSuccess;
    }
  }
  if (modes) CFRelease(modes);

  if (!ok) {
    error = "macOS made the virtual display " + describeMode(id) +
            " and would not switch it to " + std::to_string(plan.width) + "x" +
            std::to_string(plan.height) + " @ " +
            std::to_string(plan.refreshHz) + " Hz";
  }
  return ok;
}

class VirtualDisplaySource final : public Source {
 public:
  // `::id` because the Source interface's id() hides the Objective-C type.
  VirtualDisplaySource(NodeConfig config, ::id display, ::id activity,
                       std::shared_ptr<std::atomic<bool>> terminated,
                       std::unique_ptr<Source> capture)
      : config_(std::move(config)),
        display_(display),
        activity_(activity),
        terminated_(std::move(terminated)),
        capture_(std::move(capture)) {}

  ~VirtualDisplaySource() override {
    // The capture first: a stream whose display has gone stops with an
    // error, and there is no reason to provoke one.
    capture_.reset();
    display_ = nil;
    if (activity_) [NSProcessInfo.processInfo endActivity:activity_];
    activity_ = nil;
  }

  const std::string& id() const override { return config_.id; }

  /// False once macOS has torn the display down (a window server restart
  /// does it), so the sinks it feeds go black with a reason rather than
  /// holding the last frame.
  bool connected() const override {
    return !terminated_->load() && capture_->connected();
  }

  bool poll(unsigned timeoutMs,
            const std::function<void(const VideoFrame&)>& onVideo) override {
    if (terminated_->load()) return false;
    return capture_->poll(timeoutMs, onVideo);
  }

 private:
  NodeConfig config_;
  ::id display_ = nil;
  ::id activity_ = nil;
  std::shared_ptr<std::atomic<bool>> terminated_;
  std::unique_ptr<Source> capture_;
};

}  // namespace

std::unique_ptr<Source> makeVirtualDisplaySource(const NodeConfig& config,
                                                 std::string& error) {
  VirtualDisplayPlan plan;
  if (!planVirtualDisplay(config, &plan, error)) return nullptr;

  // Permission before the display: without Screen Recording the capture would
  // be black, and a display nobody can see out of is worse than none.
  if (!ScreenCapture::permitted()) {
    error = ScreenCapture::permissionReason();
    return nullptr;
  }

  Class descriptorClass = NSClassFromString(@"CGVirtualDisplayDescriptor");
  Class modeClass = NSClassFromString(@"CGVirtualDisplayMode");
  Class settingsClass = NSClassFromString(@"CGVirtualDisplaySettings");
  Class displayClass = NSClassFromString(@"CGVirtualDisplay");
  if (!descriptorClass || !modeClass || !settingsClass || !displayClass) {
    error =
        "this macOS has no CGVirtualDisplay. The virtual display uses private "
        "CoreGraphics classes, and this release has removed or renamed them; "
        "there is no public replacement to fall back to";
    return nullptr;
  }

  auto terminated = std::make_shared<std::atomic<bool>>(false);
  const std::string nodeId = config.id;

  id<FerretVirtualDisplayDescriptor> descriptor = [[descriptorClass alloc] init];
  // Its own queue, not the main one: the termination handler must run even
  // while the main thread is busy, and nothing else is delivered on it.
  descriptor.queue =
      dispatch_queue_create("ferret.virtual-display", DISPATCH_QUEUE_SERIAL);
  descriptor.name = [NSString stringWithUTF8String:plan.name.c_str()];
  descriptor.maxPixelsWide = static_cast<unsigned int>(plan.width);
  descriptor.maxPixelsHigh = static_cast<unsigned int>(plan.height);
  descriptor.sizeInMillimeters = CGSizeMake(plan.widthMm, plan.heightMm);
  descriptor.serialNum = plan.serial;
  descriptor.vendorID = kVendorId;
  descriptor.productID = kProductId;
  descriptor.terminationHandler = ^(id, id) {
    terminated->store(true);
    std::fprintf(stderr, "virtual display %s: macOS removed it\n",
                 nodeId.c_str());
  };

  id<FerretVirtualDisplay> display =
      [(id<FerretVirtualDisplay>)[displayClass alloc]
          initWithDescriptor:descriptor];
  if (!display) {
    error = "macOS refused to create the virtual display";
    return nullptr;
  }

  id<FerretVirtualDisplaySettings> settings = [[settingsClass alloc] init];
  settings.hiDPI = 0;
  settings.modes = @[ [(id<FerretVirtualDisplayMode>)[modeClass alloc]
      initWithWidth:static_cast<unsigned int>(plan.width)
             height:static_cast<unsigned int>(plan.height)
        refreshRate:plan.refreshHz] ];
  if (![display applySettings:settings]) {
    error = "macOS would not give the virtual display a " +
            std::to_string(plan.width) + "x" + std::to_string(plan.height) +
            " @ " + std::to_string(plan.refreshHz) + " Hz mode";
    return nullptr;
  }

  const CGDirectDisplayID displayId = display.displayID;

  // The window server publishes the display asynchronously and in stages: it
  // is in the active list before it has a mode (CGDisplayCopyDisplayMode is
  // NULL for a while — measured, not assumed), and in ScreenCaptureKit's list
  // later still. The spike waited a second and a half for all of it.
  auto hasMode = [](CGDirectDisplayID d) {
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(d);
    if (!mode) return false;
    CGDisplayModeRelease(mode);
    return true;
  };
  for (int i = 0; i < 50 && !(displayIsActive(displayId) && hasMode(displayId));
       ++i) {
    waitServicingMainLoop(0.1);
  }
  if (!displayIsActive(displayId) || !hasMode(displayId)) {
    error = "the virtual display was created but macOS never made it active";
    return nullptr;
  }
  if (!ensureMode(displayId, plan, error)) return nullptr;

  // Under App Nap a process with no window is throttled, and its timers and
  // the window server's composites for its display slip with it.
  id activity = [NSProcessInfo.processInfo
      beginActivityWithOptions:NSActivityUserInitiated |
                               NSActivityLatencyCritical
                        reason:@"Frame Ferret virtual display"];

  std::unique_ptr<Source> capture;
  std::string captureError;
  for (int i = 0; i < 20 && !capture; ++i) {
    captureError.clear();
    capture = makeDisplaySourceForId(config, displayId, plan.refreshHz,
                                     /*showsCursor=*/false, captureError);
    if (!capture) waitServicingMainLoop(0.25);
  }
  if (!capture) {
    [NSProcessInfo.processInfo endActivity:activity];
    error = "the virtual display exists but could not be captured: " +
            captureError;
    return nullptr;
  }

  std::fprintf(stderr, "virtual display %s: \"%s\" is display %u, %s\n",
               config.id.c_str(), plan.name.c_str(), displayId,
               describeMode(displayId).c_str());
  return std::make_unique<VirtualDisplaySource>(
      config, display, activity, std::move(terminated), std::move(capture));
}

}  // namespace ferret

// Screen, window and application capture on macOS, via ScreenCaptureKit.
//
// ScreenCaptureKit is the supported route on 12.3 and later, and by now the
// only one worth using: CGDisplayStream is deprecated and
// CGWindowListCreateImage cannot see other applications' windows at all under
// current privacy rules.
//
// ## The permission trap
//
// Without Screen Recording permission, `SCStream` starts **successfully** and
// then delivers black frames forever. No error, no callback, no log line. An
// operator sees a black output and every counter looks healthy.
//
// So permission is checked up front by asking for shareable content: without
// the grant that returns an error or an empty display list, and the node fails
// to build with the permission named as the reason. That turns an invisible
// fault into a sentence.
//
// A CLI binary asks for the grant on behalf of its *parent* — the terminal —
// so during development the permission that matters is the terminal's.

#include "sources/screen_capture.h"


#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <atomic>
#include <cstdio>
#include <mutex>

#include "core/convert.h"

namespace ferret {
namespace {

/// Fetches shareable content synchronously. ScreenCaptureKit is entirely
/// asynchronous, and everything here needs an answer before it can decide
/// anything, so this is the one place that waits.
SCShareableContent* fetchContent(NSError** outError, double timeoutSeconds) {
  __block SCShareableContent* result = nil;
  __block NSError* failure = nil;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);

  [SCShareableContent
      getShareableContentWithCompletionHandler:^(SCShareableContent* content,
                                                 NSError* error) {
        result = content;
        failure = error;
        dispatch_semaphore_signal(done);
      }];

  const dispatch_time_t deadline = dispatch_time(
      DISPATCH_TIME_NOW, static_cast<int64_t>(timeoutSeconds * NSEC_PER_SEC));
  if (dispatch_semaphore_wait(done, deadline) != 0) {
    if (outError) *outError = nil;
    return nil;
  }
  if (outError) *outError = failure;
  return result;
}

std::string toStd(NSString* s) {
  return s ? std::string(s.UTF8String ? s.UTF8String : "") : std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// The capture source itself.
// ---------------------------------------------------------------------------
namespace {
class ScreenSource;
}
}  // namespace ferret

/// The stream output delegate. Objective-C, so it lives outside the namespace.
@interface FerretCaptureDelegate : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) void* owner;
@end

namespace ferret {
namespace {

class ScreenSource final : public Source {
 public:
  ScreenSource(NodeConfig config, SCStream* stream,
               FerretCaptureDelegate* delegate)
      : config_(std::move(config)), stream_(stream), delegate_(delegate) {
    stride_ = tightStrideBytes(PixelFormat::bgra8, config_.width);
  }

  ~ScreenSource() override {
    if (stream_) {
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      [stream_ stopCaptureWithCompletionHandler:^(NSError*) {
        dispatch_semaphore_signal(done);
      }];
      dispatch_semaphore_wait(
          done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
      stream_ = nil;
    }
    delegate_.owner = nullptr;
    delegate_ = nil;
  }

  const std::string& id() const override { return config_.id; }

  /// True once a frame has arrived. A capture that has been started but has
  /// produced nothing — the shape a missing permission takes — reports false,
  /// so the router emits black with a reason rather than a frozen first frame.
  bool connected() const override { return frames_.load() > 0; }

  bool poll(unsigned,
            const std::function<void(const VideoFrame&)>& onVideo) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasFrame_) return false;
    if (onVideo) onVideo(latest_.frame());
    return true;
  }

  /// Called from ScreenCaptureKit's queue.
  void onSampleBuffer(CMSampleBufferRef sample) {
    CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
    if (!image) return;

    CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    const auto* base =
        static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image));
    const size_t bytesPerRow = CVPixelBufferGetBytesPerRow(image);
    const size_t width = CVPixelBufferGetWidth(image);
    const size_t height = CVPixelBufferGetHeight(image);

    if (base && width > 0 && height > 0) {
      VideoFrame f;
      f.width = static_cast<int>(width);
      f.height = static_cast<int>(height);
      f.strideBytes = static_cast<int>(bytesPerRow);
      f.data = base;
      f.format = PixelFormat::bgra8;
      // A desktop is sRGB and full range. Declaring it narrow is the exact
      // mistake that makes everything downstream washed out.
      f.colour = height >= 720 ? ColourSpace::bt709 : ColourSpace::bt601;
      f.range = QuantRange::full;
      f.rate = config_.rate;

      std::lock_guard<std::mutex> lock(mutex_);
      latest_.assign(f);
      hasFrame_ = true;
    }

    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    ++frames_;
  }

 private:
  NodeConfig config_;
  SCStream* stream_ = nil;
  FerretCaptureDelegate* delegate_ = nil;
  int stride_ = 0;
  std::atomic<uint64_t> frames_{0};
  std::mutex mutex_;
  FrameBuffer latest_;
  bool hasFrame_ = false;
};

}  // namespace
}  // namespace ferret

@implementation FerretCaptureDelegate

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
  if (type != SCStreamOutputTypeScreen) return;
  if (!CMSampleBufferIsValid(sampleBuffer)) return;
  // ScreenCaptureKit sends "idle" samples with no image when nothing on screen
  // has changed. Those are not frames and must not be counted as ones, or a
  // static desktop reads as a live source delivering at full rate.
  NSArray* attachments = (__bridge NSArray*)CMSampleBufferGetSampleAttachmentsArray(
      sampleBuffer, false);
  NSDictionary* info = attachments.firstObject;
  if (info) {
    NSNumber* status = info[SCStreamFrameInfoStatus];
    if (status && status.intValue != SCFrameStatusComplete) return;
  }

  auto* owner = static_cast<ferret::ScreenSource*>(self.owner);
  if (owner) owner->onSampleBuffer(sampleBuffer);
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
  std::fprintf(stderr, "screen capture stopped: %s\n",
               error.localizedDescription.UTF8String);
}

@end

namespace ferret {
namespace {

/// Builds and starts a stream for an already-chosen filter.
std::unique_ptr<Source> startCapture(const NodeConfig& config,
                                     SCContentFilter* filter,
                                     std::string& error) {
  SCStreamConfiguration* streamConfig = [[SCStreamConfiguration alloc] init];
  streamConfig.width = static_cast<size_t>(config.width);
  streamConfig.height = static_cast<size_t>(config.height);
  streamConfig.pixelFormat = kCVPixelFormatType_32BGRA;
  streamConfig.showsCursor = YES;
  streamConfig.queueDepth = 5;
  // The rational, exactly — a capture at "50" that is really 50.0000001 drifts
  // against every other clock in the program.
  streamConfig.minimumFrameInterval =
      CMTimeMake(config.rate.den, static_cast<int32_t>(config.rate.num));

  // A region of interest is a source rect, so the crop happens before anything
  // is copied — it is not a full capture that is then thrown away.
  if (config.cropW > 0 && config.cropH > 0) {
    streamConfig.sourceRect =
        CGRectMake(config.cropX, config.cropY, config.cropW, config.cropH);
    streamConfig.scalesToFit = YES;
  }

  FerretCaptureDelegate* delegate = [[FerretCaptureDelegate alloc] init];
  NSError* streamError = nil;
  SCStream* stream = [[SCStream alloc] initWithFilter:filter
                                        configuration:streamConfig
                                             delegate:delegate];
  if (!stream) {
    error = "SCStream could not be created";
    return nullptr;
  }

  dispatch_queue_t queue =
      dispatch_queue_create("ferret.capture", DISPATCH_QUEUE_SERIAL);
  if (![stream addStreamOutput:delegate
                          type:SCStreamOutputTypeScreen
            sampleHandlerQueue:queue
                         error:&streamError]) {
    error = "addStreamOutput failed: " +
            toStd(streamError.localizedDescription);
    return nullptr;
  }

  auto source = std::make_unique<ScreenSource>(config, stream, delegate);
  delegate.owner = source.get();

  __block NSError* startError = nil;
  dispatch_semaphore_t started = dispatch_semaphore_create(0);
  [stream startCaptureWithCompletionHandler:^(NSError* e) {
    startError = e;
    dispatch_semaphore_signal(started);
  }];
  if (dispatch_semaphore_wait(
          started, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
    error = "the capture did not start within five seconds";
    return nullptr;
  }
  if (startError) {
    error = "startCapture failed: " + toStd(startError.localizedDescription);
    return nullptr;
  }

  return source;
}

}  // namespace

// ---------------------------------------------------------------------------

bool ScreenCapture::supported() { return true; }
std::string ScreenCapture::unsupportedReason() { return {}; }

bool ScreenCapture::permitted() {
  NSError* error = nil;
  SCShareableContent* content = fetchContent(&error, 5.0);
  return content != nil && content.displays.count > 0;
}

std::string ScreenCapture::permissionReason() {
  if (permitted()) return {};
  return "Screen Recording permission has not been granted, so ScreenCaptureKit "
         "reports no displays. Without it a capture starts successfully and "
         "delivers black frames forever with no error at all — which is why "
         "this is checked here. Grant it in System Settings > Privacy & "
         "Security > Screen Recording, to whichever application launched "
         "Frame Ferret (the terminal, if run from one), then start it again.";
}

std::vector<ScreenCapture::Display> ScreenCapture::listDisplays() {
  std::vector<Display> out;
  SCShareableContent* content = fetchContent(nullptr, 5.0);
  if (!content) return out;
  for (SCDisplay* d in content.displays) {
    Display item;
    item.id = d.displayID;
    item.width = static_cast<int>(d.width);
    item.height = static_cast<int>(d.height);
    item.name = "Display " + std::to_string(d.displayID);
    out.push_back(std::move(item));
  }
  return out;
}

std::vector<ScreenCapture::Window> ScreenCapture::listWindows() {
  std::vector<Window> out;
  SCShareableContent* content = fetchContent(nullptr, 5.0);
  if (!content) return out;
  for (SCWindow* w in content.windows) {
    if (!w.title || w.title.length == 0) continue;
    Window item;
    item.id = w.windowID;
    item.title = toStd(w.title);
    item.application = toStd(w.owningApplication.applicationName);
    item.width = static_cast<int>(w.frame.size.width);
    item.height = static_cast<int>(w.frame.size.height);
    out.push_back(std::move(item));
  }
  return out;
}

std::vector<std::string> ScreenCapture::listApplications() {
  std::vector<std::string> out;
  SCShareableContent* content = fetchContent(nullptr, 5.0);
  if (!content) return out;
  for (SCRunningApplication* a in content.applications) {
    const std::string name = toStd(a.applicationName);
    if (!name.empty()) out.push_back(name);
  }
  return out;
}

std::unique_ptr<Source> makeDisplaySource(const NodeConfig& config,
                                          std::string& error) {
  SCShareableContent* content = fetchContent(nullptr, 5.0);
  if (!content || content.displays.count == 0) {
    error = ScreenCapture::permissionReason();
    return nullptr;
  }

  int index = 0;
  if (!config.target.empty()) {
    index = std::atoi(config.target.c_str());
  }
  if (index < 0 || index >= static_cast<int>(content.displays.count)) {
    error = "no display " + config.target + " — this machine has " +
            std::to_string(content.displays.count);
    return nullptr;
  }

  SCDisplay* display = content.displays[index];
  SCContentFilter* filter =
      [[SCContentFilter alloc] initWithDisplay:display
                              excludingWindows:@[]];
  return startCapture(config, filter, error);
}

std::unique_ptr<Source> makeWindowSource(const NodeConfig& config,
                                         std::string& error) {
  SCShareableContent* content = fetchContent(nullptr, 5.0);
  if (!content) {
    error = ScreenCapture::permissionReason();
    return nullptr;
  }
  if (config.target.empty()) {
    error = "a window source needs a \"target\" — part of the window's title";
    return nullptr;
  }

  NSString* wanted = [NSString stringWithUTF8String:config.target.c_str()];
  for (SCWindow* w in content.windows) {
    if (!w.title) continue;
    if ([w.title rangeOfString:wanted
                       options:NSCaseInsensitiveSearch].location ==
        NSNotFound) {
      continue;
    }
    SCContentFilter* filter =
        [[SCContentFilter alloc] initWithDesktopIndependentWindow:w];
    return startCapture(config, filter, error);
  }

  error = "no window matching \"" + config.target +
          "\" — run `frame-ferret windows` to list what is open";
  return nullptr;
}

std::unique_ptr<Source> makeApplicationSource(const NodeConfig& config,
                                              std::string& error) {
  SCShareableContent* content = fetchContent(nullptr, 5.0);
  if (!content || content.displays.count == 0) {
    error = ScreenCapture::permissionReason();
    return nullptr;
  }
  if (config.target.empty()) {
    error = "an application source needs a \"target\" — part of its name";
    return nullptr;
  }

  NSString* wanted = [NSString stringWithUTF8String:config.target.c_str()];
  NSMutableArray<SCRunningApplication*>* matches = [NSMutableArray array];
  for (SCRunningApplication* a in content.applications) {
    if (!a.applicationName) continue;
    if ([a.applicationName rangeOfString:wanted
                                 options:NSCaseInsensitiveSearch].location !=
        NSNotFound) {
      [matches addObject:a];
    }
  }
  if (matches.count == 0) {
    error = "no running application matching \"" + config.target + "\"";
    return nullptr;
  }

  SCContentFilter* filter =
      [[SCContentFilter alloc] initWithDisplay:content.displays[0]
                         includingApplications:matches
                              exceptingWindows:@[]];
  return startCapture(config, filter, error);
}

}  // namespace ferret

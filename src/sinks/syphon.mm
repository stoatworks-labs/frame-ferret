// Syphon server, macOS.
//
// Ported from oxbow's `src/io/syphon.mm`, which is verified end to end against
// two consumers that are not our code: WebLinked's `tools/syphon_probe.mm`
// (which links Resolume Arena's bundled Syphon 5) and OBS's own `syphon-input`
// source, which lists the server and renders its colour bars in the right
// order — that last part being what proves BGRA is not channel-swapped.
//
// The frame path skips both of Syphon's renderers. Their job is to get a GL or
// Metal texture into a shared surface; Frame Ferret already holds CPU pixels,
// so this writes the IOSurface directly:
//
//     newSurfaceForWidth:height:options:   (SyphonSubclassing, BGRA8)
//     IOSurfaceLock -> copy rows -> IOSurfaceUnlock
//     publish                              (SyphonSubclassing)
//
// ## The server MUST be created on the main thread
//
// `SyphonServerBase` registers for the announce-request notification in
// `-init`, and NSDistributedNotificationCenter delivers those on the **main**
// run loop. A private CFRunLoop thread is not good enough, and this was
// measured, not assumed: on its own run-loop thread the server is well-formed —
// right name, right UUID, SyphonSurfaceTypeIOSurface — announces itself, and is
// invisible to every consumer.
//
// That is the worst shape the failure could take. The server answers its
// opening announce, so a consumer already running finds it and one started
// afterwards never does. It survives every short test and appears on the night.
//
// Frame Ferret's main thread waits in `cmdRun`, so that wait goes through
// `waitServicingMainLoop` — see app/main_loop.h. Publishing does not need the
// main thread; oxbow publishes from its frame thread against a main-thread
// server and that is the path verified against Resolume and OBS.

#include "sinks/syphon.h"

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Syphon/SyphonServerBase.h>
#import <Syphon/SyphonSubclassing.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/convert.h"

/// The two SyphonSubclassing hooks this sink needs, and nothing else. A
/// subclass rather than the category on a bare SyphonServerBase, so that "what
/// we actually use of Syphon" is one short interface instead of a grep.
@interface FerretSyphonServer : SyphonServerBase
/// A BGRA8 IOSurface of this size. Retained; the caller CFReleases it. Syphon
/// caches one internally and returns the same surface while the dimensions are
/// unchanged.
- (IOSurfaceRef)newSurfaceForWidth:(size_t)width height:(size_t)height;
/// Announces that the surface holds a new frame.
- (void)publishFrame;
@end

@implementation FerretSyphonServer

- (IOSurfaceRef)newSurfaceForWidth:(size_t)width height:(size_t)height {
  return [self newSurfaceForWidth:width height:height options:nil];
}

- (void)publishFrame {
  [self publish];
}

@end

namespace ferret {
namespace {

/// Runs `block` on the main thread and waits for it.
///
/// The isMainThread test is not an optimisation: `dispatch_sync` to the main
/// queue *from* the main thread deadlocks outright, and both cases are real
/// here — `selftest` drives the engine from a worker while a future
/// single-threaded caller could drive it from main.
void runOnMain(void (^block)(void)) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
}

class SyphonSink final : public Sink {
 public:
  explicit SyphonSink(NodeConfig config) : config_(std::move(config)) {
    name_ = config_.target.empty() ? config_.id : config_.target;
  }

  ~SyphonSink() override {
    // Detach under the lock, tear down outside it: teardown marshals to the
    // main thread, and holding the lock across that would let a busy run loop
    // block a frame thread inside send().
    FerretSyphonServer* server = nil;
    IOSurfaceRef surface = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      std::swap(server, server_);
      std::swap(surface, surface_);
    }
    if (surface) CFRelease(surface);
    if (server) {
      runOnMain(^{
        [server stop];
      });
    }
  }

  const std::string& id() const override { return config_.id; }

  /// BGRA only. The shared surface is BGRA8 by definition, so anything else is
  /// a conversion the router should plan explicitly rather than one hidden in
  /// here.
  std::vector<PixelFormat> preferredFormats() const override {
    return {PixelFormat::bgra8};
  }

  void send(const VideoFrame& frame) override {
    if (!frame.data || frame.width <= 0 || frame.height <= 0) return;
    if (frame.format != PixelFormat::bgra8) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureSurface(frame.width, frame.height)) return;

    // Syphon's own advice: with nobody attached there is no point copying 8 MB
    // fifty times a second.
    if (![server_ hasClients]) {
      ++skipped_;
      return;
    }

    if (IOSurfaceLock(surface_, 0, nullptr) != kIOReturnSuccess) return;

    auto* destination =
        static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surface_));
    const size_t destinationStride = IOSurfaceGetBytesPerRow(surface_);
    const size_t sourceStride = static_cast<size_t>(frame.strideBytes);
    // IOSurface pads its rows to its own alignment, which is not the frame's,
    // so this is row by row rather than one memcpy.
    const size_t rowBytes = std::min(sourceStride, destinationStride);
    for (int y = 0; y < frame.height; ++y) {
      std::memcpy(destination + static_cast<size_t>(y) * destinationStride,
                  frame.data + static_cast<size_t>(y) * sourceStride, rowBytes);
    }
    IOSurfaceUnlock(surface_, 0, nullptr);

    [server_ publishFrame];
    ++published_;
  }

  void sendBlack() override {
    // Publishing real black rather than going quiet. A Syphon server that
    // stops publishing keeps its last frame on screen in every consumer, so
    // silence here is a frozen picture — exactly what this program's invariant
    // exists to prevent.
    const int w = config_.width > 0 ? config_.width : 1920;
    const int h = config_.height > 0 ? config_.height : 1080;
    const int stride = tightStrideBytes(PixelFormat::bgra8, w);
    const size_t bytes = static_cast<size_t>(stride) * h;

    if (black_.size() != bytes) {
      black_.assign(bytes, 0);
      fillBlack(PixelFormat::bgra8, w, h, stride, QuantRange::full,
                black_.data());
    }

    VideoFrame f;
    f.width = w;
    f.height = h;
    f.strideBytes = stride;
    f.data = black_.data();
    f.format = PixelFormat::bgra8;
    f.colour = ColourSpace::bt709;
    f.range = QuantRange::full;
    f.rate = config_.rate;
    send(f);
  }

 private:
  bool ensureSurface(int width, int height) {
    if (server_ != nil && surface_ && width == width_ && height == height_) {
      return true;
    }

    if (surface_) {
      CFRelease(surface_);
      surface_ = nullptr;
    }

    __block FerretSyphonServer* server = server_;
    __block IOSurfaceRef surface = nullptr;
    NSString* serverName = [NSString stringWithUTF8String:name_.c_str()];
    const size_t surfaceWidth = static_cast<size_t>(width);
    const size_t surfaceHeight = static_cast<size_t>(height);

    runOnMain(^{
      if (server == nil) {
        server = [[FerretSyphonServer alloc] initWithName:serverName
                                                  options:nil];
      }
      if (server != nil) {
        surface = [server newSurfaceForWidth:surfaceWidth
                                      height:surfaceHeight];
      }
    });

    // One line on stderr, the first time only. This is the moment that can
    // fail invisibly — a Syphon server that exists but cannot be discovered
    // looks exactly like one that was never created — so it is reported even
    // when it works, and it is the line to ask an operator for.
    if (!reported_) {
      reported_ = true;
      if (server != nil && surface) {
        std::fprintf(stderr, "syphon: publishing \"%s\" %dx%d\n", name_.c_str(),
                     width, height);
      } else {
        std::fprintf(stderr, "syphon: could not start server \"%s\" (%s)\n",
                     name_.c_str(), server == nil ? "server" : "surface");
      }
    }

    if (server == nil || !surface) return false;

    server_ = server;
    surface_ = surface;
    width_ = width;
    height_ = height;
    return true;
  }

  NodeConfig config_;
  std::string name_;
  std::mutex mutex_;
  FerretSyphonServer* server_ = nil;
  IOSurfaceRef surface_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  std::vector<uint8_t> black_;
  int64_t published_ = 0;
  int64_t skipped_ = 0;
  bool reported_ = false;
};

}  // namespace

bool SyphonRuntime::supported() { return true; }
std::string SyphonRuntime::unsupportedReason() { return {}; }

std::unique_ptr<Sink> makeSyphonSink(const NodeConfig& config,
                                     std::string& error) {
  if (config.id.empty()) {
    error = "a Syphon server needs a node id";
    return nullptr;
  }
  // Creation cannot fail here: the server is made when the first frame arrives
  // and its size is known. A name clash is not an error — Syphon disambiguates
  // by process.
  (void)error;
  return std::make_unique<SyphonSink>(config);
}

}  // namespace ferret

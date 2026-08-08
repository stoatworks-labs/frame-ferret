#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/node.h"
#include "app/router.h"
#include "core/frame.h"

namespace ferret {

/// Runs the frame loop: poll every source, ask the router what each sink
/// should do, do it.
///
/// One thread drives everything. Not because concurrency would be wrong, but
/// because the ordering guarantee — every sink sees exactly one frame per tick,
/// in a defined order — is the thing that makes the plan-every-sink invariant
/// observable. A thread per sink would make "did every output get a frame this
/// tick?" unanswerable.
class Engine {
 public:
  Engine();
  ~Engine();

  /// Takes ownership. Must be called before `start`.
  void addSource(std::unique_ptr<Source> source, PixelFormat nativeFormat);
  void addSink(std::unique_ptr<Sink> sink);

  Router& router() { return router_; }

  /// The rate the loop is paced at. Sources produce at their own rates; this
  /// is the rate sinks are served at.
  void setRate(Rate rate) { rate_ = rate; }
  Rate rate() const { return rate_; }

  bool start(std::string& error);
  void stop();
  bool running() const { return running_.load(); }

  struct Counters {
    uint64_t ticks = 0;
    uint64_t framesDelivered = 0;
    uint64_t blackDelivered = 0;
    uint64_t conversions = 0;
    uint64_t lateTicks = 0;   ///< deadline already passed when we got there
    double measuredFps = 0.0;
  };
  Counters counters() const;

  /// Which direction a node was actually built as. A transport kind can be
  /// either, and the config decides — so the control API must ask rather than
  /// infer from the kind, or an NDI receiver is rendered as a sink as well and
  /// the crosspoint grows a row nothing can ever feed.
  bool hasSource(const std::string& id) const {
    return sourceIndex_.find(id) != sourceIndex_.end();
  }
  bool hasSink(const std::string& id) const {
    return sinkIndex_.find(id) != sinkIndex_.end();
  }

  /// Route reasons from the most recent plan, keyed by sink id. Empty string
  /// means the sink is receiving video. Surfaced by the control API so an
  /// operator sees *why* an output is black.
  std::map<std::string, std::string> sinkReasons() const;

 private:
  void loop();
  void serveTick(int64_t tick);

  struct SourceEntry {
    std::unique_ptr<Source> source;
    PixelFormat nativeFormat = PixelFormat::unknown;
    FrameBuffer latest;
    bool hasFrame = false;
  };

  Router router_;
  std::vector<SourceEntry> sources_;
  std::vector<std::unique_ptr<Sink>> sinks_;
  std::map<std::string, size_t> sourceIndex_;
  std::map<std::string, size_t> sinkIndex_;

  Rate rate_{50, 1};
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};

  // Scratch reused every tick. Reallocating a 1080p conversion buffer 50 times
  // a second is a measurable share of the frame budget and a reliable source of
  // jitter under memory pressure.
  std::vector<uint8_t> convertScratch_;

  mutable std::mutex countersMutex_;
  Counters counters_;
  std::map<std::string, std::string> reasons_;
};

}  // namespace ferret

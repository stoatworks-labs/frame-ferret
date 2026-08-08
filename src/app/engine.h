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

  /// Takes ownership. Safe **while running**: the node is queued and applied
  /// at the top of the next tick, before any source is polled.
  ///
  /// Queued rather than locked because the frame loop walks these vectors every
  /// tick and a mutex on that path would be held for the whole of serving every
  /// sink. A queue drained at one defined point costs nothing per tick and
  /// makes "when does this take effect" answerable: the next frame, never
  /// half-way through one.
  void addSource(std::unique_ptr<Source> source, PixelFormat nativeFormat);
  void addSink(std::unique_ptr<Sink> sink);

  /// Removes a node by id, source or sink. Queued the same way.
  ///
  /// Removing a source clears every route pointing at it, so the sinks it fed
  /// go black **with a reason** on the next tick rather than being skipped —
  /// the invariant holds across a reconfiguration exactly as it does across a
  /// lost signal.
  void removeNode(const std::string& id);

  /// Whether `id` is known, counting nodes still queued. A caller that has just
  /// added a node and immediately asks would otherwise be told it does not
  /// exist.
  bool knows(const std::string& id) const;

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
    uint64_t audioFramesDelivered = 0;
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

    /// Audio taken from this source this tick, if any.
    ///
    /// Taken **once per source per tick** and then handed to every sink routed
    /// to it. `Source::takeAudio()` is destructive — it moves the pending
    /// frame out — so calling it per sink would give the audio to whichever
    /// sink happened to be served first and silence to the rest, which on a
    /// two-output show is a fault nobody would think to look for.
    std::unique_ptr<AudioFrame> audio;
  };

  /// A queued change, applied between ticks.
  struct Pending {
    enum class What { addSource, addSink, remove } what;
    std::string id;
    std::unique_ptr<Source> source;
    std::unique_ptr<Sink> sink;
    PixelFormat nativeFormat = PixelFormat::unknown;
  };

  /// Applies every queued change. Called from the frame thread only, at the
  /// top of a tick.
  void applyPending();

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

  mutable std::mutex pendingMutex_;
  std::vector<Pending> pending_;
  /// Ids that exist or are queued, so `knows()` can answer before a drain.
  std::vector<std::string> announced_;

  mutable std::mutex countersMutex_;
  Counters counters_;
  std::map<std::string, std::string> reasons_;
};

}  // namespace ferret

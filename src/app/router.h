#pragma once

#include <map>
#include <string>
#include <vector>

#include "app/node.h"

namespace ferret {

/// What the router decided one sink should do this tick.
struct RouteAction {
  std::string sinkId;
  std::string sourceId;  ///< Empty when the sink is to emit black.

  enum class What {
    black,    ///< No source, no signal, or muted. Emit black — never skip.
    copy,     ///< Source format is one the sink takes natively.
    convert,  ///< A pixel conversion is required.
  };
  What what = What::black;

  /// The format to hand the sink. Equals the source format for `copy`.
  PixelFormat targetFormat = PixelFormat::unknown;

  /// Why black, when it is black. Surfaced verbatim in the control API so an
  /// operator looking at a black output is told which of the four reasons it
  /// is, rather than having to guess.
  std::string reason;
};

/// The crosspoint. Each sink is fed by exactly one source, switchable live;
/// one source may feed any number of sinks. Same mental model as a broadcast
/// router, and as srt-router, which is where the shape came from.
///
/// This class is deliberately free of I/O, threads and GPU work so that the
/// routing decisions are testable on their own. It is the only place that
/// decides what a sink does.
class Router {
 public:
  void addSource(const std::string& id, PixelFormat nativeFormat);
  void addSink(const std::string& id, std::vector<PixelFormat> accepts);

  /// Removes a node. Removing a source also **clears every route pointing at
  /// it** — leaving them would give sinks a route to something that no longer
  /// exists, and `plan()` would fall through to its defensive branch every
  /// tick rather than saying plainly that the source is gone.
  ///
  /// Returns false if the id is unknown, so a caller can tell "removed" from
  /// "was never there".
  bool removeSource(const std::string& id);
  bool removeSink(const std::string& id);

  bool hasSource(const std::string& id) const {
    return sources_.find(id) != sources_.end();
  }
  bool hasSink(const std::string& id) const {
    return sinks_.find(id) != sinks_.end();
  }

  /// Points a sink at a source. An empty `sourceId` clears the route. Returns
  /// false if either id is unknown — a typo'd route is rejected rather than
  /// stored, so the crosspoint never contains a route that cannot fire.
  bool route(const std::string& sinkId, const std::string& sourceId,
             std::string& error);

  std::string routedSource(const std::string& sinkId) const;

  /// Live connection state, pushed in by the frame loop each tick.
  void setSourceConnected(const std::string& id, bool connected);

  /// The global mute. Mutes to black; it does *not* tear down routes, so
  /// unmuting restores the previous crosspoint exactly.
  void setMuted(bool muted) { muted_ = muted; }
  bool muted() const { return muted_; }

  /// One entry per sink, every tick, whatever the routing or connection state
  /// says. Never fewer.
  ///
  /// This is the invariant the whole program rests on. A sink that stops
  /// emitting is not a quiet sink — a UVC device that stops gets dropped by
  /// the host app, an SDI output that stops has to be re-locked downstream,
  /// and a Syphon server that stops disappears from every consumer's menu. So
  /// an unrouted sink, a disconnected source and a global mute all produce a
  /// `black` action rather than an absent one.
  std::vector<RouteAction> plan() const;

  size_t sourceCount() const { return sources_.size(); }
  size_t sinkCount() const { return sinks_.size(); }

 private:
  struct SourceState {
    PixelFormat nativeFormat = PixelFormat::unknown;
    bool connected = false;
  };
  struct SinkState {
    std::vector<PixelFormat> accepts;
    std::string routedFrom;
  };

  // Ordered so that plan() is deterministic across runs. A map keyed on the
  // user's own ids also means the control API and the plan agree on ordering,
  // which matters when a UI renders the crosspoint as a grid.
  std::map<std::string, SourceState> sources_;
  std::map<std::string, SinkState> sinks_;
  bool muted_ = false;
};

}  // namespace ferret

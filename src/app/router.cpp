#include "app/router.h"

#include <algorithm>

namespace ferret {

void Router::addSource(const std::string& id, PixelFormat nativeFormat) {
  sources_[id].nativeFormat = nativeFormat;
}

void Router::addSink(const std::string& id, std::vector<PixelFormat> accepts) {
  sinks_[id].accepts = std::move(accepts);
}

bool Router::removeSource(const std::string& id) {
  if (sources_.erase(id) == 0) return false;
  // Any sink pointed at it becomes unrouted, which plan() then reports as
  // "no source routed" — the honest state — rather than as a dangling route.
  for (auto& [sinkId, sink] : sinks_) {
    if (sink.routedFrom == id) sink.routedFrom.clear();
  }
  return true;
}

bool Router::removeSink(const std::string& id) {
  return sinks_.erase(id) > 0;
}

bool Router::route(const std::string& sinkId, const std::string& sourceId,
                   std::string& error) {
  auto sink = sinks_.find(sinkId);
  if (sink == sinks_.end()) {
    error = "no such sink: " + sinkId;
    return false;
  }
  if (!sourceId.empty() && sources_.find(sourceId) == sources_.end()) {
    error = "no such source: " + sourceId;
    return false;
  }
  sink->second.routedFrom = sourceId;
  return true;
}

std::string Router::routedSource(const std::string& sinkId) const {
  auto it = sinks_.find(sinkId);
  return it == sinks_.end() ? std::string() : it->second.routedFrom;
}

void Router::setSourceConnected(const std::string& id, bool connected) {
  auto it = sources_.find(id);
  if (it != sources_.end()) it->second.connected = connected;
}

std::vector<RouteAction> Router::plan() const {
  std::vector<RouteAction> out;
  out.reserve(sinks_.size());

  for (const auto& [sinkId, sink] : sinks_) {
    RouteAction a;
    a.sinkId = sinkId;

    if (muted_) {
      a.reason = "globally muted";
      out.push_back(std::move(a));
      continue;
    }
    if (sink.routedFrom.empty()) {
      a.reason = "no source routed";
      out.push_back(std::move(a));
      continue;
    }

    auto src = sources_.find(sink.routedFrom);
    if (src == sources_.end()) {
      // Defensive: route() rejects unknown ids, so reaching here means a
      // source was removed while routed. Black, not a crash, and the reason
      // names the missing id.
      a.reason = "routed source '" + sink.routedFrom + "' no longer exists";
      out.push_back(std::move(a));
      continue;
    }
    if (!src->second.connected) {
      a.reason = "source '" + sink.routedFrom + "' is not connected";
      a.sourceId = sink.routedFrom;
      out.push_back(std::move(a));
      continue;
    }

    a.sourceId = sink.routedFrom;
    const PixelFormat native = src->second.nativeFormat;

    // An empty accepts list means the sink converts internally, so handing it
    // the native format is both a copy for us and correct for it.
    const bool nativeOk =
        sink.accepts.empty() ||
        std::find(sink.accepts.begin(), sink.accepts.end(), native) !=
            sink.accepts.end();

    if (nativeOk) {
      a.what = RouteAction::What::copy;
      a.targetFormat = native;
    } else {
      a.what = RouteAction::What::convert;
      // First preference wins. Sinks list their formats best-first, so this
      // picks the highest-fidelity option the sink actually supports rather
      // than the first one that happens to work.
      a.targetFormat = sink.accepts.front();
    }
    out.push_back(std::move(a));
  }

  return out;
}

}  // namespace ferret

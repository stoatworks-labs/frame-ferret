#include "app/engine.h"

#include <algorithm>
#include <chrono>

#include "core/convert.h"

namespace ferret {

Engine::Engine() = default;

Engine::~Engine() { stop(); }

void Engine::addSource(std::unique_ptr<Source> source,
                       PixelFormat nativeFormat) {
  if (!source) return;
  Pending p;
  p.what = Pending::What::addSource;
  p.id = source->id();
  p.source = std::move(source);
  p.nativeFormat = nativeFormat;
  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    announced_.push_back(p.id);
    pending_.push_back(std::move(p));
  }
  // Before the loop exists there is no tick to wait for, so drain here. That
  // keeps buildNodes() and the tests reading exactly as they did.
  if (!running_.load()) applyPending();
}

void Engine::addSink(std::unique_ptr<Sink> sink) {
  if (!sink) return;
  Pending p;
  p.what = Pending::What::addSink;
  p.id = sink->id();
  p.sink = std::move(sink);
  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    announced_.push_back(p.id);
    pending_.push_back(std::move(p));
  }
  if (!running_.load()) applyPending();
}

void Engine::removeNode(const std::string& id) {
  Pending p;
  p.what = Pending::What::remove;
  p.id = id;
  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    announced_.erase(std::remove(announced_.begin(), announced_.end(), id),
                     announced_.end());
    pending_.push_back(std::move(p));
  }
  if (!running_.load()) applyPending();
}

bool Engine::knows(const std::string& id) const {
  std::lock_guard<std::mutex> lock(pendingMutex_);
  return std::find(announced_.begin(), announced_.end(), id) !=
         announced_.end();
}

void Engine::applyPending() {
  std::vector<Pending> batch;
  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    if (pending_.empty()) return;
    batch.swap(pending_);
  }

  for (auto& p : batch) {
    switch (p.what) {
      case Pending::What::addSource: {
        SourceEntry e;
        e.source = std::move(p.source);
        e.nativeFormat = p.nativeFormat;
        sources_.push_back(std::move(e));
        router_.addSource(p.id, p.nativeFormat);
        break;
      }
      case Pending::What::addSink: {
        router_.addSink(p.id, p.sink->preferredFormats());
        sinks_.push_back(std::move(p.sink));
        break;
      }
      case Pending::What::remove: {
        router_.removeSource(p.id);
        router_.removeSink(p.id);
        // Erase by identity rather than by a cached index: every index after
        // a removal shifts, so the maps are rebuilt below in one pass instead
        // of being patched here.
        sources_.erase(
            std::remove_if(sources_.begin(), sources_.end(),
                           [&](const SourceEntry& e) {
                             return e.source && e.source->id() == p.id;
                           }),
            sources_.end());
        sinks_.erase(std::remove_if(sinks_.begin(), sinks_.end(),
                                    [&](const std::unique_ptr<Sink>& s) {
                                      return s && s->id() == p.id;
                                    }),
                     sinks_.end());
        break;
      }
    }
  }

  // One rebuild for the whole batch. Indices are positions in the vectors and
  // any insert or erase invalidates the ones after it, so keeping them
  // incrementally correct is more moving parts than recomputing.
  sourceIndex_.clear();
  for (size_t i = 0; i < sources_.size(); ++i) {
    sourceIndex_[sources_[i].source->id()] = i;
  }
  sinkIndex_.clear();
  for (size_t i = 0; i < sinks_.size(); ++i) {
    sinkIndex_[sinks_[i]->id()] = i;
  }
}

bool Engine::start(std::string& error) {
  if (running_.load()) {
    error = "engine is already running";
    return false;
  }
  if (sinks_.empty()) {
    error = "engine has no sinks — nothing to serve";
    return false;
  }
  if (!rate_.valid()) {
    error = "engine rate is invalid";
    return false;
  }
  stopping_.store(false);
  running_.store(true);
  thread_ = std::thread([this] { loop(); });
  return true;
}

void Engine::stop() {
  if (!running_.load()) return;
  stopping_.store(true);
  if (thread_.joinable()) thread_.join();
  running_.store(false);
}

void Engine::loop() {
  const auto start = std::chrono::steady_clock::now();
  int64_t tick = 0;
  auto fpsWindowStart = start;
  uint64_t fpsWindowTicks = 0;

  while (!stopping_.load()) {
    // Nodes added or removed since the last tick land here, before anything is
    // polled or served — so a tick always sees one consistent set.
    applyPending();

    // Poll every source, every tick, unconditionally.
    //
    // Never gate this on `connected()`. A network receiver only *becomes*
    // connected as a result of being polled — NDI reports nothing until its
    // first captured frame — so skipping the poll for a disconnected source is
    // a deadlock: it never polls, so it never connects, so it never polls. The
    // synthetic test-pattern source hides this completely, because it reports
    // connected from construction. This cost a real debugging session the first
    // time a transport was attached, and the regression is pinned in
    // tests/test_engine.cpp.
    for (auto& entry : sources_) {
      // Short timeout: this loop owns the pacing, not the source.
      entry.source->poll(1, [&entry](const VideoFrame& f) {
        entry.latest.assign(f);
        entry.hasFrame = true;
      });

      // A source that delivered nothing this tick keeps its previous frame —
      // correct for any source slower than the loop. A source that reports
      // itself disconnected drops it, so the router emits black rather than
      // holding a frozen picture.
      // Audio, once, before any sink is served.
      entry.audio = entry.source->takeAudio();

      const bool connected = entry.source->connected();
      if (!connected) entry.hasFrame = false;
      router_.setSourceConnected(entry.source->id(), connected && entry.hasFrame);
    }

    serveTick(tick);

    ++tick;
    ++fpsWindowTicks;

    const auto now = std::chrono::steady_clock::now();
    if (now - fpsWindowStart >= std::chrono::seconds(1)) {
      const double seconds =
          std::chrono::duration<double>(now - fpsWindowStart).count();
      std::lock_guard<std::mutex> lock(countersMutex_);
      counters_.measuredFps = fpsWindowTicks / seconds;
      fpsWindowStart = now;
      fpsWindowTicks = 0;
    }

    // Deadline from the rational, against tick zero. Signed throughout: a tick
    // already late must produce a negative wait, not an enormous positive one.
    const int64_t deadlineNs = tickDeadlineNs(rate_, tick);
    const auto target = start + std::chrono::nanoseconds(deadlineNs);
    if (target > now) {
      std::this_thread::sleep_until(target);
    } else {
      std::lock_guard<std::mutex> lock(countersMutex_);
      ++counters_.lateTicks;
    }
  }
}

void Engine::serveTick(int64_t tick) {
  const auto plan = router_.plan();

  uint64_t delivered = 0, black = 0, conversions = 0, audioDelivered = 0;
  std::map<std::string, std::string> reasons;

  for (const auto& action : plan) {
    auto sinkIt = sinkIndex_.find(action.sinkId);
    if (sinkIt == sinkIndex_.end()) continue;
    Sink* sink = sinks_[sinkIt->second].get();

    if (action.what == RouteAction::What::black) {
      sink->sendBlack();
      ++black;
      reasons[action.sinkId] = action.reason;
      continue;
    }

    auto srcIt = sourceIndex_.find(action.sourceId);
    if (srcIt == sourceIndex_.end()) {
      // Cannot happen through route(), which validates ids. Black rather than
      // skip, so the invariant holds even when something upstream is wrong.
      sink->sendBlack();
      ++black;
      reasons[action.sinkId] = "internal: source vanished";
      continue;
    }

    SourceEntry& entry = sources_[srcIt->second];
    if (!entry.hasFrame) {
      sink->sendBlack();
      ++black;
      reasons[action.sinkId] = "source has produced no frame yet";
      continue;
    }

    const VideoFrame& frame = entry.latest.frame();
    reasons[action.sinkId].clear();

    // Audio follows the same crosspoint as video — one route, both media.
    // Deliberately not sent on a `black` action: video going quiet is a fault
    // downstream equipment must recover from, which is why black frames are
    // still emitted, but audio going quiet *is* silence and needs no filler.
    if (entry.audio) {
      sink->sendAudio(*entry.audio);
      ++audioDelivered;
    }

    if (action.what == RouteAction::What::copy) {
      sink->send(frame);
      ++delivered;
      continue;
    }

    PixelFormat outFormat;
    QuantRange outRange = QuantRange::unknown;
    int outStride = 0;
    std::string error;
    if (!convert(frame, action.targetFormat, outFormat, outRange,
                 convertScratch_, outStride, error)) {
      // A conversion the router planned and the converter refused. Black, with
      // the converter's own message, rather than a silently skipped output.
      sink->sendBlack();
      ++black;
      reasons[action.sinkId] = "conversion failed: " + error;
      continue;
    }

    VideoFrame converted = frame;
    converted.format = outFormat;
    // The range may have been normalised — an RGB source becomes narrow YCbCr
    // — so the frame must be relabelled or the sink encodes it a second time.
    converted.range = outRange;
    converted.data = convertScratch_.data();
    converted.strideBytes = outStride;
    sink->send(converted);
    ++delivered;
    ++conversions;
  }

  std::lock_guard<std::mutex> lock(countersMutex_);
  counters_.ticks = static_cast<uint64_t>(tick) + 1;
  counters_.framesDelivered += delivered;
  counters_.blackDelivered += black;
  counters_.conversions += conversions;
  counters_.audioFramesDelivered += audioDelivered;
  reasons_.swap(reasons);
}

Engine::Counters Engine::counters() const {
  std::lock_guard<std::mutex> lock(countersMutex_);
  return counters_;
}

std::map<std::string, std::string> Engine::sinkReasons() const {
  std::lock_guard<std::mutex> lock(countersMutex_);
  return reasons_;
}

}  // namespace ferret

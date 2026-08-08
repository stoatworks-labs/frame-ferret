#include "app/engine.h"

#include <chrono>

#include "core/convert.h"

namespace ferret {

Engine::Engine() = default;

Engine::~Engine() { stop(); }

void Engine::addSource(std::unique_ptr<Source> source,
                       PixelFormat nativeFormat) {
  if (!source) return;
  const std::string id = source->id();
  router_.addSource(id, nativeFormat);
  sourceIndex_[id] = sources_.size();
  SourceEntry e;
  e.source = std::move(source);
  e.nativeFormat = nativeFormat;
  sources_.push_back(std::move(e));
}

void Engine::addSink(std::unique_ptr<Sink> sink) {
  if (!sink) return;
  const std::string id = sink->id();
  router_.addSink(id, sink->preferredFormats());
  sinkIndex_[id] = sinks_.size();
  sinks_.push_back(std::move(sink));
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

  uint64_t delivered = 0, black = 0, conversions = 0;
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

    if (action.what == RouteAction::What::copy) {
      sink->send(frame);
      ++delivered;
      continue;
    }

    PixelFormat outFormat;
    int outStride = 0;
    std::string error;
    if (!convert(frame, action.targetFormat, outFormat, convertScratch_,
                 outStride, error)) {
      // A conversion the router planned and the converter refused. Black, with
      // the converter's own message, rather than a silently skipped output.
      sink->sendBlack();
      ++black;
      reasons[action.sinkId] = "conversion failed: " + error;
      continue;
    }

    VideoFrame converted = frame;
    converted.format = outFormat;
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

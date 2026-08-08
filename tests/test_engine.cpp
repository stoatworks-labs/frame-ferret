#include "app/engine.h"

#include <chrono>
#include <thread>

#include "check.h"
#include "core/convert.h"

using namespace ferret;

namespace {

/// A sink that records what it was handed, so the engine's behaviour can be
/// asserted rather than inferred from counters.
class RecordingSink : public Sink {
 public:
  RecordingSink(std::string id, std::vector<PixelFormat> accepts)
      : id_(std::move(id)), accepts_(std::move(accepts)) {}

  const std::string& id() const override { return id_; }
  std::vector<PixelFormat> preferredFormats() const override {
    return accepts_;
  }

  void send(const VideoFrame& frame) override {
    ++frames;
    lastFormat = frame.format;
    lastWidth = frame.width;
    // Sample a pixel so a sink that receives a valid header and garbage
    // pixels is distinguishable from one that receives a real frame.
    if (frame.data && frame.strideBytes > 0) lastByte = frame.data[0];
  }

  void sendBlack() override {
    ++blacks;
    lastFormat = PixelFormat::unknown;
  }

  std::string id_;
  std::vector<PixelFormat> accepts_;
  int frames = 0;
  int blacks = 0;
  PixelFormat lastFormat = PixelFormat::unknown;
  int lastWidth = 0;
  uint8_t lastByte = 0;
};

/// A source under the test's control: it produces only when told to, and can
/// be disconnected at will.
class ScriptedSource : public Source {
 public:
  ScriptedSource(std::string id, PixelFormat format, int w, int h)
      : id_(std::move(id)), format_(format), w_(w), h_(h) {
    stride_ = tightStrideBytes(format, w);
    pixels_.assign(static_cast<size_t>(stride_) * h, 0x40);
  }

  const std::string& id() const override { return id_; }
  bool connected() const override { return connected_; }

  bool poll(unsigned,
            const std::function<void(const VideoFrame&)>& onVideo) override {
    if (!producing_) return false;
    VideoFrame f;
    f.width = w_;
    f.height = h_;
    f.strideBytes = stride_;
    f.data = pixels_.data();
    f.format = format_;
    f.colour = ColourSpace::bt709;
    f.range = QuantRange::full;
    if (onVideo) onVideo(f);
    return true;
  }

  void setConnected(bool c) { connected_ = c; }
  void setProducing(bool p) { producing_ = p; }

 private:
  std::string id_;
  PixelFormat format_;
  int w_, h_, stride_;
  std::vector<uint8_t> pixels_;
  bool connected_ = true;
  bool producing_ = true;
};

void waitForTicks(const Engine& engine, uint64_t target) {
  for (int i = 0; i < 200; ++i) {
    if (engine.counters().ticks >= target) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

/// The invariant, observed through a real running engine rather than through
/// Router::plan() alone: every sink is served every tick.
void everySinkIsServedEveryTick() {
  Engine engine;
  engine.setRate(Rate{100, 1});  // fast, so the test is short

  auto src = std::make_unique<ScriptedSource>("src", PixelFormat::bgra8, 16, 8);
  engine.addSource(std::move(src), PixelFormat::bgra8);

  auto* a = new RecordingSink("a", {});
  auto* b = new RecordingSink("b", {});
  engine.addSink(std::unique_ptr<Sink>(a));
  engine.addSink(std::unique_ptr<Sink>(b));

  std::string err;
  CHECK(engine.start(err));
  waitForTicks(engine, 10);
  const uint64_t ticks = engine.counters().ticks;
  engine.stop();

  // Nothing is routed, so both sinks get black — but they get it every tick.
  CHECK(a->blacks > 0);
  CHECK(b->blacks > 0);
  CHECK_EQ(a->frames, 0);
  // Each sink saw one action per tick. Allow a tick of slack for the sample
  // being taken between the two counter updates.
  CHECK(a->blacks >= static_cast<int>(ticks) - 1);
  CHECK(b->blacks >= static_cast<int>(ticks) - 1);
}

void aRoutedSinkReceivesFrames() {
  Engine engine;
  engine.setRate(Rate{100, 1});

  engine.addSource(
      std::make_unique<ScriptedSource>("src", PixelFormat::bgra8, 16, 8),
      PixelFormat::bgra8);
  auto* sink = new RecordingSink("out", {});
  engine.addSink(std::unique_ptr<Sink>(sink));

  std::string err;
  CHECK(engine.router().route("out", "src", err));
  CHECK(engine.start(err));
  waitForTicks(engine, 10);
  engine.stop();

  CHECK(sink->frames > 0);
  CHECK_EQ(sink->blacks, 0);
  CHECK_EQ(sink->lastFormat, PixelFormat::bgra8);
  CHECK_EQ(sink->lastWidth, 16);
  CHECK_EQ(sink->lastByte, uint8_t{0x40});
}

/// A source that goes away must produce black, not a frozen last frame. This
/// is the behaviour the whole program is designed around.
void aDisconnectedSourceGivesBlackNotAFrozenFrame() {
  Engine engine;
  engine.setRate(Rate{100, 1});

  auto owned =
      std::make_unique<ScriptedSource>("src", PixelFormat::bgra8, 16, 8);
  auto* src = owned.get();
  engine.addSource(std::move(owned), PixelFormat::bgra8);

  auto* sink = new RecordingSink("out", {});
  engine.addSink(std::unique_ptr<Sink>(sink));

  std::string err;
  CHECK(engine.router().route("out", "src", err));
  CHECK(engine.start(err));
  waitForTicks(engine, 10);

  const int framesBefore = sink->frames;
  CHECK(framesBefore > 0);

  src->setConnected(false);
  const int blacksAtCut = sink->blacks;
  waitForTicks(engine, engine.counters().ticks + 10);
  engine.stop();

  // Frames stopped, black started.
  CHECK(sink->blacks > blacksAtCut);
  CHECK(sink->frames < framesBefore + 5);
}

/// The router plans a conversion; the engine must actually perform it and hand
/// the sink its own format.
void aConvertRouteReallyConverts() {
  Engine engine;
  engine.setRate(Rate{100, 1});

  engine.addSource(
      std::make_unique<ScriptedSource>("src", PixelFormat::bgra8, 12, 4),
      PixelFormat::bgra8);

  // This sink takes only v210, which the source does not produce.
  auto* sink = new RecordingSink("sdi", {PixelFormat::v210});
  engine.addSink(std::unique_ptr<Sink>(sink));

  std::string err;
  CHECK(engine.router().route("sdi", "src", err));
  CHECK(engine.start(err));
  waitForTicks(engine, 10);
  const auto counters = engine.counters();
  engine.stop();

  CHECK(sink->frames > 0);
  CHECK_EQ(sink->lastFormat, PixelFormat::v210);
  CHECK(counters.conversions > 0);
}

void muteMakesEverySinkBlackAndKeepsTheRoute() {
  Engine engine;
  engine.setRate(Rate{100, 1});
  engine.addSource(
      std::make_unique<ScriptedSource>("src", PixelFormat::bgra8, 16, 8),
      PixelFormat::bgra8);
  auto* sink = new RecordingSink("out", {});
  engine.addSink(std::unique_ptr<Sink>(sink));

  std::string err;
  CHECK(engine.router().route("out", "src", err));
  CHECK(engine.start(err));
  waitForTicks(engine, 10);

  engine.router().setMuted(true);
  const int blacksAtMute = sink->blacks;
  waitForTicks(engine, engine.counters().ticks + 10);
  engine.stop();

  CHECK(sink->blacks > blacksAtMute);
  // The route survives the mute, so unmuting restores it exactly.
  CHECK_EQ(engine.router().routedSource("out"), std::string("src"));
}

/// Every black must carry a reason the control API can show. A black output
/// with an empty explanation is the thing this program exists to avoid.
void everyBlackCarriesAReason() {
  Engine engine;
  engine.setRate(Rate{100, 1});
  engine.addSource(
      std::make_unique<ScriptedSource>("src", PixelFormat::bgra8, 16, 8),
      PixelFormat::bgra8);
  engine.addSink(std::unique_ptr<Sink>(new RecordingSink("unrouted", {})));

  std::string err;
  CHECK(engine.start(err));
  waitForTicks(engine, 5);
  const auto reasons = engine.sinkReasons();
  engine.stop();

  auto it = reasons.find("unrouted");
  CHECK(it != reasons.end());
  CHECK(!it->second.empty());
}

/// A source that only reports itself connected once it has been polled — which
/// is how every real network receiver behaves, NDI included.
class ConnectsOnFirstPollSource : public Source {
 public:
  explicit ConnectsOnFirstPollSource(std::string id) : id_(std::move(id)) {
    pixels_.assign(16 * 8 * 4, 0x77);
  }

  const std::string& id() const override { return id_; }
  bool connected() const override { return polled_ > 0; }

  bool poll(unsigned,
            const std::function<void(const VideoFrame&)>& onVideo) override {
    ++polled_;
    VideoFrame f;
    f.width = 16;
    f.height = 8;
    f.strideBytes = 16 * 4;
    f.data = pixels_.data();
    f.format = PixelFormat::bgra8;
    if (onVideo) onVideo(f);
    return true;
  }

  int polled() const { return polled_; }

 private:
  std::string id_;
  std::vector<uint8_t> pixels_;
  int polled_ = 0;
};

/// The regression: the engine must poll every source unconditionally.
///
/// Gating the poll on `connected()` deadlocks any real receiver — it never
/// polls, so it never connects, so it never polls — and the deadlock is
/// invisible to every synthetic source, because those report connected from
/// construction. This is what actually happened the first time NDI was
/// attached: 654 ticks, 0 frames, "source is not connected".
void aSourceThatConnectsOnlyWhenPolledStillWorks() {
  Engine engine;
  engine.setRate(Rate{100, 1});

  auto owned = std::make_unique<ConnectsOnFirstPollSource>("late");
  auto* src = owned.get();
  engine.addSource(std::move(owned), PixelFormat::bgra8);

  auto* sink = new RecordingSink("out", {});
  engine.addSink(std::unique_ptr<Sink>(sink));

  std::string err;
  CHECK(engine.router().route("out", "late", err));
  CHECK(engine.start(err));
  waitForTicks(engine, 15);
  engine.stop();

  CHECK(src->polled() > 0);
  CHECK(sink->frames > 0);
  CHECK_EQ(sink->lastByte, uint8_t{0x77});
}

void startingWithNoSinksIsAnError() {
  Engine engine;
  std::string err;
  CHECK(!engine.start(err));
  CHECK(err.find("no sinks") != std::string::npos);
}

void anInvalidRateIsRejected() {
  Engine engine;
  engine.addSink(std::unique_ptr<Sink>(new RecordingSink("out", {})));
  engine.setRate(Rate{0, 1});
  std::string err;
  CHECK(!engine.start(err));
  CHECK(err.find("rate") != std::string::npos);
}

/// Pacing: over a known window the tick count must land near the nominal rate.
/// Loose bounds — this runs on a shared CI box — but tight enough to catch a
/// loop that is free-running or not sleeping at all.
void theLoopIsPacedByItsRate() {
  Engine engine;
  engine.setRate(Rate{50, 1});
  engine.addSink(std::unique_ptr<Sink>(new RecordingSink("out", {})));

  std::string err;
  CHECK(engine.start(err));
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  const auto counters = engine.counters();
  engine.stop();

  // 600 ms at 50 fps is ~30 ticks. A free-running loop would be thousands.
  CHECK(counters.ticks > 15);
  CHECK(counters.ticks < 60);
}

void run() {
  everySinkIsServedEveryTick();
  aRoutedSinkReceivesFrames();
  aDisconnectedSourceGivesBlackNotAFrozenFrame();
  aConvertRouteReallyConverts();
  muteMakesEverySinkBlackAndKeepsTheRoute();
  everyBlackCarriesAReason();
  aSourceThatConnectsOnlyWhenPolledStillWorks();
  startingWithNoSinksIsAnError();
  anInvalidRateIsRejected();
  theLoopIsPacedByItsRate();
}

}  // namespace

TEST_MAIN("engine")

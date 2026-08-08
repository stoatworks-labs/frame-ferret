#include "sources/test_pattern.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "core/convert.h"

namespace ferret {
namespace {

// 75% bars: 191 rather than 255 on each active component. 100% bars overload
// several legal-range encodings and are the wrong default for a broadcast
// test source.
const std::vector<BarColour> kBars = {
    {"white", 191, 191, 191}, {"yellow", 191, 191, 0},
    {"cyan", 0, 191, 191},    {"green", 0, 191, 0},
    {"magenta", 191, 0, 191}, {"red", 191, 0, 0},
    {"blue", 0, 0, 191},      {"black", 0, 0, 0},
};

class TestPatternSource : public Source {
 public:
  TestPatternSource(NodeConfig config) : config_(std::move(config)) {
    stride_ = tightStrideBytes(PixelFormat::bgra8, config_.width);
    pixels_.resize(static_cast<size_t>(stride_) * config_.height);
    drawBars();
    start_ = std::chrono::steady_clock::now();
  }

  const std::string& id() const override { return config_.id; }

  /// Always true. A generator has nothing to connect to, and reporting false
  /// would make the router emit black from a source that is working perfectly.
  bool connected() const override { return true; }

  bool poll(unsigned timeoutMs,
            const std::function<void(const VideoFrame&)>& onVideo) override {
    // Paced from the rational against tick zero, exactly as the engine does.
    // A generator that free-runs would mask a pacing bug everywhere else.
    const int64_t deadline = tickDeadlineNs(config_.rate, tick_);
    const auto target = start_ + std::chrono::nanoseconds(deadline);
    const auto now = std::chrono::steady_clock::now();

    if (target > now) {
      const auto wait = target - now;
      const auto limit = std::chrono::milliseconds(timeoutMs);
      if (wait > limit) {
        std::this_thread::sleep_for(limit);
        return false;  // Not yet — the caller polls again.
      }
      std::this_thread::sleep_for(wait);
    }

    drawMarker();

    VideoFrame f;
    f.width = config_.width;
    f.height = config_.height;
    f.strideBytes = stride_;
    f.data = pixels_.data();
    f.format = PixelFormat::bgra8;
    // The same raster rule the NDI and OMT receivers use to infer an untagged
    // source. Claiming 709 at every size made the generator disagree with every
    // consumer below 720 lines.
    f.colour = config_.height >= 720 ? ColourSpace::bt709 : ColourSpace::bt601;
    // A generated RGB pattern is full range. Declaring it narrow is the exact
    // mistake that produces washed-out bars downstream.
    f.range = QuantRange::full;
    f.rate = config_.rate;
    f.timestampNs = deadline;
    f.ptpLocked = false;

    generateAudio();

    ++tick_;
    if (onVideo) onVideo(f);
    return true;
  }

  std::unique_ptr<AudioFrame> takeAudio() override {
    return std::move(pendingAudio_);
  }

  int64_t tick() const { return tick_; }

 private:
  /// One frame's worth of 1 kHz tone, planar float, phase-continuous across
  /// frames.
  ///
  /// The phase accumulator is the point. Restarting the sine at zero every
  /// frame produces a click at the frame rate — clearly audible, and a fault
  /// that a spectrum check for "is there 1 kHz" would happily pass.
  void generateAudio() {
    const double rate = config_.rate.approx();
    if (rate <= 0) return;

    // Sample counts must come out to exactly the sample rate over one second,
    // whatever the frame rate — computed from the tick so 59.94 does not
    // accumulate an error the way a per-frame rounding would.
    const int64_t nextTotal = static_cast<int64_t>(
        (tick_ + 1) * kToneSampleRate * config_.rate.den / config_.rate.num);
    const int64_t thisTotal = static_cast<int64_t>(
        tick_ * kToneSampleRate * config_.rate.den / config_.rate.num);
    const int samples = static_cast<int>(nextTotal - thisTotal);
    if (samples <= 0) return;

    auto audio = std::make_unique<AudioFrame>();
    audio->sampleRate = kToneSampleRate;
    audio->channels = kToneChannels;
    audio->samplesPerChannel = samples;
    audio->data.resize(static_cast<size_t>(kToneChannels) * samples);

    const double step = 2.0 * M_PI * kToneHz / kToneSampleRate;
    for (int i = 0; i < samples; ++i) {
      const float value =
          static_cast<float>(std::sin(phase_) * kToneAmplitude);
      for (int c = 0; c < kToneChannels; ++c) {
        audio->data[static_cast<size_t>(c) * samples + i] = value;
      }
      phase_ += step;
      if (phase_ > 2.0 * M_PI) phase_ -= 2.0 * M_PI;
    }
    pendingAudio_ = std::move(audio);
  }

  void drawBars() {
    const int n = static_cast<int>(kBars.size());
    for (int y = 0; y < config_.height; ++y) {
      uint8_t* row = pixels_.data() + static_cast<size_t>(y) * stride_;
      for (int x = 0; x < config_.width; ++x) {
        // Computed per pixel from the width so the last bar reaches the right
        // edge exactly, whatever the width. Dividing the width into n and
        // multiplying back leaves a gap on any width not divisible by 8.
        const int bar = (x * n) / config_.width;
        const BarColour& c = kBars[bar];
        row[x * 4 + 0] = c.b;
        row[x * 4 + 1] = c.g;
        row[x * 4 + 2] = c.r;
        row[x * 4 + 3] = 255;
      }
    }
  }

  /// A white block that steps left to right once per second, over a strip of
  /// bars that is redrawn each frame so the marker leaves no trail.
  void drawMarker() {
    const int stripTop = config_.height * 7 / 8;
    const int n = static_cast<int>(kBars.size());

    for (int y = stripTop; y < config_.height; ++y) {
      uint8_t* row = pixels_.data() + static_cast<size_t>(y) * stride_;
      for (int x = 0; x < config_.width; ++x) {
        const BarColour& c = kBars[(x * n) / config_.width];
        row[x * 4 + 0] = c.b;
        row[x * 4 + 1] = c.g;
        row[x * 4 + 2] = c.r;
        row[x * 4 + 3] = 255;
      }
    }

    const int64_t fps =
        config_.rate.num / (config_.rate.den ? config_.rate.den : 1);
    const int steps = 16;
    const int64_t seconds = fps > 0 ? tick_ / fps : 0;
    const int step = static_cast<int>(seconds % steps);
    const int blockW = config_.width / steps;
    const int x0 = step * blockW;

    for (int y = stripTop; y < config_.height; ++y) {
      uint8_t* row = pixels_.data() + static_cast<size_t>(y) * stride_;
      for (int x = x0; x < x0 + blockW && x < config_.width; ++x) {
        row[x * 4 + 0] = 255;
        row[x * 4 + 1] = 255;
        row[x * 4 + 2] = 255;
        row[x * 4 + 3] = 255;
      }
    }
  }

  NodeConfig config_;
  std::vector<uint8_t> pixels_;
  int stride_ = 0;
  int64_t tick_ = 0;
  std::chrono::steady_clock::time_point start_;
  double phase_ = 0.0;
  std::unique_ptr<AudioFrame> pendingAudio_;
};

}  // namespace

const std::vector<BarColour>& colourBars75() { return kBars; }

std::unique_ptr<Source> makeTestPatternSource(const NodeConfig& config,
                                              std::string& error) {
  if (config.width <= 0 || config.height <= 0) {
    error = "test pattern needs a positive width and height";
    return nullptr;
  }
  if (!config.rate.valid()) {
    error = "test pattern needs a valid frame rate";
    return nullptr;
  }
  return std::make_unique<TestPatternSource>(config);
}

}  // namespace ferret

#include "transports/srt.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "core/convert.h"
#include "core/subprocess.h"
#include "transports/ffmpeg.h"
#include "transports/srt_socket.h"

namespace ferret {
namespace {

/// UYVY throughout: it is what nearly every H.264 decode lands on after
/// conversion, it is half the bytes of BGRA over the pipe, and it is what the
/// DeckLink and NDI sinks want next. Asking ffmpeg for BGRA here would add a
/// conversion at both ends for nothing.
constexpr PixelFormat kPipeFormat = PixelFormat::uyvy8;

/// A serviceable default for the raster and rate, when none was configured.
/// Roughly 0.1 bits per pixel per second, which is a normal contribution
/// bitrate for H.264 at these sizes.
int64_t defaultBitrateBps(const NodeConfig& config) {
  const double pixels = static_cast<double>(config.width) * config.height;
  const double bps = pixels * config.rate.approx() * 0.1;
  // Clamped so a tiny raster still gets something usable and a large one
  // does not ask for more than a link is likely to carry.
  const double clamped = bps < 500000.0 ? 500000.0
                         : (bps > 20000000.0 ? 20000000.0 : bps);
  return static_cast<int64_t>(clamped);
}

/// The link's statistics, kept fresh while it is up, and one line about them
/// when it closes: how much crossed it, and whether any of it was lost for
/// good. "Dropped" is the number that matters — those packets never reached
/// the decoder, and each hole is a damaged picture downstream.
///
/// Sampled rather than read at the end because libsrt reports nothing for a
/// broken socket, and a broken link is exactly when the numbers are wanted.
class LinkStats {
 public:
  void sample(const SrtConnection& connection) {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_) return;
    next_ = now + std::chrono::seconds(1);
    const SrtConnection::Stats s = connection.stats();
    if (s.packetsSent > 0 || s.packetsReceived > 0) {
      last_ = s;
      have_ = true;
    }
  }

  void reportClosed(const SrtConnection& connection, bool sending) {
    next_ = {};
    sample(connection);
    if (!have_) {
      std::fprintf(stderr, "srt: %s closed\n", connection.describe().c_str());
    } else {
      std::fprintf(stderr,
                   "srt: %s closed: %lld packets %s, %lld lost and %lld "
                   "retransmitted, %lld dropped too late\n",
                   connection.describe().c_str(),
                   static_cast<long long>(sending ? last_.packetsSent
                                                  : last_.packetsReceived),
                   sending ? "sent" : "received",
                   static_cast<long long>(last_.packetsLost),
                   static_cast<long long>(last_.packetsRetransmitted),
                   static_cast<long long>(last_.packetsDropped));
    }
    *this = LinkStats();
  }

 private:
  SrtConnection::Stats last_;
  bool have_ = false;
  std::chrono::steady_clock::time_point next_{};
};

class SrtSource final : public Source {
 public:
  SrtSource(NodeConfig config, SrtConfig srt, std::string ffmpegPath)
      : config_(std::move(config)),
        srtConfig_(std::move(srt)),
        ffmpeg_(std::move(ffmpegPath)) {
    frameBytes_ = static_cast<size_t>(
                      tightStrideBytes(kPipeFormat, config_.width)) *
                  config_.height;
    worker_ = std::thread([this] { run(); });
  }

  ~SrtSource() override {
    stopping_.store(true);
    if (worker_.joinable()) worker_.join();
  }

  const std::string& id() const override { return config_.id; }
  bool connected() const override { return connected_.load(); }

  bool poll(unsigned,
            const std::function<void(const VideoFrame&)>& onVideo) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasFrame_) return false;
    if (onVideo) onVideo(latest_.frame());
    return true;
  }

 private:
  /// Owns the whole chain: connect, spawn ffmpeg, pump TS in, pull frames out.
  ///
  /// One thread rather than two. The obvious shape is a feeder thread and a
  /// reader thread, but ffmpeg only produces output once it has been fed, so
  /// alternating a bounded read of each is both simpler and enough — and it
  /// means there is exactly one place that owns the subprocess's lifetime.
  void run() {
    while (!stopping_.load()) {
      if (!open()) {
        // Nothing to reconnect to yet. Wait rather than spin: a listener with
        // no caller and a caller with no listener both land here.
        for (int i = 0; i < 20 && !stopping_.load(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        continue;
      }
      pump();
      close();
    }
    close();
  }

  bool open() {
    std::string error;
    connection_ = srtConnect(srtConfig_, error);
    if (!connection_) {
      reportOnce("srt: " + error);
      return false;
    }

    // -fflags nobuffer + -flags low_delay: without them ffmpeg buffers half a
    // second before emitting anything, which reads as the source being dead.
    // -vf scale forces the raster, because rawvideo has no framing and the
    // reader can only find boundaries in fixed-size records.
    const std::vector<std::string> arguments = {
        "-hide_banner",
        "-loglevel", "error",
        "-fflags", "nobuffer",
        "-flags", "low_delay",
        "-f", "mpegts",
        "-i", "pipe:0",
        "-an",
        "-vf", "scale=" + std::to_string(config_.width) + ":" +
                   std::to_string(config_.height),
        "-f", "rawvideo",
        "-pix_fmt", "uyvy422",
        "pipe:1",
    };

    std::string spawnError;
    codec_ = spawnSubprocess(ffmpeg_, arguments, spawnError);
    if (!codec_) {
      reportOnce("srt: " + spawnError);
      connection_.reset();
      return false;
    }

    std::fprintf(stderr, "srt: %s, decoding with %s\n",
                 connection_->describe().c_str(), ffmpeg_.c_str());
    return true;
  }

  void close() {
    if (codec_) {
      reportDecoderErrors();
      codec_->stop();
      codec_.reset();
    }
    if (connection_) linkStats_.reportClosed(*connection_, false);
    connection_.reset();
    connected_.store(false);
  }

  /// What the decoder complained about since the last report, as one line.
  /// After the first frame this is the only way a damaged stream shows up: a
  /// dropped packet becomes "concealing 812 errors" on ffmpeg's stderr and a
  /// smeared picture on every output, and nothing else in the log.
  void reportDecoderErrors() {
    const std::string errors = codec_ ? codec_->drainErrors() : std::string();
    if (errors.empty()) return;
    size_t lines = 0;
    for (const char c : errors) lines += c == '\n';
    const std::string first = Ffmpeg::summariseError(errors);
    std::fprintf(stderr, "srt: decoder reports: %s%s\n", first.c_str(),
                 lines > 1 ? (" (and " + std::to_string(lines - 1) +
                              " more lines)").c_str()
                           : "");
  }

  void pump() {
    std::vector<uint8_t> wire(SrtConnection::kDefaultPayloadSize);
    std::vector<uint8_t> frame(frameBytes_);

    while (!stopping_.load()) {
      // Feed. A handful of payloads per turn, so that a busy link cannot
      // starve the decode side of this loop.
      bool fed = false;
      for (int i = 0; i < 16; ++i) {
        const int n = connection_->receive(wire.data(),
                                           static_cast<int>(wire.size()), 2);
        if (n < 0) return;  // link gone; run() reconnects
        if (n == 0) break;
        if (!codec_->write(wire.data(), static_cast<size_t>(n))) {
          reportCodecFailure();
          return;
        }
        bytesIn_ += static_cast<uint64_t>(n);
        fed = true;
      }

      linkStats_.sample(*connection_);

      // Drain. Whole frames only — a partial read is a torn picture, not a
      // short one.
      while (codec_->readExactly(frame.data(), frameBytes_, fed ? 5 : 20)) {
        deliver(frame);
        ++framesOut_;
        if (stopping_.load()) return;
      }

      // One line every couple of seconds until frames appear. "No picture" has
      // three quite different causes — nothing arriving on the wire, the
      // decoder refusing it, or the decoder never being fed — and this is what
      // tells them apart.
      const auto now = std::chrono::steady_clock::now();
      if (framesOut_ > 0 && now - lastReport_ > std::chrono::seconds(2)) {
        lastReport_ = now;
        reportDecoderErrors();
      }
      if (framesOut_ == 0 && now - lastReport_ > std::chrono::seconds(2)) {
        lastReport_ = now;
        const std::string errors = codec_->drainErrors();
        std::fprintf(stderr,
                     "srt: %llu bytes in, 0 frames out%s%s\n",
                     (unsigned long long)bytesIn_,
                     errors.empty() ? "" : " — decoder says: ",
                     errors.c_str());
      }

      if (!codec_->running()) {
        reportCodecFailure();
        return;
      }
    }
  }

  void deliver(const std::vector<uint8_t>& pixels) {
    VideoFrame f;
    f.width = config_.width;
    f.height = config_.height;
    f.strideBytes = tightStrideBytes(kPipeFormat, config_.width);
    f.data = pixels.data();
    f.format = kPipeFormat;
    // ffmpeg's rawvideo output is narrow-range YCbCr, and 709 for HD rasters.
    f.colour = config_.height >= 720 ? ColourSpace::bt709 : ColourSpace::bt601;
    f.range = QuantRange::narrow;
    f.rate = config_.rate;
    f.ptpLocked = false;

    std::lock_guard<std::mutex> lock(mutex_);
    latest_.assign(f);
    hasFrame_ = true;
    connected_.store(true);
  }

  void reportCodecFailure() {
    // ffmpeg explains itself on stderr and nowhere else, so a failure without
    // this is just "no picture".
    const std::string errors = codec_ ? codec_->drainErrors() : std::string();
    reportOnce("srt: the decoder stopped" +
               (errors.empty() ? std::string() : ": " + errors));
  }

  void reportOnce(const std::string& message) {
    if (reported_) return;
    reported_ = true;
    std::fprintf(stderr, "%s\n", message.c_str());
  }

  NodeConfig config_;
  SrtConfig srtConfig_;
  std::string ffmpeg_;
  size_t frameBytes_ = 0;

  std::unique_ptr<SrtConnection> connection_;
  std::unique_ptr<Subprocess> codec_;
  LinkStats linkStats_;

  std::thread worker_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> connected_{false};
  bool reported_ = false;

  uint64_t bytesIn_ = 0;
  uint64_t framesOut_ = 0;
  std::chrono::steady_clock::time_point lastReport_ =
      std::chrono::steady_clock::now();

  std::mutex mutex_;
  FrameBuffer latest_;
  bool hasFrame_ = false;
};

// ---------------------------------------------------------------------------
// Sender
// ---------------------------------------------------------------------------

/// Frames in, MPEG-TS over SRT out.
///
/// The frame loop must never block here, so `send()` only ever copies into a
/// single-frame slot and returns. A worker owns the encoder: it writes the
/// pending frame to ffmpeg's stdin, then drains whatever TS ffmpeg has produced
/// and puts it on the wire.
///
/// **The slot holds one frame, and a new frame replaces an unsent one.** For
/// live video that is the right loss: if the encoder cannot keep up, the useful
/// thing to send next is the newest picture, not a queued old one. A growing
/// queue would add latency without ever recovering it.
class SrtSink final : public Sink {
 public:
  SrtSink(NodeConfig config, SrtConfig srt, std::string ffmpegPath,
          std::string encoder)
      : config_(std::move(config)),
        srtConfig_(std::move(srt)),
        ffmpeg_(std::move(ffmpegPath)),
        encoder_(std::move(encoder)) {
    stride_ = tightStrideBytes(kPipeFormat, config_.width);
    frameBytes_ = static_cast<size_t>(stride_) * config_.height;
    worker_ = std::thread([this] { run(); });
  }

  ~SrtSink() override {
    stopping_.store(true);
    if (worker_.joinable()) worker_.join();
  }

  const std::string& id() const override { return config_.id; }

  /// UYVY only. The encoder is told to expect exactly one layout, and letting
  /// the router hand over anything else would mean guessing which — so
  /// everything else becomes an explicit conversion the router plans.
  std::vector<PixelFormat> preferredFormats() const override {
    return {kPipeFormat};
  }

  void send(const VideoFrame& frame) override {
    if (!frame.data || frame.format != kPipeFormat) return;
    if (frame.width != config_.width || frame.height != config_.height) {
      if (!warnedRaster_) {
        warnedRaster_ = true;
        std::fprintf(stderr,
                     "srt: input is %dx%d but this sender is configured for "
                     "%dx%d; frames are being dropped\n",
                     frame.width, frame.height, config_.width, config_.height);
      }
      return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pending_.assign(frame);
    hasPending_ = true;
  }

  void sendBlack() override {
    if (black_.size() != frameBytes_) {
      black_.assign(frameBytes_, 0);
      fillBlack(kPipeFormat, config_.width, config_.height, stride_,
                QuantRange::narrow, black_.data());
    }
    VideoFrame f;
    f.width = config_.width;
    f.height = config_.height;
    f.strideBytes = stride_;
    f.data = black_.data();
    f.format = kPipeFormat;
    f.colour = ColourSpace::bt709;
    f.range = QuantRange::narrow;
    f.rate = config_.rate;
    send(f);
  }

 private:
  void run() {
    while (!stopping_.load()) {
      if (!open()) {
        for (int i = 0; i < 20 && !stopping_.load(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        continue;
      }
      pump();
      close();
    }
    close();
  }

  bool open() {
    std::string error;
    connection_ = srtConnect(srtConfig_, error);
    if (!connection_) {
      reportOnce("srt: " + error);
      return false;
    }

    // Everything up to the muxer comes from the same builder the encoder
    // trial used, so the command that runs here is the one that was proven
    // to open when this node was made.
    H264Encode encode;
    encode.encoder = encoder_;
    encode.width = config_.width;
    encode.height = config_.height;
    encode.rate = config_.rate;
    encode.bitrateBps = bitrateBps();
    std::vector<std::string> arguments = Ffmpeg::h264EncodeArguments(encode);

    // -muxrate and -pcr_period are not decoration. A live TS with no fixed mux
    // rate emits PCRs irregularly, and hardware decoders lock badly or not at
    // all on a stream whose clock wanders.
    arguments.insert(arguments.end(),
                     {"-f", "mpegts",
                      "-muxrate", std::to_string(bitrateBps() * 5 / 4),
                      "-pcr_period", "20",
                      "pipe:1"});

    std::string spawnError;
    codec_ = spawnSubprocess(ffmpeg_, arguments, spawnError);
    if (!codec_) {
      reportOnce("srt: " + spawnError);
      connection_.reset();
      return false;
    }

    std::fprintf(stderr, "srt: %s, encoding %dx%d with %s via %s\n",
                 connection_->describe().c_str(), config_.width,
                 config_.height, encoder_.c_str(), ffmpeg_.c_str());
    return true;
  }

  void close() {
    if (codec_) {
      codec_->stop();
      codec_.reset();
    }
    if (connection_) linkStats_.reportClosed(*connection_, true);
    connection_.reset();
  }

  int64_t bitrateBps() const { return defaultBitrateBps(config_); }

  void pump() {
    std::vector<uint8_t> ts(SrtConnection::kDefaultPayloadSize);
    std::vector<uint8_t> outgoing;
    outgoing.reserve(SrtConnection::kDefaultPayloadSize * 4);

    while (!stopping_.load()) {
      // Encode whatever is waiting.
      bool wrote = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hasPending_) {
          const VideoFrame& f = pending_.frame();
          if (f.data && !codec_->write(f.data, frameBytes_)) {
            reportCodecFailure();
            return;
          }
          hasPending_ = false;
          wrote = true;
          ++framesIn_;
        }
      }

      // Drain the muxer and put it on the wire in SRT-sized payloads. 1316 is
      // seven 188-byte TS packets, which is why SRT chose it: a TS packet never
      // straddles a payload boundary.
      for (int i = 0; i < 32; ++i) {
        const int n = codec_->read(ts.data(), ts.size(), wrote ? 2 : 10);
        if (n < 0) {
          reportCodecFailure();
          return;
        }
        if (n == 0) break;
        outgoing.insert(outgoing.end(), ts.begin(), ts.begin() + n);

        while (outgoing.size() >= SrtConnection::kDefaultPayloadSize) {
          if (!connection_->send(outgoing.data(),
                                 SrtConnection::kDefaultPayloadSize)) {
            return;  // link gone; run() reconnects
          }
          outgoing.erase(outgoing.begin(),
                         outgoing.begin() + SrtConnection::kDefaultPayloadSize);
          ++payloadsOut_;
        }
      }

      linkStats_.sample(*connection_);
      if (!codec_->running()) {
        reportCodecFailure();
        return;
      }
      if (!wrote) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
  }

  void reportCodecFailure() {
    const std::string errors = codec_ ? codec_->drainErrors() : std::string();
    reportOnce("srt: the encoder stopped" +
               (errors.empty() ? std::string() : ": " + errors));
  }

  void reportOnce(const std::string& message) {
    if (reported_) return;
    reported_ = true;
    std::fprintf(stderr, "%s\n", message.c_str());
  }

  NodeConfig config_;
  SrtConfig srtConfig_;
  std::string ffmpeg_;
  std::string encoder_;
  int stride_ = 0;
  size_t frameBytes_ = 0;

  std::unique_ptr<SrtConnection> connection_;
  std::unique_ptr<Subprocess> codec_;
  LinkStats linkStats_;

  std::thread worker_;
  std::atomic<bool> stopping_{false};
  bool reported_ = false;
  bool warnedRaster_ = false;
  uint64_t framesIn_ = 0;
  uint64_t payloadsOut_ = 0;

  std::mutex mutex_;
  FrameBuffer pending_;
  bool hasPending_ = false;
  std::vector<uint8_t> black_;
};

}  // namespace

std::unique_ptr<Source> makeSrtSource(const NodeConfig& config,
                                      std::string& error) {
  if (!SrtRuntime::available()) {
    error = SrtRuntime::unavailableReason();
    return nullptr;
  }
  if (config.target.empty()) {
    error =
        "an SRT node needs a \"target\" URL, e.g. "
        "srt://10.0.0.5:9000?mode=listener";
    return nullptr;
  }

  SrtConfig srt;
  if (!parseSrtUrl(config.target, &srt, error)) return nullptr;
  srt.interfaceSelector = config.interfaceSelector;

  // The interface selector is honoured here, unlike NDI and OMT — srt_bind
  // takes a real address. Resolution failures surface at connect time with the
  // selector named.

  const std::string ffmpeg = Ffmpeg::locate(config.ffmpegPath);
  if (ffmpeg.empty()) {
    error = Ffmpeg::searchReport(config.ffmpegPath);
    return nullptr;
  }
  if (config.width <= 0 || config.height <= 0) {
    error = "an SRT node needs a width and height — rawvideo has no framing, "
            "so the raster has to be fixed and known";
    return nullptr;
  }

  return std::make_unique<SrtSource>(config, srt, ffmpeg);
}


std::unique_ptr<Sink> makeSrtSink(const NodeConfig& config,
                                  std::string& error) {
  if (!SrtRuntime::available()) {
    error = SrtRuntime::unavailableReason();
    return nullptr;
  }
  if (config.target.empty()) {
    error =
        "an SRT node needs a \"target\" URL, e.g. "
        "srt://0.0.0.0:9000?mode=listener";
    return nullptr;
  }
  if (config.width <= 0 || config.height <= 0) {
    error = "an SRT sender needs a width and height";
    return nullptr;
  }

  SrtConfig srt;
  if (!parseSrtUrl(config.target, &srt, error)) return nullptr;
  srt.interfaceSelector = config.interfaceSelector;

  const std::string ffmpeg = Ffmpeg::locate(config.ffmpegPath);
  if (ffmpeg.empty()) {
    error = Ffmpeg::searchReport(config.ffmpegPath);
    return nullptr;
  }

  // Each candidate is proven with a one-frame encode at this node's own raster,
  // rate and bitrate before it is chosen: `-encoders` lists what ffmpeg was
  // built with, not what this machine can open, and an NVENC that cannot open
  // would otherwise be found only as an encoder that dies on every connect.
  H264Encode settings;
  settings.width = config.width;
  settings.height = config.height;
  settings.rate = config.rate;
  settings.bitrateBps = defaultBitrateBps(config);
  const EncoderPick pick =
      Ffmpeg::pickH264Encoder(ffmpeg, config.encoder, settings);
  for (const auto& line : pick.skipped) {
    std::fprintf(stderr, "srt: %s\n", line.c_str());
  }
  if (pick.encoder.empty()) {
    error = "the ffmpeg at " + ffmpeg + ": " + pick.error;
    return nullptr;
  }

  return std::make_unique<SrtSink>(config, srt, ffmpeg, pick.encoder);
}

}  // namespace ferret

#include "transports/omt.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "core/convert.h"
#include "core/dylib.h"
#include "transports/omt_abi.h"

namespace ferret {
namespace {

using namespace omt_abi;

struct Api {
  // Handles are `omt_receive_t*` / `omt_send_t*`, where both typedefs are
  // `long long`. Opaque pointers in practice.
  void* (*receive_create)(const char* address, int32_t frameTypes,
                          int32_t format, int32_t flags) = nullptr;
  void (*receive_destroy)(void*) = nullptr;
  MediaFrame* (*receive)(void*, int32_t frameTypes, int timeoutMs) = nullptr;

  void* (*send_create)(const char* name, int32_t quality) = nullptr;
  void (*send_destroy)(void*) = nullptr;
  int (*send)(void*, MediaFrame*) = nullptr;
  int (*send_connections)(void*) = nullptr;

  char** (*discovery_getaddresses)(int* count) = nullptr;
};

Dylib g_lib;
Api g_api;
bool g_tried = false;
bool g_ok = false;
std::string g_error;
std::mutex g_mutex;
std::atomic<bool> g_dotNetUp{false};

std::vector<std::string> runtimeCandidates() {
  std::vector<std::string> out;

  if (const char* env = std::getenv("FERRET_OMT_RUNTIME")) {
    if (*env) out.emplace_back(env);
  }

  for (const auto& dir : Dylib::localSearchPaths()) {
#ifdef _WIN32
    out.emplace_back(dir + "\\libomt.dll");
#elif defined(__APPLE__)
    out.emplace_back(dir + "/libomt.dylib");
#else
    out.emplace_back(dir + "/libomt.so");
#endif
  }

#ifdef _WIN32
  out.emplace_back("libomt.dll");
#elif defined(__APPLE__)
  // Where the OMT distribution and this fleet actually put it.
  if (const char* home = std::getenv("HOME")) {
    out.emplace_back(std::string(home) + "/.local/lib/omt/libomt.dylib");
  }
  out.emplace_back("/usr/local/lib/libomt.dylib");
  out.emplace_back("/opt/homebrew/lib/libomt.dylib");
  out.emplace_back("libomt.dylib");
#else
  // There is no published Linux libomt — it wraps .NET's libomtnet and the
  // vendor ships no Linux build. These are tried anyway so that an operator who
  // has built one is not blocked, but expect this to fail on Linux and report
  // it honestly rather than as a missing install.
  out.emplace_back("/usr/local/lib/libomt.so");
  out.emplace_back("libomt.so");
#endif
  return out;
}

bool loadRuntime(std::string& error) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_tried) {
    error = g_error;
    return g_ok;
  }
  g_tried = true;

  if (!g_lib.open(runtimeCandidates())) {
#ifdef __linux__
    g_error =
        "libomt is not available on this machine, and there is no published "
        "Linux build of it — it wraps the .NET assembly libomtnet, which the "
        "vendor ships for macOS and Windows only. OMT nodes cannot run here. "
        "See " +
        std::string(OmtRuntime::downloadUrl());
#else
    g_error = "the OMT runtime (libomt) is not installed. Frame Ferret does "
              "not ship it — it embeds the .NET runtime — so install it from " +
              std::string(OmtRuntime::downloadUrl()) +
              " (details: " + g_lib.lastError() + ")";
#endif
    error = g_error;
    return false;
  }

  auto need = [&](const char* name, auto& fn) {
    if (!g_lib.symbol(name, fn)) {
      g_error = std::string("the OMT runtime at ") + g_lib.loadedPath() +
                " is missing " + name;
      return false;
    }
    return true;
  };

  if (!need("omt_receive_create", g_api.receive_create) ||
      !need("omt_receive_destroy", g_api.receive_destroy) ||
      !need("omt_receive", g_api.receive) ||
      !need("omt_send_create", g_api.send_create) ||
      !need("omt_send_destroy", g_api.send_destroy) ||
      !need("omt_send", g_api.send)) {
    error = g_error;
    return false;
  }

  g_lib.symbol("omt_send_connections", g_api.send_connections);
  g_lib.symbol("omt_discovery_getaddresses", g_api.discovery_getaddresses);

  g_ok = true;
  return true;
}

PixelFormat fromCodec(int32_t codec) {
  switch (codec) {
    case kCodecUyvy:
    case kCodecUyva:  // alpha ignored; the luma/chroma layout is UYVY's
      return PixelFormat::uyvy8;
    case kCodecYuy2: return PixelFormat::yuy2_8;
    case kCodecBgra: return PixelFormat::bgra8;
    case kCodecNv12: return PixelFormat::nv12;
    default: return PixelFormat::unknown;  // VMX1 and the 16-bit formats
  }
}

int32_t toCodec(PixelFormat format) {
  switch (format) {
    case PixelFormat::uyvy8: return kCodecUyvy;
    case PixelFormat::yuy2_8: return kCodecYuy2;
    case PixelFormat::bgra8: return kCodecBgra;
    case PixelFormat::nv12: return kCodecNv12;
    default: return 0;
  }
}

// ---------------------------------------------------------------------------
// Receiver
// ---------------------------------------------------------------------------
class OmtSource : public Source {
 public:
  OmtSource(NodeConfig config, void* recv)
      : config_(std::move(config)), recv_(recv) {
    reader_ = std::thread([this] { readLoop(); });
  }

  ~OmtSource() override {
    stopping_.store(true);
    if (reader_.joinable()) reader_.join();
    if (recv_) g_api.receive_destroy(recv_);
  }

  const std::string& id() const override { return config_.id; }
  bool connected() const override { return connected_.load(); }

  /// Non-blocking. Hands over whatever the reader thread last captured.
  ///
  /// The blocking receive lives on its own thread so that a transport which
  /// stalls cannot hold up every other sink in the program. `omt_receive` is a
  /// blocking call into a .NET-backed library, and the frame loop serves all
  /// sinks from one thread, so anything slow here is felt everywhere.
  ///
  /// **An honest note, because the first version of this comment was wrong.**
  /// This thread was added while chasing an OMT receiver stuck at 19 fps with
  /// every tick late, and the comment here claimed libomt was ignoring short
  /// timeouts and blocking ~50 ms. That was a misattribution: the cost was the
  /// preview's UYVY->BGRA conversion (29.7 ms/frame at the time) plus two
  /// 3.7 MB allocations per frame. Fixing those took the same configuration to
  /// a clean 50 fps. Threading the receive is still the right shape for a
  /// network source, but it was *not* what fixed the frame rate, and no
  /// measurement here shows libomt mishandling its timeout.
  bool poll(unsigned,
            const std::function<void(const VideoFrame&)>& onVideo) override {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (!hasFrame_) return false;
    if (onVideo) onVideo(latest_.frame());
    return true;
  }

  std::unique_ptr<AudioFrame> takeAudio() override {
    std::lock_guard<std::mutex> lock(audioMutex_);
    return std::move(pendingAudio_);
  }

 private:
  void readLoop() {
    while (!stopping_.load()) {
      // A timeout long enough that libomt is not spinning, short enough that
      // shutdown stays responsive.
      MediaFrame* frame =
          g_api.receive(recv_, kFrameVideo | kFrameAudio, 100);
      if (!frame) continue;  // a timeout, not a disconnection

      connected_.store(true);

      if (frame->Type == kFrameVideo) {
        store(*frame);
      } else if (frame->Type == kFrameAudio) {
        takeAudioFrame(*frame);
      }
      // The frame is SDK-owned and valid only until the next omt_receive on
      // this receiver, which is why store() copies rather than retains.
    }
  }

  void store(const MediaFrame& frame) {
    const PixelFormat format = fromCodec(frame.Codec);
    if (format == PixelFormat::unknown || !frame.Data) return;

    VideoFrame f;
    f.width = frame.Width;
    f.height = frame.Height;
    f.strideBytes = frame.Stride > 0 ? frame.Stride
                                     : tightStrideBytes(format, frame.Width);
    f.data = static_cast<const uint8_t*>(frame.Data);
    f.format = format;

    // OMT states its colour space, so unlike NDI there is nothing to infer.
    // Fall back on the raster only when it says undefined.
    f.colour = frame.ColorSpace == kColour601   ? ColourSpace::bt601
               : frame.ColorSpace == kColour709 ? ColourSpace::bt709
               : (frame.Height >= 720 ? ColourSpace::bt709
                                      : ColourSpace::bt601);
    f.range = QuantRange::narrow;
    f.rate = Rate{frame.FrameRateN > 0 ? frame.FrameRateN : 50,
                  frame.FrameRateD > 0 ? frame.FrameRateD : 1};
    f.interlaced = (frame.Flags & kFlagInterlaced) != 0;
    f.timestampNs = frame.Timestamp * 100;  // 100 ns units
    f.ptpLocked = false;
    f.bottomUp = false;

    std::lock_guard<std::mutex> lock(frameMutex_);
    latest_.assign(f);
    hasFrame_ = true;
  }

  void takeAudioFrame(const MediaFrame& frame) {
    if (frame.Codec != kCodecFpa1 || !frame.Data) return;
    if (frame.Channels <= 0 || frame.SamplesPerChannel <= 0) return;

    auto out = std::make_unique<AudioFrame>();
    out->sampleRate = frame.SampleRate;
    out->channels = frame.Channels;
    out->samplesPerChannel = frame.SamplesPerChannel;
    out->timestampNs = frame.Timestamp * 100;
    out->data.resize(static_cast<size_t>(frame.Channels) *
                     frame.SamplesPerChannel);

    // OMT documents planar audio as exactly SamplesPerChannel*4 bytes per
    // plane, with no padding — unlike NDI, which pads. One copy is safe here.
    std::memcpy(out->data.data(), frame.Data,
                out->data.size() * sizeof(float));

    std::lock_guard<std::mutex> lock(audioMutex_);
    pendingAudio_ = std::move(out);
  }

  NodeConfig config_;
  void* recv_ = nullptr;
  std::thread reader_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> connected_{false};

  std::mutex frameMutex_;
  FrameBuffer latest_;
  bool hasFrame_ = false;

  std::mutex audioMutex_;
  std::unique_ptr<AudioFrame> pendingAudio_;
};

// ---------------------------------------------------------------------------
// Sender
// ---------------------------------------------------------------------------
class OmtSink : public Sink {
 public:
  OmtSink(NodeConfig config, void* send)
      : config_(std::move(config)), send_(send) {}

  ~OmtSink() override {
    if (send_) g_api.send_destroy(send_);
  }

  const std::string& id() const override { return config_.id; }

  /// UYVY first — it is what OMT's VMX encoder wants and half the bytes of
  /// BGRA. BGRA second for sources that carry alpha.
  std::vector<PixelFormat> preferredFormats() const override {
    return {PixelFormat::uyvy8, PixelFormat::bgra8};
  }

  void send(const VideoFrame& frame) override {
    if (!send_ || !frame.data) return;
    const int32_t codec = toCodec(frame.format);
    if (codec == 0) return;

    MediaFrame m;
    m.Type = kFrameVideo;
    // An explicit timestamp, never kTimestampAuto: -1 asks OMT to throttle to
    // the declared rate, and the router already paces this sink. Two clocks on
    // one path is how a sender ends up a frame behind and jittering.
    m.Timestamp = frame.timestampNs / 100;
    m.Codec = codec;
    m.Width = frame.width;
    m.Height = frame.height;
    m.Stride = frame.strideBytes;
    m.Flags = frame.interlaced ? kFlagInterlaced : kFlagNone;
    m.FrameRateN = static_cast<int32_t>(frame.rate.num);
    m.FrameRateD = static_cast<int32_t>(frame.rate.den);
    m.AspectRatio = frame.height > 0 ? static_cast<float>(frame.width) /
                                           static_cast<float>(frame.height)
                                     : 0.0f;
    m.ColorSpace = frame.colour == ColourSpace::bt601 ? kColour601 : kColour709;
    m.Data = const_cast<uint8_t*>(frame.data);
    m.DataLength = frame.strideBytes * frame.height;

    g_api.send(send_, &m);
  }

  void sendAudio(const AudioFrame& frame) override {
    if (!send_ || frame.data.empty() || frame.channels <= 0) return;

    MediaFrame m;
    m.Type = kFrameAudio;
    m.Timestamp = frame.timestampNs / 100;
    m.Codec = kCodecFpa1;
    m.SampleRate = frame.sampleRate;
    m.Channels = frame.channels;
    m.SamplesPerChannel = frame.samplesPerChannel;
    m.Data = const_cast<float*>(frame.data.data());
    m.DataLength = static_cast<int32_t>(frame.data.size() * sizeof(float));

    g_api.send(send_, &m);
  }

  void sendBlack() override {
    if (!send_) return;

    const int w = config_.width > 0 ? config_.width : 1920;
    const int h = config_.height > 0 ? config_.height : 1080;
    const int stride = tightStrideBytes(PixelFormat::uyvy8, w);
    const size_t bytes = static_cast<size_t>(stride) * h;

    if (black_.size() != bytes) {
      black_.assign(bytes, 0);
      fillBlack(PixelFormat::uyvy8, w, h, stride, QuantRange::narrow,
                black_.data());
    }

    VideoFrame f;
    f.width = w;
    f.height = h;
    f.strideBytes = stride;
    f.data = black_.data();
    f.format = PixelFormat::uyvy8;
    f.colour = ColourSpace::bt709;
    f.range = QuantRange::narrow;
    f.rate = config_.rate;
    send(f);
  }

 private:
  NodeConfig config_;
  void* send_ = nullptr;
  std::vector<uint8_t> black_;
};

}  // namespace

const char* OmtRuntime::downloadUrl() {
  return "https://github.com/openmediatransport";
}

bool OmtRuntime::available() {
  std::string ignored;
  return loadRuntime(ignored);
}

std::string OmtRuntime::loadedPath() {
  std::string ignored;
  if (!loadRuntime(ignored)) return {};
  return g_lib.loadedPath();
}

std::string OmtRuntime::unavailableReason() {
  std::string error;
  if (loadRuntime(error)) return {};
  return error;
}

bool OmtRuntime::dotNetInitialised() { return g_dotNetUp.load(); }

std::vector<std::string> omtListSources(std::string& error) {
  std::vector<std::string> out;
  if (!loadRuntime(error)) return out;
  if (!g_api.discovery_getaddresses) {
    error = "this libomt has no omt_discovery_getaddresses";
    return out;
  }

  // Poll, don't ask once.
  //
  // libomt's discovery browser starts on the first call and has learned
  // nothing at that moment, so a single call reliably returns zero even when
  // senders are plainly on the network — confirmed with macOS's own
  // `dns-sd -B _omt._tcp`, which listed a sender this function was returning
  // nothing for. NDI has the same shape and gives you
  // NDIlib_find_wait_for_sources for it; OMT exposes no wait, so poll.
  for (int attempt = 0; attempt < 20; ++attempt) {
    int count = 0;
    char** addresses = g_api.discovery_getaddresses(&count);
    if (addresses && count > 0) {
      for (int i = 0; i < count; ++i) {
        if (addresses[i]) out.emplace_back(addresses[i]);
      }
      return out;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return out;
}

std::unique_ptr<Source> makeOmtSource(const NodeConfig& config,
                                      std::string& error) {
  if (!loadRuntime(error)) return nullptr;
  if (config.target.empty()) {
    error =
        "an OMT source needs a \"target\" address — OMT has no browse-by-name, "
        "so give it what `frame-ferret sources --protocol omt` lists";
    return nullptr;
  }

  void* recv = g_api.receive_create(config.target.c_str(),
                                    kFrameVideo | kFrameAudio,
                                    kPreferUyvyOrBgra, kReceiveNone);
  if (!recv) {
    error = "omt_receive_create failed for \"" + config.target + "\"";
    return nullptr;
  }
  // .NET comes up inside this call, taking the signal handlers with it.
  g_dotNetUp.store(true);
  return std::make_unique<OmtSource>(config, recv);
}

std::unique_ptr<Sink> makeOmtSink(const NodeConfig& config,
                                  std::string& error) {
  if (!loadRuntime(error)) return nullptr;

  const std::string name = config.target.empty() ? config.id : config.target;
  void* send = g_api.send_create(name.c_str(), kQualityDefault);
  if (!send) {
    error = "omt_send_create failed for \"" + name + "\"";
    return nullptr;
  }
  g_dotNetUp.store(true);
  return std::make_unique<OmtSink>(config, send);
}

}  // namespace ferret

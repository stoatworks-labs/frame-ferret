#include "transports/ndi.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

#include "core/convert.h"
#include "core/dylib.h"
#include "transports/ndi_abi.h"

namespace ferret {
namespace {

using namespace ndi_abi;

// ---------------------------------------------------------------------------
// The entry points we bind. Flat C ABI only.
// ---------------------------------------------------------------------------
struct Api {
  bool (*initialize)() = nullptr;
  void (*destroy)() = nullptr;
  const char* (*version)() = nullptr;

  void* (*find_create_v2)(const FindCreate*) = nullptr;
  void (*find_destroy)(void*) = nullptr;
  const SourceRef* (*find_get_current_sources)(void*, uint32_t*) = nullptr;
  bool (*find_wait_for_sources)(void*, uint32_t) = nullptr;

  void* (*recv_create_v3)(const RecvCreateV3*) = nullptr;
  void (*recv_destroy)(void*) = nullptr;
  int (*recv_capture_v3)(void*, VideoFrameV2*, AudioFrameV3*, void*,
                         uint32_t) = nullptr;
  void (*recv_free_video_v2)(void*, const VideoFrameV2*) = nullptr;
  void (*recv_free_audio_v3)(void*, const AudioFrameV3*) = nullptr;
  int (*recv_get_no_connections)(void*) = nullptr;

  void* (*send_create)(const SendCreate*) = nullptr;
  void (*send_destroy)(void*) = nullptr;
  void (*send_send_video_v2)(void*, const VideoFrameV2*) = nullptr;
  void (*send_send_audio_v3)(void*, const AudioFrameV3*) = nullptr;
};

Dylib g_lib;
Api g_api;
bool g_tried = false;
bool g_ok = false;
std::string g_error;
std::mutex g_mutex;

std::vector<std::string> runtimeCandidates() {
  std::vector<std::string> out;

  // An explicit override always wins, and is how the "no runtime installed"
  // path gets tested on a machine that has one.
  if (const char* env = std::getenv("FERRET_NDI_RUNTIME")) {
    if (*env) out.emplace_back(env);
  }
  // The SDK's own environment variable, which NDI Tools sets.
  if (const char* env = std::getenv("NDI_RUNTIME_DIR_V6")) {
    if (*env) {
      out.emplace_back(std::string(env) + "/libndi.dylib");
      out.emplace_back(std::string(env) + "/libndi.so.6");
      out.emplace_back(std::string(env) + "/Processing.NDI.Lib.x64.dll");
    }
  }

  for (const auto& dir : Dylib::localSearchPaths()) {
#ifdef _WIN32
    out.emplace_back(dir + "\\Processing.NDI.Lib.x64.dll");
#elif defined(__APPLE__)
    out.emplace_back(dir + "/libndi.dylib");
#else
    out.emplace_back(dir + "/libndi.so.6");
#endif
  }

#ifdef _WIN32
  out.emplace_back("Processing.NDI.Lib.x64.dll");
  out.emplace_back(
      "C:\\Program Files\\NDI\\NDI 6 Runtime\\v6\\Processing.NDI.Lib.x64.dll");
#elif defined(__APPLE__)
  out.emplace_back("/usr/local/lib/libndi.dylib");
  out.emplace_back("/Library/NDI SDK for Apple/lib/macOS/libndi.dylib");
  out.emplace_back("libndi.dylib");
#else
  // Version 6 first, then 5 — the flat C ABI is identical across both, which
  // is the whole reason for binding it rather than the versioned load struct.
  out.emplace_back("libndi.so.6");
  out.emplace_back("libndi.so.5");
  out.emplace_back("libndi.so");
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
    g_error =
        "the NDI runtime is not installed on this machine. Frame Ferret does "
        "not ship it — NDI's licence requires redistributors to forbid reverse "
        "engineering, which this repo's MIT licence permits — so install it "
        "from " +
        std::string(NdiRuntime::downloadUrl()) +
        " (details: " + g_lib.lastError() + ")";
    error = g_error;
    return false;
  }

  auto need = [&](const char* name, auto& fn) {
    if (!g_lib.symbol(name, fn)) {
      g_error = std::string("the NDI runtime at ") + g_lib.loadedPath() +
                " is missing " + name +
                " — it is probably too old; NDI 5 or later is required";
      return false;
    }
    return true;
  };

  if (!need("NDIlib_initialize", g_api.initialize) ||
      !need("NDIlib_destroy", g_api.destroy) ||
      !need("NDIlib_find_create_v2", g_api.find_create_v2) ||
      !need("NDIlib_find_destroy", g_api.find_destroy) ||
      !need("NDIlib_find_get_current_sources",
            g_api.find_get_current_sources) ||
      !need("NDIlib_find_wait_for_sources", g_api.find_wait_for_sources) ||
      !need("NDIlib_recv_create_v3", g_api.recv_create_v3) ||
      !need("NDIlib_recv_destroy", g_api.recv_destroy) ||
      !need("NDIlib_recv_capture_v3", g_api.recv_capture_v3) ||
      !need("NDIlib_recv_free_video_v2", g_api.recv_free_video_v2) ||
      !need("NDIlib_recv_free_audio_v3", g_api.recv_free_audio_v3) ||
      !need("NDIlib_send_create", g_api.send_create) ||
      !need("NDIlib_send_destroy", g_api.send_destroy) ||
      !need("NDIlib_send_send_video_v2", g_api.send_send_video_v2) ||
      !need("NDIlib_send_send_audio_v3", g_api.send_send_audio_v3)) {
    error = g_error;
    return false;
  }

  // Optional — only used for reporting, so a runtime without it still works.
  g_lib.symbol("NDIlib_version", g_api.version);
  g_lib.symbol("NDIlib_recv_get_no_connections", g_api.recv_get_no_connections);

  if (!g_api.initialize()) {
    // The documented reason is an unsupported CPU (NDI needs SSE4.2 on x86).
    g_error =
        "NDIlib_initialize() failed — the runtime loaded but refused to start, "
        "which usually means this CPU is unsupported";
    error = g_error;
    return false;
  }

  g_ok = true;
  return true;
}

/// Maps an NDI FourCC to ours. Returns unknown for the formats we do not take,
/// which the caller reports rather than guessing at.
PixelFormat fromFourCc(uint32_t fourCc) {
  switch (fourCc) {
    case kFourCcUyvy: return PixelFormat::uyvy8;
    case kFourCcBgra:
    case kFourCcBgrx: return PixelFormat::bgra8;
    case kFourCcRgba: return PixelFormat::rgba8;
    default: return PixelFormat::unknown;
  }
}

// ---------------------------------------------------------------------------
// Receiver
// ---------------------------------------------------------------------------
class NdiSource : public Source {
 public:
  NdiSource(NodeConfig config, void* recv)
      : config_(std::move(config)), recv_(recv) {}

  ~NdiSource() override {
    if (recv_) g_api.recv_destroy(recv_);
  }

  const std::string& id() const override { return config_.id; }

  bool connected() const override { return connected_; }

  bool poll(unsigned timeoutMs,
            const std::function<void(const VideoFrame&)>& onVideo) override {
    if (!recv_) return false;

    VideoFrameV2 video;
    AudioFrameV3 audio;
    const int type =
        g_api.recv_capture_v3(recv_, &video, &audio, nullptr, timeoutMs);

    switch (type) {
      case kFrameVideo: {
        deliver(video, onVideo);
        // SDK-owned memory. Freed before returning, always — including on the
        // unsupported-format path above, or the receiver leaks a frame per
        // tick and the process grows without bound.
        g_api.recv_free_video_v2(recv_, &video);
        connected_ = true;
        return true;
      }
      case kFrameAudio: {
        takeAudio(audio);
        g_api.recv_free_audio_v3(recv_, &audio);
        connected_ = true;
        return false;  // audio is not a video frame
      }
      case kFrameError:
        connected_ = false;
        return false;
      case kFrameNone:
        // A timeout is not a disconnection. NDI delivers nothing between
        // frames on a slow source, and treating that as a loss would flap the
        // output to black on every sub-rate source.
        return false;
      default:
        return false;
    }
  }

  std::unique_ptr<AudioFrame> takeAudio() override {
    std::lock_guard<std::mutex> lock(audioMutex_);
    return std::move(pendingAudio_);
  }

 private:
  void deliver(const VideoFrameV2& video,
               const std::function<void(const VideoFrame&)>& onVideo) {
    const PixelFormat format = fromFourCc(video.FourCC);
    if (format == PixelFormat::unknown || !video.p_data) return;

    VideoFrame f;
    f.width = video.xres;
    f.height = video.yres;
    f.strideBytes = video.line_stride_in_bytes > 0
                        ? video.line_stride_in_bytes
                        : tightStrideBytes(format, video.xres);
    f.data = video.p_data;
    f.format = format;
    // NDI carries no colorimetry, so it is inferred from the raster the way
    // every other NDI implementation does: 709 for HD and above.
    f.colour = video.yres >= 720 ? ColourSpace::bt709 : ColourSpace::bt601;
    f.range = QuantRange::narrow;
    f.rate = Rate{video.frame_rate_N, video.frame_rate_D};
    f.interlaced = video.frame_format_type == kFormatInterleaved;
    f.timestampNs = video.timestamp * 100;  // 100 ns units -> ns
    f.ptpLocked = false;

    // NDI's bottom-up receive format is Windows-only — the SDK guards
    // BGRX_BGRA_flipped with #ifdef _WIN32 — so rows arrive top-down
    // everywhere else. We never request the flipped format, so this stays
    // false; the field exists so a future change to the requested format has
    // somewhere honest to record itself.
    f.bottomUp = false;

    if (onVideo) onVideo(f);
  }

  void takeAudio(const AudioFrameV3& audio) {
    if (audio.FourCC != kFourCcFltp || !audio.p_data) return;
    if (audio.no_channels <= 0 || audio.no_samples <= 0) return;

    auto frame = std::make_unique<AudioFrame>();
    frame->sampleRate = audio.sample_rate;
    frame->channels = audio.no_channels;
    frame->samplesPerChannel = audio.no_samples;
    frame->timestampNs = audio.timestamp * 100;
    frame->data.resize(static_cast<size_t>(audio.no_channels) *
                       audio.no_samples);

    // Planar, with a per-channel stride that is NOT necessarily
    // no_samples * 4 — the SDK pads. Copying plane by plane rather than in one
    // memcpy is what makes that safe.
    const int stride = audio.channel_stride_in_bytes > 0
                           ? audio.channel_stride_in_bytes
                           : audio.no_samples * 4;
    for (int c = 0; c < audio.no_channels; ++c) {
      const auto* src = reinterpret_cast<const float*>(
          audio.p_data + static_cast<size_t>(c) * stride);
      std::memcpy(frame->data.data() + static_cast<size_t>(c) * audio.no_samples,
                  src, static_cast<size_t>(audio.no_samples) * sizeof(float));
    }

    std::lock_guard<std::mutex> lock(audioMutex_);
    pendingAudio_ = std::move(frame);
  }

  NodeConfig config_;
  void* recv_ = nullptr;
  bool connected_ = false;
  std::mutex audioMutex_;
  std::unique_ptr<AudioFrame> pendingAudio_;
};

// ---------------------------------------------------------------------------
// Sender
// ---------------------------------------------------------------------------
class NdiSink : public Sink {
 public:
  NdiSink(NodeConfig config, void* send, std::string name)
      : config_(std::move(config)), send_(send), name_(std::move(name)) {}

  ~NdiSink() override {
    if (send_) g_api.send_destroy(send_);
  }

  const std::string& id() const override { return config_.id; }

  /// UYVY first: it is NDI's native wire format, half the bytes of BGRA, and
  /// the one the SDK compresses without an extra conversion of its own. BGRA
  /// is offered second because an RGB source with alpha should not be forced
  /// through a chroma subsample when the operator wanted a key.
  std::vector<PixelFormat> preferredFormats() const override {
    return {PixelFormat::uyvy8, PixelFormat::bgra8};
  }

  void send(const VideoFrame& frame) override {
    if (!send_ || !frame.data) return;

    VideoFrameV2 v;
    v.xres = frame.width;
    v.yres = frame.height;
    v.FourCC = frame.format == PixelFormat::uyvy8   ? kFourCcUyvy
               : frame.format == PixelFormat::rgba8 ? kFourCcRgba
                                                    : kFourCcBgra;
    v.frame_rate_N = static_cast<int32_t>(frame.rate.num);
    v.frame_rate_D = static_cast<int32_t>(frame.rate.den);
    v.picture_aspect_ratio = 0.0f;  // 0 means square pixels
    v.frame_format_type =
        frame.interlaced ? kFormatInterleaved : kFormatProgressive;
    v.timecode = kTimecodeSynthesise;
    v.p_data = const_cast<uint8_t*>(frame.data);
    v.line_stride_in_bytes = frame.strideBytes;
    v.p_metadata = nullptr;
    v.timestamp = 0;

    // Synchronous: the SDK copies before returning, so `frame.data` may be
    // reused the moment this comes back. The async variant would need us to
    // keep the buffer alive until the *next* call, which the router's
    // single scratch buffer cannot promise.
    g_api.send_send_video_v2(send_, &v);
    ++frames_;
  }

  void sendAudio(const AudioFrame& frame) override {
    if (!send_ || frame.data.empty() || frame.channels <= 0) return;

    AudioFrameV3 a;
    a.sample_rate = frame.sampleRate;
    a.no_channels = frame.channels;
    a.no_samples = frame.samplesPerChannel;
    a.timecode = kTimecodeSynthesise;
    a.FourCC = kFourCcFltp;
    a.p_data = const_cast<uint8_t*>(
        reinterpret_cast<const uint8_t*>(frame.data.data()));
    a.channel_stride_in_bytes =
        static_cast<int32_t>(frame.samplesPerChannel * sizeof(float));
    a.p_metadata = nullptr;
    a.timestamp = 0;

    g_api.send_send_audio_v3(send_, &a);
  }

  void sendBlack() override {
    if (!send_) return;

    // A real black frame, at the configured raster, every time it is asked
    // for. Not "send nothing": an NDI receiver that stops getting frames shows
    // its last one and then times out, which on a video wall is a frozen
    // picture rather than an obvious fault.
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

  uint64_t frames() const { return frames_; }

 private:
  NodeConfig config_;
  void* send_ = nullptr;
  std::string name_;
  std::vector<uint8_t> black_;
  uint64_t frames_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------

const char* NdiRuntime::downloadUrl() {
  // NDILIB_REDIST_URL from the SDK headers is EMPTY on Linux — no one-click
  // redistributable exists there — so never print that; use the page that
  // always exists.
  return "https://ndi.video/for-developers/ndi-sdk/";
}

NdiRuntime* NdiRuntime::instance(std::string& error) {
  static NdiRuntime runtime;
  return loadRuntime(error) ? &runtime : nullptr;
}

bool NdiRuntime::available() {
  std::string ignored;
  return loadRuntime(ignored);
}

std::string NdiRuntime::loadedPath() {
  std::string ignored;
  if (!loadRuntime(ignored)) return {};
  return g_lib.loadedPath();
}

std::string NdiRuntime::unavailableReason() {
  std::string error;
  if (loadRuntime(error)) return {};
  return error;
}

std::vector<std::string> ndiListSources(unsigned waitMs, std::string& error) {
  std::vector<std::string> out;
  if (!loadRuntime(error)) return out;

  FindCreate create;
  create.show_local_sources = true;
  void* finder = g_api.find_create_v2(&create);
  if (!finder) {
    error = "NDIlib_find_create_v2 failed";
    return out;
  }

  // Discovery needs a moment before its first answer means anything — asking
  // immediately reliably returns nothing on a network that has plenty.
  g_api.find_wait_for_sources(finder, waitMs);

  uint32_t count = 0;
  const SourceRef* sources = g_api.find_get_current_sources(finder, &count);
  for (uint32_t i = 0; i < count; ++i) {
    if (sources[i].p_ndi_name) out.emplace_back(sources[i].p_ndi_name);
  }

  g_api.find_destroy(finder);
  return out;
}

std::unique_ptr<Source> makeNdiSource(const NodeConfig& config,
                                      std::string& error) {
  if (!loadRuntime(error)) return nullptr;

  FindCreate create;
  create.show_local_sources = true;
  void* finder = g_api.find_create_v2(&create);
  if (!finder) {
    error = "NDIlib_find_create_v2 failed";
    return nullptr;
  }

  g_api.find_wait_for_sources(finder, 2000);
  uint32_t count = 0;
  const SourceRef* sources = g_api.find_get_current_sources(finder, &count);

  const SourceRef* chosen = nullptr;
  for (uint32_t i = 0; i < count; ++i) {
    if (!sources[i].p_ndi_name) continue;
    if (config.target.empty()) {
      chosen = &sources[i];
      break;
    }
    if (std::string(sources[i].p_ndi_name).find(config.target) !=
        std::string::npos) {
      chosen = &sources[i];
      break;
    }
  }

  if (!chosen) {
    error = config.target.empty()
                ? "no NDI sources are visible on the network"
                : "no NDI source matching \"" + config.target +
                      "\" is visible (" + std::to_string(count) + " found)";
    g_api.find_destroy(finder);
    return nullptr;
  }

  RecvCreateV3 recvCreate;
  recvCreate.source_to_connect_to = *chosen;
  // BGRX_BGRA gives us 8-bit RGB and asks the SDK to do the YCbCr conversion.
  // UYVY_BGRA is the better default here: it hands over UYVY untouched for
  // sources that are already 4:2:2, which is nearly all of them, and only
  // converts when the source genuinely carries alpha.
  recvCreate.color_format = kRecvUyvyBgra;
  recvCreate.bandwidth = kBandwidthHighest;
  recvCreate.allow_video_fields = true;
  const std::string recvName = "Frame Ferret " + config.id;
  recvCreate.p_ndi_recv_name = recvName.c_str();

  void* recv = g_api.recv_create_v3(&recvCreate);
  g_api.find_destroy(finder);  // the receiver has copied what it needs

  if (!recv) {
    error = "NDIlib_recv_create_v3 failed";
    return nullptr;
  }
  return std::make_unique<NdiSource>(config, recv);
}

std::unique_ptr<Sink> makeNdiSink(const NodeConfig& config,
                                  std::string& error) {
  if (!loadRuntime(error)) return nullptr;

  const std::string name =
      config.target.empty() ? config.id : config.target;

  SendCreate create;
  create.p_ndi_name = name.c_str();
  create.p_groups = nullptr;
  // Both false: the router already paces every sink from the rational clock,
  // and letting the SDK also rate-limit would fight it — two clocks on one
  // path is how a sender ends up sitting a frame behind and jittering.
  create.clock_video = false;
  create.clock_audio = false;

  void* send = g_api.send_create(&create);
  if (!send) {
    error = "NDIlib_send_create failed for \"" + name + "\"";
    return nullptr;
  }
  return std::make_unique<NdiSink>(config, send, name);
}

}  // namespace ferret

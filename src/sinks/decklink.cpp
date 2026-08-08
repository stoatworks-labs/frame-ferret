// DeckLink output.
//
// The scheduled-playback sequence here is lifted from oxbow's
// `src/io/decklink.cpp`, which has been run on a real Duo 2: connector 1 out,
// connector 4 in, cabled, with all eight colour bars captured back in the right
// places. That is the reason to copy it faithfully rather than write it fresh.
//
// **Nothing in this file has been run against a card from Frame Ferret.** No
// DeckLink was attached to this machine when it was written. The sequence is
// proven; this port of it is not.
//
// What differs from oxbow's version, deliberately:
//
//   - **v210 first.** oxbow sends BGRA only, because its pump holds BGRA.
//     Frame Ferret carries the pixel format on the frame precisely so that a
//     10-bit source can reach the card without being quantised, so this sink
//     advertises v210 ahead of BGRA and the router picks the best available.
//   - **sendBlack() puts a real black frame on the timeline.** An SDI output
//     that stops has to be re-locked by whatever is downstream, so the
//     plan-every-sink invariant matters more here than almost anywhere else.
//
// Scheduled playback, not DisplayVideoFrameSync: the card runs a timeline, the
// host stays a few frames ahead, and each frame appears at the moment it was
// scheduled for. The synchronous call is simpler and judders.

#include "sinks/decklink.h"

#include "core/convert.h"

#ifndef FERRET_HAVE_DECKLINK

// ---------------------------------------------------------------------------
// Built without the SDK. Everything still answers, and says why.
// ---------------------------------------------------------------------------
namespace ferret {

namespace {
const char* kNoSdk =
    "this build has no DeckLink support: the Blackmagic SDK is not vendored "
    "(its licence is not ours to redistribute), so configure with "
    "-DDECKLINK_SDK_DIR=/path/to/DeckLink_SDK to enable it";
}

bool DeckLinkRuntime::builtIn() { return false; }
bool DeckLinkRuntime::available() { return false; }
std::string DeckLinkRuntime::unavailableReason() { return kNoSdk; }
std::vector<std::string> DeckLinkRuntime::listDevices() { return {}; }

std::unique_ptr<Sink> makeDeckLinkSink(const NodeConfig&, std::string& error) {
  error = kNoSdk;
  return nullptr;
}

}  // namespace ferret

#else  // FERRET_HAVE_DECKLINK

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "DeckLinkAPI.h"

namespace ferret {
namespace {

/// How many frames to stay ahead of the card. Three is Blackmagic's usual
/// suggestion: enough that a late tick does not underflow, few enough that the
/// added latency stays near one frame at 50/60p.
constexpr int kPreRollFrames = 3;

#if defined(__APPLE__)
/// GetDisplayName returns CFStringRef on macOS, BSTR on Windows and char* on
/// Linux. One place knows which.
std::string toStdString(CFStringRef value) {
  if (!value) return {};
  char buffer[256] = {};
  const bool ok =
      CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8);
  CFRelease(value);
  return ok ? std::string(buffer) : std::string();
}
using DeckLinkString = CFStringRef;
#elif defined(_WIN32)
std::string toStdString(BSTR value) {
  if (!value) return {};
  const int length =
      WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  std::string out(length > 0 ? length - 1 : 0, '\0');
  if (length > 0) {
    WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), length, nullptr,
                        nullptr);
  }
  SysFreeString(value);
  return out;
}
using DeckLinkString = BSTR;
#else
std::string toStdString(const char* value) {
  if (!value) return {};
  std::string out(value);
  free(const_cast<char*>(value));
  return out;
}
using DeckLinkString = const char*;
#endif

BMDPixelFormat toBmdFormat(PixelFormat format) {
  switch (format) {
    case PixelFormat::v210: return bmdFormat10BitYUV;
    case PixelFormat::uyvy8: return bmdFormat8BitYUV;
    case PixelFormat::bgra8: return bmdFormat8BitBGRA;
    default: return 0;
  }
}

/// Releases our reference once the card has finished displaying a frame.
///
/// The card takes its own reference in ScheduleVideoFrame, so a frame lives
/// until both are gone. Releasing at schedule time frees the buffer while the
/// card is still reading it — which shows as tearing, or as nothing at all,
/// depending on the allocator's mood.
class CompletionCallback final : public IDeckLinkVideoOutputCallback {
 public:
  HRESULT STDMETHODCALLTYPE
  ScheduledFrameCompleted(IDeckLinkVideoFrame* completedFrame,
                          BMDOutputFrameCompletionResult result) override {
    if (result == bmdOutputFrameDisplayedLate) ++late_;
    if (result == bmdOutputFrameDropped) ++dropped_;
    if (completedFrame) completedFrame->Release();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override {
    return S_OK;
  }

  // The card never queries this across apartments and its lifetime is the
  // sink's, so the reference count is a formality the interface requires.
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override {
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  int64_t late() const { return late_.load(std::memory_order_relaxed); }
  int64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  std::atomic<int64_t> late_{0};
  std::atomic<int64_t> dropped_{0};
};

class DeckLinkSink final : public Sink {
 public:
  explicit DeckLinkSink(NodeConfig config) : config_(std::move(config)) {}

  ~DeckLinkSink() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (output_) {
      if (playing_) {
        BMDTimeValue stoppedAt = 0;
        output_->StopScheduledPlayback(0, &stoppedAt, timeScale_);
      }
      output_->SetScheduledFrameCompletionCallback(nullptr);
      output_->DisableVideoOutput();
      output_->Release();
    }
    if (device_) device_->Release();

    if (scheduled_ > 0) {
      std::fprintf(stderr, "decklink: %lld frames, %lld late, %lld dropped\n",
                   (long long)scheduled_, (long long)callback_.late(),
                   (long long)callback_.dropped());
    }
  }

  const std::string& id() const override { return config_.id; }

  /// YCbCr only — v210 first, UYVY second. **BGRA is deliberately absent.**
  ///
  /// The router treats any format it accepts as a copy, and rightly so: that
  /// is what stops it converting for no reason. But a card advertising BGRA
  /// therefore *gets* BGRA whenever the source happens to be RGB, and a Duo 2
  /// will not carry 1080p50 as 8-bit BGRA at all — the whole output then fails
  /// with "will not carry that mode", having never tried the format it can do.
  ///
  /// So this sink advertises only what the card reliably carries at broadcast
  /// rasters, and an RGB source becomes an explicit conversion to v210 — which
  /// is the 10-bit path the pixel-format model exists for. BGRA comes back when
  /// key+fill is implemented, where alpha actually needs it.
  std::vector<PixelFormat> preferredFormats() const override {
    return {PixelFormat::v210, PixelFormat::uyvy8};
  }

  void send(const VideoFrame& frame) override {
    if (!frame.data || frame.width <= 0 || frame.height <= 0) return;

    const BMDPixelFormat bmd = toBmdFormat(frame.format);
    if (bmd == 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureOpen(frame, bmd)) return;

    IDeckLinkMutableVideoFrame* cardFrame = nullptr;
    const HRESULT created = output_->CreateVideoFrame(
        frame.width, frame.height, frame.strideBytes, bmd,
        bmdFrameFlagDefault, &cardFrame);
    if (created != S_OK || !cardFrame) {
      if (!reportedCreate_) {
        reportedCreate_ = true;
        std::fprintf(stderr, "decklink: CreateVideoFrame failed (0x%08x)\n",
                     (unsigned)created);
      }
      return;
    }

    void* bytes = nullptr;
    if (cardFrame->GetBytes(&bytes) == S_OK && bytes) {
      // The card picks its own row alignment, which need not match ours — v210
      // in particular is padded to 128 bytes on most devices, so this copy is
      // per row rather than one memcpy whenever the strides differ.
      const long cardStride = cardFrame->GetRowBytes();
      auto* destination = static_cast<uint8_t*>(bytes);
      const size_t copyable =
          std::min<size_t>(static_cast<size_t>(cardStride),
                           static_cast<size_t>(frame.strideBytes));
      if (static_cast<long>(frame.strideBytes) == cardStride) {
        std::memcpy(destination, frame.data,
                    static_cast<size_t>(cardStride) * frame.height);
      } else {
        for (int y = 0; y < frame.height; ++y) {
          std::memcpy(destination + static_cast<size_t>(y) * cardStride,
                      frame.data + static_cast<size_t>(y) * frame.strideBytes,
                      copyable);
        }
      }
    }

    scheduleFrame(cardFrame);
  }

  void sendBlack() override {
    // A real black frame on the timeline, every time. An SDI output that stops
    // has to be re-locked by whatever is downstream, so going quiet is worse
    // here than anywhere else in the program.
    const int w = config_.width > 0 ? config_.width : 1920;
    const int h = config_.height > 0 ? config_.height : 1080;
    const PixelFormat format =
        openFormat_ != PixelFormat::unknown ? openFormat_ : PixelFormat::v210;
    const int stride = tightStrideBytes(format, w);
    const size_t bytes = static_cast<size_t>(stride) * h;

    if (black_.size() != bytes || blackFormat_ != format) {
      black_.assign(bytes, 0);
      fillBlack(format, w, h, stride, QuantRange::narrow, black_.data());
      blackFormat_ = format;
    }

    VideoFrame f;
    f.width = w;
    f.height = h;
    f.strideBytes = stride;
    f.data = black_.data();
    f.format = format;
    f.colour = ColourSpace::bt709;
    f.range = QuantRange::narrow;
    f.rate = config_.rate;
    send(f);
  }

 private:
  /// Schedules at the next free slot on the card's timeline.
  ///
  /// Display times come from a monotonically increasing frame index, never from
  /// wall-clock arithmetic, so a late tick shortens the queue instead of
  /// scheduling a frame in the past — which the card rejects outright.
  void scheduleFrame(IDeckLinkMutableVideoFrame* frame) {
    const HRESULT ok = output_->ScheduleVideoFrame(
        frame, scheduled_ * frameDuration_, frameDuration_, timeScale_);
    if (ok != S_OK) {
      if (!reportedSchedule_) {
        reportedSchedule_ = true;
        std::fprintf(stderr,
                     "decklink: ScheduleVideoFrame failed (0x%08x) at t=%lld\n",
                     (unsigned)ok, (long long)(scheduled_ * frameDuration_));
      }
      frame->Release();
      return;
    }
    ++scheduled_;

    // Playback starts once there is a queue to play. Starting on the first
    // frame underflows immediately.
    if (!playing_ && scheduled_ >= kPreRollFrames) {
      const HRESULT started =
          output_->StartScheduledPlayback(0, timeScale_, 1.0);
      playing_ = started == S_OK;
      std::fprintf(stderr, "decklink: StartScheduledPlayback %s (0x%08x)\n",
                   playing_ ? "ok" : "FAILED", (unsigned)started);
    }
  }

  bool ensureOpen(const VideoFrame& frame, BMDPixelFormat bmd) {
    if (open_) {
      // A raster or format change mid-run needs the output torn down and
      // re-enabled. Refuse rather than half-do it, and say so once.
      if (frame.width != width_ || frame.height != height_ ||
          frame.format != openFormat_) {
        if (!warnedChange_) {
          warnedChange_ = true;
          std::fprintf(stderr,
                       "decklink: input changed to %dx%d %s; this output stays "
                       "at %dx%d %s\n",
                       frame.width, frame.height, toString(frame.format),
                       width_, height_, toString(openFormat_));
        }
        return false;
      }
      return true;
    }
    if (failed_) return false;

    if (!openDevice()) return fail();
    if (!findDisplayMode(frame, bmd)) return fail();

    if (output_->SetScheduledFrameCompletionCallback(&callback_) != S_OK) {
      std::fprintf(stderr, "decklink: SetScheduledFrameCompletionCallback "
                           "failed\n");
      return fail();
    }
    if (output_->EnableVideoOutput(displayMode_, bmdVideoOutputFlagDefault) !=
        S_OK) {
      std::fprintf(stderr, "decklink: EnableVideoOutput failed on \"%s\"\n",
                   deviceName_.c_str());
      return fail();
    }

    width_ = frame.width;
    height_ = frame.height;
    openFormat_ = frame.format;
    open_ = true;
    std::fprintf(stderr, "decklink: \"%s\" %dx%d %.3f fps, %s\n",
                 deviceName_.c_str(), width_, height_,
                 frameDuration_ > 0
                     ? double(timeScale_) / double(frameDuration_)
                     : 0.0,
                 toString(openFormat_));
    return true;
  }

  bool fail() {
    failed_ = true;
    return false;
  }

  bool openDevice() {
    IDeckLinkIterator* iterator = CreateDeckLinkIteratorInstance();
    if (!iterator) {
      std::fprintf(stderr, "decklink: no drivers found — is Desktop Video "
                           "installed?\n");
      return false;
    }

    const std::string& selector = config_.target;
    const bool byIndex =
        !selector.empty() &&
        selector.find_first_not_of("0123456789") == std::string::npos;
    const int wanted = byIndex ? std::atoi(selector.c_str()) : -1;

    std::vector<std::string> seen;
    IDeckLink* device = nullptr;
    int index = 0;
    while (iterator->Next(&device) == S_OK) {
      DeckLinkString rawName = nullptr;
      device->GetDisplayName(&rawName);
      const std::string name = toStdString(rawName);
      seen.push_back(name);

      const bool matches =
          selector.empty()
              ? index == 0
              : (byIndex ? index == wanted
                         : name.find(selector) != std::string::npos);
      if (matches &&
          device->QueryInterface(IID_IDeckLinkOutput, (void**)&output_) ==
              S_OK) {
        device_ = device;
        deviceName_ = name;
        break;
      }
      device->Release();
      device = nullptr;
      ++index;
    }
    iterator->Release();

    if (!output_) {
      std::string list;
      for (size_t i = 0; i < seen.size(); ++i) {
        list += (i ? ", " : "") + std::to_string(i) + ": " + seen[i];
      }
      std::fprintf(stderr, "decklink: no output matching \"%s\" (%s)\n",
                   selector.c_str(),
                   list.empty() ? "no devices found" : list.c_str());
      return false;
    }
    return true;
  }

  bool findDisplayMode(const VideoFrame& frame, BMDPixelFormat bmd) {
    IDeckLinkDisplayModeIterator* iterator = nullptr;
    if (output_->GetDisplayModeIterator(&iterator) != S_OK || !iterator) {
      std::fprintf(stderr, "decklink: GetDisplayModeIterator failed\n");
      return false;
    }

    std::string offered;
    IDeckLinkDisplayMode* mode = nullptr;
    bool found = false;
    while (iterator->Next(&mode) == S_OK) {
      BMDTimeValue duration = 0;
      BMDTimeScale scale = 0;
      mode->GetFrameRate(&duration, &scale);

      const bool sizeMatches =
          mode->GetWidth() == frame.width && mode->GetHeight() == frame.height;
      // Cross-multiplied rationals: duration/scale is the period, so the rate
      // is scale/duration. Never compare these as doubles — 59.94 is 60000/1001
      // and nothing else.
      const bool rateMatches =
          duration > 0 && frame.rate.den > 0 &&
          static_cast<int64_t>(scale) * frame.rate.den ==
              static_cast<int64_t>(duration) * frame.rate.num;

      if (sizeMatches && rateMatches) {
        displayMode_ = mode->GetDisplayMode();
        frameDuration_ = duration;
        timeScale_ = scale;
        found = true;
        mode->Release();
        break;
      }
      if (sizeMatches) {
        offered += (offered.empty() ? "" : ", ") + std::to_string(scale) + "/" +
                   std::to_string(duration);
      }
      mode->Release();
    }
    iterator->Release();

    if (!found) {
      std::fprintf(stderr,
                   "decklink: \"%s\" cannot do %dx%d @ %s%s%s\n",
                   deviceName_.c_str(), frame.width, frame.height,
                   frame.rate.label().c_str(),
                   offered.empty() ? "" : " — it offers these rates there: ",
                   offered.c_str());
      return false;
    }

    // Ask the card too: enumeration lists what the hardware knows, not what
    // this connection and pixel format can actually carry.
    BMDDisplayMode actual = displayMode_;
    bool supported = false;
    if (output_->DoesSupportVideoMode(
            bmdVideoConnectionUnspecified, displayMode_, bmd,
            bmdNoVideoOutputConversion, bmdSupportedVideoModeDefault, &actual,
            &supported) == S_OK &&
        !supported) {
      std::fprintf(stderr, "decklink: \"%s\" will not carry that mode as %s\n",
                   deviceName_.c_str(), toString(frame.format));
      return false;
    }
    return true;
  }

  NodeConfig config_;
  std::string deviceName_;
  std::mutex mutex_;

  IDeckLink* device_ = nullptr;
  IDeckLinkOutput* output_ = nullptr;
  CompletionCallback callback_;

  BMDDisplayMode displayMode_ = bmdModeHD1080p50;
  BMDTimeValue frameDuration_ = 0;
  BMDTimeScale timeScale_ = 0;

  int width_ = 0;
  int height_ = 0;
  PixelFormat openFormat_ = PixelFormat::unknown;
  std::vector<uint8_t> black_;
  PixelFormat blackFormat_ = PixelFormat::unknown;

  int64_t scheduled_ = 0;
  bool open_ = false;
  bool playing_ = false;
  bool failed_ = false;
  bool warnedChange_ = false;
  bool reportedCreate_ = false;
  bool reportedSchedule_ = false;
};

}  // namespace

bool DeckLinkRuntime::builtIn() { return true; }

std::vector<std::string> DeckLinkRuntime::listDevices() {
  std::vector<std::string> out;
  IDeckLinkIterator* iterator = CreateDeckLinkIteratorInstance();
  if (!iterator) return out;

  IDeckLink* device = nullptr;
  while (iterator->Next(&device) == S_OK) {
    DeckLinkString rawName = nullptr;
    device->GetDisplayName(&rawName);
    out.push_back(toStdString(rawName));
    device->Release();
  }
  iterator->Release();
  return out;
}

bool DeckLinkRuntime::available() { return !listDevices().empty(); }

std::string DeckLinkRuntime::unavailableReason() {
  if (!CreateDeckLinkIteratorInstance()) {
    return "the DeckLink drivers are not installed — install Blackmagic "
           "Desktop Video";
  }
  if (listDevices().empty()) {
    return "the DeckLink drivers are installed but no device is connected";
  }
  return {};
}

std::unique_ptr<Sink> makeDeckLinkSink(const NodeConfig& config,
                                       std::string& error) {
  // Device selection is deferred to the first frame along with the raster and
  // rate, because the display mode cannot be chosen until they are known. A
  // missing card is therefore reported by the backend, not guessed at here.
  (void)error;
  return std::make_unique<DeckLinkSink>(config);
}

}  // namespace ferret

#endif  // FERRET_HAVE_DECKLINK

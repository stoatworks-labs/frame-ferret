// Display capture on Windows, with DXGI Desktop Duplication.
//
// Desktop Duplication is the API every Windows screen recorder and streamer
// sits on: one duplication per output, a frame handed over only when the
// desktop actually changed, on the GPU that drives the output. It is chosen
// over Windows.Graphics.Capture here because it needs no WinRT, takes a
// display by its GDI name, and works on the Microsoft Basic Render Driver a
// GPU-less VM has.
//
// Things that are easy to get wrong, and are handled below:
//
// - **Session 0 has no displays.** Run from ssh or as a service, every display
//   API answers empty and DuplicateOutput fails. Checked up front, with a
//   sentence (interactiveDesktop()).
// - **No change, no frame.** AcquireNextFrame times out on a still desktop.
//   That is not a loss; the last frame is held. A frame whose LastPresentTime
//   is zero only moved the pointer and is not counted.
// - **The duplication dies on every mode change**, desktop switch (lock, UAC)
//   and display renumbering, with DXGI_ERROR_ACCESS_LOST. The capture thread
//   reopens it, asking `resolve` for the display's name each time.
// - **The desktop's alpha is not defined.** It is often 255 and sometimes 0;
//   it is forced to 255 so a BGRA sink never sends a transparent picture.
#include "sources/screen_capture.h"

#if defined(_WIN32)

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

#include "core/frame.h"
#include "sources/windows_displays.h"
#include "sources/windows_duplication.h"

namespace ferret {
namespace {

using Microsoft::WRL::ComPtr;

/// One open duplication and what it needs to read frames back.
struct Duplication {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGIOutputDuplication> dup;
  ComPtr<ID3D11Texture2D> staging;
  D3D11_BOX box = {};
  std::wstring gdiName;
};

/// The DXGI output whose desktop slot is `gdiName`, on whichever adapter
/// enumerates it. An indirect display (VDD) is enumerated under the adapter
/// that renders it, not under a GPU of its own, so every adapter is searched.
bool findOutput(const std::wstring& gdiName, ComPtr<IDXGIAdapter1>& adapter,
                ComPtr<IDXGIOutput1>& output, std::string& error) {
  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    error = "CreateDXGIFactory1 failed: " + win::hresultText(hr);
    return false;
  }
  std::string seen;
  for (UINT a = 0;; ++a) {
    ComPtr<IDXGIAdapter1> candidate;
    if (factory->EnumAdapters1(a, &candidate) == DXGI_ERROR_NOT_FOUND) break;
    for (UINT o = 0;; ++o) {
      ComPtr<IDXGIOutput> out;
      if (candidate->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_OUTPUT_DESC desc;
      if (FAILED(out->GetDesc(&desc))) continue;
      if (gdiName == desc.DeviceName) {
        if (FAILED(out.As(&output))) {
          error = "this Windows has no IDXGIOutput1, which Desktop "
                  "Duplication needs (Windows 8 or later)";
          return false;
        }
        adapter = candidate;
        return true;
      }
      if (!seen.empty()) seen += ", ";
      seen += win::narrow(desc.DeviceName);
    }
  }
  error = "DXGI has no output " + win::narrow(gdiName) +
          (seen.empty() ? std::string(" (it lists none at all)")
                        : " (it lists " + seen + ")");
  return false;
}

/// What a failed DuplicateOutput means, as a sentence.
std::string duplicateFailure(HRESULT hr, const std::wstring& gdiName) {
  const std::string display = win::narrow(gdiName);
  switch (static_cast<unsigned long>(hr)) {
    case 0x80070005:  // E_ACCESSDENIED
    case 0x887A002B:  // DXGI_ERROR_ACCESS_DENIED
      return "Windows would not let Frame Ferret capture " + display +
             ": the desktop is locked or switched to a secure one, or this is "
             "not the signed-in user's session";
    case 0x887A0022:  // DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
      return display +
             " is already being captured by as many applications as Windows "
             "allows; close another screen recorder or streamer";
    case 0x887A0004:  // DXGI_ERROR_UNSUPPORTED
      return "the adapter driving " + display +
             " does not support Desktop Duplication. On a laptop with two "
             "GPUs, Frame Ferret has to run on the one the display hangs off "
             "(Settings > System > Display > Graphics)";
    case 0x887A0028:  // DXGI_ERROR_SESSION_DISCONNECTED
      return "the session showing " + display + " is disconnected";
    default:
      return "DuplicateOutput failed on " + display + ": " +
             win::hresultText(hr);
  }
}

bool openDuplication(const std::wstring& gdiName, const NodeConfig& config,
                     Duplication& out, std::string& error) {
  out = {};
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIOutput1> output;
  if (!findOutput(gdiName, adapter, output, error)) return false;

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
  // D3D_DRIVER_TYPE_UNKNOWN, because an adapter is given.
  HRESULT hr = D3D11CreateDevice(
      adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
      static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &out.device,
      nullptr, &out.context);
  if (FAILED(hr)) {
    error = "no Direct3D 11 device on the adapter driving " +
            win::narrow(gdiName) + ": " + win::hresultText(hr);
    return false;
  }

  hr = output->DuplicateOutput(out.device.Get(), &out.dup);
  if (FAILED(hr)) {
    error = duplicateFailure(hr, gdiName);
    return false;
  }

  DXGI_OUTDUPL_DESC desc;
  out.dup->GetDesc(&desc);
  if (desc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
    error = win::narrow(gdiName) + "'s desktop is in DXGI format " +
            std::to_string(desc.ModeDesc.Format) +
            ", not 8-bit BGRA: an HDR desktop needs DuplicateOutput1, which "
            "this does not use yet. Turn HDR off for this display";
    return false;
  }
  if (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
      desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
    error = win::narrow(gdiName) +
            " is rotated, and Desktop Duplication hands a rotated display "
            "over unrotated; set its orientation to Landscape";
    return false;
  }

  const int width = static_cast<int>(desc.ModeDesc.Width);
  const int height = static_cast<int>(desc.ModeDesc.Height);
  int x0 = 0, y0 = 0, x1 = width, y1 = height;
  if (config.cropW > 0 && config.cropH > 0) {
    x0 = std::clamp(config.cropX, 0, width);
    y0 = std::clamp(config.cropY, 0, height);
    x1 = std::clamp(config.cropX + config.cropW, x0, width);
    y1 = std::clamp(config.cropY + config.cropH, y0, height);
    if (x1 - x0 < 2 || y1 - y0 < 2) {
      error = "the crop is outside " + win::narrow(gdiName) + "'s " +
              std::to_string(width) + "x" + std::to_string(height) + " desktop";
      return false;
    }
  }
  out.box = {static_cast<UINT>(x0), static_cast<UINT>(y0), 0,
             static_cast<UINT>(x1), static_cast<UINT>(y1), 1};

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = static_cast<UINT>(x1 - x0);
  td.Height = static_cast<UINT>(y1 - y0);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  hr = out.device->CreateTexture2D(&td, nullptr, &out.staging);
  if (FAILED(hr)) {
    error = "could not make a readback texture: " + win::hresultText(hr);
    return false;
  }
  out.gdiName = gdiName;
  return true;
}

class DuplicationSource final : public Source {
 public:
  DuplicationSource(NodeConfig config, std::function<std::wstring(std::string*)> resolve,
                    Duplication first)
      : config_(std::move(config)),
        resolve_(std::move(resolve)),
        dup_(std::move(first)) {
    thread_ = std::thread([this] { run(); });
  }

  ~DuplicationSource() override {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  const std::string& id() const override { return config_.id; }

  /// True once a frame has arrived and while the duplication is open. A
  /// display that has gone (lost and not reopened) reports false, so the
  /// sinks it feeds go black with a reason rather than hold a stale frame.
  bool connected() const override {
    return frames_.load() > 0 && !lost_.load();
  }

  bool poll(unsigned,
            const std::function<void(const VideoFrame&)>& onVideo) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (image_.empty() || lost_.load()) return false;
    VideoFrame f;
    f.width = width_;
    f.height = height_;
    f.strideBytes = width_ * 4;
    f.data = image_.data();
    f.format = PixelFormat::bgra8;
    // A desktop is sRGB and full range. Declaring it narrow is the exact
    // mistake that makes everything downstream washed out.
    f.colour = height_ >= 720 ? ColourSpace::bt709 : ColourSpace::bt601;
    f.range = QuantRange::full;
    f.rate = config_.rate;
    if (onVideo) onVideo(f);
    return true;
  }

 private:
  void run() {
    // Desktop Duplication wakes this thread once per composition; anything
    // that delays it shows up as a late frame.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    std::string lastError;
    while (!stop_) {
      if (!dup_.dup) {
        std::string error;
        const std::wstring name = resolve_(&error);
        if (name.empty()) {
          if (error.empty()) error = "the display is gone";
        } else if (openDuplication(name, config_, dup_, error)) {
          // A new duplication hands over the whole desktop first; keep that
          // frame even if nothing has been presented since.
          firstAfterOpen_ = true;
          lost_ = false;
          if (!lastError.empty()) {
            std::fprintf(stderr, "display capture %s: capturing %s again\n",
                         config_.id.c_str(), win::narrow(name).c_str());
          }
          lastError.clear();
          continue;
        }
        lost_ = true;
        if (error != lastError) {
          std::fprintf(stderr, "display capture %s: %s\n", config_.id.c_str(),
                       error.c_str());
          lastError = error;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        continue;
      }

      DXGI_OUTDUPL_FRAME_INFO info = {};
      ComPtr<IDXGIResource> resource;
      const HRESULT hr = dup_.dup->AcquireNextFrame(100, &info, &resource);
      if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
      if (FAILED(hr)) {
        // ACCESS_LOST is ordinary (mode change, lock, renumbering): reopen.
        if (hr != DXGI_ERROR_ACCESS_LOST) {
          std::fprintf(stderr, "display capture %s: AcquireNextFrame %s\n",
                       config_.id.c_str(), win::hresultText(hr).c_str());
        }
        lost_ = true;
        lastError = "lost";
        dup_ = {};
        continue;
      }

      // Zero means only the pointer changed: no new picture. The first frame
      // is kept regardless, so a still desktop still gives one.
      const bool picture =
          info.LastPresentTime.QuadPart != 0 || firstAfterOpen_;
      if (picture) {
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(resource.As(&texture))) {
          dup_.context->CopySubresourceRegion(dup_.staging.Get(), 0, 0, 0, 0,
                                              texture.Get(), 0, &dup_.box);
          // Read back before the frame goes back to DWM, as Microsoft's
          // samples do: the copy is only queued until Map, so releasing first
          // hands DWM a surface still waiting to be read.
          if (readBack()) firstAfterOpen_ = false;
        }
      }
      dup_.dup->ReleaseFrame();
    }
  }

  /// Maps the staging texture into the back buffer, then swaps it in as the
  /// latest frame. The lock is held only for the swap, so a poll that is
  /// slow with its frame never holds up the next one.
  bool readBack() {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(dup_.context->Map(dup_.staging.Get(), 0, D3D11_MAP_READ, 0,
                                 &mapped))) {
      return false;
    }
    const int w = static_cast<int>(dup_.box.right - dup_.box.left);
    const int h = static_cast<int>(dup_.box.bottom - dup_.box.top);
    back_.resize(static_cast<size_t>(w) * h * 4);
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < h; ++y) {
      const auto* in = reinterpret_cast<const uint32_t*>(
          src + static_cast<size_t>(y) * mapped.RowPitch);
      auto* out =
          reinterpret_cast<uint32_t*>(back_.data() + static_cast<size_t>(y) * w * 4);
      for (int x = 0; x < w; ++x) out[x] = in[x] | 0xFF000000u;
    }
    dup_.context->Unmap(dup_.staging.Get(), 0);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      image_.swap(back_);
      width_ = w;
      height_ = h;
    }
    lost_ = false;
    ++frames_;
    return true;
  }

  NodeConfig config_;
  std::function<std::wstring(std::string*)> resolve_;
  Duplication dup_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> lost_{false};
  std::atomic<uint64_t> frames_{0};
  bool firstAfterOpen_ = true;  // capture thread only
  std::vector<uint8_t> back_;   // capture thread only

  std::mutex mutex_;
  std::vector<uint8_t> image_;
  int width_ = 0;
  int height_ = 0;
};

/// Active displays in index order: primary first, then by GDI number.
std::vector<win::DisplayTarget> activeDisplays(std::string* error = nullptr) {
  std::vector<win::DisplayTarget> out;
  for (auto& t : win::displayTargets(error)) {
    if (t.active && !t.gdiName.empty()) out.push_back(std::move(t));
  }
  return out;
}

const char* kNoWindowCapture =
    "window and application capture are not written for Windows yet (they "
    "need Windows.Graphics.Capture); display capture works, so capture the "
    "display the window is on, with a crop if need be";

}  // namespace

std::unique_ptr<Source> win::makeDuplicationSource(
    const NodeConfig& config, std::function<std::wstring(std::string*)> resolve,
    std::string& error) {
  std::string why;
  if (!win::interactiveDesktop(&why)) {
    error = why;
    return nullptr;
  }
  const std::wstring name = resolve(&error);
  if (name.empty()) {
    if (error.empty()) error = "the display to capture is not part of the desktop";
    return nullptr;
  }
  Duplication first;
  if (!openDuplication(name, config, first, error)) return nullptr;
  return std::make_unique<DuplicationSource>(config, std::move(resolve),
                                             std::move(first));
}

bool ScreenCapture::supported() { return true; }
std::string ScreenCapture::unsupportedReason() { return {}; }

bool ScreenCapture::permitted() { return win::interactiveDesktop(nullptr); }

std::string ScreenCapture::permissionReason() {
  std::string why;
  if (win::interactiveDesktop(&why)) return {};
  return why;
}

std::vector<ScreenCapture::Display> ScreenCapture::listDisplays() {
  std::vector<Display> out;
  for (const auto& t : activeDisplays()) {
    Display d;
    d.id = win::gdiNumber(t.gdiName);
    d.width = t.width;
    d.height = t.height;
    d.name = (t.friendlyName.empty() ? std::string("Display")
                                     : win::narrow(t.friendlyName)) +
             " (" + win::narrow(t.gdiName.substr(4)) + ")";
    out.push_back(std::move(d));
  }
  return out;
}

std::vector<ScreenCapture::Window> ScreenCapture::listWindows() { return {}; }
std::vector<std::string> ScreenCapture::listApplications() { return {}; }

std::unique_ptr<Source> makeDisplaySource(const NodeConfig& config,
                                          std::string& error) {
  std::string why;
  if (!win::interactiveDesktop(&why)) {
    error = why;
    return nullptr;
  }
  const auto displays = activeDisplays(&error);
  if (displays.empty()) {
    if (error.empty()) error = "Windows reports no displays on this desktop";
    return nullptr;
  }
  const int index = config.target.empty() ? 0 : std::atoi(config.target.c_str());
  if (index < 0 || index >= static_cast<int>(displays.size())) {
    error = "no display " + config.target + " — this machine has " +
            std::to_string(displays.size()) +
            "; run `frame-ferret screens` to list them";
    return nullptr;
  }
  const std::wstring name = displays[static_cast<size_t>(index)].gdiName;
  return win::makeDuplicationSource(
      config, [name](std::string*) { return name; }, error);
}

std::unique_ptr<Source> makeDisplaySourceForId(const NodeConfig& config,
                                               uint32_t displayId, int,
                                               bool, std::string& error) {
  const std::wstring name = win::gdiNameFor(displayId);
  return win::makeDuplicationSource(
      config, [name](std::string*) { return name; }, error);
}

std::unique_ptr<Source> makeWindowSource(const NodeConfig&,
                                         std::string& error) {
  error = kNoWindowCapture;
  return nullptr;
}

std::unique_ptr<Source> makeApplicationSource(const NodeConfig&,
                                              std::string& error) {
  error = kNoWindowCapture;
  return nullptr;
}

}  // namespace ferret

#endif

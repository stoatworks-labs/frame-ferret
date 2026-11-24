// vdbench: the bench proof for the Windows virtual display. Windows only.
//
//   vdbench card <display> <seconds> [hz]
//       A counter test card, full screen on one display: 75% bars on top, a
//       grey ramp, and the frame number as 16 black/white blocks along the
//       bottom, advanced <hz> times a second (default 60). <display> is a GDI
//       number (2 for \\.\DISPLAY2) or "vdd" for the first VDD display.
//
//   vdbench probe <ndi-name> <seconds>
//       Receives an NDI source (a substring of its name) and judges it as that
//       card: frames, counter steps, repeats, skips, backwards, orientation and
//       bar error. The receiver is Frame Ferret's own NDI source.
//
//   vdbench direct <seconds> [width height hz]
//       Builds a virtual-display node in this process and judges what it
//       captures, with no transport in between. Run `card vdd` alongside.
//
//   vdbench modes [width height freq]
//       Lists the modes Windows offers on the VDD display (EnumDisplaySettings,
//       whole hertz) and the exact rate DisplayConfig reports; with a mode,
//       sets it the old way first (ChangeDisplaySettingsEx).
//
//   vdbench dup <seconds> [copy]
//       Desktop Duplication on the VDD display with nothing of Frame Ferret's
//       in the way: how often DWM hands over a new picture, how many presents
//       each one folds together, and (with "copy") the counter in each and the
//       time the copy to memory takes. Tells a slow desktop from a slow node.
//
// The card's layout and encoding are omttest's (resolume-omt, tools/omttest),
// so a card from either is read by either. Both card and anything capturing
// must run in the signed-in user's session; tools/winlab/s1run.ps1 does that
// from ssh.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#include "app/main_loop.h"
#include "app/node.h"
#include "core/convert.h"
#include "sources/virtual_display.h"
#include "sources/windows_displays.h"
#include "transports/ndi.h"

using namespace ferret;

namespace {

using clock_ = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

const uint8_t kBars[8][3] = {{191, 191, 191}, {191, 191, 0}, {0, 191, 191},
                             {0, 191, 0},     {191, 0, 191}, {191, 0, 0},
                             {0, 0, 191},     {0, 0, 0}};

// ---- reading a card (BGRA, top row first) -----------------------------------

double patch(const uint8_t* p, int stride, int cx, int cy, int c) {
  double sum = 0;
  for (int y = cy - 4; y <= cy + 4; ++y) {
    for (int x = cx - 4; x <= cx + 4; ++x) {
      sum += p[static_cast<size_t>(y) * stride + x * 4 + c];
    }
  }
  return sum / 81.0;
}

/// The frame number, or -1 if a block is between black and white.
long decodeCounter(const uint8_t* p, int stride, int w, int h) {
  uint32_t v = 0;
  for (int bit = 0; bit < 16; ++bit) {
    const int cx = (2 * bit + 1) * w / 32;
    const double g = patch(p, stride, cx, 7 * h / 8, 1);
    if (g > 170) {
      v |= 1u << bit;
    } else if (g > 80) {
      return -1;
    }
  }
  return static_cast<long>(v);
}

struct Judged {
  double worstBar = 0;
  bool upright = true;
};

Judged checkBars(const uint8_t* p, int stride, int w, int h) {
  Judged j;
  for (int bar = 0; bar < 8; ++bar) {
    const int cx = (2 * bar + 1) * w / 16;
    const int cy = h / 4;
    // BGRA: B=0 G=1 R=2.
    const double got[3] = {patch(p, stride, cx, cy, 2),
                           patch(p, stride, cx, cy, 1),
                           patch(p, stride, cx, cy, 0)};
    for (int c = 0; c < 3; ++c) {
      j.worstBar = std::max(j.worstBar, std::fabs(got[c] - kBars[bar][c]));
    }
  }
  const double top = patch(p, stride, w / 16, h / 4, 1);
  const double rampLeft = patch(p, stride, w / 32, 5 * h / 8, 1);
  const double rampRight = patch(p, stride, w - w / 32, 5 * h / 8, 1);
  j.upright = top > 150 && rampLeft < 60 && rampRight > 200;
  return j;
}

/// A frame as a 32-bit BMP, for when the numbers look wrong.
void saveBmp(const char* path, const VideoFrame& f) {
  FILE* out = std::fopen(path, "wb");
  if (!out) return;
  BITMAPFILEHEADER fh = {};
  BITMAPINFOHEADER ih = {};
  ih.biSize = sizeof(ih);
  ih.biWidth = f.width;
  ih.biHeight = -f.height;
  ih.biPlanes = 1;
  ih.biBitCount = 32;
  ih.biCompression = BI_RGB;
  const DWORD bytes = static_cast<DWORD>(f.width) * f.height * 4;
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof(fh) + sizeof(ih);
  fh.bfSize = fh.bfOffBits + bytes;
  std::fwrite(&fh, sizeof(fh), 1, out);
  std::fwrite(&ih, sizeof(ih), 1, out);
  for (int y = 0; y < f.height; ++y) {
    std::fwrite(f.data + static_cast<size_t>(y) * f.strideBytes, 1,
                static_cast<size_t>(f.width) * 4, out);
  }
  std::fclose(out);
}

/// Tallies a stream of frames by their counters.
struct Tally {
  int frames = 0;       // frames handed over
  int readable = 0;     // with a readable counter
  int distinct = 0;     // whose counter differs from the one before
  int repeats = 0;      // same counter as the one before
  int skips = 0;        // counter jumped by more than one
  long skipped = 0;     // counter values never seen across those jumps
  int backwards = 0;
  int bad = 0;          // frames with a bar off by more than 4 or upside down
  long firstBad = -1;
  long first = -1;
  long last = -1;
  Judged worst;
  int width = 0;
  int height = 0;

  void add(const VideoFrame& f, std::vector<uint8_t>& scratch) {
    ++frames;
    const uint8_t* bgra = f.data;
    int stride = f.strideBytes;
    if (f.format != PixelFormat::bgra8) {
      PixelFormat outFormat;
      QuantRange outRange;
      std::string error;
      if (!convert(f, PixelFormat::bgra8, outFormat, outRange, scratch, stride,
                   error)) {
        return;
      }
      bgra = scratch.data();
    }
    width = f.width;
    height = f.height;
    const long c = decodeCounter(bgra, stride, f.width, f.height);
    if (c < 0) return;
    ++readable;
    if (first < 0) first = c;
    if (last >= 0) {
      if (c == last) {
        ++repeats;
      } else if (c < last) {
        if (backwards < 4) {
          std::printf("  backwards: %ld (0x%04lx) after %ld (0x%04lx)\n", c, c,
                      last, last);
        }
        ++backwards;
        ++distinct;
      } else {
        ++distinct;
        if (c > last + 1) {
          ++skips;
          skipped += c - last - 1;
        }
      }
    } else {
      ++distinct;
    }
    last = c;
    const Judged j = checkBars(bgra, stride, f.width, f.height);
    worst.worstBar = std::max(worst.worstBar, j.worstBar);
    worst.upright = worst.upright && j.upright;
    if (!j.upright || j.worstBar > 4) {
      if (bad++ == 0) {
        firstBad = c;
        if (const char* temp = std::getenv("TEMP")) {
          VideoFrame copy = f;
          copy.data = bgra;
          copy.strideBytes = stride;
          copy.format = PixelFormat::bgra8;
          saveBmp((std::string(temp) + "\\ffvd\\bad.bmp").c_str(), copy);
        }
      }
    }
  }

  int report(const char* what, double seconds) const {
    std::printf("%s: %d frames in %.1f s (%.2f fps), %dx%d\n", what, frames,
                seconds, frames / seconds, width, height);
    if (!readable) {
      std::printf("  no test card in the picture\n");
      return 1;
    }
    std::printf(
        "  test card: %d readable, counter %ld -> %ld (%ld steps), %d distinct "
        "(%.2f a second), %d repeats, %d skips (%ld values missed), %d "
        "backwards, %s, worst bar error %.1f levels\n",
        readable, first, last, last - first, distinct, distinct / seconds,
        repeats, skips, skipped, backwards,
        worst.upright ? "upright" : "UPSIDE DOWN", worst.worstBar);
    if (bad) {
      std::printf("  %d frame(s) with wrong bars or upside down, the first at "
                  "counter %ld (saved as %%TEMP%%\\ffvd\\bad.bmp)\n",
                  bad, firstBad);
    }
    return backwards == 0 && worst.upright && worst.worstBar <= 4 ? 0 : 1;
  }
};

/// Whether a frame is the card: a readable counter and the bars and ramp
/// where they belong. A counter alone is not enough, because an all-black
/// picture (a fresh VDD desktop, say) reads as counter 0.
bool isCard(const VideoFrame& f, std::vector<uint8_t>& scratch) {
  const uint8_t* bgra = f.data;
  int stride = f.strideBytes;
  if (f.format != PixelFormat::bgra8) {
    PixelFormat outFormat;
    QuantRange outRange;
    std::string error;
    if (!convert(f, PixelFormat::bgra8, outFormat, outRange, scratch, stride,
                 error)) {
      return false;
    }
    bgra = scratch.data();
  }
  if (decodeCounter(bgra, stride, f.width, f.height) < 0) return false;
  const Judged j = checkBars(bgra, stride, f.width, f.height);
  return j.upright && j.worstBar <= 4;
}

// ---- the card ---------------------------------------------------------------

bool findDisplay(const std::string& which, win::DisplayTarget* out) {
  for (auto& t : win::displayTargets()) {
    if (!t.active || t.gdiName.empty()) continue;
    if (which == "vdd") {
      std::wstring path = t.devicePath;
      for (auto& c : path) c = static_cast<wchar_t>(towupper(c));
      if (path.find(L"#MTT1337#") == std::wstring::npos) continue;
    } else if (win::gdiNumber(t.gdiName) !=
               static_cast<uint32_t>(std::atoi(which.c_str()))) {
      continue;
    }
    *out = t;
    return true;
  }
  return false;
}

LRESULT CALLBACK cardProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_SETCURSOR) {
    SetCursor(nullptr);
    return TRUE;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

int card(const std::string& which, double seconds, double hz) {
  win::DisplayTarget d;
  if (!findDisplay(which, &d)) {
    std::printf("no active display \"%s\"\n", which.c_str());
    return 1;
  }
  const int w = d.width, h = d.height;
  std::printf("card on %s (%s), %dx%d at (%d,%d), %.4g Hz display, %.4g Hz card\n",
              win::narrow(d.gdiName).c_str(), win::narrow(d.friendlyName).c_str(),
              w, h, d.x, d.y, d.refresh.approx(), hz);

  WNDCLASSW wc = {};
  wc.lpfnWndProc = cardProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"FerretVdbenchCard";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName,
                              L"vdbench card", WS_POPUP | WS_VISIBLE, d.x, d.y,
                              w, h, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    std::printf("CreateWindowEx failed: %lu\n", GetLastError());
    return 1;
  }

  // The still part, once: bars on the top half, a ramp below.
  std::vector<uint32_t> still(static_cast<size_t>(w) * (3 * h / 4));
  for (int y = 0; y < 3 * h / 4; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t r, g, b;
      if (y < h / 2) {
        const int bar = std::min(7, x * 8 / w);
        r = kBars[bar][0];
        g = kBars[bar][1];
        b = kBars[bar][2];
      } else {
        r = g = b = static_cast<uint8_t>(x * 255 / (w - 1));
      }
      still[static_cast<size_t>(y) * w + x] = (uint32_t{r} << 16) |
                                              (uint32_t{g} << 8) | b;
    }
  }
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -(3 * h / 4);  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  // The counter strip, drawn into memory and put up in one call: drawn as 16
  // separate cells, DWM composes some frames halfway through and the counter
  // reads as a mix of two frames (on winlab, 3 "backwards" in 15 s).
  const int stripTop = 3 * h / 4;
  const int stripH = h - stripTop;
  std::vector<uint32_t> strip(static_cast<size_t>(w) * stripH);
  BITMAPINFO sbi = bi;
  sbi.bmiHeader.biHeight = -stripH;
  HDC dc = GetDC(hwnd);

  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  const auto start = clock_::now();
  const auto period = std::chrono::duration<double>(1.0 / hz);
  uint32_t frame = 0;
  bool stillDrawn = false;
  int late = 0;
  while (std::chrono::duration<double>(clock_::now() - start).count() < seconds) {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
    // Redrawn every second too, in case anything covered it.
    if (!stillDrawn || frame % static_cast<uint32_t>(hz) == 0) {
      SetDIBitsToDevice(dc, 0, 0, w, 3 * h / 4, 0, 0, 0, 3 * h / 4,
                        still.data(), &bi, DIB_RGB_COLORS);
      stillDrawn = true;
    }
    uint32_t row[16];
    for (int bit = 0; bit < 16; ++bit) {
      row[bit] = (frame >> bit) & 1u ? 0x00EBEBEBu : 0x00101010u;
    }
    for (int x = 0; x < w; ++x) strip[x] = row[x * 16 / w];
    for (int y = 1; y < stripH; ++y) {
      std::memcpy(&strip[static_cast<size_t>(y) * w], strip.data(), w * 4);
    }
    SetDIBitsToDevice(dc, 0, stripTop, w, stripH, 0, 0, 0, stripH, strip.data(),
                      &sbi, DIB_RGB_COLORS);
    GdiFlush();
    ++frame;

    const auto due = start + std::chrono::duration_cast<clock_::duration>(period * frame);
    const auto wait = due - clock_::now();
    if (wait.count() <= 0) {
      ++late;
      continue;
    }
    LARGE_INTEGER rel;
    rel.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count() / 100;
    SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE);
    MsgWaitForMultipleObjects(1, &timer, FALSE, INFINITE, QS_ALLINPUT);
    while (clock_::now() < due) {
      MSG m2;
      if (PeekMessageW(&m2, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m2);
        DispatchMessageW(&m2);
      }
      WaitForSingleObject(timer, 1);
    }
  }
  std::printf("card: drew %u frames in %.1f s, %d late\n", frame, seconds, late);
  ReleaseDC(hwnd, dc);
  DestroyWindow(hwnd);
  return 0;
}

// ---- modes ------------------------------------------------------------------

int modes(int width, int height, int freq) {
  win::DisplayTarget d;
  if (!findDisplay("vdd", &d)) {
    std::printf("no active VDD display\n");
    return 1;
  }
  if (width > 0) {
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = static_cast<DWORD>(width);
    dm.dmPelsHeight = static_cast<DWORD>(height);
    dm.dmDisplayFrequency = static_cast<DWORD>(freq);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    const LONG r = ChangeDisplaySettingsExW(d.gdiName.c_str(), &dm, nullptr,
                                            CDS_UPDATEREGISTRY, nullptr);
    std::printf("ChangeDisplaySettingsEx %dx%d @ %d: %ld\n", width, height, freq, r);
    Sleep(1500);
    findDisplay("vdd", &d);
  }
  std::printf("%s now %dx%d @ %lld/%lld (%.6f Hz)\n", win::narrow(d.gdiName).c_str(),
              d.width, d.height, static_cast<long long>(d.refresh.num),
              static_cast<long long>(d.refresh.den), d.refresh.approx());
  std::vector<std::string> seen;
  DEVMODEW dm = {};
  dm.dmSize = sizeof(dm);
  for (DWORD i = 0; EnumDisplaySettingsExW(d.gdiName.c_str(), i, &dm, 0); ++i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lux%lu@%lu", dm.dmPelsWidth, dm.dmPelsHeight,
                  dm.dmDisplayFrequency);
    if (std::find(seen.begin(), seen.end(), buf) == seen.end()) seen.push_back(buf);
  }
  std::printf("%zu modes:", seen.size());
  for (const auto& m : seen) std::printf(" %s", m.c_str());
  std::printf("\n");
  return 0;
}


// ---- judging ----------------------------------------------------------------

int probe(const std::string& name, double seconds) {
  NodeConfig c;
  c.id = "probe";
  c.kind = NodeKind::ndi;
  c.target = name;
  std::string error;
  auto source = makeNdiSource(c, error);
  if (!source) {
    std::printf("no NDI receiver: %s\n", error.c_str());
    return 1;
  }
  // The clock starts at the first frame of the card, not at connection: the
  // stream carries whatever was on the display before the card went up.
  std::vector<uint8_t> scratch;
  bool readable = false;
  const auto waitUntil = clock_::now() + std::chrono::seconds(30);
  while (!readable && clock_::now() < waitUntil) {
    source->poll(100, [&](const VideoFrame& f) { readable = isCard(f, scratch); });
  }
  if (!readable) {
    std::printf("probe \"%s\": no readable test card within 30 s (%s)\n",
                name.c_str(),
                source->connected() ? "connected" : "never connected");
    return 1;
  }
  Tally t;
  const auto start = clock_::now();
  while (std::chrono::duration<double>(clock_::now() - start).count() < seconds) {
    source->poll(100, [&](const VideoFrame& f) { t.add(f, scratch); });
  }
  return t.report(("probe \"" + name + "\"").c_str(), seconds);
}

/// 60 -> 60/1; 59.94 -> 60000/1001.
Rate rateFor(double hz) {
  if (std::fabs(hz - std::round(hz)) < 1e-3) return Rate{std::llround(hz), 1};
  return Rate{std::llround(hz * 1.001) * 1000, 1001};
}

int direct(double seconds, int width, int height, double hz) {
  NodeConfig c;
  c.id = "vd";
  c.kind = NodeKind::virtualDisplay;
  c.width = width;
  c.height = height;
  c.rate = rateFor(hz);
  VirtualDisplayPlan plan;
  std::string error;
  if (!planVirtualDisplay(c, &plan, error)) {
    std::printf("plan: %s\n", error.c_str());
    return 1;
  }
  if (!plan.warning.empty()) std::printf("plan: %s\n", plan.warning.c_str());
  const auto made = clock_::now();
  auto source = makeVirtualDisplaySource(c, error);
  if (!source) {
    std::printf("virtual display: %s\n", error.c_str());
    return 1;
  }
  std::printf("direct: node built in %.2f s; waiting for the card\n",
              std::chrono::duration<double>(clock_::now() - made).count());
  // The clock starts at the first frame of the card.
  std::vector<uint8_t> scratch;
  bool readable = false;
  const auto waitUntil = clock_::now() + std::chrono::seconds(30);
  while (!readable && clock_::now() < waitUntil) {
    source->poll(0, [&](const VideoFrame& f) { readable = isCard(f, scratch); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!readable) {
    std::printf("direct: no readable test card within 30 s (%s)\n",
                source->connected() ? "capturing" : "no frame captured");
    return 1;
  }
  // Latest-frame source: poll faster than the display and count changes.
  Tally t;
  long last = -2;
  int polls = 0;
  const auto start = clock_::now();
  while (std::chrono::duration<double>(clock_::now() - start).count() < seconds) {
    source->poll(0, [&](const VideoFrame& f) {
      ++polls;
      const long counter = decodeCounter(f.data, f.strideBytes, f.width, f.height);
      if (counter != last) {
        t.add(f, scratch);
        last = counter;
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  std::printf("direct: %d polls\n", polls);
  if (const char* temp = std::getenv("TEMP")) {
    const std::string path = std::string(temp) + "\\ffvd\\direct.bmp";
    source->poll(0, [&](const VideoFrame& f) {
      if (f.format == PixelFormat::bgra8) saveBmp(path.c_str(), f);
    });
  }
  return t.report("direct", seconds);
}

// ---- raw duplication --------------------------------------------------------

int dup(double seconds, bool copy) {
  win::DisplayTarget d;
  if (!findDisplay("vdd", &d)) {
    std::printf("no active VDD display\n");
    return 1;
  }
  ComPtr<IDXGIFactory1> factory;
  CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIOutput1> output;
  for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
    ComPtr<IDXGIOutput> o;
    for (UINT i = 0; adapter->EnumOutputs(i, &o) == S_OK; ++i) {
      DXGI_OUTPUT_DESC desc;
      o->GetDesc(&desc);
      if (d.gdiName == desc.DeviceName) {
        o.As(&output);
        break;
      }
    }
    if (!output) adapter.Reset();
  }
  if (!output) {
    std::printf("no DXGI output for %s\n", win::narrow(d.gdiName).c_str());
    return 1;
  }
  DXGI_ADAPTER_DESC1 ad;
  adapter->GetDesc1(&ad);
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, &context);
  if (FAILED(hr)) {
    std::printf("D3D11CreateDevice %s\n", win::hresultText(hr).c_str());
    return 1;
  }
  ComPtr<IDXGIOutputDuplication> dupl;
  hr = output->DuplicateOutput(device.Get(), &dupl);
  if (FAILED(hr)) {
    std::printf("DuplicateOutput %s\n", win::hresultText(hr).c_str());
    return 1;
  }
  DXGI_OUTDUPL_DESC od;
  dupl->GetDesc(&od);
  std::printf("dup on %s via %s: %ux%u, %.4g Hz, %s\n",
              win::narrow(d.gdiName).c_str(), win::narrow(ad.Description).c_str(),
              od.ModeDesc.Width, od.ModeDesc.Height,
              od.ModeDesc.RefreshRate.Denominator
                  ? double(od.ModeDesc.RefreshRate.Numerator) /
                        od.ModeDesc.RefreshRate.Denominator
                  : 0.0,
              od.DesktopImageInSystemMemory ? "image in system memory"
                                            : "image in video memory");
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = od.ModeDesc.Width;
  td.Height = od.ModeDesc.Height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  device->CreateTexture2D(&td, nullptr, &staging);

  long acquired = 0, timeouts = 0, pictures = 0, accumulated = 0, readable = 0;
  long distinct = 0, missed = 0, lastCounter = -1;
  double copyMs = 0, worstCopyMs = 0, worstGapMs = 0;
  LARGE_INTEGER qpf;
  QueryPerformanceFrequency(&qpf);
  LONGLONG lastPresent = 0;
  const auto start = clock_::now();
  while (std::chrono::duration<double>(clock_::now() - start).count() < seconds) {
    DXGI_OUTDUPL_FRAME_INFO info = {};
    ComPtr<IDXGIResource> resource;
    hr = dupl->AcquireNextFrame(100, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
      ++timeouts;
      continue;
    }
    if (FAILED(hr)) {
      std::printf("AcquireNextFrame %s\n", win::hresultText(hr).c_str());
      return 1;
    }
    ++acquired;
    if (info.LastPresentTime.QuadPart != 0) {
      ++pictures;
      accumulated += info.AccumulatedFrames;
      if (lastPresent) {
        worstGapMs = std::max(
            worstGapMs,
            1000.0 * double(info.LastPresentTime.QuadPart - lastPresent) / qpf.QuadPart);
      }
      lastPresent = info.LastPresentTime.QuadPart;
      if (copy) {
        const auto t0 = clock_::now();
        ComPtr<ID3D11Texture2D> texture;
        resource.As(&texture);
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
          const double ms =
              std::chrono::duration<double, std::milli>(clock_::now() - t0).count();
          copyMs += ms;
          worstCopyMs = std::max(worstCopyMs, ms);
          const long c = decodeCounter(static_cast<const uint8_t*>(m.pData),
                                       static_cast<int>(m.RowPitch),
                                       static_cast<int>(td.Width),
                                       static_cast<int>(td.Height));
          context->Unmap(staging.Get(), 0);
          if (c >= 0) {
            ++readable;
            if (c != lastCounter) {
              ++distinct;
              if (lastCounter >= 0 && c > lastCounter + 1) missed += c - lastCounter - 1;
            }
            lastCounter = c;
          }
        }
      }
    }
    dupl->ReleaseFrame();
  }
  std::printf("dup: %ld acquired in %.1f s, %ld timeouts, %ld new pictures "
              "(%.2f a second), %ld presents folded in (%.2f a second), worst "
              "gap between presents %.1f ms\n",
              acquired, seconds, timeouts, pictures, pictures / seconds,
              accumulated, accumulated / seconds, worstGapMs);
  if (copy) {
    std::printf("  copy: %.2f ms average, %.2f ms worst; card: %ld readable, "
                "%ld distinct (%.2f a second), %ld values missed\n",
                pictures ? copyMs / pictures : 0.0, worstCopyMs, readable,
                distinct, distinct / seconds, missed);
  }
  return 0;
}

int usage() {
  std::printf(
      "vdbench card <display|vdd> <seconds> [hz]\n"
      "vdbench probe <ndi-name> <seconds>\n"
      "vdbench direct <seconds> [width height hz]\n"
      "vdbench modes [width height freq]\n"
      "vdbench dup <seconds> [copy]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  initialiseWindowServer();
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) return usage();
  const std::string cmd = argv[1];
  if (cmd == "card" && argc >= 4) {
    return card(argv[2], std::atof(argv[3]), argc >= 5 ? std::atof(argv[4]) : 60.0);
  }
  if (cmd == "probe" && argc >= 4) return probe(argv[2], std::atof(argv[3]));
  if (cmd == "direct" && argc >= 3) {
    return direct(std::atof(argv[2]), argc >= 4 ? std::atoi(argv[3]) : 1920,
                  argc >= 5 ? std::atoi(argv[4]) : 1080,
                  argc >= 6 ? std::atof(argv[5]) : 60.0);
  }
  if (cmd == "modes") {
    return modes(argc >= 5 ? std::atoi(argv[2]) : 0,
                 argc >= 5 ? std::atoi(argv[3]) : 0,
                 argc >= 5 ? std::atoi(argv[4]) : 0);
  }
  if (cmd == "dup" && argc >= 3) {
    return dup(std::atof(argv[2]), argc >= 4 && std::string(argv[3]) == "copy");
  }
  return usage();
}

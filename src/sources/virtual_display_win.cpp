// The virtual display on Windows: one of VDD's displays, claimed, given the
// planned mode, and captured with Desktop Duplication.
//
// ## Why someone else's driver
//
// A display on Windows means an Indirect Display Driver, and Windows loads a
// driver only once Microsoft has attestation-signed it, which needs an EV
// certificate and a Partner Center account this project does not have. An
// unsigned driver would run only with test signing switched on, which no one
// should be asked to do to a show machine. VirtualDrivers' Virtual Display
// Driver (VDD) is MIT-licensed and signed through the SignPath Foundation, so
// the operator installs it once and this drives it. See
// docs/03-os-extensions.md.
//
// ## What VDD gives, measured on winlab
//
// Windows 11 26200; VDD release 25.7.23, whose driver is dated 2024-12-24.
//
// - Its displays carry EDID maker "MTT" and product 0x1337, so the monitor's
//   device path contains "#MTT1337#". That is how they are told apart from
//   real monitors, whatever Windows has numbered them.
// - Modes come from C:\VirtualDisplayDriver\vdd_settings.xml (the folder is
//   in HKLM\SOFTWARE\MikeTheTech\VirtualDisplayDriver\VDDPATH): every
//   <resolution> at its own <refresh_rate>, plus every <g_refresh_rate>. The
//   folder is writable by any signed-in user.
// - RELOAD_DRIVER, written as UTF-16 to the pipe \\.\pipe\MTTVirtualDisplayPipe
//   (open to everyone), re-reads the file in about 1.2 s. It also RENUMBERS
//   the display (\\.\DISPLAY2 came back as \\.\DISPLAY3), so a display is
//   followed by its connector ("UID256"), the one part of its identity that
//   survives a reload.
// - Whole rates only. With <refresh_rate>59.9401</refresh_rate> in the file,
//   Windows lists no 59 Hz mode and ChangeDisplaySettingsEx at 59 answers
//   BADMODE; VDD's CreateTargetMode computes the pixel rate in 32-bit UINTs,
//   which overflows at any four-place rate. The plan folds fractional rates
//   to the whole rate above (virtual_display_plan.cpp).
// - A VDD reload while a display is captured usually costs the duplication
//   (ACCESS_LOST) and nothing more; the capture reopens on the new number.
//   Once in three it left VDD's device in an error state until the device was
//   restarted, so the capture's "gone" message says so when the pipe is gone.

#include "sources/virtual_display.h"

#if defined(_WIN32)

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "sources/windows_displays.h"
#include "sources/windows_duplication.h"

namespace ferret {
namespace {

constexpr wchar_t kPipe[] = L"\\\\.\\pipe\\MTTVirtualDisplayPipe";
constexpr const char* kGetVdd =
    "install it from "
    "https://github.com/VirtualDrivers/Virtual-Display-Driver/releases (or "
    "`winget install --id=VirtualDrivers.Virtual-Display-Driver -e`), then "
    "start Frame Ferret again";

std::wstring upper(std::wstring s) {
  for (auto& c : s) c = static_cast<wchar_t>(std::towupper(c));
  return s;
}

bool isVdd(const win::DisplayTarget& t) {
  return upper(t.devicePath).find(L"#MTT1337#") != std::wstring::npos;
}

/// "UID256" from "\\?\DISPLAY#MTT1337#1&28a6823a&6&UID256#{...}".
std::wstring connectorOf(const std::wstring& devicePath) {
  const std::wstring path = upper(devicePath);
  const size_t uid = path.find(L"&UID");
  if (uid == std::wstring::npos) return {};
  const size_t end = path.find(L'#', uid);
  return path.substr(uid + 1, end == std::wstring::npos ? std::wstring::npos
                                                        : end - uid - 1);
}

std::vector<win::DisplayTarget> vddTargets() {
  std::vector<win::DisplayTarget> out;
  for (auto& t : win::displayTargets()) {
    if (isVdd(t)) out.push_back(std::move(t));
  }
  return out;
}

bool findConnector(const std::wstring& connector, win::DisplayTarget* out) {
  for (auto& t : vddTargets()) {
    if (connectorOf(t.devicePath) == connector) {
      *out = std::move(t);
      return true;
    }
  }
  return false;
}

bool vddRunning() {
  if (WaitNamedPipeW(kPipe, 1)) return true;
  // Busy is still there; only "no such pipe" means VDD is not running.
  return GetLastError() != ERROR_FILE_NOT_FOUND;
}

/// Which VDD displays this process's nodes hold, by connector.
std::mutex g_claimMutex;
std::set<std::wstring> g_claimed;

std::string describe(const win::DisplayTarget& t) {
  if (!t.active) return "not part of the desktop";
  char buf[128];
  int64_t num = t.refresh.num, den = t.refresh.den;
  const int64_t g = std::gcd(num, den);
  if (g > 1) num /= g, den /= g;
  if (den == 1) {
    std::snprintf(buf, sizeof(buf), "%dx%d @ %lld Hz", t.width, t.height,
                  static_cast<long long>(num));
  } else {
    // The exact rational, as Windows reports it, rather than a rounded one.
    std::snprintf(buf, sizeof(buf), "%dx%d @ %.4f Hz (%lld/%lld)", t.width,
                  t.height, t.refresh.approx(), static_cast<long long>(num),
                  static_cast<long long>(den));
  }
  return buf;
}

std::string describe(const VirtualDisplayPlan& plan) {
  return std::to_string(plan.width) + "x" + std::to_string(plan.height) +
         " @ " + plan.vddRefresh + " Hz";
}

bool matches(const win::DisplayTarget& t, const VirtualDisplayPlan& plan) {
  return t.active && t.width == plan.width && t.height == plan.height &&
         t.refresh.valid() && t.refresh == plan.refresh;
}

// ---- VDD's settings file ----------------------------------------------------

std::wstring settingsPath() {
  wchar_t dir[MAX_PATH] = {};
  DWORD size = sizeof(dir);
  if (RegGetValueW(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver", L"VDDPATH",
                   RRF_RT_REG_SZ, nullptr, dir, &size) == ERROR_SUCCESS &&
      dir[0]) {
    return std::wstring(dir) + L"\\vdd_settings.xml";
  }
  return L"C:\\VirtualDisplayDriver\\vdd_settings.xml";
}

/// The text of <tag>...</tag> inside `block`, trimmed; empty if absent.
std::string tagValue(const std::string& block, const std::string& tag) {
  const size_t open = block.find("<" + tag + ">");
  if (open == std::string::npos) return {};
  const size_t start = open + tag.size() + 2;
  const size_t close = block.find("</" + tag + ">", start);
  if (close == std::string::npos) return {};
  std::string v = block.substr(start, close - start);
  const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
  v.erase(v.begin(), std::find_if(v.begin(), v.end(), notSpace));
  v.erase(std::find_if(v.rbegin(), v.rend(), notSpace).base(), v.end());
  return v;
}

/// Whether the file already gives VDD this mode: a <resolution> at this
/// raster and rate, or at this raster with the rate among the global ones.
bool settingsHave(const std::string& xml, const VirtualDisplayPlan& plan) {
  const double want = std::atof(plan.vddRefresh.c_str());
  bool rasterListed = false;
  for (size_t at = xml.find("<resolution>"); at != std::string::npos;
       at = xml.find("<resolution>", at + 1)) {
    const size_t end = xml.find("</resolution>", at);
    if (end == std::string::npos) break;
    const std::string block = xml.substr(at, end - at);
    if (std::atoi(tagValue(block, "width").c_str()) != plan.width ||
        std::atoi(tagValue(block, "height").c_str()) != plan.height) {
      continue;
    }
    rasterListed = true;
    const std::string rate = tagValue(block, "refresh_rate");
    if (!rate.empty() && std::fabs(std::atof(rate.c_str()) - want) < 5e-5) {
      return true;
    }
  }
  // Global rates are whole numbers (VDD reads them with stoi).
  if (rasterListed && plan.refresh.den == 1) {
    for (size_t at = xml.find("<g_refresh_rate>"); at != std::string::npos;
         at = xml.find("<g_refresh_rate>", at + 1)) {
      const size_t end = xml.find("</g_refresh_rate>", at);
      if (end == std::string::npos) break;
      if (std::atoi(xml.substr(at + 16, end - at - 16).c_str()) ==
          plan.refreshHz) {
        return true;
      }
    }
  }
  return false;
}

/// Adds the planned mode to VDD's settings file unless it is there. `added`
/// says whether the file changed (and so VDD must reload to see it).
bool ensureInSettings(const std::wstring& path, const VirtualDisplayPlan& plan,
                      bool* added, std::string& error) {
  *added = false;
  const std::string where = win::narrow(path);
  std::string xml;
  {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      error = "could not read VDD's settings file " + where +
              ", where VDD keeps its modes";
      return false;
    }
    xml.assign(std::istreambuf_iterator<char>(in), {});
  }
  if (settingsHave(xml, plan)) return true;

  const size_t close = xml.find("</resolutions>");
  if (close == std::string::npos) {
    error = "VDD's settings file " + where +
            " has no <resolutions> section to add " + describe(plan) + " to";
    return false;
  }
  const std::string nl = xml.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  const std::string entry =
      "    <!-- added by Frame Ferret -->" + nl + "        <resolution>" + nl +
      "            <width>" + std::to_string(plan.width) + "</width>" + nl +
      "            <height>" + std::to_string(plan.height) + "</height>" + nl +
      "            <refresh_rate>" + plan.vddRefresh + "</refresh_rate>" + nl +
      "        </resolution>" + nl + "    ";
  xml.insert(close, entry);

  // Write beside it and swap, so VDD never reads half a file.
  const std::wstring temp = path + L".ferret";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(xml.data(), static_cast<std::streamsize>(xml.size()));
    if (!out) {
      error = "could not write beside VDD's settings file " + where;
      return false;
    }
  }
  if (!MoveFileExW(temp.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error = "could not replace VDD's settings file " + where + " (error " +
            std::to_string(GetLastError()) + "); add a <resolution> of " +
            describe(plan) + " to it by hand";
    DeleteFileW(temp.c_str());
    return false;
  }
  *added = true;
  return true;
}

// ---- VDD's control pipe -----------------------------------------------------

/// Sends one command and waits for VDD to finish with it: VDD runs the
/// command, may log back down the pipe meanwhile, then disconnects.
bool vddCommand(const wchar_t* command, std::string& error) {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 20; ++attempt) {
    pipe = CreateFileW(kPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                       OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) {
      break;
    }
    WaitNamedPipeW(kPipe, 500);
  }
  if (pipe == INVALID_HANDLE_VALUE) {
    error = "VDD's control pipe did not answer (error " +
            std::to_string(GetLastError()) + ")";
    return false;
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

  const DWORD bytes =
      static_cast<DWORD>(std::wcslen(command) * sizeof(wchar_t));
  DWORD written = 0;
  if (!WriteFile(pipe, command, bytes, &written, nullptr) || written != bytes) {
    error = "could not send " + win::narrow(command) + " to VDD (error " +
            std::to_string(GetLastError()) + ")";
    CloseHandle(pipe);
    return false;
  }

  char buf[1024];
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (std::chrono::steady_clock::now() < until) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
    if (available == 0) {
      Sleep(20);
      continue;
    }
    DWORD got = 0;
    ReadFile(pipe, buf, std::min<DWORD>(available, sizeof(buf)), &got, nullptr);
  }
  CloseHandle(pipe);
  return true;
}

/// Reloads VDD and waits for this display to come back. Every VDD display
/// goes away for about a second and returns renumbered; other nodes'
/// captures follow theirs by connector.
bool reloadVdd(const std::wstring& connector, std::string& error) {
  if (!vddCommand(L"RELOAD_DRIVER", error)) return false;
  win::DisplayTarget t;
  for (int i = 0; i < 100; ++i) {
    if (findConnector(connector, &t) && t.active && !t.gdiName.empty()) {
      return true;
    }
    Sleep(100);
  }
  error = "VDD reloaded but its display " + win::narrow(connector) +
          " did not come back within ten seconds";
  return false;
}

// ---- the mode ---------------------------------------------------------------

/// Sets the display's raster and refresh rate with the display configuration
/// API: the source mode's raster and the path's refresh rational, the target
/// mode left for Windows to match. (ChangeDisplaySettingsEx would do for the
/// whole rates VDD offers, but it addresses the display by its GDI name,
/// which a reload changes under it.)
bool applyMode(const std::wstring& connector, const VirtualDisplayPlan& plan,
               std::string& error) {
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  LONG result = ERROR_SUCCESS;
  for (int attempt = 0; attempt < 4; ++attempt) {
    UINT32 pathCount = 0, modeCount = 0;
    result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount,
                                         &modeCount);
    if (result != ERROR_SUCCESS) break;
    paths.resize(pathCount);
    modes.resize(modeCount);
    result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                                &modeCount, modes.data(), nullptr);
    if (result == ERROR_SUCCESS) {
      paths.resize(pathCount);
      modes.resize(modeCount);
      break;
    }
    if (result != ERROR_INSUFFICIENT_BUFFER) break;
  }
  if (result != ERROR_SUCCESS) {
    error = "QueryDisplayConfig failed with error " + std::to_string(result);
    return false;
  }

  DISPLAYCONFIG_PATH_INFO* ours = nullptr;
  for (auto& path : paths) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME name = {};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = path.targetInfo.adapterId;
    name.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) continue;
    win::DisplayTarget t;
    t.devicePath = name.monitorDevicePath;
    if (isVdd(t) && connectorOf(t.devicePath) == connector) {
      ours = &path;
      break;
    }
  }
  if (!ours) {
    error = "VDD's display " + win::narrow(connector) +
            " is not part of the desktop";
    return false;
  }
  const UINT32 index = ours->sourceInfo.modeInfoIdx;
  if (index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || index >= modes.size()) {
    error = "Windows gave VDD's display no source mode to change";
    return false;
  }
  modes[index].sourceMode.width = static_cast<UINT32>(plan.width);
  modes[index].sourceMode.height = static_cast<UINT32>(plan.height);
  ours->targetInfo.refreshRate.Numerator = static_cast<UINT32>(plan.refresh.num);
  ours->targetInfo.refreshRate.Denominator =
      static_cast<UINT32>(plan.refresh.den);
  ours->targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;

  result = SetDisplayConfig(static_cast<UINT32>(paths.size()), paths.data(),
                            static_cast<UINT32>(modes.size()), modes.data(),
                            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                                SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
  if (result != ERROR_SUCCESS) {
    error = "Windows would not set VDD's display to " + describe(plan) +
            " (SetDisplayConfig error " + std::to_string(result) + ")";
    return false;
  }
  return true;
}

bool waitForMode(const std::wstring& connector, const VirtualDisplayPlan& plan,
                 win::DisplayTarget* now) {
  for (int i = 0; i < 30; ++i) {
    if (findConnector(connector, now) && matches(*now, plan)) return true;
    Sleep(100);
  }
  return false;
}

/// Makes the claimed display run the plan: adds the mode to VDD's settings
/// if it is missing, reloads VDD if that changed anything (or if the file had
/// the mode but VDD had not read it), then sets it.
bool setUp(const std::wstring& connector, const VirtualDisplayPlan& plan,
           const std::string& nodeId, std::string& error) {
  win::DisplayTarget now;
  if (findConnector(connector, &now) && matches(now, plan)) return true;

  const std::wstring path = settingsPath();
  bool added = false;
  if (!ensureInSettings(path, plan, &added, error)) return false;
  bool reloaded = false;
  if (added) {
    std::fprintf(stderr,
                 "virtual display %s: added %s to VDD's settings (%s) and is "
                 "reloading VDD, which takes every VDD display away for about "
                 "a second\n",
                 nodeId.c_str(), describe(plan).c_str(),
                 win::narrow(path).c_str());
    if (!reloadVdd(connector, error)) return false;
    reloaded = true;
  }

  std::string applyError;
  for (int attempt = 0; attempt < 2; ++attempt) {
    applyError.clear();
    if (applyMode(connector, plan, applyError) &&
        waitForMode(connector, plan, &now)) {
      return true;
    }
    if (reloaded) break;
    std::fprintf(stderr,
                 "virtual display %s: VDD's settings list %s but VDD has not "
                 "read them; reloading it\n",
                 nodeId.c_str(), describe(plan).c_str());
    if (!reloadVdd(connector, error)) return false;
    reloaded = true;
  }
  findConnector(connector, &now);
  error = "VDD's display would not run " + describe(plan) + " (" +
          (applyError.empty() ? "it runs " + describe(now) : applyError) +
          "); check the <resolution> entries in " + win::narrow(path);
  return false;
}

class VirtualDisplaySource final : public Source {
 public:
  VirtualDisplaySource(NodeConfig config, std::wstring connector,
                       std::unique_ptr<Source> capture)
      : config_(std::move(config)),
        connector_(std::move(connector)),
        capture_(std::move(capture)) {}

  ~VirtualDisplaySource() override {
    capture_.reset();
    std::lock_guard<std::mutex> lock(g_claimMutex);
    g_claimed.erase(connector_);
  }

  const std::string& id() const override { return config_.id; }
  bool connected() const override { return capture_->connected(); }
  bool poll(unsigned timeoutMs,
            const std::function<void(const VideoFrame&)>& onVideo) override {
    return capture_->poll(timeoutMs, onVideo);
  }

 private:
  NodeConfig config_;
  std::wstring connector_;
  std::unique_ptr<Source> capture_;
};

}  // namespace

std::unique_ptr<Source> makeVirtualDisplaySource(const NodeConfig& config,
                                                 std::string& error) {
  VirtualDisplayPlan plan;
  if (!planVirtualDisplay(config, &plan, error)) return nullptr;

  std::string why;
  if (!win::interactiveDesktop(&why)) {
    error = why;
    return nullptr;
  }
  if (!vddRunning()) {
    error =
        std::string(
            "the Windows virtual display is one of VirtualDrivers' Virtual "
            "Display Driver (VDD) displays, and VDD is not installed or not "
            "running: ") +
        kGetVdd;
    return nullptr;
  }

  // Claim a VDD display no other node holds.
  std::wstring connector;
  {
    std::lock_guard<std::mutex> lock(g_claimMutex);
    const auto targets = vddTargets();
    size_t active = 0;
    for (const auto& t : targets) {
      if (!t.active) continue;
      ++active;
      const std::wstring c = connectorOf(t.devicePath);
      if (!c.empty() && !g_claimed.count(c)) {
        connector = c;
        break;
      }
    }
    if (connector.empty()) {
      const std::string where = win::narrow(settingsPath());
      if (targets.empty()) {
        error = "VDD is running but has made no display; check <count> under "
                "<monitors> in " + where;
      } else if (active == 0) {
        error = "VDD's display is connected but not part of the desktop: in "
                "Settings > System > Display, choose \"Extend these displays\"";
      } else {
        error = "VDD has " + std::to_string(active) +
                " display(s) on the desktop and every one is in use by "
                "another virtual-display node; raise <count> under "
                "<monitors> in " + where + " and start Frame Ferret again";
      }
      return nullptr;
    }
    g_claimed.insert(connector);
  }
  const auto release = [&connector] {
    std::lock_guard<std::mutex> lock(g_claimMutex);
    g_claimed.erase(connector);
  };

  if (!setUp(connector, plan, config.id, error)) {
    release();
    return nullptr;
  }

  // Followed by connector, because a VDD reload renumbers the display.
  auto resolve = [connector](std::string* why) -> std::wstring {
    win::DisplayTarget t;
    if (findConnector(connector, &t) && t.active) return t.gdiName;
    if (why && !vddRunning()) {
      // Seen once in three VDD reloads made while a display was being
      // captured: VDD's device went into an error state and stayed there.
      *why = "VDD has stopped, and its device may be in an error state; "
             "restart it in Device Manager (Display adapters > Virtual "
             "Display Driver: Disable device, then Enable device) and "
             "capture picks the display up again";
    } else if (why) {
      *why = "VDD's display " + win::narrow(connector) +
             " is not part of the desktop (VDD reloading, or the display "
             "disconnected in Settings > System > Display); waiting for it";
    }
    return {};
  };
  std::string captureError;
  auto capture = win::makeDuplicationSource(config, resolve, captureError);
  if (!capture) {
    release();
    error = "VDD's display is set up but could not be captured: " +
            captureError;
    return nullptr;
  }

  win::DisplayTarget now;
  findConnector(connector, &now);
  std::fprintf(stderr,
               "virtual display %s: VDD display %s (%s), %s\n",
               config.id.c_str(), win::narrow(now.gdiName.substr(4)).c_str(),
               win::narrow(connector).c_str(), describe(now).c_str());
  return std::make_unique<VirtualDisplaySource>(config, connector,
                                                std::move(capture));
}

}  // namespace ferret

#endif

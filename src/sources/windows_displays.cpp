// What Windows says about its displays. See windows_displays.h.
#include "sources/windows_displays.h"

#if defined(_WIN32)

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <map>

namespace ferret::win {

std::string narrow(const std::wstring& s) {
  if (s.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0,
                                    nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      out.data(), n);
  return out;
}

uint32_t gdiNumber(const std::wstring& name) {
  const std::wstring prefix = L"\\\\.\\DISPLAY";
  if (name.size() <= prefix.size()) return 0;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::towupper(name[i]) != prefix[i]) return 0;
  }
  uint32_t n = 0;
  for (size_t i = prefix.size(); i < name.size(); ++i) {
    if (name[i] < L'0' || name[i] > L'9') return 0;
    n = n * 10 + static_cast<uint32_t>(name[i] - L'0');
  }
  return n;
}

std::wstring gdiNameFor(uint32_t number) {
  return L"\\\\.\\DISPLAY" + std::to_wstring(number);
}

std::string hresultText(long hr) {
  struct Known {
    unsigned long code;
    const char* name;
  };
  static const Known known[] = {
      {0x887A0001, "DXGI_ERROR_INVALID_CALL"},
      {0x887A0004, "DXGI_ERROR_UNSUPPORTED"},
      {0x887A0005, "DXGI_ERROR_DEVICE_REMOVED"},
      {0x887A0022, "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE"},
      {0x887A0026, "DXGI_ERROR_ACCESS_LOST"},
      {0x887A0027, "DXGI_ERROR_WAIT_TIMEOUT"},
      {0x887A0028, "DXGI_ERROR_SESSION_DISCONNECTED"},
      {0x887A002B, "DXGI_ERROR_ACCESS_DENIED"},
      {0x80070005, "E_ACCESSDENIED"},
      {0x80070057, "E_INVALIDARG"},
      {0x80004005, "E_FAIL"},
  };
  char buf[80];
  const unsigned long code = static_cast<unsigned long>(hr);
  for (const auto& k : known) {
    if (k.code == code) {
      std::snprintf(buf, sizeof(buf), "0x%08lX (%s)", code, k.name);
      return buf;
    }
  }
  std::snprintf(buf, sizeof(buf), "0x%08lX", code);
  return buf;
}

bool interactiveDesktop(std::string* why) {
  DWORD session = 0;
  if (ProcessIdToSessionId(GetCurrentProcessId(), &session) && session == 0) {
    if (why) {
      *why =
          "Frame Ferret is running in session 0 (a service, or an ssh or "
          "remote shell), which has no displays at all. Screen capture and the "
          "virtual display work only on the signed-in user's desktop: run it "
          "from that desktop, or from a scheduled task that runs only when the "
          "user is logged on";
    }
    return false;
  }
  HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!desk) {
    if (why) {
      *why =
          "this session's desktop is not the one being shown: the machine is "
          "locked, at the sign-in screen, or behind a User Account Control "
          "prompt, and Windows lets nothing capture those";
    }
    return false;
  }
  CloseDesktop(desk);
  return true;
}

std::vector<DisplayTarget> displayTargets(std::string* error) {
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  LONG result = ERROR_SUCCESS;
  for (int attempt = 0; attempt < 4; ++attempt) {
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    result = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount);
    if (result != ERROR_SUCCESS) break;
    paths.resize(pathCount);
    modes.resize(modeCount);
    result = QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(),
                                &modeCount, modes.data(), nullptr);
    if (result == ERROR_SUCCESS) {
      paths.resize(pathCount);
      modes.resize(modeCount);
      break;
    }
    // A display came or went between the two calls.
    if (result != ERROR_INSUFFICIENT_BUFFER) break;
  }
  if (result != ERROR_SUCCESS) {
    if (error) {
      *error = "QueryDisplayConfig failed with error " + std::to_string(result);
    }
    return {};
  }

  // QDC_ALL_PATHS lists every source a target could be driven from, so one
  // monitor appears many times; key on the target.
  std::map<std::pair<uint64_t, uint32_t>, DisplayTarget> byTarget;
  for (const auto& path : paths) {
    if (!path.targetInfo.targetAvailable) continue;
    const LUID adapter = path.targetInfo.adapterId;
    const auto key = std::make_pair(
        (static_cast<uint64_t>(static_cast<uint32_t>(adapter.HighPart)) << 32) |
            adapter.LowPart,
        path.targetInfo.id);

    auto found = byTarget.find(key);
    if (found == byTarget.end()) {
      DisplayTarget t;
      DISPLAYCONFIG_TARGET_DEVICE_NAME name = {};
      name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
      name.header.size = sizeof(name);
      name.header.adapterId = adapter;
      name.header.id = path.targetInfo.id;
      if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS) {
        t.friendlyName = name.monitorFriendlyDeviceName;
        t.devicePath = name.monitorDevicePath;
      }
      found = byTarget.emplace(key, std::move(t)).first;
    }
    DisplayTarget& t = found->second;

    if (!(path.flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;
    t.active = true;

    DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS) {
      t.gdiName = source.viewGdiDeviceName;
    }

    const UINT32 index = path.sourceInfo.modeInfoIdx;
    if (index != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && index < modes.size() &&
        modes[index].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
      const auto& mode = modes[index].sourceMode;
      t.width = static_cast<int>(mode.width);
      t.height = static_cast<int>(mode.height);
      t.x = mode.position.x;
      t.y = mode.position.y;
      t.primary = mode.position.x == 0 && mode.position.y == 0;
    }
    if (path.targetInfo.refreshRate.Denominator != 0) {
      t.refresh = Rate{path.targetInfo.refreshRate.Numerator,
                       path.targetInfo.refreshRate.Denominator};
    }
  }

  std::vector<DisplayTarget> out;
  for (auto& [key, t] : byTarget) out.push_back(std::move(t));
  std::sort(out.begin(), out.end(),
            [](const DisplayTarget& a, const DisplayTarget& b) {
              if (a.primary != b.primary) return a.primary;
              if (a.active != b.active) return a.active;
              return gdiNumber(a.gdiName) < gdiNumber(b.gdiName);
            });
  return out;
}

}  // namespace ferret::win

#endif

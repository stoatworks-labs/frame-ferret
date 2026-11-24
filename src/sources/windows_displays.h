#pragma once

// What Windows says about its displays, for the capture and virtual display
// nodes. Windows only.
//
// There are three names for one display on Windows and every API wants a
// different one: the GDI name of the desktop slot ("\\.\DISPLAY2", which
// DXGI's output description and ChangeDisplaySettingsEx use), the monitor's
// device path ("\\?\DISPLAY#MTT1337#...", which says which driver made it),
// and the friendly name from its EDID ("VDD by MTT"). The display
// configuration API is the one place that has all three, so everything goes
// through it.
//
// None of it works in session 0: an ssh or service session has no displays at
// all, and QueryDisplayConfig there returns nothing.

#include <cstdint>
#include <string>
#include <vector>

#include "core/rational.h"

namespace ferret::win {

struct DisplayTarget {
  /// The desktop slot, "\\.\DISPLAY2". Empty when the display is connected
  /// but not part of the desktop.
  std::wstring gdiName;
  /// From the monitor's EDID, "VDD by MTT". May be empty.
  std::wstring friendlyName;
  /// "\\?\DISPLAY#MTT1337#1&28a6823a&6&UID256#{...}": the EDID's maker and
  /// product code are the second field.
  std::wstring devicePath;
  bool active = false;
  bool primary = false;
  /// The desktop mode, when active.
  int width = 0;
  int height = 0;
  int x = 0;
  int y = 0;
  /// The refresh rate the target runs at, exactly, when active.
  Rate refresh{0, 1};
};

/// Every display Windows can show a desktop on, active or not, primary first
/// and then in GDI number order. Empty in session 0 or on failure; `error`
/// says which.
std::vector<DisplayTarget> displayTargets(std::string* error = nullptr);

/// 2 for "\\.\DISPLAY2"; 0 for anything else.
uint32_t gdiNumber(const std::wstring& gdiName);

/// "\\.\DISPLAY" + n.
std::wstring gdiNameFor(uint32_t number);

std::string narrow(const std::wstring& s);
std::wstring widen(const std::string& s);

/// "0x887A0026 (DXGI_ERROR_ACCESS_LOST)", for messages.
std::string hresultText(long hr);

/// True when this process is in the signed-in user's session with its input
/// desktop reachable: what Desktop Duplication and every display API need.
/// `why` says what is missing.
bool interactiveDesktop(std::string* why);

}  // namespace ferret::win

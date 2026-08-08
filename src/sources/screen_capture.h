#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/node.h"

namespace ferret {

/// Screen, window and application capture.
///
/// macOS uses ScreenCaptureKit (12.3+), which is the supported route and the
/// only one that still works: the older CGDisplayStream and
/// CGWindowListCreateImage paths are deprecated and increasingly restricted.
///
/// **This needs Screen Recording permission**, and the failure mode is worth
/// knowing before it happens: without it the capture starts successfully and
/// delivers black frames forever, with no error anywhere. Frame Ferret checks
/// for content up front and reports the permission as the reason rather than
/// letting an operator stare at a black output.
class ScreenCapture {
 public:
  /// False on platforms with no implementation.
  static bool supported();
  static std::string unsupportedReason();

  /// Whether Screen Recording has been granted. On macOS this is inferred by
  /// asking for shareable content — there is no direct query that does not
  /// also prompt.
  static bool permitted();
  static std::string permissionReason();

  struct Display {
    uint32_t id = 0;
    int width = 0;
    int height = 0;
    std::string name;
  };
  struct Window {
    uint32_t id = 0;
    std::string title;
    std::string application;
    int width = 0;
    int height = 0;
  };

  static std::vector<Display> listDisplays();
  static std::vector<Window> listWindows();
  static std::vector<std::string> listApplications();
};

/// A whole display. `config.target` is a display index (0, 1, …) or empty for
/// the main one.
///
/// A region of interest is expressed with `config.cropX/Y/W/H`: ScreenCaptureKit
/// takes a source rect directly, so a crop costs nothing — it is not captured
/// and then thrown away.
std::unique_ptr<Source> makeDisplaySource(const NodeConfig& config,
                                          std::string& error);

/// One window, matched as a substring against its title. Follows the window if
/// it moves or resizes; the raster stays at the configured size.
std::unique_ptr<Source> makeWindowSource(const NodeConfig& config,
                                         std::string& error);

/// Every window of one application, matched as a substring against its name.
std::unique_ptr<Source> makeApplicationSource(const NodeConfig& config,
                                              std::string& error);

}  // namespace ferret

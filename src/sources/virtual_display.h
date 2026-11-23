#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "app/node.h"

namespace ferret {

/// A display Frame Ferret presents to the OS and then captures.
///
/// Other applications see an ordinary screen: it is in System Settings >
/// Displays, it can be arranged, windows can be dragged onto it, and an
/// application that outputs full screen to a display can pick it. Resolume's
/// Advanced Output is the case that drove it: its output devices are compiled
/// into the app, so a virtual display is the only way to send a slice out over
/// a transport Resolume does not speak — route this node to an OMT, NDI or SRT
/// sink and the display goes out over that.
///
/// **macOS only, and private API.** There is no public way to create a display
/// on macOS; this uses CoreGraphics' `CGVirtualDisplay`, as Sidecar,
/// BetterDisplay and DeskPad do. The classes are looked up by name at run time,
/// so a macOS release that removes them fails this node with a sentence and
/// leaves the rest of the program untouched. See docs/03-os-extensions.md.
///
/// The display exists for as long as the node does, and goes when Frame Ferret
/// stops.

/// The decisions about one virtual display, made without touching the OS so a
/// test can reach them.
struct VirtualDisplayPlan {
  /// What System Settings and other applications call the display.
  std::string name;
  int width = 1920;
  int height = 1080;
  /// The refresh rate the display is created at, in whole hertz.
  int refreshHz = 60;
  /// Stable for a given name, so macOS remembers where the display was
  /// arranged and which application last output to it.
  uint32_t serial = 1;
  /// Physical size reported to the OS. A plausible desktop pixel pitch, so
  /// macOS neither treats the display as Retina nor offers scaled modes.
  double widthMm = 0;
  double heightMm = 0;
  /// Set when the display cannot run at exactly the configured rate.
  std::string warning;
};

/// Works out the display for `config`. The name is `target`, else `label`,
/// else "Frame Ferret". Fails, with a sentence in `error`, for a raster or a
/// rate a display cannot have.
///
/// A fractional rate (59.94, 29.97, 23.98) is accepted with a warning: macOS
/// folds fractional virtual-display rates up to the whole rate, so a 59.94
/// display measured at 60.03 Hz over a minute on macOS 26.4. Capturing it into
/// a 59.94 engine drops about one frame every 16.7 seconds.
bool planVirtualDisplay(const NodeConfig& config, VirtualDisplayPlan* out,
                        std::string& error);

/// Creates the display and starts capturing it. On platforms without one, or
/// on a macOS that no longer has the private classes, returns null with the
/// reason in `error`.
///
/// Call from the main thread: it waits, servicing the main run loop, for the
/// window server to publish the new display.
std::unique_ptr<Source> makeVirtualDisplaySource(const NodeConfig& config,
                                                 std::string& error);

}  // namespace ferret

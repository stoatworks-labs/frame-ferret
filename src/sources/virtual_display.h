#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "app/node.h"

namespace ferret {

/// A display Frame Ferret presents to the OS and then captures.
///
/// Other applications see an ordinary screen: it is in the system's display
/// settings, it can be arranged, windows can be dragged onto it, and an
/// application that outputs full screen to a display can pick it. Resolume's
/// Advanced Output is the case that drove it: its output devices are compiled
/// into the app, so a virtual display is the only way to send a slice out over
/// a transport Resolume does not speak — route this node to an OMT, NDI or SRT
/// sink and the display goes out over that.
///
/// **macOS: private API.** There is no public way to create a display on
/// macOS; this uses CoreGraphics' `CGVirtualDisplay`, as Sidecar, BetterDisplay
/// and DeskPad do. The classes are looked up by name at run time, so a macOS
/// release that removes them fails this node with a sentence and leaves the
/// rest of the program untouched. The display exists for as long as the node
/// does, and goes when Frame Ferret stops.
///
/// **Windows: a signed driver someone else ships.** A display on Windows needs
/// an Indirect Display Driver, and a driver loads only when Microsoft has
/// attestation-signed it. Rather than ship an unsigned one, this drives
/// VirtualDrivers' Virtual Display Driver (VDD: MIT, signed through the
/// SignPath Foundation), which the operator installs once. Its displays exist
/// whether Frame Ferret runs or not; the node claims one, gives it the planned
/// mode (adding the mode to VDD's settings file if VDD does not list it yet)
/// and captures it with Desktop Duplication. See docs/03-os-extensions.md.

/// Which mechanism a plan is for. Both run whole rates only; they differ in
/// what they say about it and in what VDD's settings file needs.
enum class VirtualDisplayHost {
  macos,       ///< CGVirtualDisplay.
  windowsVdd,  ///< VirtualDrivers' VDD.
};

/// The host this build creates displays with.
VirtualDisplayHost currentVirtualDisplayHost();

/// The decisions about one virtual display, made without touching the OS so a
/// test can reach them.
struct VirtualDisplayPlan {
  /// What the display is called. On macOS this is the name System Settings
  /// and other applications show. On Windows the display's name comes from
  /// VDD's EDID ("VDD by MTT"), so this is only in Frame Ferret's messages.
  std::string name;
  int width = 1920;
  int height = 1080;
  /// The refresh rate the display runs at, a whole number of hertz: the
  /// configured rate, or the whole rate above a fractional one (60 for 59.94).
  int refreshHz = 60;
  /// The same, as the rate Windows reports and is asked for.
  Rate refresh{60, 1};
  /// The refresh rate as written in VDD's settings file ("50"). Empty on
  /// macOS.
  std::string vddRefresh;
  /// Stable for a given name, so macOS remembers where the display was
  /// arranged and which application last output to it. Unused on Windows.
  uint32_t serial = 1;
  /// Physical size reported to macOS. A plausible desktop pixel pitch, so
  /// macOS neither treats the display as Retina nor offers scaled modes.
  double widthMm = 0;
  double heightMm = 0;
  /// Set when the display cannot run at exactly the configured rate.
  std::string warning;
};

/// Works out the display for `config` on `host`. The name is `target`, else
/// `label`, else "Frame Ferret". Fails, with a sentence in `error`, for a
/// raster or a rate a display cannot have.
///
/// A fractional rate (59.94, 29.97, 23.98) is accepted with a warning, which
/// differs by host:
///
/// - macOS folds fractional virtual-display rates up to the whole rate: a
///   59.94 display measured 60.03 Hz over a minute on macOS 26.4. Capturing it
///   into a 59.94 engine drops about one frame every 16.7 seconds.
/// - VDD reads its refresh rates to four decimal places (it multiplies by
///   10000 and rounds), so 60000/1001 becomes 599401/10000 Hz: off by four
///   parts in ten million, about one frame every seven hours.
bool planVirtualDisplayFor(VirtualDisplayHost host, const NodeConfig& config,
                           VirtualDisplayPlan* out, std::string& error);

/// planVirtualDisplayFor() for this build's host.
bool planVirtualDisplay(const NodeConfig& config, VirtualDisplayPlan* out,
                        std::string& error);

/// Creates (macOS) or claims (Windows) the display and starts capturing it.
/// On platforms without one, on a macOS that no longer has the private
/// classes, or on a Windows without VDD, returns null with the reason in
/// `error`.
///
/// Call from the main thread: on macOS it waits, servicing the main run loop,
/// for the window server to publish the new display.
std::unique_ptr<Source> makeVirtualDisplaySource(const NodeConfig& config,
                                                 std::string& error);

}  // namespace ferret

// The OS-free half of the virtual display: what to create, decided where a
// test can reach it. The display itself is in virtual_display.mm.
#include <cmath>

#include "sources/virtual_display.h"

namespace ferret {
namespace {

/// FNV-1a. The serial only has to be stable for a name and unlikely to collide
/// between two displays on one machine; it is never zero, which macOS reads as
/// "no serial" and then confuses displays of the same product.
uint32_t serialFor(const std::string& name) {
  uint32_t h = 2166136261u;
  for (unsigned char c : name) {
    h ^= c;
    h *= 16777619u;
  }
  return h == 0 ? 1 : h;
}

}  // namespace

bool planVirtualDisplay(const NodeConfig& config, VirtualDisplayPlan* out,
                        std::string& error) {
  VirtualDisplayPlan plan;

  plan.name = !config.target.empty() ? config.target
              : !config.label.empty() ? config.label
                                      : std::string("Frame Ferret");
  // System Settings truncates long names, and an EDID-style product name is
  // short. 48 is generous and still keeps a menu readable.
  if (plan.name.size() > 48) {
    error = "a virtual display name is at most 48 characters — \"" +
            plan.name + "\" is " + std::to_string(plan.name.size());
    return false;
  }

  if (config.width < 640 || config.height < 360 || config.width > 7680 ||
      config.height > 4320) {
    error = "a virtual display must be between 640x360 and 7680x4320, not " +
            std::to_string(config.width) + "x" + std::to_string(config.height);
    return false;
  }
  if (config.width % 2 != 0 || config.height % 2 != 0) {
    // Every transport downstream is 4:2:2 or 4:2:0, which needs an even
    // raster; a display that cannot be sent is not worth creating.
    error = "a virtual display needs an even width and height, not " +
            std::to_string(config.width) + "x" + std::to_string(config.height);
    return false;
  }
  plan.width = config.width;
  plan.height = config.height;

  if (!config.rate.valid() || config.rate.approx() < 23.0 ||
      config.rate.approx() > 120.5) {
    error = "a virtual display runs between 23.98 and 120 Hz, not " +
            config.rate.label();
    return false;
  }
  const double hz = config.rate.approx();
  plan.refreshHz = static_cast<int>(std::ceil(hz - 1e-6));
  if (config.rate.num % config.rate.den != 0) {
    plan.warning =
        "macOS runs a virtual display at a whole rate, so this " +
        config.rate.label() + " display runs at " +
        std::to_string(plan.refreshHz) +
        " Hz and its capture drops about one frame every " +
        std::to_string(static_cast<int>(std::lround(1.0 / (plan.refreshHz - hz)))) +
        " seconds. A whole rate (25, 30, 50, 60) has no drop.";
  }

  plan.serial = serialFor(plan.name);
  // 0.25 mm a pixel is about 100 ppi: an ordinary desktop monitor, so macOS
  // offers the one native mode rather than HiDPI variants of it.
  plan.widthMm = plan.width * 0.25;
  plan.heightMm = plan.height * 0.25;

  *out = plan;
  return true;
}

}  // namespace ferret

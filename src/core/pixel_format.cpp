#include "core/pixel_format.h"

namespace ferret {
namespace {

// Indexed by PixelFormat. Keep in the same order as the enum.
const PixelFormatInfo kInfo[] = {
    {"unknown", 0, false, false, 1, 1},
    {"bgra8", 8, true, true, 1, 1},
    {"rgba8", 8, true, true, 1, 1},
    {"uyvy8", 8, false, false, 2, 1},
    {"yuy2_8", 8, false, false, 2, 1},
    {"v210", 10, false, false, 2, 1},
    {"nv12", 8, false, false, 2, 2},
    {"ycbcr422_10_pgroup", 10, false, false, 2, 1},
};

}  // namespace

const PixelFormatInfo& describe(PixelFormat f) {
  return kInfo[static_cast<int>(f)];
}

int tightStrideBytes(PixelFormat f, int width) {
  if (width <= 0) return 0;
  switch (f) {
    case PixelFormat::bgra8:
    case PixelFormat::rgba8:
      return width * 4;
    case PixelFormat::uyvy8:
    case PixelFormat::yuy2_8:
      return width * 2;
    case PixelFormat::v210:
      // 6 pixels per 16 bytes, rounded up. Note this is the *tight* stride;
      // DeckLink wants it aligned up to 128 and will not tell you unless you
      // ask GetBytesPerRow.
      return ((width + 5) / 6) * 16;
    case PixelFormat::nv12:
      return width;  // Y plane. Chroma plane is the same stride, half height.
    case PixelFormat::ycbcr422_10_pgroup:
      // 2 pixels (one pgroup) per 5 bytes, per SMPTE 2110-20 §6.
      return ((width + 1) / 2) * 5;
    case PixelFormat::unknown:
      return 0;
  }
  return 0;
}

const char* toString(PixelFormat f) { return describe(f).name; }

const char* toString(ColourSpace c) {
  switch (c) {
    case ColourSpace::bt601: return "bt601";
    case ColourSpace::bt709: return "bt709";
    case ColourSpace::bt2020: return "bt2020";
    case ColourSpace::unknown: return "unknown";
  }
  return "unknown";
}

const char* toString(QuantRange q) {
  switch (q) {
    case QuantRange::narrow: return "narrow";
    case QuantRange::full: return "full";
    case QuantRange::unknown: return "unknown";
  }
  return "unknown";
}

PixelFormat pixelFormatFromString(const std::string& s) {
  for (int i = 0; i <= static_cast<int>(PixelFormat::ycbcr422_10_pgroup); ++i) {
    auto f = static_cast<PixelFormat>(i);
    if (s == describe(f).name) return f;
  }
  return PixelFormat::unknown;
}

}  // namespace ferret

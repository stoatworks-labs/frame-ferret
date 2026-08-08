#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/frame.h"

namespace ferret {

/// The YCbCr luma coefficients for a colour space. Kg is derived, never
/// stored: 1 - Kr - Kb by definition, and a stored third coefficient is one
/// more thing that can disagree with the other two.
struct LumaCoefficients {
  double kr;
  double kb;
  double kg() const { return 1.0 - kr - kb; }
};

LumaCoefficients coefficientsFor(ColourSpace c);

/// Whether `from` can be converted to `to` by this module. The router assumes
/// any `convert` action it plans is achievable, so a sink must not advertise a
/// format that fails this.
bool canConvert(PixelFormat from, PixelFormat to);

/// Converts `src` into `dst`, allocating `dst`'s storage as needed.
///
/// Returns false and sets `error` on an unsupported pair or a malformed
/// source. Colour space and quantisation range come from `src`; the output
/// carries the same ones, because this module changes *layout*, never
/// colorimetry. Converting 709 to 601 is a different operation and is not
/// hidden inside a format change — that is precisely how a path ends up
/// subtly wrong instead of obviously broken.
bool convert(const VideoFrame& src, PixelFormat to, PixelFormat& outFormat,
             std::vector<uint8_t>& dst, int& dstStride, std::string& error);

/// Fills `dst` with legal black for `format`. For YCbCr that is Y=16, C=128 at
/// 8 bits (not all-zero, which is superblack and illegal on SDI); for RGB it is
/// all-zero with an opaque alpha.
///
/// Used on every `black` route action, so it is on the hot path whenever an
/// output has no source.
void fillBlack(PixelFormat format, int width, int height, int strideBytes,
               QuantRange range, uint8_t* dst);

}  // namespace ferret

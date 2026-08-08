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
/// Colour *space* is never changed — 709 stays 709. Converting 601 to 709 is a
/// different operation and is not hidden inside a format change.
///
/// Quantisation **range is** normalised, and must be, which is why `outRange`
/// exists rather than the caller assuming the source's:
///
///   - **RGB source to YCbCr destination -> narrow.** Every transport here
///     (NDI, OMT, SRT, DeckLink) carries narrow-range YCbCr by convention. A
///     full-range RGB source — which is every screen capture and every
///     generated test pattern — must be encoded narrow, or the receiver
///     expands it a second time. That was a real bug: colour bars sent over
///     SRT came back with green at 240 instead of 191, worst on the channels
///     with the most luma weight, while the hues and ordering stayed perfect.
///   - **Any source to an RGB destination -> full.** RGB is full-range by
///     convention, and the shared-surface and preview paths expect it.
///   - **YCbCr to YCbCr** keeps the source's range; nothing is rescaled.
///
/// Returns false and sets `error` on an unsupported pair or a malformed source.
bool convert(const VideoFrame& src, PixelFormat to, PixelFormat& outFormat,
             QuantRange& outRange, std::vector<uint8_t>& dst, int& dstStride,
             std::string& error);

/// Fills `dst` with legal black for `format`. For YCbCr that is Y=16, C=128 at
/// 8 bits (not all-zero, which is superblack and illegal on SDI); for RGB it is
/// all-zero with an opaque alpha.
///
/// Used on every `black` route action, so it is on the hot path whenever an
/// output has no source.
void fillBlack(PixelFormat format, int width, int height, int strideBytes,
               QuantRange range, uint8_t* dst);

}  // namespace ferret

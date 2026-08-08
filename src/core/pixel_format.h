#pragma once

#include <cstdint>
#include <string>

namespace ferret {

/// The pixel formats that cross Frame Ferret's router.
///
/// oxbow and WebLinked normalise everything to BGRA at ingest, which is right
/// for them: both feed a GPU effect chain that wants 8-bit RGBA anyway. Frame Ferret
/// must not. Its two highest-value paths — ST 2110-20 and DeckLink — are
/// natively 10-bit YCbCr 4:2:2, and a BGRA waypoint would quantise both to 8
/// bits *and* pay two chroma resamples for a route that should be a copy.
///
/// So the format travels with the frame and conversion happens only where a
/// sink genuinely cannot take what a source produces. `2110 -> DeckLink` is a
/// repack, not a convert; `2110 -> Syphon` is a real conversion and is charged
/// as one.
enum class PixelFormat {
  unknown,

  // Packed RGB. The shared-surface and HTML paths speak these.
  bgra8,  ///< 8:8:8:8, B in the low byte. Syphon/Spout/CEF native.
  rgba8,  ///< 8:8:8:8, R in the low byte.

  // Packed YCbCr 4:2:2. The broadcast middle ground.
  uyvy8,  ///< 8-bit, Cb Y0 Cr Y1. DeckLink '2vuy', UVC-friendly.
  yuy2_8, ///< 8-bit, Y0 Cb Y1 Cr. What most UVC hosts ask for first.
  v210,   ///< 10-bit, 6 pixels per 16 bytes. DeckLink native, ST 2110 adjacent.

  // Planar YCbCr 4:2:0. Only ever a UVC/codec concession — never a route we
  // pick, because 4:2:0 chroma loss is not recoverable downstream.
  nv12,

  // ST 2110-20 on the wire. Not a memory layout you can index into: it is
  // pgroup-packed big-endian samples per SMPTE 2110-20 §6. Held only between
  // the depacketiser and the first conversion.
  ycbcr422_10_pgroup,
};

struct PixelFormatInfo {
  const char* name;
  int bitsPerComponent;
  bool hasAlpha;
  bool isRgb;      ///< false = YCbCr, and so carries a colour space.
  int chromaSubH;  ///< 1 = 4:4:4, 2 = 4:2:2 / 4:2:0 horizontally.
  int chromaSubV;  ///< 1 = 4:4:4 / 4:2:2, 2 = 4:2:0.
};

const PixelFormatInfo& describe(PixelFormat f);

/// Bytes per row for a tightly packed image. Returns 0 for `unknown`.
///
/// v210 is the one that catches people: it packs 6 pixels into 16 bytes, so a
/// width that is not a multiple of 6 rounds up, and DeckLink additionally
/// aligns the result to 128 bytes. Ask the sink for its own stride rather than
/// assuming this value is what the card wants.
int tightStrideBytes(PixelFormat f, int width);

/// Colour space, carried separately because it is not implied by the format.
/// A UYVY frame off a DeckLink is 709 for HD and 601 for SD, and getting this
/// wrong is the single most common way a video path ends up subtly wrong
/// rather than obviously broken — see reference_ffmpeg_test_pattern_traps in
/// the fleet notes, where rgb24 silently used 601 for HD.
enum class ColourSpace { unknown, bt601, bt709, bt2020 };

/// Whether the YCbCr samples use the narrow (16..235 luma) or full (0..255)
/// range. Broadcast is narrow; almost every screen-capture source is full.
/// Conflating them is a visible black-level shift, not a subtle one.
enum class QuantRange { unknown, narrow, full };

const char* toString(PixelFormat f);
const char* toString(ColourSpace c);
const char* toString(QuantRange q);

PixelFormat pixelFormatFromString(const std::string& s);

}  // namespace ferret

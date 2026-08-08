#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/pixel_format.h"
#include "core/rational.h"

namespace ferret {

/// One video frame, in whatever format its producer natively emits.
///
/// `data` is borrowed for the duration of the callback that delivered it and
/// is never owned by this struct — several transports hand back SDK-owned
/// memory that must be released before the next capture call. A sink that
/// needs to keep a frame must copy it into a `FrameBuffer`.
struct VideoFrame {
  int width = 0;
  int height = 0;
  int strideBytes = 0;
  const uint8_t* data = nullptr;

  PixelFormat format = PixelFormat::unknown;
  ColourSpace colour = ColourSpace::bt709;
  QuantRange range = QuantRange::narrow;

  /// True when row 0 is the *bottom* of the picture. Only NDI on Windows and
  /// raw GL readback deliver that; everything else is top-down. Normalised at
  /// ingest rather than carried through the router, but the flag exists so an
  /// ingest that forgets is a visible field rather than a silent flip.
  bool bottomUp = false;

  /// Progressive unless stated. 2110-20 and SDI both carry interlaced rasters
  /// and a virtual camera that reports them as progressive will look fine on a
  /// still and comb on any motion.
  bool interlaced = false;
  bool topFieldFirst = true;

  Rate rate{60, 1};

  /// Nanoseconds on the TAI timeline when PTP is locked, otherwise on the
  /// monotonic clock. `ptpLocked` says which — a receiver must not treat a
  /// free-running timestamp as a media clock.
  int64_t timestampNs = 0;
  bool ptpLocked = false;
};

/// Audio, always planar float32. Both NDI (FLTP) and OMT (FPA1) are natively
/// planar float, so protocol bridging stays a copy; 2110-30 (L24/L16 big-endian
/// interleaved) and DeckLink (S32 interleaved) convert at their own edges,
/// where the cost is unavoidable anyway.
struct AudioFrame {
  int sampleRate = 48000;
  int channels = 0;
  int samplesPerChannel = 0;
  std::vector<float> data;  ///< `channels` planes of `samplesPerChannel`.
  int64_t timestampNs = 0;
  bool ptpLocked = false;
};

/// SMPTE 291M ancillary data — closed captions, timecode, AFD. Carried as
/// opaque DIDs so 2110-40 in and SDI out is a passthrough rather than a parse.
/// Nothing in Frame Ferret interprets these; they exist so that a route which
/// claims to be transparent actually is.
struct AncillaryPacket {
  uint8_t did = 0;
  uint8_t sdid = 0;
  uint16_t lineNumber = 0;
  bool cPair = false;  ///< true = chroma (C) stream, false = luma (Y).
  std::vector<uint8_t> udw;
};

struct AncillaryFrame {
  std::vector<AncillaryPacket> packets;
  int64_t timestampNs = 0;
};

/// An owning copy of a video frame. Allocation is pooled by the router rather
/// than done per frame — at 1080p50 a naive allocator is a measurable share of
/// the frame budget and a reliable source of jitter under memory pressure.
class FrameBuffer {
 public:
  FrameBuffer() = default;
  FrameBuffer(const FrameBuffer&) = delete;
  FrameBuffer& operator=(const FrameBuffer&) = delete;

  /// Resizes if the geometry changed, otherwise reuses the existing storage.
  void assign(const VideoFrame& src);

  const VideoFrame& frame() const { return frame_; }
  bool empty() const { return storage_.empty(); }

 private:
  VideoFrame frame_;
  std::vector<uint8_t> storage_;
};

}  // namespace ferret

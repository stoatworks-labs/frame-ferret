#pragma once

#include <cstddef>
#include <cstdint>

/// Our mirror of libomt's C ABI (Open Media Transport, libomt 1.0.0.16).
///
/// `libomt.h` is not vendored here, so this is the only description of the ABI
/// in the tree. Every number was printed by `tools/omt_abi.c` compiled against
/// the real header and is pinned by a static_assert — same discipline as
/// ndi_abi.h, and for the same reason: a wrong offset reads the wrong field
/// rather than crashing.
///
/// One structural difference from NDI worth knowing: `omt_receive` returns a
/// **pointer to an SDK-owned OMTMediaFrame**, rather than filling in a caller's
/// struct. The frame is valid only until the next call on that receiver.

namespace ferret::omt_abi {

enum FrameType : int32_t {
  kFrameNone = 0,
  kFrameMetadata = 1,
  kFrameVideo = 2,
  kFrameAudio = 4,
};

/// FourCC-packed, little-endian, exactly like NDI's.
enum Codec : int32_t {
  kCodecVmx1 = 0x31584D56,  // 'VMX1', OMT's own compressed format
  kCodecFpa1 = 0x31415046,  // 'FPA1', planar float audio
  kCodecUyvy = 0x59565955,  // 'UYVY'
  kCodecYuy2 = 0x32595559,  // 'YUY2'
  kCodecBgra = 0x41524742,  // 'BGRA'
  kCodecNv12 = 0x3231564E,  // 'NV12'
  kCodecYv12 = 0x32315659,  // 'YV12'
  kCodecUyva = 0x41565955,  // 'UYVA'
  kCodecP216 = 0x36313250,  // 'P216'
  kCodecPa16 = 0x36314150,  // 'PA16'
};

enum Quality : int32_t {
  kQualityDefault = 0,
  kQualityLow = 1,
  kQualityMedium = 50,
  kQualityHigh = 100,
};

enum PreferredVideoFormat : int32_t {
  kPreferUyvy = 0,
  kPreferUyvyOrBgra = 1,
  kPreferBgra = 2,
  kPreferUyvyOrUyva = 3,
  kPreferUyvyOrUyvaOrP216OrPa16 = 4,
  kPreferP216 = 5,
};

/// OMT states its colour space explicitly as 601 or 709 — a real improvement
/// over NDI, which carries none and leaves every implementation to infer it
/// from the raster.
/// Named OmtColourSpace, not ColourSpace: `ferret::ColourSpace` already exists
/// and having both visible under a `using namespace` makes every mention
/// ambiguous — the same clash NDIlib_source_t caused.
enum OmtColourSpace : int32_t {
  kColourUndefined = 0,
  kColour601 = 601,
  kColour709 = 709,
};

enum VideoFlags : int32_t {
  kFlagNone = 0,
  kFlagInterlaced = 1,
  kFlagAlpha = 2,
  kFlagPreMultiplied = 4,
  kFlagPreview = 8,
  kFlagHighBitDepth = 16,
};

enum ReceiveFlags : int32_t {
  kReceiveNone = 0,
  kReceivePreview = 1,
  kReceiveIncludeCompressed = 2,
  kReceiveCompressedOnly = 4,
};

/// Timestamps are in 100 ns units, like NDI's. -1 asks the sender to generate
/// them and throttle to the declared frame rate — which we never use, because
/// the router already paces every sink and two clocks on one path fight.
constexpr int64_t kTimestampAuto = -1;

struct MediaFrame {
  int32_t Type = kFrameNone;
  // 4 bytes of padding here, before the 8-aligned Timestamp.
  int64_t Timestamp = 0;
  int32_t Codec = 0;
  int32_t Width = 0;
  int32_t Height = 0;
  int32_t Stride = 0;
  int32_t Flags = kFlagNone;
  int32_t FrameRateN = 0;
  int32_t FrameRateD = 1;
  float AspectRatio = 0.0f;
  int32_t ColorSpace = kColourUndefined;
  int32_t SampleRate = 0;
  int32_t Channels = 0;
  int32_t SamplesPerChannel = 0;
  void* Data = nullptr;
  int32_t DataLength = 0;
  // 4 bytes of padding.
  void* CompressedData = nullptr;
  int32_t CompressedLength = 0;
  // 4 bytes of padding.
  void* FrameMetadata = nullptr;
  int32_t FrameMetadataLength = 0;
};

static_assert(sizeof(MediaFrame) == 112, "OMTMediaFrame is 112 bytes");
static_assert(offsetof(MediaFrame, Type) == 0, "");
static_assert(offsetof(MediaFrame, Timestamp) == 8, "");
static_assert(offsetof(MediaFrame, Codec) == 16, "");
static_assert(offsetof(MediaFrame, Width) == 20, "");
static_assert(offsetof(MediaFrame, Height) == 24, "");
static_assert(offsetof(MediaFrame, Stride) == 28, "");
static_assert(offsetof(MediaFrame, Flags) == 32, "");
static_assert(offsetof(MediaFrame, FrameRateN) == 36, "");
static_assert(offsetof(MediaFrame, FrameRateD) == 40, "");
static_assert(offsetof(MediaFrame, AspectRatio) == 44, "");
static_assert(offsetof(MediaFrame, ColorSpace) == 48, "");
static_assert(offsetof(MediaFrame, SampleRate) == 52, "");
static_assert(offsetof(MediaFrame, Channels) == 56, "");
static_assert(offsetof(MediaFrame, SamplesPerChannel) == 60, "");
static_assert(offsetof(MediaFrame, Data) == 64, "");
static_assert(offsetof(MediaFrame, DataLength) == 72, "");
static_assert(offsetof(MediaFrame, CompressedData) == 80, "");
static_assert(offsetof(MediaFrame, CompressedLength) == 88, "");
static_assert(offsetof(MediaFrame, FrameMetadata) == 96, "");
static_assert(offsetof(MediaFrame, FrameMetadataLength) == 104, "");

static_assert(sizeof(void*) == 8, "the pinned OMT offsets assume 64-bit");

}  // namespace ferret::omt_abi

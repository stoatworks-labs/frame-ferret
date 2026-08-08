#pragma once

#include <cstddef>
#include <cstdint>

/// Our own mirror of the NDI SDK's C ABI.
///
/// The SDK headers are NOT vendored — the licence forbids redistributing them
/// under MIT — so this file is the only description of the ABI in the tree and
/// it must be exactly right.
///
/// Two rules, both learned the hard way in this fleet:
///
///  1. **Bind the flat C ABI** (`NDIlib_send_create` and friends, exported by
///     every NDI 5 and 6 runtime), never the versioned `NDIlib_v6_load()`
///     struct whose layout changes between SDK generations.
///  2. **Never work the layouts out by hand.** Every number below was printed
///     by `tools/ndi_abi.c` compiled against the real headers (SDK 6.3.2.0,
///     macOS arm64) and is pinned by a static_assert. Hand arithmetic
///     previously got `NDIlib_audio_frame_v3_t` wrong — it is 64 bytes, not
///     56, because `no_samples` is followed by four bytes of padding before
///     `timecode`. A wrong layout does not crash: it reads the wrong field, so
///     a receiver connects and delivers rubbish.
///
/// Offsets are asserted as well as sizes. Two adjacent fields of the same type
/// swapped produce an identical total size and entirely different behaviour.

namespace ferret::ndi_abi {

// ---------------------------------------------------------------------------
// Enums. Values printed by tools/ndi_abi.c, not assumed.
// ---------------------------------------------------------------------------

enum FrameType : int {
  kFrameNone = 0,
  kFrameVideo = 1,
  kFrameAudio = 2,
  kFrameMetadata = 3,
  kFrameError = 4,
};

enum FrameFormat : int {
  kFormatInterleaved = 0,
  kFormatProgressive = 1,
  kFormatField0 = 2,
  kFormatField1 = 3,
};

enum RecvColourFormat : int {
  kRecvBgrxBgra = 0,
  kRecvUyvyBgra = 1,
  kRecvFastest = 100,
  kRecvBest = 101,
};

enum RecvBandwidth : int {
  kBandwidthMetadataOnly = -10,
  kBandwidthLowest = 0,
  kBandwidthAudioOnly = 10,
  kBandwidthHighest = 100,
};

// FourCCs, little-endian packed as the SDK's NDI_LIB_FOURCC macro produces.
constexpr uint32_t kFourCcUyvy = 0x59565955;  // 'UYVY'
constexpr uint32_t kFourCcBgra = 0x41524742;  // 'BGRA'
constexpr uint32_t kFourCcBgrx = 0x58524742;  // 'BGRX'
constexpr uint32_t kFourCcRgba = 0x41424752;  // 'RGBA'

/// Planar float audio — and note the spelling: the SDK declares this as
/// `NDI_LIB_FOURCC('F', 'L', 'T', 'p')`, with a **lowercase p**, even though
/// every document (including NDI's own prose) writes it "FLTP". Assume the
/// obvious spelling and the constant is 0x50544C46 instead of 0x70544C46, the
/// SDK treats every audio frame as an unknown format, and audio silently never
/// arrives while video works perfectly.
constexpr uint32_t kFourCcFltp = 0x70544C46;  // 'FLTp'

/// The SDK's "no timecode, synthesise one" sentinel.
constexpr int64_t kTimecodeSynthesise = INT64_MAX;

// ---------------------------------------------------------------------------
// Structs.
// ---------------------------------------------------------------------------

/// NDIlib_source_t. Named SourceRef rather than Source because `ferret::Source`
/// is the node abstraction, and having both visible under a `using namespace`
/// makes every mention ambiguous.
struct SourceRef {
  const char* p_ndi_name = nullptr;
  const char* p_url_address = nullptr;  // union with the deprecated p_ip_address
};
static_assert(sizeof(SourceRef) == 16, "NDIlib_source_t is 16 bytes");
static_assert(offsetof(SourceRef, p_ndi_name) == 0, "");
static_assert(offsetof(SourceRef, p_url_address) == 8, "");

struct VideoFrameV2 {
  int32_t xres = 0;
  int32_t yres = 0;
  uint32_t FourCC = kFourCcUyvy;
  int32_t frame_rate_N = 30000;
  int32_t frame_rate_D = 1001;
  float picture_aspect_ratio = 0.0f;
  int32_t frame_format_type = kFormatProgressive;
  // Four bytes of padding here, before the 8-byte-aligned timecode. Implicit
  // rather than declared, and confirmed by the offsets below.
  int64_t timecode = kTimecodeSynthesise;
  uint8_t* p_data = nullptr;
  int32_t line_stride_in_bytes = 0;  // union with data_size_in_bytes
  const char* p_metadata = nullptr;
  int64_t timestamp = 0;
};
static_assert(sizeof(VideoFrameV2) == 72, "NDIlib_video_frame_v2_t is 72 bytes");
static_assert(offsetof(VideoFrameV2, xres) == 0, "");
static_assert(offsetof(VideoFrameV2, yres) == 4, "");
static_assert(offsetof(VideoFrameV2, FourCC) == 8, "");
static_assert(offsetof(VideoFrameV2, frame_rate_N) == 12, "");
static_assert(offsetof(VideoFrameV2, frame_rate_D) == 16, "");
static_assert(offsetof(VideoFrameV2, picture_aspect_ratio) == 20, "");
static_assert(offsetof(VideoFrameV2, frame_format_type) == 24, "");
static_assert(offsetof(VideoFrameV2, timecode) == 32, "");
static_assert(offsetof(VideoFrameV2, p_data) == 40, "");
static_assert(offsetof(VideoFrameV2, line_stride_in_bytes) == 48, "");
static_assert(offsetof(VideoFrameV2, p_metadata) == 56, "");
static_assert(offsetof(VideoFrameV2, timestamp) == 64, "");

struct AudioFrameV3 {
  int32_t sample_rate = 48000;
  int32_t no_channels = 0;
  int32_t no_samples = 0;
  // Padding here too — this is the one hand arithmetic got wrong.
  int64_t timecode = kTimecodeSynthesise;
  uint32_t FourCC = kFourCcFltp;
  uint8_t* p_data = nullptr;
  int32_t channel_stride_in_bytes = 0;  // union with data_size_in_bytes
  const char* p_metadata = nullptr;
  int64_t timestamp = 0;
};
static_assert(sizeof(AudioFrameV3) == 64, "NDIlib_audio_frame_v3_t is 64 bytes");
static_assert(offsetof(AudioFrameV3, sample_rate) == 0, "");
static_assert(offsetof(AudioFrameV3, no_channels) == 4, "");
static_assert(offsetof(AudioFrameV3, no_samples) == 8, "");
static_assert(offsetof(AudioFrameV3, timecode) == 16, "");
static_assert(offsetof(AudioFrameV3, FourCC) == 24, "");
static_assert(offsetof(AudioFrameV3, p_data) == 32, "");
static_assert(offsetof(AudioFrameV3, channel_stride_in_bytes) == 40, "");
static_assert(offsetof(AudioFrameV3, p_metadata) == 48, "");
static_assert(offsetof(AudioFrameV3, timestamp) == 56, "");

struct FindCreate {
  bool show_local_sources = true;
  const char* p_groups = nullptr;
  const char* p_extra_ips = nullptr;
};
static_assert(sizeof(FindCreate) == 24, "NDIlib_find_create_t is 24 bytes");
static_assert(offsetof(FindCreate, show_local_sources) == 0, "");
static_assert(offsetof(FindCreate, p_groups) == 8, "");
static_assert(offsetof(FindCreate, p_extra_ips) == 16, "");

struct SendCreate {
  const char* p_ndi_name = nullptr;
  const char* p_groups = nullptr;
  bool clock_video = true;
  bool clock_audio = true;
};
static_assert(sizeof(SendCreate) == 24, "NDIlib_send_create_t is 24 bytes");
static_assert(offsetof(SendCreate, p_ndi_name) == 0, "");
static_assert(offsetof(SendCreate, p_groups) == 8, "");
static_assert(offsetof(SendCreate, clock_video) == 16, "");
static_assert(offsetof(SendCreate, clock_audio) == 17, "");

struct RecvCreateV3 {
  SourceRef source_to_connect_to;
  int32_t color_format = kRecvBgrxBgra;
  int32_t bandwidth = kBandwidthHighest;
  bool allow_video_fields = true;
  const char* p_ndi_recv_name = nullptr;
};
static_assert(sizeof(RecvCreateV3) == 40, "NDIlib_recv_create_v3_t is 40 bytes");
static_assert(offsetof(RecvCreateV3, source_to_connect_to) == 0, "");
static_assert(offsetof(RecvCreateV3, color_format) == 16, "");
static_assert(offsetof(RecvCreateV3, bandwidth) == 20, "");
static_assert(offsetof(RecvCreateV3, allow_video_fields) == 24, "");
static_assert(offsetof(RecvCreateV3, p_ndi_recv_name) == 32, "");

// A pointer size other than 8 invalidates every offset above.
static_assert(sizeof(void*) == 8, "the pinned NDI offsets assume 64-bit");

}  // namespace ferret::ndi_abi

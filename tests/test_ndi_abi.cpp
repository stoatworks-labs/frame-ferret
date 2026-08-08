#include "transports/ndi_abi.h"

#include <cstring>

#include "check.h"
#include "transports/ndi.h"

using namespace ferret;
using namespace ferret::ndi_abi;

namespace {

/// The static_asserts in ndi_abi.h already fail the build on a layout change,
/// which is the real guard. These make the numbers visible in a test run too,
/// so a future reader sees what the ABI is without reading the header.
void layoutsMatchTheRealSdk() {
  CHECK_EQ(sizeof(SourceRef), size_t{16});
  CHECK_EQ(sizeof(VideoFrameV2), size_t{72});
  CHECK_EQ(sizeof(AudioFrameV3), size_t{64});
  CHECK_EQ(sizeof(FindCreate), size_t{24});
  CHECK_EQ(sizeof(SendCreate), size_t{24});
  CHECK_EQ(sizeof(RecvCreateV3), size_t{40});

  // The two the padding catches out. no_samples is at 8 and timecode at 16,
  // not 12 — four bytes of padding sit between them. Getting this wrong is
  // what made a hand-computed audio struct 56 bytes instead of 64.
  CHECK_EQ(offsetof(AudioFrameV3, no_samples), size_t{8});
  CHECK_EQ(offsetof(AudioFrameV3, timecode), size_t{16});
  CHECK_EQ(offsetof(VideoFrameV2, frame_format_type), size_t{24});
  CHECK_EQ(offsetof(VideoFrameV2, timecode), size_t{32});
}

/// FourCCs are little-endian packed. If these are byte-reversed the SDK
/// silently treats every frame as an unknown format and sends nothing.
void fourCcsArePackedTheRightWayRound() {
  auto fourCc = [](const char* s) {
    return static_cast<uint32_t>(s[0]) | (static_cast<uint32_t>(s[1]) << 8) |
           (static_cast<uint32_t>(s[2]) << 16) |
           (static_cast<uint32_t>(s[3]) << 24);
  };
  CHECK_EQ(kFourCcUyvy, fourCc("UYVY"));
  CHECK_EQ(kFourCcBgra, fourCc("BGRA"));
  CHECK_EQ(kFourCcBgrx, fourCc("BGRX"));
  CHECK_EQ(kFourCcRgba, fourCc("RGBA"));

  // Lowercase p, and it is not a typo. The SDK declares the audio FourCC as
  // NDI_LIB_FOURCC('F','L','T','p') while every document writes "FLTP". This
  // test was originally written with the obvious spelling and failed, which is
  // exactly the point of checking constants against the real headers instead of
  // against what the documentation calls them.
  CHECK_EQ(kFourCcFltp, fourCc("FLTp"));
  CHECK(kFourCcFltp != fourCc("FLTP"));
}

/// Enum values read off the real SDK. `interleaved` being 0 and `progressive`
/// 1 is the counter-intuitive one — a default-constructed 0 means interlaced,
/// not progressive, so every sender must set it explicitly.
void enumValuesMatchTheSdk() {
  CHECK_EQ(static_cast<int>(kFrameNone), 0);
  CHECK_EQ(static_cast<int>(kFrameVideo), 1);
  CHECK_EQ(static_cast<int>(kFrameAudio), 2);
  CHECK_EQ(static_cast<int>(kFrameError), 4);

  CHECK_EQ(static_cast<int>(kFormatInterleaved), 0);
  CHECK_EQ(static_cast<int>(kFormatProgressive), 1);

  CHECK_EQ(static_cast<int>(kRecvBgrxBgra), 0);
  CHECK_EQ(static_cast<int>(kRecvUyvyBgra), 1);
  CHECK_EQ(static_cast<int>(kBandwidthHighest), 100);
  CHECK_EQ(static_cast<int>(kBandwidthLowest), 0);
}

/// A default-constructed VideoFrameV2 must be safe to hand to the SDK: it is
/// what every send path starts from.
void defaultsAreSane() {
  VideoFrameV2 v;
  CHECK_EQ(v.p_data, static_cast<uint8_t*>(nullptr));
  CHECK_EQ(v.p_metadata, static_cast<const char*>(nullptr));
  CHECK_EQ(static_cast<int>(v.frame_format_type),
           static_cast<int>(kFormatProgressive));

  AudioFrameV3 a;
  CHECK_EQ(a.FourCC, kFourCcFltp);
  CHECK_EQ(a.sample_rate, 48000);
}

/// NDI has no interface-binding parameter anywhere in its C API — not on send,
/// receive or discovery. This constant is what makes the factory warn instead
/// of silently dropping the setting, so it is worth pinning: if someone ever
/// flips it to true, they must have found a real mechanism.
void interfaceBindingIsHonestlyUnsupported() {
  CHECK(!kNdiSupportsInterfaceBinding);
}

/// The runtime is optional. Every one of these must answer without crashing on
/// a machine that has no NDI installed at all — CI is exactly that machine.
void theRuntimeApiIsSafeWithoutARuntime() {
  const bool present = NdiRuntime::available();

  const std::string path = NdiRuntime::loadedPath();
  const std::string why = NdiRuntime::unavailableReason();

  // Exactly one of the two is populated, whichever way it went.
  if (present) {
    CHECK(!path.empty());
    CHECK(why.empty());
  } else {
    CHECK(path.empty());
    CHECK(!why.empty());
    // And the message must tell an operator where to get it.
    CHECK(why.find("ndi.video") != std::string::npos);
  }

  std::string error;
  auto sources = ndiListSources(10, error);
  if (!present) {
    CHECK(sources.empty());
    CHECK(!error.empty());
  }

  CHECK(std::strlen(NdiRuntime::downloadUrl()) > 0);

  std::printf("  NDI runtime: %s\n",
              present ? path.c_str() : "not installed (this is not a failure)");
}

void run() {
  layoutsMatchTheRealSdk();
  fourCcsArePackedTheRightWayRound();
  enumValuesMatchTheSdk();
  defaultsAreSane();
  interfaceBindingIsHonestlyUnsupported();
  theRuntimeApiIsSafeWithoutARuntime();
}

}  // namespace

TEST_MAIN("ndi_abi")

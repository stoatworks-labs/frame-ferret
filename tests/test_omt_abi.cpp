#include "transports/omt_abi.h"

#include <cstring>

#include "check.h"
#include "transports/omt.h"

using namespace ferret;
using namespace ferret::omt_abi;

namespace {

void layoutsMatchTheRealSdk() {
  CHECK_EQ(sizeof(MediaFrame), size_t{112});

  // The padded pairs — an int followed by an 8-aligned member. Getting any of
  // these wrong shifts everything after it.
  CHECK_EQ(offsetof(MediaFrame, Type), size_t{0});
  CHECK_EQ(offsetof(MediaFrame, Timestamp), size_t{8});    // 4 B pad after Type
  CHECK_EQ(offsetof(MediaFrame, Data), size_t{64});
  CHECK_EQ(offsetof(MediaFrame, DataLength), size_t{72});
  CHECK_EQ(offsetof(MediaFrame, CompressedData), size_t{80});   // 4 B pad
  CHECK_EQ(offsetof(MediaFrame, FrameMetadata), size_t{96});    // 4 B pad
  CHECK_EQ(offsetof(MediaFrame, FrameMetadataLength), size_t{104});

  CHECK_EQ(offsetof(MediaFrame, Codec), size_t{16});
  CHECK_EQ(offsetof(MediaFrame, Stride), size_t{28});
  CHECK_EQ(offsetof(MediaFrame, AspectRatio), size_t{44});
  CHECK_EQ(offsetof(MediaFrame, ColorSpace), size_t{48});
  CHECK_EQ(offsetof(MediaFrame, SamplesPerChannel), size_t{60});
}

void codecsArePackedTheRightWayRound() {
  auto fourCc = [](const char* s) {
    return static_cast<int32_t>(static_cast<uint32_t>(s[0]) |
                                (static_cast<uint32_t>(s[1]) << 8) |
                                (static_cast<uint32_t>(s[2]) << 16) |
                                (static_cast<uint32_t>(s[3]) << 24));
  };
  CHECK_EQ(static_cast<int32_t>(kCodecUyvy), fourCc("UYVY"));
  CHECK_EQ(static_cast<int32_t>(kCodecBgra), fourCc("BGRA"));
  CHECK_EQ(static_cast<int32_t>(kCodecNv12), fourCc("NV12"));
  CHECK_EQ(static_cast<int32_t>(kCodecVmx1), fourCc("VMX1"));

  // OMT's audio FourCC really is uppercase 'FPA1' — unlike NDI's 'FLTp', whose
  // lowercase p catches everyone. Both are planar float; only the spelling of
  // the tag differs. Worth asserting side by side so nobody "fixes" one to
  // match the other.
  CHECK_EQ(static_cast<int32_t>(kCodecFpa1), fourCc("FPA1"));
}

/// OMT states colour space explicitly, as the literal numbers 601 and 709.
/// NDI carries none at all. That difference is why the OMT receiver reads the
/// field and the NDI one infers from the raster.
void colourSpaceIsExplicitAndNumeric() {
  CHECK_EQ(static_cast<int>(kColour601), 601);
  CHECK_EQ(static_cast<int>(kColour709), 709);
  CHECK_EQ(static_cast<int>(kColourUndefined), 0);
}

void enumValuesMatchTheSdk() {
  // Frame types are a BITMASK here — 1, 2, 4 — not the sequential 0..4 NDI
  // uses. `kFrameVideo | kFrameAudio` is a meaningful request to omt_receive;
  // the same expression against NDI's enum would be nonsense.
  CHECK_EQ(static_cast<int>(kFrameNone), 0);
  CHECK_EQ(static_cast<int>(kFrameMetadata), 1);
  CHECK_EQ(static_cast<int>(kFrameVideo), 2);
  CHECK_EQ(static_cast<int>(kFrameAudio), 4);
  CHECK_EQ(static_cast<int>(kFrameVideo | kFrameAudio), 6);

  CHECK_EQ(static_cast<int>(kQualityDefault), 0);
  CHECK_EQ(static_cast<int>(kQualityHigh), 100);
  CHECK_EQ(static_cast<int>(kPreferUyvy), 0);
  CHECK_EQ(static_cast<int>(kPreferUyvyOrBgra), 1);

  CHECK_EQ(static_cast<int>(kFlagInterlaced), 1);
  CHECK_EQ(static_cast<int>(kFlagAlpha), 2);
  CHECK_EQ(static_cast<int>(kFlagHighBitDepth), 16);
}

void interfaceBindingIsHonestlyUnsupported() {
  CHECK(!kOmtSupportsInterfaceBinding);
}

/// Everything must answer safely on a machine with no libomt — which is every
/// CI runner, and every Linux machine, since no Linux build of libomt exists.
void theRuntimeApiIsSafeWithoutARuntime() {
  const bool present = OmtRuntime::available();
  const std::string path = OmtRuntime::loadedPath();
  const std::string why = OmtRuntime::unavailableReason();

  if (present) {
    CHECK(!path.empty());
    CHECK(why.empty());
  } else {
    CHECK(path.empty());
    CHECK(!why.empty());
  }

  std::string error;
  auto sources = omtListSources(error);
  if (!present) {
    CHECK(sources.empty());
    CHECK(!error.empty());
  }

  // Nothing has been created yet in this process, so .NET must not have
  // started. This is the flag that decides when signal handlers are safe to
  // install, so it must not be true merely because the library loaded.
  CHECK(!OmtRuntime::dotNetInitialised());

  CHECK(std::strlen(OmtRuntime::downloadUrl()) > 0);

  std::printf("  OMT runtime: %s\n",
              present ? path.c_str() : "not installed (this is not a failure)");
}

void run() {
  layoutsMatchTheRealSdk();
  codecsArePackedTheRightWayRound();
  colourSpaceIsExplicitAndNumeric();
  enumValuesMatchTheSdk();
  interfaceBindingIsHonestlyUnsupported();
  theRuntimeApiIsSafeWithoutARuntime();
}

}  // namespace

TEST_MAIN("omt_abi")

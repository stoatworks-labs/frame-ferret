#include "core/pixel_format.h"

#include "check.h"

using namespace ferret;

namespace {

void stridesMatchTheirPackings() {
  CHECK_EQ(tightStrideBytes(PixelFormat::bgra8, 1920), 1920 * 4);
  CHECK_EQ(tightStrideBytes(PixelFormat::uyvy8, 1920), 1920 * 2);
  CHECK_EQ(tightStrideBytes(PixelFormat::nv12, 1920), 1920);

  // v210: 6 pixels per 16 bytes. 1920 / 6 = 320 groups exactly.
  CHECK_EQ(tightStrideBytes(PixelFormat::v210, 1920), 320 * 16);

  // 2110-20 pgroup: 2 pixels per 5 bytes.
  CHECK_EQ(tightStrideBytes(PixelFormat::ycbcr422_10_pgroup, 1920), 960 * 5);
}

/// The widths that are not a whole number of packing groups. These are where a
/// stride calculation quietly truncates and the last few pixels of every row
/// come out as garbage — a defect that looks like a bad cable, not like maths.
void nonMultipleWidthsRoundUp() {
  // 1918 / 6 = 319.67 → 320 groups.
  CHECK_EQ(tightStrideBytes(PixelFormat::v210, 1918), 320 * 16);
  // 1921 / 6 = 320.17 → 321 groups.
  CHECK_EQ(tightStrideBytes(PixelFormat::v210, 1921), 321 * 16);

  // An odd width is half a pgroup, which still needs the whole 5 bytes.
  CHECK_EQ(tightStrideBytes(PixelFormat::ycbcr422_10_pgroup, 1919), 960 * 5);
}

void degenerateInputsAreZeroNotNegative() {
  CHECK_EQ(tightStrideBytes(PixelFormat::bgra8, 0), 0);
  CHECK_EQ(tightStrideBytes(PixelFormat::bgra8, -1), 0);
  CHECK_EQ(tightStrideBytes(PixelFormat::unknown, 1920), 0);
}

/// describe() is indexed by the enum, so the table and the enum must stay in
/// step. If someone inserts a format mid-enum without touching kInfo, every
/// format after it silently reports the wrong properties — this catches it.
void theDescriptorTableLinesUpWithTheEnum() {
  CHECK_EQ(std::string(describe(PixelFormat::unknown).name), std::string("unknown"));
  CHECK_EQ(std::string(describe(PixelFormat::bgra8).name), std::string("bgra8"));
  CHECK_EQ(std::string(describe(PixelFormat::v210).name), std::string("v210"));
  CHECK_EQ(std::string(describe(PixelFormat::nv12).name), std::string("nv12"));
  CHECK_EQ(std::string(describe(PixelFormat::ycbcr422_10_pgroup).name),
           std::string("ycbcr422_10_pgroup"));
}

void propertiesAreRight() {
  CHECK(describe(PixelFormat::bgra8).hasAlpha);
  CHECK(describe(PixelFormat::bgra8).isRgb);
  CHECK(!describe(PixelFormat::uyvy8).hasAlpha);
  CHECK(!describe(PixelFormat::uyvy8).isRgb);

  CHECK_EQ(describe(PixelFormat::v210).bitsPerComponent, 10);
  CHECK_EQ(describe(PixelFormat::uyvy8).bitsPerComponent, 8);

  // 4:2:0 is the only vertically subsampled format here, and the only one we
  // never route to by choice.
  CHECK_EQ(describe(PixelFormat::nv12).chromaSubV, 2);
  CHECK_EQ(describe(PixelFormat::uyvy8).chromaSubV, 1);
}

void namesRoundTrip() {
  for (int i = 0; i <= static_cast<int>(PixelFormat::ycbcr422_10_pgroup); ++i) {
    auto f = static_cast<PixelFormat>(i);
    CHECK_EQ(pixelFormatFromString(toString(f)), f);
  }
  CHECK_EQ(pixelFormatFromString("nonsense"), PixelFormat::unknown);
}

void run() {
  stridesMatchTheirPackings();
  nonMultipleWidthsRoundUp();
  degenerateInputsAreZeroNotNegative();
  theDescriptorTableLinesUpWithTheEnum();
  propertiesAreRight();
  namesRoundTrip();
}

}  // namespace

TEST_MAIN("pixel_format")

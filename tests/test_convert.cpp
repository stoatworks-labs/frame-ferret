#include "core/convert.h"

#include <cmath>

#include "check.h"
#include "sources/test_pattern.h"

using namespace ferret;

namespace {
// Every conversion now reports the range it produced; most tests do not care
// which, so they share one sink for it.
QuantRange outRange = QuantRange::unknown;
}  // namespace

namespace {

VideoFrame makeBgra(std::vector<uint8_t>& store, int w, int h,
                    QuantRange range = QuantRange::full) {
  store.assign(static_cast<size_t>(w) * h * 4, 0);
  VideoFrame f;
  f.width = w;
  f.height = h;
  f.strideBytes = w * 4;
  f.data = store.data();
  f.format = PixelFormat::bgra8;
  f.colour = ColourSpace::bt709;
  f.range = range;
  return f;
}

void setPixel(std::vector<uint8_t>& store, int w, int x, int y, uint8_t r,
              uint8_t g, uint8_t b) {
  uint8_t* p = store.data() + (static_cast<size_t>(y) * w + x) * 4;
  p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
}

void getPixel(const std::vector<uint8_t>& buf, int stride, int x, int y,
              uint8_t* r, uint8_t* g, uint8_t* b) {
  const uint8_t* p = buf.data() + static_cast<size_t>(y) * stride + x * 4;
  *b = p[0]; *g = p[1]; *r = p[2];
}

/// Grey survives every hop exactly, because it has no chroma to subsample.
/// If this drifts, the luma scaling is wrong, and nothing else in this file
/// will be trustworthy.
void greyIsExactThroughEveryFormat() {
  std::vector<uint8_t> store;
  VideoFrame src = makeBgra(store, 12, 2);
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 12; ++x) setPixel(store, 12, x, y, 128, 128, 128);

  const PixelFormat hops[] = {PixelFormat::uyvy8, PixelFormat::yuy2_8,
                              PixelFormat::v210};
  for (PixelFormat hop : hops) {
    std::vector<uint8_t> mid;
    PixelFormat midFmt;
    int midStride;
    std::string err;
    CHECK(convert(src, hop, midFmt, outRange, mid, midStride, err));

    VideoFrame midFrame = src;
    midFrame.format = midFmt;
    midFrame.range = outRange;
    midFrame.data = mid.data();
    midFrame.strideBytes = midStride;

    std::vector<uint8_t> back;
    PixelFormat backFmt;
    int backStride;
    CHECK(convert(midFrame, PixelFormat::bgra8, backFmt, outRange, back, backStride, err));

    uint8_t r, g, b;
    getPixel(back, backStride, 5, 1, &r, &g, &b);
    // One code value of rounding is the most a correct round trip should cost.
    CHECK(std::abs(r - 128) <= 1);
    CHECK(std::abs(g - 128) <= 1);
    CHECK(std::abs(b - 128) <= 1);
  }
}

/// The real check on the colour maths: every 75% bar through BGRA -> UYVY ->
/// BGRA. Chroma is subsampled 2:1 so the bars must be sampled away from their
/// edges, which is why the pattern is 64 wide and each bar 8 px.
void colourBarsSurviveAYcbcrRoundTrip() {
  const auto& bars = colourBars75();
  const int barW = 8;
  const int w = barW * static_cast<int>(bars.size());

  std::vector<uint8_t> store;
  VideoFrame src = makeBgra(store, w, 2);
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < w; ++x) {
      const auto& c = bars[x / barW];
      setPixel(store, w, x, y, c.r, c.g, c.b);
    }
  }

  std::vector<uint8_t> mid;
  PixelFormat midFmt;
  int midStride;
  std::string err;
  CHECK(convert(src, PixelFormat::uyvy8, midFmt, outRange, mid, midStride, err));

  VideoFrame midFrame = src;
  midFrame.format = midFmt;
  midFrame.range = outRange;
  midFrame.data = mid.data();
  midFrame.strideBytes = midStride;

  std::vector<uint8_t> back;
  PixelFormat backFmt;
  int backStride;
  CHECK(convert(midFrame, PixelFormat::bgra8, backFmt, outRange, back, backStride, err));

  for (size_t i = 0; i < bars.size(); ++i) {
    const int x = static_cast<int>(i) * barW + barW / 2;  // centre of the bar
    uint8_t r, g, b;
    getPixel(back, backStride, x, 1, &r, &g, &b);
    // 8-bit 4:2:2 through two matrix multiplications: 2 code values is a fair
    // budget, and a channel swap or a wrong matrix blows straight past it.
    CHECK(std::abs(r - bars[i].r) <= 2);
    CHECK(std::abs(g - bars[i].g) <= 2);
    CHECK(std::abs(b - bars[i].b) <= 2);
  }
}

/// Red must come back red, not blue. The single most common bug in this kind
/// of code is a channel swap, and it survives a grey test untouched.
void channelsAreNotSwapped() {
  std::vector<uint8_t> store;
  VideoFrame src = makeBgra(store, 8, 2);
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 8; ++x) setPixel(store, 8, x, y, 255, 0, 0);  // red

  std::vector<uint8_t> mid, back;
  PixelFormat midFmt, backFmt;
  int midStride, backStride;
  std::string err;
  CHECK(convert(src, PixelFormat::uyvy8, midFmt, outRange, mid, midStride, err));

  VideoFrame m = src;
  m.format = midFmt;
  m.range = outRange; m.data = mid.data(); m.strideBytes = midStride;
  CHECK(convert(m, PixelFormat::bgra8, backFmt, outRange, back, backStride, err));

  uint8_t r, g, b;
  getPixel(back, backStride, 4, 1, &r, &g, &b);
  CHECK(r > 240);
  CHECK(g < 15);
  CHECK(b < 15);

  // And BGRA vs RGBA really differ. If these agree, one of them is ignoring
  // its byte order.
  std::vector<uint8_t> asRgba;
  PixelFormat rgbaFmt;
  int rgbaStride;
  CHECK(convert(m, PixelFormat::rgba8, rgbaFmt, outRange, asRgba, rgbaStride, err));
  CHECK(asRgba[0] != back[0]);  // R in the low byte vs B in the low byte
}

/// v210 is 10-bit, so a value that cannot be expressed in 8 bits must survive
/// a v210 round trip that an 8-bit hop would destroy.
void v210CarriesMoreThanEightBits() {
  std::vector<uint8_t> store;
  VideoFrame src = makeBgra(store, 12, 2);
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 12; ++x) setPixel(store, 12, x, y, 100, 150, 200);

  std::vector<uint8_t> v;
  PixelFormat vf;
  int vs;
  std::string err;
  CHECK(convert(src, PixelFormat::v210, vf, outRange, v, vs, err));

  // The packing: 6 pixels in 16 bytes, so 12 pixels is exactly 32 bytes.
  CHECK_EQ(vs, 32);

  VideoFrame m = src;
  m.format = vf;
  m.range = outRange; m.data = v.data(); m.strideBytes = vs;

  std::vector<uint8_t> back;
  PixelFormat bf;
  int bs;
  CHECK(convert(m, PixelFormat::bgra8, bf, outRange, back, bs, err));

  uint8_t r, g, b;
  getPixel(back, bs, 6, 1, &r, &g, &b);
  CHECK(std::abs(r - 100) <= 1);
  CHECK(std::abs(g - 150) <= 1);
  CHECK(std::abs(b - 200) <= 1);
}

/// uyvy and yuy2 are the same data in a different byte order, so a round trip
/// through the pair must be lossless — not merely close.
void uyvyAndYuy2AreTheSameDataReordered() {
  std::vector<uint8_t> store;
  VideoFrame src = makeBgra(store, 8, 2);
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 8; ++x) setPixel(store, 8, x, y, 30, 200, 90);

  std::string err;
  std::vector<uint8_t> u;
  PixelFormat uf;
  int us;
  CHECK(convert(src, PixelFormat::uyvy8, uf, outRange, u, us, err));

  VideoFrame uframe = src;
  uframe.format = uf;
  uframe.range = outRange; uframe.data = u.data(); uframe.strideBytes = us;

  std::vector<uint8_t> y2;
  PixelFormat yf;
  int ys;
  CHECK(convert(uframe, PixelFormat::yuy2_8, yf, outRange, y2, ys, err));

  // Same length, and every 4-byte group is a rotation of the other.
  CHECK_EQ(u.size(), y2.size());
  CHECK_EQ(u[0], y2[1]);  // Cb
  CHECK_EQ(u[1], y2[0]);  // Y0
  CHECK_EQ(u[2], y2[3]);  // Cr
  CHECK_EQ(u[3], y2[2]);  // Y1

  VideoFrame yframe = src;
  yframe.format = yf;
  yframe.range = outRange; yframe.data = y2.data(); yframe.strideBytes = ys;

  std::vector<uint8_t> backToU;
  PixelFormat buf;
  int bus;
  CHECK(convert(yframe, PixelFormat::uyvy8, buf, outRange, backToU, bus, err));
  CHECK(backToU == u);  // exactly, no tolerance
}

/// An RGB source always encodes to NARROW YCbCr, whatever range it declares.
///
/// This is the rule, not an implementation detail, and it changed after a real
/// bug: colour bars generated as full-range RGB were encoded as full-range
/// YCbCr and sent over SRT, where the receiver expanded them a second time.
/// Green came back at 240 instead of 191 — worst on the channels carrying the
/// most luma weight — while hues and bar order stayed perfect, which is exactly
/// what makes a range fault easy to misread as "nearly working".
///
/// Every transport here (NDI, OMT, SRT, DeckLink) carries narrow YCbCr by
/// convention, so this is where that convention is enforced.
void rgbAlwaysEncodesToNarrowYcbcr() {
  std::vector<uint8_t> fullStore, narrowStore;
  VideoFrame fullSrc = makeBgra(fullStore, 8, 2, QuantRange::full);
  VideoFrame narrowSrc = makeBgra(narrowStore, 8, 2, QuantRange::narrow);
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 8; ++x) {
      setPixel(fullStore, 8, x, y, 0, 0, 0);
      setPixel(narrowStore, 8, x, y, 0, 0, 0);
    }
  }

  std::string err;
  std::vector<uint8_t> a, b;
  PixelFormat af, bf;
  QuantRange aRange = QuantRange::unknown, bRange = QuantRange::unknown;
  int as, bs;
  CHECK(convert(fullSrc, PixelFormat::uyvy8, af, aRange, a, as, err));
  CHECK(convert(narrowSrc, PixelFormat::uyvy8, bf, bRange, b, bs, err));

  // Black encodes to legal black either way, and the reported range says so.
  CHECK_EQ(a[1], uint8_t{16});
  CHECK_EQ(b[1], uint8_t{16});
  CHECK_EQ(static_cast<int>(aRange), static_cast<int>(QuantRange::narrow));
  CHECK_EQ(static_cast<int>(bRange), static_cast<int>(QuantRange::narrow));
  // Chroma is centred regardless.
  CHECK_EQ(a[0], uint8_t{128});
}

/// Going the other way, an RGB destination is always full range — that is the
/// convention the shared-surface and preview paths expect.
void ycbcrToRgbIsAlwaysFullRange() {
  std::vector<uint8_t> store;
  VideoFrame src = makeBgra(store, 8, 2, QuantRange::full);
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 8; ++x) setPixel(store, 8, x, y, 0, 0, 0);

  std::string err;
  std::vector<uint8_t> mid;
  PixelFormat midFmt;
  QuantRange midRange = QuantRange::unknown;
  int midStride;
  CHECK(convert(src, PixelFormat::uyvy8, midFmt, midRange, mid, midStride, err));

  VideoFrame m = src;
  m.format = midFmt;
  m.range = midRange;
  m.data = mid.data();
  m.strideBytes = midStride;

  std::vector<uint8_t> back;
  PixelFormat backFmt;
  QuantRange backRange = QuantRange::unknown;
  int backStride;
  CHECK(convert(m, PixelFormat::bgra8, backFmt, backRange, back, backStride, err));
  CHECK_EQ(static_cast<int>(backRange), static_cast<int>(QuantRange::full));

  // And black really does come back as black, not as 16.
  CHECK(back[0] <= 2);
  CHECK(back[1] <= 2);
  CHECK(back[2] <= 2);
}

/// 601 and 709 have different luma coefficients, so a saturated colour must
/// encode differently under each. Grey would not show this.
void colourSpacesAreNotInterchangeable() {
  std::vector<uint8_t> store;
  VideoFrame src = makeBgra(store, 8, 2);
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 8; ++x) setPixel(store, 8, x, y, 0, 255, 0);  // green

  std::string err;
  src.colour = ColourSpace::bt709;
  std::vector<uint8_t> a;
  PixelFormat af;
  int as;
  CHECK(convert(src, PixelFormat::uyvy8, af, outRange, a, as, err));

  src.colour = ColourSpace::bt601;
  std::vector<uint8_t> b;
  PixelFormat bf;
  int bs;
  CHECK(convert(src, PixelFormat::uyvy8, bf, outRange, b, bs, err));

  // Green's luma is 0.7152 under 709 and 0.587 under 601 — a large gap.
  CHECK(a[1] != b[1]);
  CHECK(a[1] > b[1]);
}

void oddWidthsDoNotOverrun() {
  // 7 is neither a multiple of 2 (chroma pairs) nor of 6 (v210 groups).
  std::vector<uint8_t> store;
  VideoFrame src = makeBgra(store, 7, 3);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 7; ++x) setPixel(store, 7, x, y, 10, 20, 30);

  std::string err;
  for (PixelFormat to : {PixelFormat::uyvy8, PixelFormat::yuy2_8,
                         PixelFormat::v210}) {
    std::vector<uint8_t> out;
    PixelFormat of;
    int os;
    CHECK(convert(src, to, of, outRange, out, os, err));
    CHECK_EQ(out.size(), static_cast<size_t>(os) * 3);
    CHECK_EQ(os, tightStrideBytes(to, 7));
  }
}

void unsupportedPairsFailLoudly() {
  std::vector<uint8_t> store;
  VideoFrame src = makeBgra(store, 8, 2);

  std::vector<uint8_t> out;
  PixelFormat of;
  int os;
  std::string err;
  // nv12 is 4:2:0 planar and deliberately not a destination this module
  // produces — routing to it would throw away vertical chroma nothing
  // downstream can recover.
  CHECK(!convert(src, PixelFormat::nv12, of, outRange, out, os, err));
  CHECK(!err.empty());

  // The 2110 pgroup, by contrast, IS supported now — it is the wire format of
  // ST 2110-20.
  CHECK(convert(src, PixelFormat::ycbcr422_10_pgroup, of, outRange, out, os, err));

  err.clear();
  VideoFrame empty;
  CHECK(!convert(empty, PixelFormat::uyvy8, of, outRange, out, os, err));
  CHECK(err.find("empty") != std::string::npos);
}

/// Black must be *legal* black, not all-zero. All-zero YCbCr is superblack,
/// which is illegal on SDI and clips on a conformant receiver — and this runs
/// on every unrouted output, so it is not a rare path.
void blackIsLegalBlackNotZero() {
  std::vector<uint8_t> buf;

  const int stride8 = tightStrideBytes(PixelFormat::uyvy8, 8);
  buf.assign(static_cast<size_t>(stride8) * 2, 0xAB);
  fillBlack(PixelFormat::uyvy8, 8, 2, stride8, QuantRange::narrow, buf.data());
  CHECK_EQ(buf[0], uint8_t{128});  // Cb
  CHECK_EQ(buf[1], uint8_t{16});   // Y — not 0
  CHECK_EQ(buf[2], uint8_t{128});  // Cr
  CHECK_EQ(buf[3], uint8_t{16});

  // Full range black really is Y=0.
  fillBlack(PixelFormat::uyvy8, 8, 2, stride8, QuantRange::full, buf.data());
  CHECK_EQ(buf[1], uint8_t{0});
  CHECK_EQ(buf[0], uint8_t{128});  // chroma stays centred either way

  // RGB black is zero, but opaque — a transparent black would key out.
  const int strideRgb = tightStrideBytes(PixelFormat::bgra8, 8);
  buf.assign(static_cast<size_t>(strideRgb) * 2, 0xAB);
  fillBlack(PixelFormat::bgra8, 8, 2, strideRgb, QuantRange::full, buf.data());
  CHECK_EQ(buf[0], uint8_t{0});
  CHECK_EQ(buf[3], uint8_t{255});

  // v210 black decodes back to black rather than to noise.
  const int strideV = tightStrideBytes(PixelFormat::v210, 12);
  buf.assign(static_cast<size_t>(strideV) * 2, 0xAB);
  fillBlack(PixelFormat::v210, 12, 2, strideV, QuantRange::narrow, buf.data());

  VideoFrame vf;
  vf.width = 12; vf.height = 2; vf.strideBytes = strideV;
  vf.data = buf.data(); vf.format = PixelFormat::v210;
  vf.colour = ColourSpace::bt709; vf.range = QuantRange::narrow;

  std::vector<uint8_t> rgb;
  PixelFormat rf;
  int rs;
  std::string err;
  CHECK(convert(vf, PixelFormat::bgra8, rf, outRange, rgb, rs, err));
  CHECK(rgb[0] <= 2);
  CHECK(rgb[1] <= 2);
  CHECK(rgb[2] <= 2);
}

void canConvertMatchesWhatConvertActuallyDoes() {
  const PixelFormat all[] = {PixelFormat::bgra8, PixelFormat::rgba8,
                             PixelFormat::uyvy8, PixelFormat::yuy2_8,
                             PixelFormat::v210, PixelFormat::nv12,
                             PixelFormat::ycbcr422_10_pgroup};
  std::vector<uint8_t> store;
  VideoFrame src = makeBgra(store, 12, 2);

  for (PixelFormat to : all) {
    std::vector<uint8_t> out;
    PixelFormat of;
    int os;
    std::string err;
    const bool claimed = canConvert(PixelFormat::bgra8, to);
    const bool actual = convert(src, to, of, outRange, out, os, err);
    // The router trusts canConvert when it plans a `convert` action, so a
    // disagreement here is a route that is planned and then fails at runtime.
    CHECK_EQ(claimed, actual);
  }
}

void run() {
  greyIsExactThroughEveryFormat();
  colourBarsSurviveAYcbcrRoundTrip();
  channelsAreNotSwapped();
  v210CarriesMoreThanEightBits();
  uyvyAndYuy2AreTheSameDataReordered();
  rgbAlwaysEncodesToNarrowYcbcr();
  ycbcrToRgbIsAlwaysFullRange();
  colourSpacesAreNotInterchangeable();
  oddWidthsDoNotOverrun();
  unsupportedPairsFailLoudly();
  blackIsLegalBlackNotZero();
  canConvertMatchesWhatConvertActuallyDoes();
}

}  // namespace

TEST_MAIN("convert")

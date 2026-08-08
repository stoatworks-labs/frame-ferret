#include "core/convert.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ferret {
namespace {

inline uint8_t clamp8(double v) {
  return static_cast<uint8_t>(std::lround(std::min(255.0, std::max(0.0, v))));
}

inline uint16_t clamp10(double v) {
  return static_cast<uint16_t>(std::lround(std::min(1023.0, std::max(0.0, v))));
}

/// Scaling from normalised Y'CbCr into integer code values.
struct Scaling {
  double yOffset, yScale, cOffset, cScale;
};

Scaling scalingFor(QuantRange range, int bits) {
  const double max = (1 << bits) - 1;          // 255 or 1023
  const double unit = static_cast<double>(1 << (bits - 8));  // 1 or 4
  if (range == QuantRange::full) {
    return {0.0, max, 128.0 * unit, max};
  }
  // Narrow: Y spans 16..235, C spans 16..240 centred on 128, scaled by bits.
  return {16.0 * unit, 219.0 * unit, 128.0 * unit, 224.0 * unit};
}

/// Fixed-point coefficients for one (colour space, range) pair, computed once
/// per convert() call.
///
/// The first cut of this file did the whole thing in `double`, normalising to
/// 0..1 and back with std::lround per component. It was correct and it cost
/// **29.7 ms for a 1280x720 UYVY->BGRA frame** — a 33.6 fps ceiling, measured,
/// which showed up as an OMT receiver stuck at 19 fps with every tick late.
/// Conversion is on the critical path for every route the router marks
/// `convert`, so it has to be integer.
///
/// 16 fractional bits: enough that the round trip stays inside the 2-code-value
/// tolerance the colour-bar test asserts, and small enough that intermediates
/// stay well inside int32 for 10-bit inputs.
struct FixedCoeffs {
  int32_t yGain;    // (y - yOffset) -> 8-bit luma contribution
  int32_t crToR;
  int32_t cbToB;
  int32_t cbToG;
  int32_t crToG;
  int32_t yOffset;
  int32_t cOffset;

  // Forward direction (RGB -> YCbCr), 8-bit in, 10-bit out.
  //
  // Fully folded: each output component is one dot product of the 8-bit RGB
  // triple, with the luma weights, the range scaling and the chroma
  // normalisation all multiplied together in advance. The first integer version
  // did it in two stages — weights, then scale — with an int64 intermediate per
  // pixel, and that cost 26.6 ms for a 1920x1080 BGRA->v210 frame: a 38 fps
  // ceiling, which showed up as a DeckLink output running at 34 fps with a
  // third of its ticks late.
  //
  // Range: an 8-bit channel times a folded coefficient is at most ~1.3e7, and
  // three of those sum well inside int32. No 64-bit arithmetic is needed.
  int32_t yR, yG, yB, yOffsetFwd;
  int32_t cbR, cbG, cbB;
  int32_t crR, crG, crB;
  int32_t cOffsetFwd;
};

constexpr int kShift = 16;
constexpr int32_t kOne = 1 << kShift;

FixedCoeffs makeCoeffs(const LumaCoefficients& k, const Scaling& s) {
  FixedCoeffs c{};
  const double toByte = 255.0;

  c.yOffset = static_cast<int32_t>(s.yOffset);
  c.cOffset = static_cast<int32_t>(s.cOffset);

  c.yGain = static_cast<int32_t>((toByte / s.yScale) * kOne);
  c.crToR = static_cast<int32_t>((toByte * 2.0 * (1.0 - k.kr) / s.cScale) * kOne);
  c.cbToB = static_cast<int32_t>((toByte * 2.0 * (1.0 - k.kb) / s.cScale) * kOne);
  c.cbToG = static_cast<int32_t>(
      (toByte * 2.0 * (1.0 - k.kb) * k.kb / (k.kg() * s.cScale)) * kOne);
  c.crToG = static_cast<int32_t>(
      (toByte * 2.0 * (1.0 - k.kr) * k.kr / (k.kg() * s.cScale)) * kOne);

  // Luma: weights times the range scale, in one coefficient each.
  // std::lround, not a cast. A cast truncates toward zero, so every negative
  // chroma coefficient came out one step short and the round trip drifted past
  // the 2-code-value budget the colour-bar test holds.
  auto fixed = [](double v) { return static_cast<int32_t>(std::lround(v * kOne)); };

  const double yGainFwd = s.yScale / toByte;
  c.yR = fixed(k.kr * yGainFwd);
  c.yG = fixed(k.kg() * yGainFwd);
  c.yB = fixed(k.kb * yGainFwd);
  c.yOffsetFwd = static_cast<int32_t>(s.yOffset);

  // Chroma: (B - Y) and (R - Y) expanded back into R, G and B terms, so each
  // is also a single dot product rather than a subtraction of a separate luma.
  const double cbGain = s.cScale / (2.0 * (1.0 - k.kb) * toByte);
  c.cbR = fixed(-k.kr * cbGain);
  c.cbG = fixed(-k.kg() * cbGain);
  c.cbB = fixed((1.0 - k.kb) * cbGain);

  const double crGain = s.cScale / (2.0 * (1.0 - k.kr) * toByte);
  c.crR = fixed((1.0 - k.kr) * crGain);
  c.crG = fixed(-k.kg() * crGain);
  c.crB = fixed(-k.kb * crGain);
  c.cOffsetFwd = static_cast<int32_t>(s.cOffset);
  return c;
}

inline uint8_t clampByte(int32_t v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

struct Rgb {
  double r, g, b;  // 0..1
};
struct Ycc {
  double y, cb, cr;  // y 0..1, cb/cr -0.5..0.5
};

Ycc rgbToYcc(const Rgb& p, const LumaCoefficients& k) {
  const double y = k.kr * p.r + k.kg() * p.g + k.kb * p.b;
  return {y, (p.b - y) / (2.0 * (1.0 - k.kb)),
          (p.r - y) / (2.0 * (1.0 - k.kr))};
}

Rgb yccToRgb(const Ycc& c, const LumaCoefficients& k) {
  const double r = c.y + 2.0 * (1.0 - k.kr) * c.cr;
  const double b = c.y + 2.0 * (1.0 - k.kb) * c.cb;
  const double g = (c.y - k.kr * r - k.kb * b) / k.kg();
  return {r, g, b};
}

// --- v210 -------------------------------------------------------------------
//
// 6 pixels per 16 bytes, as four little-endian 32-bit words, 10 bits each with
// the top 2 bits of each word unused:
//
//   word 0:  Cb0 | Y0  | Cr0
//   word 1:  Y1  | Cb2 | Y2
//   word 2:  Cr2 | Y3  | Cb4
//   word 3:  Y4  | Cr4 | Y5
//
// This is a *different* packing from the ST 2110-20 pgroup (2 pixels / 5 bytes,
// big-endian). Confusing the two gives correct geometry and wrong colour.

inline uint32_t pack3(uint16_t a, uint16_t b, uint16_t c) {
  return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 10) |
         (static_cast<uint32_t>(c) << 20);
}

inline void unpack3(uint32_t w, uint16_t* a, uint16_t* b, uint16_t* c) {
  *a = w & 0x3FF;
  *b = (w >> 10) & 0x3FF;
  *c = (w >> 20) & 0x3FF;
}

/// One row of 4:2:2 as separate 10-bit planes — the common currency every
/// conversion in this file passes through. Keeping one intermediate means each
/// new format needs a reader and a writer, not a pair with every other format.
struct Row422 {
  std::vector<uint16_t> y;   // width entries
  std::vector<uint16_t> cb;  // (width + 1) / 2 entries
  std::vector<uint16_t> cr;
};

void ensureRow(Row422& r, int width) {
  const size_t chroma = static_cast<size_t>((width + 1) / 2);
  if (r.y.size() != static_cast<size_t>(width)) r.y.resize(width);
  if (r.cb.size() != chroma) {
    r.cb.resize(chroma);
    r.cr.resize(chroma);
  }
}

// --- readers: source row -> Row422 at 10 bits -------------------------------

void readPacked8(const uint8_t* src, int width, bool uyvy, Row422& out) {
  for (int x = 0; x < width; x += 2) {
    const uint8_t* p = src + static_cast<size_t>(x) * 2;
    const int c = x / 2;
    if (uyvy) {
      out.cb[c] = static_cast<uint16_t>(p[0] << 2);
      out.y[x] = static_cast<uint16_t>(p[1] << 2);
      out.cr[c] = static_cast<uint16_t>(p[2] << 2);
      if (x + 1 < width) out.y[x + 1] = static_cast<uint16_t>(p[3] << 2);
    } else {  // yuy2
      out.y[x] = static_cast<uint16_t>(p[0] << 2);
      out.cb[c] = static_cast<uint16_t>(p[1] << 2);
      if (x + 1 < width) out.y[x + 1] = static_cast<uint16_t>(p[2] << 2);
      out.cr[c] = static_cast<uint16_t>(p[3] << 2);
    }
  }
}

void readV210(const uint8_t* src, int width, Row422& out) {
  const int groups = (width + 5) / 6;
  for (int g = 0; g < groups; ++g) {
    uint32_t w[4];
    std::memcpy(w, src + static_cast<size_t>(g) * 16, 16);

    uint16_t v[12];
    unpack3(w[0], &v[0], &v[1], &v[2]);   // Cb0 Y0 Cr0
    unpack3(w[1], &v[3], &v[4], &v[5]);   // Y1  Cb2 Y2
    unpack3(w[2], &v[6], &v[7], &v[8]);   // Cr2 Y3  Cb4
    unpack3(w[3], &v[9], &v[10], &v[11]); // Y4  Cr4 Y5

    const int base = g * 6;
    // Named explicitly rather than indexed through a table: this mapping is the
    // easiest thing in the file to get wrong, and a table hides it.
    const uint16_t y0 = v[1], y1 = v[3], y2 = v[5], y3 = v[7], y4 = v[9],
                   y5 = v[11];
    const uint16_t cb0 = v[0], cr0 = v[2], cb2 = v[4], cr2 = v[6], cb4 = v[8],
                   cr4 = v[10];

    const uint16_t yv[6] = {y0, y1, y2, y3, y4, y5};
    for (int i = 0; i < 6; ++i) {
      const int x = base + i;
      if (x < width) out.y[x] = yv[i];
    }
    const uint16_t cbv[3] = {cb0, cb2, cb4};
    const uint16_t crv[3] = {cr0, cr2, cr4};
    for (int i = 0; i < 3; ++i) {
      const int c = base / 2 + i;
      if (c < static_cast<int>(out.cb.size())) {
        out.cb[c] = cbv[i];
        out.cr[c] = crv[i];
      }
    }
  }
}

/// BGRA/RGBA straight to v210, with no Row422 in between.
///
/// A deliberate exception to this file's one-intermediate rule, and the comment
/// on Row422 explains why that rule exists — so here is why it is broken here.
/// This is the hot path: a screen capture or a generated pattern going to SDI.
/// Via the intermediate it cost 23.4 ms for a 1920x1080 frame, a 43 fps ceiling
/// on a 50 fps output, and the card ran at 39 fps with half its ticks late.
/// Most of that was not arithmetic but traffic: three uint16 arrays written and
/// read back per row, about 8 MB each way per frame.
///
/// Only this one pair is fused. Everything else still goes through Row422,
/// because everything else is either rare or already fast enough.
void fusedRgbToV210(const uint8_t* src, int width, bool bgra,
                    const FixedCoeffs& c, uint8_t* dst) {
  constexpr int32_t kHalf = 1 << (kShift - 1);
  constexpr int32_t kHalfPair = 1 << kShift;
  const int groups = (width + 5) / 6;

  for (int g = 0; g < groups; ++g) {
    const int base = g * 6;
    uint16_t y[6];
    uint16_t cb[3];
    uint16_t cr[3];

    for (int pair = 0; pair < 3; ++pair) {
      const int x0 = base + pair * 2;
      // Clamp to the last real pixel so a width that is not a multiple of six
      // repeats its edge rather than reading past the row.
      const int i0 = x0 < width ? x0 : width - 1;
      const int i1 = (x0 + 1) < width ? (x0 + 1) : width - 1;

      const uint8_t* p0 = src + static_cast<size_t>(i0) * 4;
      const uint8_t* p1 = src + static_cast<size_t>(i1) * 4;
      const int32_t r0 = bgra ? p0[2] : p0[0], g0 = p0[1],
                    b0 = bgra ? p0[0] : p0[2];
      const int32_t r1 = bgra ? p1[2] : p1[0], g1 = p1[1],
                    b1 = bgra ? p1[0] : p1[2];

      y[pair * 2] = static_cast<uint16_t>(
          c.yOffsetFwd + ((c.yR * r0 + c.yG * g0 + c.yB * b0 + kHalf) >> kShift));
      y[pair * 2 + 1] = static_cast<uint16_t>(
          c.yOffsetFwd + ((c.yR * r1 + c.yG * g1 + c.yB * b1 + kHalf) >> kShift));

      const int32_t rs = r0 + r1, gs = g0 + g1, bs = b0 + b1;
      int32_t u = c.cOffsetFwd +
                  ((c.cbR * rs + c.cbG * gs + c.cbB * bs + kHalfPair) >>
                   (kShift + 1));
      int32_t v = c.cOffsetFwd +
                  ((c.crR * rs + c.crG * gs + c.crB * bs + kHalfPair) >>
                   (kShift + 1));
      cb[pair] = static_cast<uint16_t>(u < 0 ? 0 : (u > 1023 ? 1023 : u));
      cr[pair] = static_cast<uint16_t>(v < 0 ? 0 : (v > 1023 ? 1023 : v));
    }

    uint32_t w[4];
    w[0] = pack3(cb[0], y[0], cr[0]);
    w[1] = pack3(y[1], cb[1], y[2]);
    w[2] = pack3(cr[1], y[3], cb[2]);
    w[3] = pack3(y[4], cr[2], y[5]);
    std::memcpy(dst + static_cast<size_t>(g) * 16, w, 16);
  }
}

void readBgraLike(const uint8_t* src, int width, bool bgra,
                  const FixedCoeffs& c, Row422& out) {
  // Chroma is averaged across each pair. The colour transform is linear, so
  // averaging RGB and converting once is identical to converting twice and
  // averaging — and it halves the chroma work. Dropping the odd pixel's chroma
  // instead would be cheaper still and visibly worse on saturated vertical
  // edges, which is exactly what test patterns are made of.
  constexpr int32_t kHalf = 1 << (kShift - 1);

  for (int x = 0; x < width; x += 2) {
    const uint8_t* p0 = src + static_cast<size_t>(x) * 4;
    const int32_t r0 = bgra ? p0[2] : p0[0];
    const int32_t g0 = p0[1];
    const int32_t b0 = bgra ? p0[0] : p0[2];

    out.y[x] = static_cast<uint16_t>(
        c.yOffsetFwd + ((c.yR * r0 + c.yG * g0 + c.yB * b0 + kHalf) >> kShift));

    int32_t rSum = r0, gSum = g0, bSum = b0;
    if (x + 1 < width) {
      const uint8_t* p1 = src + static_cast<size_t>(x + 1) * 4;
      const int32_t r1 = bgra ? p1[2] : p1[0];
      const int32_t g1 = p1[1];
      const int32_t b1 = bgra ? p1[0] : p1[2];
      out.y[x + 1] = static_cast<uint16_t>(
          c.yOffsetFwd +
          ((c.yR * r1 + c.yG * g1 + c.yB * b1 + kHalf) >> kShift));
      rSum += r1;
      gSum += g1;
      bSum += b1;
    } else {
      // Odd width: the last pixel stands in for the pair.
      rSum += r0;
      gSum += g0;
      bSum += b0;
    }

    // Sums are of two pixels, so shift one extra bit to divide by two.
    // Rounding constant matches the shift actually used — half of 2^(kShift+1),
    // not of 2^kShift.
    constexpr int32_t kHalfPair = 1 << kShift;
    const int32_t cb =
        c.cOffsetFwd + ((c.cbR * rSum + c.cbG * gSum + c.cbB * bSum +
                         kHalfPair) >> (kShift + 1));
    const int32_t cr =
        c.cOffsetFwd + ((c.crR * rSum + c.crG * gSum + c.crB * bSum +
                         kHalfPair) >> (kShift + 1));

    const int cIdx = x / 2;
    out.cb[cIdx] = static_cast<uint16_t>(cb < 0 ? 0 : (cb > 1023 ? 1023 : cb));
    out.cr[cIdx] = static_cast<uint16_t>(cr < 0 ? 0 : (cr > 1023 ? 1023 : cr));
  }
}

void readBgraLikeSlow(const uint8_t* src, int width, bool bgra,
                      const LumaCoefficients& k, const Scaling& s,
                      Row422& out) {
  // Chroma is averaged across each pair before subsampling. Dropping the odd
  // pixel's chroma instead is cheaper and visibly worse on saturated vertical
  // edges, which is exactly what test patterns are made of.
  for (int x = 0; x < width; x += 2) {
    double cbSum = 0, crSum = 0;
    int n = 0;
    for (int i = 0; i < 2 && x + i < width; ++i) {
      const uint8_t* p = src + static_cast<size_t>(x + i) * 4;
      const Rgb rgb = bgra ? Rgb{p[2] / 255.0, p[1] / 255.0, p[0] / 255.0}
                           : Rgb{p[0] / 255.0, p[1] / 255.0, p[2] / 255.0};
      const Ycc c = rgbToYcc(rgb, k);
      out.y[x + i] = clamp10(s.yOffset + c.y * s.yScale);
      cbSum += c.cb;
      crSum += c.cr;
      ++n;
    }
    const int c = x / 2;
    out.cb[c] = clamp10(s.cOffset + (cbSum / n) * s.cScale);
    out.cr[c] = clamp10(s.cOffset + (crSum / n) * s.cScale);
  }
}

// --- writers: Row422 at 10 bits -> destination row --------------------------

void writePacked8(const Row422& in, int width, bool uyvy, uint8_t* dst) {
  for (int x = 0; x < width; x += 2) {
    uint8_t* p = dst + static_cast<size_t>(x) * 2;
    const int c = x / 2;
    const uint8_t y0 = static_cast<uint8_t>(in.y[x] >> 2);
    const uint8_t y1 =
        static_cast<uint8_t>((x + 1 < width ? in.y[x + 1] : in.y[x]) >> 2);
    const uint8_t cb = static_cast<uint8_t>(in.cb[c] >> 2);
    const uint8_t cr = static_cast<uint8_t>(in.cr[c] >> 2);
    if (uyvy) {
      p[0] = cb; p[1] = y0; p[2] = cr; p[3] = y1;
    } else {
      p[0] = y0; p[1] = cb; p[2] = y1; p[3] = cr;
    }
  }
}

void writeV210(const Row422& in, int width, uint8_t* dst) {
  const int groups = (width + 5) / 6;
  const int chromaCount = static_cast<int>(in.cb.size());
  for (int g = 0; g < groups; ++g) {
    const int base = g * 6;
    auto Y = [&](int i) -> uint16_t {
      const int x = base + i;
      return x < width ? in.y[x] : 0;
    };
    auto CB = [&](int i) -> uint16_t {
      const int c = base / 2 + i;
      return c < chromaCount ? in.cb[c] : 0;
    };
    auto CR = [&](int i) -> uint16_t {
      const int c = base / 2 + i;
      return c < chromaCount ? in.cr[c] : 0;
    };

    uint32_t w[4];
    w[0] = pack3(CB(0), Y(0), CR(0));
    w[1] = pack3(Y(1), CB(1), Y(2));
    w[2] = pack3(CR(1), Y(3), CB(2));
    w[3] = pack3(Y(4), CR(2), Y(5));
    std::memcpy(dst + static_cast<size_t>(g) * 16, w, 16);
  }
}

void writeBgraLike(const Row422& in, int width, bool bgra,
                   const FixedCoeffs& c, uint8_t* dst) {
  for (int x = 0; x < width; ++x) {
    const int ci = x / 2;
    const int32_t y = (static_cast<int32_t>(in.y[x]) - c.yOffset) * c.yGain;
    const int32_t cb = static_cast<int32_t>(in.cb[ci]) - c.cOffset;
    const int32_t cr = static_cast<int32_t>(in.cr[ci]) - c.cOffset;

    // + half an LSB so this rounds rather than truncates; without it every
    // channel drifts consistently downwards through a round trip.
    constexpr int32_t kHalf = 1 << (kShift - 1);
    const uint8_t r = clampByte((y + cr * c.crToR + kHalf) >> kShift);
    const uint8_t g =
        clampByte((y - cb * c.cbToG - cr * c.crToG + kHalf) >> kShift);
    const uint8_t b = clampByte((y + cb * c.cbToB + kHalf) >> kShift);

    uint8_t* p = dst + static_cast<size_t>(x) * 4;
    if (bgra) {
      p[0] = b; p[1] = g; p[2] = r;
    } else {
      p[0] = r; p[1] = g; p[2] = b;
    }
    p[3] = 255;
  }
}

bool isPacked8(PixelFormat f) {
  return f == PixelFormat::uyvy8 || f == PixelFormat::yuy2_8;
}
bool isRgb32(PixelFormat f) {
  return f == PixelFormat::bgra8 || f == PixelFormat::rgba8;
}

}  // namespace

LumaCoefficients coefficientsFor(ColourSpace c) {
  switch (c) {
    case ColourSpace::bt601: return {0.299, 0.114};
    case ColourSpace::bt2020: return {0.2627, 0.0593};
    case ColourSpace::bt709:
    case ColourSpace::unknown:
    default: return {0.2126, 0.0722};
  }
}

bool canConvert(PixelFormat from, PixelFormat to) {
  auto ok = [](PixelFormat f) {
    return isRgb32(f) || isPacked8(f) || f == PixelFormat::v210;
  };
  return ok(from) && ok(to);
}

bool convert(const VideoFrame& src, PixelFormat to, PixelFormat& outFormat,
             QuantRange& outRange, std::vector<uint8_t>& dst, int& dstStride,
             std::string& error) {
  if (!src.data || src.width <= 0 || src.height <= 0) {
    error = "source frame is empty";
    return false;
  }
  if (!canConvert(src.format, to)) {
    error = std::string("cannot convert ") + toString(src.format) + " to " +
            toString(to);
    return false;
  }

  const bool srcIsRgb = describe(src.format).isRgb;
  const bool dstIsRgb = describe(to).isRgb;

  // The 10-bit 4:2:2 intermediate is narrow whenever the source is RGB, so an
  // RGB source lands on the broadcast convention rather than carrying its full
  // range into a YCbCr transport. A YCbCr source keeps whatever it arrived as.
  const QuantRange intermediate =
      srcIsRgb ? QuantRange::narrow
               : (src.range == QuantRange::unknown ? QuantRange::narrow
                                                   : src.range);
  outRange = dstIsRgb ? QuantRange::full : intermediate;

  const LumaCoefficients k = coefficientsFor(src.colour);
  const Scaling s10 = scalingFor(intermediate, 10);
  const FixedCoeffs coeffs = makeCoeffs(k, s10);

  dstStride = tightStrideBytes(to, src.width);
  dst.resize(static_cast<size_t>(dstStride) * src.height);
  outFormat = to;

  // The fused fast path, before the general machinery.
  if (to == PixelFormat::v210 && srcIsRgb) {
    const bool bgra = src.format == PixelFormat::bgra8;
    for (int y = 0; y < src.height; ++y) {
      fusedRgbToV210(src.data + static_cast<size_t>(y) * src.strideBytes,
                     src.width, bgra, coeffs,
                     dst.data() + static_cast<size_t>(y) * dstStride);
    }
    return true;
  }

  Row422 row;
  ensureRow(row, src.width);

  for (int y = 0; y < src.height; ++y) {
    const uint8_t* sp = src.data + static_cast<size_t>(y) * src.strideBytes;
    uint8_t* dp = dst.data() + static_cast<size_t>(y) * dstStride;

    // Read into the 10-bit 4:2:2 intermediate.
    switch (src.format) {
      case PixelFormat::uyvy8: readPacked8(sp, src.width, true, row); break;
      case PixelFormat::yuy2_8: readPacked8(sp, src.width, false, row); break;
      case PixelFormat::v210: readV210(sp, src.width, row); break;
      case PixelFormat::bgra8:
        readBgraLike(sp, src.width, true, coeffs, row);
        break;
      case PixelFormat::rgba8:
        readBgraLike(sp, src.width, false, coeffs, row);
        break;
      default:
        error = "unreadable source format";
        return false;
    }

    // Write out.
    switch (to) {
      case PixelFormat::uyvy8: writePacked8(row, src.width, true, dp); break;
      case PixelFormat::yuy2_8: writePacked8(row, src.width, false, dp); break;
      case PixelFormat::v210: writeV210(row, src.width, dp); break;
      case PixelFormat::bgra8:
        writeBgraLike(row, src.width, true, coeffs, dp);
        break;
      case PixelFormat::rgba8:
        writeBgraLike(row, src.width, false, coeffs, dp);
        break;
      default:
        error = "unwritable destination format";
        return false;
    }
  }

  return true;
}

void fillBlack(PixelFormat format, int width, int height, int strideBytes,
               QuantRange range, uint8_t* dst) {
  if (!dst || width <= 0 || height <= 0) return;

  const bool full = range == QuantRange::full;

  switch (format) {
    case PixelFormat::bgra8:
    case PixelFormat::rgba8:
      for (int y = 0; y < height; ++y) {
        uint8_t* p = dst + static_cast<size_t>(y) * strideBytes;
        for (int x = 0; x < width; ++x) {
          p[x * 4 + 0] = 0;
          p[x * 4 + 1] = 0;
          p[x * 4 + 2] = 0;
          p[x * 4 + 3] = 255;  // opaque, not transparent
        }
      }
      return;

    case PixelFormat::uyvy8:
    case PixelFormat::yuy2_8: {
      // Legal black, not all-zero: Y=16 narrow (0 full), C=128. All-zero is
      // superblack, which is illegal on SDI and clips on any conformant
      // receiver.
      const uint8_t yv = full ? 0 : 16;
      const uint8_t cv = 128;
      const bool uyvy = format == PixelFormat::uyvy8;
      for (int y = 0; y < height; ++y) {
        uint8_t* p = dst + static_cast<size_t>(y) * strideBytes;
        for (int x = 0; x < width; x += 2) {
          if (uyvy) {
            p[x * 2 + 0] = cv; p[x * 2 + 1] = yv;
            p[x * 2 + 2] = cv; p[x * 2 + 3] = yv;
          } else {
            p[x * 2 + 0] = yv; p[x * 2 + 1] = cv;
            p[x * 2 + 2] = yv; p[x * 2 + 3] = cv;
          }
        }
      }
      return;
    }

    case PixelFormat::v210: {
      const uint16_t yv = full ? 0 : 64;   // 16 << 2
      const uint16_t cv = 512;             // 128 << 2
      const uint32_t w0 = pack3(cv, yv, cv);
      const uint32_t w1 = pack3(yv, cv, yv);
      const uint32_t w2 = pack3(cv, yv, cv);
      const uint32_t w3 = pack3(yv, cv, yv);
      const uint32_t words[4] = {w0, w1, w2, w3};
      const int groups = (width + 5) / 6;
      for (int y = 0; y < height; ++y) {
        uint8_t* p = dst + static_cast<size_t>(y) * strideBytes;
        for (int g = 0; g < groups; ++g) {
          std::memcpy(p + static_cast<size_t>(g) * 16, words, 16);
        }
      }
      return;
    }

    case PixelFormat::nv12: {
      const uint8_t yv = full ? 0 : 16;
      for (int y = 0; y < height; ++y) {
        std::memset(dst + static_cast<size_t>(y) * strideBytes, yv, width);
      }
      uint8_t* chroma = dst + static_cast<size_t>(height) * strideBytes;
      std::memset(chroma, 128, static_cast<size_t>(strideBytes) * (height / 2));
      return;
    }

    default:
      // Unknown and the 2110 pgroup: zero it. The pgroup case is only ever a
      // transient between depacketiser and conversion and never reaches a sink.
      std::memset(dst, 0, static_cast<size_t>(strideBytes) * height);
      return;
  }
}

}  // namespace ferret

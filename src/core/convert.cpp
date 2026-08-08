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

void readBgraLike(const uint8_t* src, int width, bool bgra,
                  const LumaCoefficients& k, const Scaling& s, Row422& out) {
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
                   const LumaCoefficients& k, const Scaling& s, uint8_t* dst) {
  for (int x = 0; x < width; ++x) {
    const int c = x / 2;
    const Ycc ycc{(in.y[x] - s.yOffset) / s.yScale,
                  (in.cb[c] - s.cOffset) / s.cScale,
                  (in.cr[c] - s.cOffset) / s.cScale};
    const Rgb rgb = yccToRgb(ycc, k);
    uint8_t* p = dst + static_cast<size_t>(x) * 4;
    if (bgra) {
      p[0] = clamp8(rgb.b * 255.0);
      p[1] = clamp8(rgb.g * 255.0);
      p[2] = clamp8(rgb.r * 255.0);
    } else {
      p[0] = clamp8(rgb.r * 255.0);
      p[1] = clamp8(rgb.g * 255.0);
      p[2] = clamp8(rgb.b * 255.0);
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
             std::vector<uint8_t>& dst, int& dstStride, std::string& error) {
  if (!src.data || src.width <= 0 || src.height <= 0) {
    error = "source frame is empty";
    return false;
  }
  if (!canConvert(src.format, to)) {
    error = std::string("cannot convert ") + toString(src.format) + " to " +
            toString(to);
    return false;
  }

  const LumaCoefficients k = coefficientsFor(src.colour);
  const Scaling s8 = scalingFor(src.range, 8);
  const Scaling s10 = scalingFor(src.range, 10);

  dstStride = tightStrideBytes(to, src.width);
  dst.resize(static_cast<size_t>(dstStride) * src.height);
  outFormat = to;

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
        readBgraLike(sp, src.width, true, k, s10, row);
        break;
      case PixelFormat::rgba8:
        readBgraLike(sp, src.width, false, k, s10, row);
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
        writeBgraLike(row, src.width, true, k, s10, dp);
        break;
      case PixelFormat::rgba8:
        writeBgraLike(row, src.width, false, k, s10, dp);
        break;
      default:
        error = "unwritable destination format";
        return false;
    }
  }

  (void)s8;
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

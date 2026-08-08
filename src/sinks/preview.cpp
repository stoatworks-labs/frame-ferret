#include "sinks/preview.h"

#include <algorithm>
#include <cstring>

#include "core/convert.h"

namespace ferret {
namespace {

/// The preview's long edge. Small on purpose: this is re-encoded and pushed
/// over HTTP several times a second, and a full-raster preview would cost more
/// CPU than several of the real outputs.
constexpr int kMaxPreviewEdge = 480;

void previewSize(int w, int h, int* pw, int* ph) {
  if (w <= 0 || h <= 0) {
    *pw = *ph = 0;
    return;
  }
  const int longEdge = std::max(w, h);
  if (longEdge <= kMaxPreviewEdge) {
    *pw = w;
    *ph = h;
    return;
  }
  const double scale = static_cast<double>(kMaxPreviewEdge) / longEdge;
  *pw = std::max(1, static_cast<int>(w * scale));
  *ph = std::max(1, static_cast<int>(h * scale));
}

void writeLE32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

void writeLE16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}

}  // namespace

PreviewSink::PreviewSink(NodeConfig config) : config_(std::move(config)) {}

void PreviewSink::send(const VideoFrame& frame) {
  if (!frame.data || frame.width <= 0 || frame.height <= 0) return;

  if (frame.format == PixelFormat::bgra8) {
    storeScaled(frame);
  } else {
    // Convert into BGRA first. The preview is the one place a conversion is
    // always acceptable: it is a display path, not a signal path.
    PixelFormat outFmt;
    QuantRange outRange = QuantRange::unknown;
    int outStride;
    std::string error;
    if (!convert(frame, PixelFormat::bgra8, outFmt, outRange, convertScratch_,
                 outStride, error)) {
      return;
    }
    VideoFrame asRgb = frame;
    asRgb.format = outFmt;
    asRgb.range = outRange;
    asRgb.data = convertScratch_.data();
    asRgb.strideBytes = outStride;
    storeScaled(asRgb);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  sourceFormat_ = toString(frame.format);
  sourceW_ = frame.width;
  sourceH_ = frame.height;
  ++frames_;
}

void PreviewSink::sendBlack() {
  const int w = config_.width > 0 ? config_.width : 1920;
  const int h = config_.height > 0 ? config_.height : 1080;

  int pw, ph;
  previewSize(w, h, &pw, &ph);

  std::lock_guard<std::mutex> lock(mutex_);
  previewW_ = pw;
  previewH_ = ph;
  bgra_.assign(static_cast<size_t>(pw) * ph * 4, 0);
  for (size_t i = 3; i < bgra_.size(); i += 4) bgra_[i] = 255;
  sourceFormat_ = "black";
  sourceW_ = w;
  sourceH_ = h;
  ++frames_;
  ++blackFrames_;
}

void PreviewSink::storeScaled(const VideoFrame& frame) {
  int pw, ph;
  previewSize(frame.width, frame.height, &pw, &ph);
  if (pw <= 0 || ph <= 0) return;

  const size_t needed = static_cast<size_t>(pw) * ph * 4;
  if (scaleScratch_.size() != needed) scaleScratch_.resize(needed);
  std::vector<uint8_t>& scaled = scaleScratch_;

  // Nearest neighbour. This is a monitoring thumbnail, and a box filter here
  // would cost more than the conversion that produced the frame.
  for (int y = 0; y < ph; ++y) {
    const int sy = static_cast<int>(static_cast<int64_t>(y) * frame.height / ph);
    const uint8_t* srcRow =
        frame.data + static_cast<size_t>(sy) * frame.strideBytes;
    uint8_t* dstRow = scaled.data() + static_cast<size_t>(y) * pw * 4;
    for (int x = 0; x < pw; ++x) {
      const int sx =
          static_cast<int>(static_cast<int64_t>(x) * frame.width / pw);
      std::memcpy(dstRow + static_cast<size_t>(x) * 4,
                  srcRow + static_cast<size_t>(sx) * 4, 4);
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  previewW_ = pw;
  previewH_ = ph;
  // Copy rather than swap: scaleScratch_ must keep its capacity for the next
  // frame, and swapping would hand it away and take back bgra_'s buffer,
  // reallocating on every geometry change.
  bgra_.assign(scaled.begin(), scaled.end());
}

bool PreviewSink::encodeBmp(std::vector<uint8_t>& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (bgra_.empty() || previewW_ <= 0 || previewH_ <= 0) return false;

  // 24-bit BMP: rows padded to 4 bytes, stored bottom-up.
  const int rowBytes = previewW_ * 3;
  const int padded = (rowBytes + 3) & ~3;
  const uint32_t pixelBytes = static_cast<uint32_t>(padded) * previewH_;
  const uint32_t offset = 54;
  const uint32_t total = offset + pixelBytes;

  out.assign(total, 0);
  uint8_t* h = out.data();
  h[0] = 'B';
  h[1] = 'M';
  writeLE32(h + 2, total);
  writeLE32(h + 10, offset);
  writeLE32(h + 14, 40);  // DIB header size
  writeLE32(h + 18, static_cast<uint32_t>(previewW_));
  writeLE32(h + 22, static_cast<uint32_t>(previewH_));  // positive = bottom-up
  writeLE16(h + 26, 1);   // planes
  writeLE16(h + 28, 24);  // bits per pixel
  writeLE32(h + 34, pixelBytes);
  writeLE32(h + 38, 2835);  // 72 dpi
  writeLE32(h + 42, 2835);

  for (int y = 0; y < previewH_; ++y) {
    // Source is top-down, BMP is bottom-up.
    const uint8_t* src =
        bgra_.data() + static_cast<size_t>(previewH_ - 1 - y) * previewW_ * 4;
    uint8_t* dst = out.data() + offset + static_cast<size_t>(y) * padded;
    for (int x = 0; x < previewW_; ++x) {
      dst[x * 3 + 0] = src[x * 4 + 0];  // B
      dst[x * 3 + 1] = src[x * 4 + 1];  // G
      dst[x * 3 + 2] = src[x * 4 + 2];  // R
    }
  }
  return true;
}

uint64_t PreviewSink::frameCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frames_;
}

PreviewSink::Stats PreviewSink::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Stats s;
  s.width = sourceW_;
  s.height = sourceH_;
  s.frames = frames_;
  s.blackFrames = blackFrames_;
  s.format = sourceFormat_;
  return s;
}

}  // namespace ferret

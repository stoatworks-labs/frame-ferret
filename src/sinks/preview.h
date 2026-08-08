#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "app/node.h"

namespace ferret {

/// Holds the most recent frame so the control page can display it.
///
/// This is a real sink, not a debugging aid bolted on the side: it is routed
/// through the crosspoint like any other, so the picture an operator sees on
/// the control page is the picture the router actually produced — including
/// its black frames and their reasons. A preview drawn from anywhere else
/// would be capable of looking correct while the outputs were not.
///
/// Thread-safe: the frame thread writes, the HTTP threads read.
class PreviewSink : public Sink {
 public:
  explicit PreviewSink(NodeConfig config);

  const std::string& id() const override { return config_.id; }

  /// Anything, because it converts internally to BGRA for the browser.
  std::vector<PixelFormat> preferredFormats() const override { return {}; }

  void send(const VideoFrame& frame) override;
  void sendBlack() override;

  /// The latest frame as a BMP. BMP rather than PNG because it needs no
  /// compressor: a 24-bit BMP is a 54-byte header and bottom-up BGR rows, which
  /// every browser has decoded for thirty years. The preview is downscaled, so
  /// the size difference does not matter, and adding a PNG encoder to serve a
  /// thumbnail would be the wrong trade.
  ///
  /// Returns false when no frame has arrived yet.
  bool encodeBmp(std::vector<uint8_t>& out) const;

  uint64_t frameCount() const;

  struct Stats {
    int width = 0;
    int height = 0;
    uint64_t frames = 0;
    uint64_t blackFrames = 0;
    std::string format;
  };
  Stats stats() const;

 private:
  void storeScaled(const VideoFrame& frame);

  NodeConfig config_;

  // Reused across frames. A 1280x720 BGRA conversion is 3.7 MB, and
  // allocating that twice per frame — once for the conversion, once for the
  // scaled copy — measured as more of the frame budget than the conversion
  // itself. The engine already learned this with its own scratch buffer.
  std::vector<uint8_t> convertScratch_;
  std::vector<uint8_t> scaleScratch_;

  mutable std::mutex mutex_;
  std::vector<uint8_t> bgra_;  ///< previewW_ x previewH_, top-down BGRA
  int previewW_ = 0;
  int previewH_ = 0;
  int sourceW_ = 0;
  int sourceH_ = 0;
  std::string sourceFormat_ = "none";
  uint64_t frames_ = 0;
  uint64_t blackFrames_ = 0;
};

}  // namespace ferret

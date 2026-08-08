#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/frame.h"

namespace ferret {

/// RTP and RFC 4175 for ST 2110-20, as pure logic with no sockets.
///
/// Separated from the transport deliberately: this is the part that is easy to
/// get subtly wrong and easy to test exhaustively, and a packetiser that can
/// only be exercised through a UDP socket is one nobody tests properly.
///
/// ST 2110-20 is RFC 4175 with constraints, so a conformant RFC 4175
/// depayloader — GStreamer's `rtpvrawdepay`, for instance — must accept what
/// this produces. That is how it is verified.
namespace st2110 {

/// RTP fixed header. 12 bytes, big-endian on the wire.
constexpr int kRtpHeaderBytes = 12;

/// Two pixels per pgroup, five bytes each, for 10-bit YCbCr 4:2:2.
constexpr int kPgroupPixels = 2;
constexpr int kPgroupBytes = 5;

/// The 90 kHz media clock ST 2110-10 mandates for video.
constexpr int64_t kMediaClockHz = 90000;

/// A sensible default payload for a 1500-byte MTU: 1500 - 20 (IP) - 8 (UDP)
/// - 12 (RTP) leaves 1460, and rounding down to a whole number of pgroups
/// avoids splitting one across packets, which the spec permits but which no
/// receiver enjoys.
constexpr int kDefaultPayloadBytes = 1440;

/// One RFC 4175 line header: which line, where in it, and how much.
struct LineHeader {
  uint16_t lengthBytes = 0;
  uint16_t lineNumber = 0;
  uint16_t offsetPixels = 0;
  bool field = false;        ///< second field of an interlaced frame
  bool continues = false;    ///< another line header follows
};

/// Everything needed to packetise one raster.
struct StreamFormat {
  int width = 1920;
  int height = 1080;
  Rate rate{50, 1};
  bool interlaced = false;
  uint8_t payloadType = 96;
  uint32_t ssrc = 0x0DEFACED;
  int payloadBytes = kDefaultPayloadBytes;
};

/// Splits one frame of pgroup-packed pixels into RTP packets.
///
/// `pixels` must be `height` rows of `(width+1)/2 * 5` bytes, tightly packed —
/// which is exactly what `convert()` produces for
/// `PixelFormat::ycbcr422_10_pgroup`.
///
/// `emit` is called once per packet with a complete datagram. The marker bit is
/// set on the last packet of the frame, and **that is not decoration**: a
/// receiver uses it to know the frame is complete, so getting it wrong makes
/// every frame arrive one frame late.
class Packetiser {
 public:
  explicit Packetiser(StreamFormat format);

  /// `timestamp90k` is the frame's media clock value; every packet of one
  /// frame carries the same one, which is what makes them a frame.
  void packetise(const uint8_t* pixels, int strideBytes, uint32_t timestamp90k,
                 const std::function<void(const uint8_t*, int)>& emit);

  uint16_t sequenceNumber() const { return sequence_; }
  const StreamFormat& format() const { return format_; }

 private:
  StreamFormat format_;
  uint16_t sequence_ = 0;
  /// RFC 4175's extended sequence number, incremented when the 16-bit RTP one
  /// wraps. At 1080p50 the base sequence wraps about every 1.4 seconds, which
  /// is why 2110 needs the extension at all.
  uint16_t extendedSequence_ = 0;
  std::vector<uint8_t> packet_;
};

/// Reassembles RTP packets into frames.
///
/// Tolerates loss and reordering the way a live receiver must: a frame is
/// emitted on the marker bit or when the timestamp changes, whichever comes
/// first, and missing pixels are simply left as whatever the buffer held —
/// never dropped wholesale. A 2110 receiver that discards a frame for one lost
/// packet produces a black flash, which is worse than a torn line.
class Depacketiser {
 public:
  Depacketiser(int width, int height);

  /// Feeds one datagram. Returns true when `frame` now holds a complete frame.
  bool receive(const uint8_t* datagram, int size);

  /// The assembled frame, pgroup-packed, valid until the next `receive`.
  const std::vector<uint8_t>& pixels() const { return pixels_; }
  int strideBytes() const { return stride_; }
  uint32_t timestamp90k() const { return currentTimestamp_; }

  struct Counters {
    uint64_t packets = 0;
    uint64_t framesCompleted = 0;
    uint64_t packetsLost = 0;     ///< inferred from sequence gaps
    uint64_t framesIncomplete = 0;///< ended without every line arriving
    uint64_t malformed = 0;
  };
  const Counters& counters() const { return counters_; }

 private:
  void startFrame(uint32_t timestamp);

  int width_ = 0;
  int height_ = 0;
  int stride_ = 0;
  std::vector<uint8_t> pixels_;
  uint32_t currentTimestamp_ = 0;
  bool haveTimestamp_ = false;
  uint16_t lastSequence_ = 0;
  bool haveSequence_ = false;
  int64_t bytesThisFrame_ = 0;
  Counters counters_;
};

/// Builds the SDP a receiver needs. This is the interop contract in practice:
/// most 2110 equipment is configured by pasting one of these, and a wrong
/// `a=fmtp` line means a receiver that connects and shows nothing.
std::string buildSdp(const StreamFormat& format, const std::string& sourceIp,
                     const std::string& destinationIp, int port,
                     const std::string& sessionName);

}  // namespace st2110
}  // namespace ferret

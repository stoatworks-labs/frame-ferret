#include "transports/st2110_rtp.h"

#include <cstdio>
#include <cstring>

namespace ferret {
namespace st2110 {
namespace {

void writeBE16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v & 0xFF);
}

void writeBE32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[3] = static_cast<uint8_t>(v & 0xFF);
}

uint16_t readBE16(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t readBE32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

int pgroupsPerRow(int width) { return (width + 1) / 2; }

}  // namespace

// ---------------------------------------------------------------------------
// Packetiser
// ---------------------------------------------------------------------------

Packetiser::Packetiser(StreamFormat format) : format_(std::move(format)) {
  packet_.resize(kRtpHeaderBytes + 2 + 3 * 6 + format_.payloadBytes);
}

void Packetiser::packetise(
    const uint8_t* pixels, int strideBytes, uint32_t timestamp90k,
    const std::function<void(const uint8_t*, int)>& emit) {
  if (!pixels || !emit) return;

  const int rowBytes = pgroupsPerRow(format_.width) * kPgroupBytes;
  const int payloadLimit = format_.payloadBytes;

  int line = 0;
  int offsetBytes = 0;  // within the current line

  while (line < format_.height) {
    // One packet carries as many whole pgroups as fit, from one or more lines.
    // A single line header is the common case; a second appears only when a
    // packet spans a line boundary, which happens on most rasters because the
    // row length is rarely a multiple of the payload size.
    LineHeader headers[3];
    int headerCount = 0;
    int payloadUsed = 0;

    int scanLine = line;
    int scanOffset = offsetBytes;
    while (headerCount < 3 && payloadUsed < payloadLimit &&
           scanLine < format_.height) {
      const int remainingInLine = rowBytes - scanOffset;
      if (remainingInLine <= 0) {
        ++scanLine;
        scanOffset = 0;
        continue;
      }
      int take = payloadLimit - payloadUsed;
      if (take > remainingInLine) take = remainingInLine;
      // Whole pgroups only. Splitting one across packets is legal but no
      // receiver enjoys it, and it makes the offset arithmetic a trap.
      take -= take % kPgroupBytes;
      if (take <= 0) break;

      LineHeader& h = headers[headerCount++];
      h.lengthBytes = static_cast<uint16_t>(take);
      // The wire is 1-based; ours is 0-based.
      h.lineNumber = static_cast<uint16_t>(scanLine + 1);
      h.offsetPixels =
          static_cast<uint16_t>(scanOffset / kPgroupBytes * kPgroupPixels);
      h.field = false;

      payloadUsed += take;
      scanOffset += take;
      if (scanOffset >= rowBytes) {
        ++scanLine;
        scanOffset = 0;
      }
    }

    if (headerCount == 0) break;
    for (int i = 0; i < headerCount - 1; ++i) headers[i].continues = true;
    headers[headerCount - 1].continues = false;

    const bool lastPacket = scanLine >= format_.height;

    // --- assemble ---
    uint8_t* p = packet_.data();
    p[0] = 0x80;  // version 2, no padding, no extension, CC 0
    p[1] = static_cast<uint8_t>((lastPacket ? 0x80 : 0x00) |
                                (format_.payloadType & 0x7F));
    writeBE16(p + 2, sequence_);
    writeBE32(p + 4, timestamp90k);
    writeBE32(p + 8, format_.ssrc);

    int at = kRtpHeaderBytes;
    // RFC 4175 extended sequence number, the high 16 bits of a 32-bit counter.
    writeBE16(p + at, extendedSequence_);
    at += 2;

    for (int i = 0; i < headerCount; ++i) {
      writeBE16(p + at, headers[i].lengthBytes);
      at += 2;
      writeBE16(p + at, static_cast<uint16_t>((headers[i].field ? 0x8000 : 0) |
                                              (headers[i].lineNumber & 0x7FFF)));
      at += 2;
      writeBE16(p + at,
                static_cast<uint16_t>((headers[i].continues ? 0x8000 : 0) |
                                      (headers[i].offsetPixels & 0x7FFF)));
      at += 2;
    }

    // --- pixels ---
    int copyLine = line;
    int copyOffset = offsetBytes;
    for (int i = 0; i < headerCount; ++i) {
      const int take = headers[i].lengthBytes;
      std::memcpy(p + at,
                  pixels + static_cast<size_t>(copyLine) * strideBytes +
                      copyOffset,
                  static_cast<size_t>(take));
      at += take;
      copyOffset += take;
      if (copyOffset >= rowBytes) {
        ++copyLine;
        copyOffset = 0;
      }
    }

    emit(p, at);

    if (sequence_ == 0xFFFF) ++extendedSequence_;
    ++sequence_;

    line = scanLine;
    offsetBytes = scanOffset;
  }
}

// ---------------------------------------------------------------------------
// Depacketiser
// ---------------------------------------------------------------------------

Depacketiser::Depacketiser(int width, int height)
    : width_(width), height_(height) {
  stride_ = pgroupsPerRow(width) * kPgroupBytes;
  pixels_.assign(static_cast<size_t>(stride_) * height, 0);
}

void Depacketiser::startFrame(uint32_t timestamp) {
  currentTimestamp_ = timestamp;
  haveTimestamp_ = true;
  bytesThisFrame_ = 0;
}

bool Depacketiser::receive(const uint8_t* datagram, int size) {
  if (!datagram || size < kRtpHeaderBytes + 2 + 6) {
    ++counters_.malformed;
    return false;
  }
  if ((datagram[0] >> 6) != 2) {  // RTP version
    ++counters_.malformed;
    return false;
  }

  ++counters_.packets;

  const bool marker = (datagram[1] & 0x80) != 0;
  const uint16_t sequence = readBE16(datagram + 2);
  const uint32_t timestamp = readBE32(datagram + 4);

  if (haveSequence_) {
    // 16-bit wrap-safe gap. A negative difference is reordering, not loss.
    const uint16_t expected = static_cast<uint16_t>(lastSequence_ + 1);
    const uint16_t gap = static_cast<uint16_t>(sequence - expected);
    if (gap > 0 && gap < 0x8000) counters_.packetsLost += gap;
  }
  lastSequence_ = sequence;
  haveSequence_ = true;

  bool completed = false;
  if (!haveTimestamp_) {
    startFrame(timestamp);
  } else if (timestamp != currentTimestamp_) {
    // A new frame began without the previous one's marker ever arriving —
    // which means it was lost. Emit what we have rather than discarding it: a
    // torn line is better than a black flash.
    ++counters_.framesIncomplete;
    ++counters_.framesCompleted;
    completed = true;
    startFrame(timestamp);
  }

  int at = kRtpHeaderBytes + 2;  // past the extended sequence number

  // Read line headers until one says it does not continue.
  LineHeader headers[8];
  int headerCount = 0;
  bool continues = true;
  while (continues && headerCount < 8) {
    if (at + 6 > size) {
      ++counters_.malformed;
      return completed;
    }
    LineHeader& h = headers[headerCount];
    h.lengthBytes = readBE16(datagram + at);
    const uint16_t lineWord = readBE16(datagram + at + 2);
    const uint16_t offsetWord = readBE16(datagram + at + 4);
    h.field = (lineWord & 0x8000) != 0;
    h.lineNumber = lineWord & 0x7FFF;
    h.continues = (offsetWord & 0x8000) != 0;
    h.offsetPixels = offsetWord & 0x7FFF;
    continues = h.continues;
    at += 6;
    ++headerCount;
  }

  for (int i = 0; i < headerCount; ++i) {
    const LineHeader& h = headers[i];
    if (at + h.lengthBytes > size) {
      ++counters_.malformed;
      break;
    }
    // Wire line numbers are 1-based.
    const int line = static_cast<int>(h.lineNumber) - 1;
    const int offsetBytes =
        static_cast<int>(h.offsetPixels) / kPgroupPixels * kPgroupBytes;
    if (line >= 0 && line < height_ && offsetBytes >= 0 &&
        offsetBytes + h.lengthBytes <= stride_) {
      std::memcpy(pixels_.data() + static_cast<size_t>(line) * stride_ +
                      offsetBytes,
                  datagram + at, h.lengthBytes);
      bytesThisFrame_ += h.lengthBytes;
    }
    at += h.lengthBytes;
  }

  if (marker) {
    ++counters_.framesCompleted;
    if (bytesThisFrame_ < static_cast<int64_t>(stride_) * height_) {
      ++counters_.framesIncomplete;
    }
    haveTimestamp_ = false;
    return true;
  }
  return completed;
}

// ---------------------------------------------------------------------------

std::string buildSdp(const StreamFormat& format, const std::string& sourceIp,
                     const std::string& destinationIp, int port,
                     const std::string& sessionName) {
  // The exact shape ST 2110-20 receivers expect. Every parameter here is load
  // bearing: a wrong `sampling` or missing `depth` is a receiver that connects
  // and shows nothing, with no message about why.
  std::string out;
  out += "v=0\r\n";
  out += "o=- 0 0 IN IP4 " + sourceIp + "\r\n";
  out += "s=" + sessionName + "\r\n";
  out += "t=0 0\r\n";
  out += "m=video " + std::to_string(port) + " RTP/AVP " +
         std::to_string(format.payloadType) + "\r\n";
  out += "c=IN IP4 " + destinationIp + "/64\r\n";
  out += "a=source-filter: incl IN IP4 " + destinationIp + " " + sourceIp +
         "\r\n";
  out += "a=rtpmap:" + std::to_string(format.payloadType) + " raw/90000\r\n";
  out += "a=fmtp:" + std::to_string(format.payloadType) +
         " sampling=YCbCr-4:2:2; width=" + std::to_string(format.width) +
         "; height=" + std::to_string(format.height) +
         "; exactframerate=" + std::to_string(format.rate.num) + "/" +
         std::to_string(format.rate.den) +
         "; depth=10; TCS=SDR; colorimetry=BT709; PM=2110GPM; SSN=ST2110-20:2017; " +
         (format.interlaced ? "interlace" : "") + "\r\n";
  out += "a=mediaclk:direct=0\r\n";
  // TR-04/2110-21 sender type. Wide, honestly declared — this is a software
  // sender on a general-purpose NIC and cannot claim narrow.
  out += "a=TP=2110TPW\r\n";
  return out;
}

}  // namespace st2110
}  // namespace ferret

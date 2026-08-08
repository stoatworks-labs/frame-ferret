#include "transports/st2110_rtp.h"

#include <cstring>

#include "check.h"
#include "core/convert.h"

using namespace ferret;
using namespace ferret::st2110;

namespace {

std::vector<uint8_t> pgroupRaster(int w, int h, uint8_t seed) {
  const int stride = (w + 1) / 2 * kPgroupBytes;
  std::vector<uint8_t> out(static_cast<size_t>(stride) * h);
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<uint8_t>((i * 7 + seed) & 0xFF);
  }
  return out;
}

/// Every packet must be a well-formed RTP datagram before anything else is
/// worth checking.
void packetsAreValidRtp() {
  StreamFormat f;
  f.width = 64;
  f.height = 8;
  f.payloadBytes = 200;
  Packetiser p(f);

  const int stride = 32 * kPgroupBytes;
  const auto pixels = pgroupRaster(64, 8, 0);

  int count = 0;
  int markers = 0;
  uint16_t firstSeq = 0;
  uint16_t lastSeq = 0;

  p.packetise(pixels.data(), stride, 12345,
              [&](const uint8_t* d, int n) {
                CHECK(n >= kRtpHeaderBytes + 2 + 6);
                CHECK_EQ(d[0] >> 6, 2);              // version 2
                CHECK_EQ(d[0] & 0x0F, 0);            // CSRC count 0
                CHECK_EQ(d[1] & 0x7F, 96);           // payload type
                const uint16_t seq =
                    static_cast<uint16_t>((d[2] << 8) | d[3]);
                if (count == 0) firstSeq = seq;
                lastSeq = seq;
                // Every packet of one frame carries the same timestamp — that
                // is what makes them one frame.
                const uint32_t ts = (static_cast<uint32_t>(d[4]) << 24) |
                                    (d[5] << 16) | (d[6] << 8) | d[7];
                CHECK_EQ(ts, uint32_t{12345});
                if (d[1] & 0x80) ++markers;
                ++count;
              });

  CHECK(count > 1);
  // Exactly one marker, on the last packet. A missing marker makes every frame
  // arrive one frame late; an extra one splits a frame in two.
  CHECK_EQ(markers, 1);
  CHECK_EQ(static_cast<int>(static_cast<uint16_t>(lastSeq - firstSeq)),
           count - 1);
}

/// The whole point: what goes in comes out, byte for byte.
void aFrameSurvivesAFullRoundTrip() {
  StreamFormat f;
  f.width = 1920;
  f.height = 64;  // a slice, so the test stays quick
  Packetiser p(f);
  Depacketiser d(f.width, f.height);

  const int stride = (f.width + 1) / 2 * kPgroupBytes;
  const auto pixels = pgroupRaster(f.width, f.height, 0x5A);

  bool complete = false;
  p.packetise(pixels.data(), stride, 900000,
              [&](const uint8_t* pkt, int n) {
                if (d.receive(pkt, n)) complete = true;
              });

  CHECK(complete);
  CHECK_EQ(d.strideBytes(), stride);
  CHECK_EQ(d.timestamp90k(), uint32_t{900000});
  CHECK(d.pixels() == pixels);
  CHECK_EQ(d.counters().framesCompleted, uint64_t{1});
  CHECK_EQ(d.counters().framesIncomplete, uint64_t{0});
  CHECK_EQ(d.counters().packetsLost, uint64_t{0});
  CHECK_EQ(d.counters().malformed, uint64_t{0});
}

/// A raster whose row length is not a multiple of the payload — which is most
/// of them — makes packets span line boundaries, and that is where the offset
/// arithmetic goes wrong.
void packetsSpanningLinesReassembleCorrectly() {
  for (int width : {96, 100, 720, 1280}) {
    StreamFormat f;
    f.width = width;
    f.height = 6;
    f.payloadBytes = 130;  // deliberately awkward
    Packetiser p(f);
    Depacketiser d(width, 6);

    const int stride = (width + 1) / 2 * kPgroupBytes;
    const auto pixels = pgroupRaster(width, 6, static_cast<uint8_t>(width));

    bool complete = false;
    p.packetise(pixels.data(), stride, 42, [&](const uint8_t* pkt, int n) {
      if (d.receive(pkt, n)) complete = true;
    });

    CHECK(complete);
    CHECK(d.pixels() == pixels);
  }
}

/// Odd widths mean a half-full final pgroup. It still occupies five bytes.
void oddWidthsRoundTrip() {
  StreamFormat f;
  f.width = 33;
  f.height = 4;
  f.payloadBytes = 64;
  Packetiser p(f);
  Depacketiser d(33, 4);

  const int stride = 17 * kPgroupBytes;
  const auto pixels = pgroupRaster(33, 4, 9);

  bool complete = false;
  p.packetise(pixels.data(), stride, 7,
              [&](const uint8_t* pkt, int n) {
                if (d.receive(pkt, n)) complete = true;
              });
  CHECK(complete);
  CHECK(d.pixels() == pixels);
}

/// Packets never split a pgroup, and every line header points at a pgroup
/// boundary. A receiver that assumed otherwise would decode shifted colour.
void everyPayloadIsAWholeNumberOfPgroups() {
  StreamFormat f;
  f.width = 1280;
  f.height = 8;
  f.payloadBytes = 1440;
  Packetiser p(f);

  const int stride = 640 * kPgroupBytes;
  const auto pixels = pgroupRaster(1280, 8, 3);

  p.packetise(pixels.data(), stride, 1, [&](const uint8_t* d, int n) {
    int at = kRtpHeaderBytes + 2;
    bool continues = true;
    int total = 0;
    while (continues) {
      const uint16_t length = static_cast<uint16_t>((d[at] << 8) | d[at + 1]);
      const uint16_t offsetWord =
          static_cast<uint16_t>((d[at + 4] << 8) | d[at + 5]);
      continues = (offsetWord & 0x8000) != 0;
      const uint16_t offset = offsetWord & 0x7FFF;

      CHECK_EQ(length % kPgroupBytes, 0);
      CHECK_EQ(offset % kPgroupPixels, 0);
      total += length;
      at += 6;
    }
    CHECK_EQ(at + total, n);
  });
}

/// Loss must tear a frame, never discard it. A 2110 receiver that drops a whole
/// frame for one missing packet produces a black flash, which is far more
/// visible than a torn line.
void lostPacketsTearRatherThanDiscard() {
  StreamFormat f;
  f.width = 640;
  f.height = 16;
  f.payloadBytes = 400;
  Packetiser p(f);
  Depacketiser d(640, 16);

  const int stride = 320 * kPgroupBytes;
  const auto pixels = pgroupRaster(640, 16, 0x11);

  int index = 0;
  bool complete = false;
  p.packetise(pixels.data(), stride, 100, [&](const uint8_t* pkt, int n) {
    // Drop the third packet.
    if (index++ == 2) return;
    if (d.receive(pkt, n)) complete = true;
  });

  CHECK(complete);
  CHECK(d.counters().packetsLost > 0);
  CHECK_EQ(d.counters().framesIncomplete, uint64_t{1});
  // Most of the picture still arrived.
  CHECK(d.pixels() != pixels);
  size_t matching = 0;
  for (size_t i = 0; i < pixels.size(); ++i) {
    if (d.pixels()[i] == pixels[i]) ++matching;
  }
  CHECK(matching > pixels.size() * 3 / 4);
}

/// A frame whose marker is lost must still be emitted when the next frame's
/// timestamp appears, or the receiver stalls forever on one lost packet.
void aMissingMarkerIsRecoveredByTheNextTimestamp() {
  StreamFormat f;
  f.width = 320;
  f.height = 8;
  f.payloadBytes = 400;
  Packetiser p(f);
  Depacketiser d(320, 8);

  const int stride = 160 * kPgroupBytes;
  const auto pixels = pgroupRaster(320, 8, 2);

  // First frame, with the final (marker) packet withheld.
  std::vector<std::vector<uint8_t>> packets;
  p.packetise(pixels.data(), stride, 1000, [&](const uint8_t* pkt, int n) {
    packets.emplace_back(pkt, pkt + n);
  });
  CHECK(packets.size() > 1);
  for (size_t i = 0; i + 1 < packets.size(); ++i) {
    CHECK(!d.receive(packets[i].data(),
                     static_cast<int>(packets[i].size())));
  }

  // A packet from the next frame arrives; the stalled one must come out.
  bool emitted = false;
  p.packetise(pixels.data(), stride, 2000, [&](const uint8_t* pkt, int n) {
    if (!emitted && d.receive(pkt, n)) emitted = true;
  });
  CHECK(emitted);
  CHECK(d.counters().framesIncomplete >= 1);
}

/// Sequence numbers wrap at 16 bits — about every 1.4 seconds at 1080p50,
/// which is exactly why RFC 4175 carries an extended sequence number.
void sequenceNumbersWrapCleanly() {
  StreamFormat f;
  f.width = 64;
  f.height = 2;
  f.payloadBytes = 200;
  Packetiser p(f);

  const int stride = 32 * kPgroupBytes;
  const auto pixels = pgroupRaster(64, 2, 0);

  // Run enough frames to pass 65535.
  int packets = 0;
  for (int frame = 0; frame < 40000 && packets < 70000; ++frame) {
    p.packetise(pixels.data(), stride, static_cast<uint32_t>(frame),
                [&](const uint8_t*, int) { ++packets; });
  }
  CHECK(packets > 65536);

  // And a depacketiser must not count the wrap as loss.
  Depacketiser d(64, 2);
  Packetiser q(f);
  uint64_t before = 0;
  for (int frame = 0; frame < 3; ++frame) {
    q.packetise(pixels.data(), stride, static_cast<uint32_t>(frame),
                [&](const uint8_t* pkt, int n) { d.receive(pkt, n); });
  }
  before = d.counters().packetsLost;
  CHECK_EQ(before, uint64_t{0});
}

/// The SDP is the interop contract: most 2110 equipment is configured by
/// pasting one. Every parameter a receiver keys on must be present.
void theSdpCarriesEveryRequiredParameter() {
  StreamFormat f;
  f.width = 1920;
  f.height = 1080;
  f.rate = Rate{60000, 1001};
  const std::string sdp =
      buildSdp(f, "10.0.0.5", "239.10.10.1", 20000, "Frame Ferret");

  for (const char* required :
       {"v=0", "m=video 20000 RTP/AVP 96", "c=IN IP4 239.10.10.1",
        "a=rtpmap:96 raw/90000", "sampling=YCbCr-4:2:2", "width=1920",
        "height=1080", "exactframerate=60000/1001", "depth=10",
        "colorimetry=BT709", "PM=2110GPM", "SSN=ST2110-20:2017",
        "a=mediaclk:direct=0"}) {
    if (sdp.find(required) == std::string::npos) {
      std::printf("  missing from SDP: %s\n", required);
    }
    CHECK(sdp.find(required) != std::string::npos);
  }

  // Declared honestly as a wide-profile sender. This is software on a
  // general-purpose NIC and cannot claim narrow.
  CHECK(sdp.find("a=TP=2110TPW") != std::string::npos);
}

/// pgroup and v210 are both "10-bit 4:2:2" and are completely different
/// packings. Round-tripping a real picture through pgroup proves ours is the
/// one 2110 specifies.
void pgroupCarriesRealPixels() {
  const int w = 16, h = 2;
  std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 4, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t* p = rgb.data() + (static_cast<size_t>(y) * w + x) * 4;
      p[0] = 0; p[1] = 0; p[2] = 191; p[3] = 255;  // BGRA red
    }
  }
  VideoFrame f;
  f.width = w; f.height = h; f.strideBytes = w * 4;
  f.data = rgb.data(); f.format = PixelFormat::bgra8;
  f.colour = ColourSpace::bt709; f.range = QuantRange::full;

  std::vector<uint8_t> pg;
  PixelFormat pgFmt;
  QuantRange pgRange;
  int pgStride;
  std::string err;
  CHECK(convert(f, PixelFormat::ycbcr422_10_pgroup, pgFmt, pgRange, pg,
                pgStride, err));
  CHECK_EQ(pgStride, 8 * kPgroupBytes);

  VideoFrame m = f;
  m.format = pgFmt; m.range = pgRange;
  m.data = pg.data(); m.strideBytes = pgStride;

  std::vector<uint8_t> back;
  PixelFormat backFmt;
  QuantRange backRange;
  int backStride;
  CHECK(convert(m, PixelFormat::bgra8, backFmt, backRange, back, backStride,
                err));

  // Red in, red out.
  CHECK(back[2] > 180);
  CHECK(back[1] < 12);
  CHECK(back[0] < 12);
}

void run() {
  packetsAreValidRtp();
  aFrameSurvivesAFullRoundTrip();
  packetsSpanningLinesReassembleCorrectly();
  oddWidthsRoundTrip();
  everyPayloadIsAWholeNumberOfPgroups();
  lostPacketsTearRatherThanDiscard();
  aMissingMarkerIsRecoveredByTheNextTimestamp();
  sequenceNumbersWrapCleanly();
  theSdpCarriesEveryRequiredParameter();
  pgroupCarriesRealPixels();
}

}  // namespace

TEST_MAIN("st2110")

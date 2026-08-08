#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "net/interfaces.h"

namespace ferret {

/// libsrt, loaded at run time.
///
/// SRT is MPL-2.0, so unlike NDI it *could* be linked and shipped. It is
/// runtime-loaded anyway so that a build without libsrt still runs everything
/// else — the same reason the DeckLink path is optional. An operator with no
/// SRT should not be unable to use NDI.
class SrtRuntime {
 public:
  static bool available();
  static std::string loadedPath();
  static std::string unavailableReason();
  static std::string version();
};

/// How this endpoint establishes the connection. SRT is peer-to-peer: exactly
/// one side listens and the other calls, and which is which is independent of
/// which way the video flows. A *sender* commonly listens and a *receiver*
/// commonly calls, which catches people out.
enum class SrtMode {
  caller,     ///< connects out to a listener
  listener,   ///< waits for a caller
  rendezvous, ///< both sides call simultaneously, for symmetric NAT
};

bool srtModeFromString(const std::string& s, SrtMode* out);
const char* toString(SrtMode mode);

struct SrtConfig {
  SrtMode mode = SrtMode::caller;
  std::string host = "127.0.0.1";
  int port = 9000;

  /// The local interface to bind. **SRT is the first transport here that can
  /// actually honour this** — `srt_bind()` takes a real sockaddr, and there is
  /// SRTO_BINDTODEVICE as well. NDI and OMT both have no such parameter
  /// anywhere in their APIs, so a config that pins an interface means something
  /// here and is only a warning there.
  std::string interfaceSelector;

  /// Receiver latency buffer in milliseconds. The single most important SRT
  /// setting: it must be at least ~4x the round-trip time or the link drops
  /// packets it could have recovered. 120 ms is the usual starting point for
  /// the public internet and far more than a LAN needs.
  int latencyMs = 120;

  std::string passphrase;  ///< 10-79 chars enables AES; shorter is rejected
  std::string streamId;    ///< SRTO_STREAMID, how most services route a feed

  int connectTimeoutMs = 5000;
};

/// One SRT connection, carrying an MPEG-TS byte stream.
///
/// SRT does not carry frames. It carries an MPEG transport stream containing
/// compressed video, which is why this class deals in byte buffers and the
/// encode/decode lives above it.
class SrtConnection {
 public:
  virtual ~SrtConnection() = default;

  /// Reads up to one SRT payload. Returns bytes read, 0 on timeout, and -1
  /// when the connection is gone.
  virtual int receive(uint8_t* buffer, int capacity, int timeoutMs) = 0;

  /// Sends one payload. `size` must not exceed payloadSize().
  virtual bool send(const uint8_t* data, int size) = 0;

  virtual bool connected() const = 0;
  virtual std::string describe() const = 0;

  /// SRT's own view of the link, for the control API. All zero when the
  /// runtime does not export srt_bstats.
  struct Stats {
    int64_t packetsSent = 0;
    int64_t packetsReceived = 0;
    int64_t packetsLost = 0;
    int64_t packetsRetransmitted = 0;
    double roundTripMs = 0.0;
    double sendRateMbps = 0.0;
    double receiveRateMbps = 0.0;
    int negotiatedLatencyMs = 0;
  };
  virtual Stats stats() const = 0;

  /// The maximum single payload, which SRT defaults to 1316 bytes — seven
  /// 188-byte MPEG-TS packets, chosen so a TS stream never straddles an SRT
  /// payload boundary.
  static constexpr int kDefaultPayloadSize = 1316;
  virtual int payloadSize() const { return kDefaultPayloadSize; }
};

/// Opens a connection. For `listener` this blocks until a caller arrives or
/// `connectTimeoutMs` elapses.
std::unique_ptr<SrtConnection> srtConnect(const SrtConfig& config,
                                          std::string& error);

/// Parses `srt://host:port?latency=120&mode=caller&streamid=x` into a config.
/// Bare `host:port` is accepted too. Every unrecognised parameter is an error
/// rather than being ignored — a misspelled `latencey=` that silently left the
/// buffer at its default would be found on site, not here.
bool parseSrtUrl(const std::string& url, SrtConfig* out, std::string& error);

}  // namespace ferret

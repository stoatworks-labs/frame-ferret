#include "transports/srt_socket.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "core/dylib.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace ferret {
namespace {

// libsrt's own constants, printed by tools/srt_abi.c against the real header
// (libsrt 1.5.5) rather than mirrored by hand.
//
// That tool earned its keep on the first run: SRTO_SNDTIMEO and SRTO_RCVTIMEO
// had been written from memory as 38 and 37 and are really 13 and 14, and
// SRTO_STREAMID carries no explicit value in the header at all — it is
// positional, so no amount of reading finds it. Setting an option by the wrong
// number does not fail loudly; it configures a different option.
constexpr int SRTO_RCVSYN = 2;
constexpr int SRTO_SNDSYN = 1;
constexpr int SRTO_LATENCY = 23;
constexpr int SRTO_PASSPHRASE = 26;
constexpr int SRTO_TRANSTYPE = 50;
constexpr int SRTO_STREAMID = 46;
constexpr int SRTO_SNDTIMEO = 13;
constexpr int SRTO_RCVTIMEO = 14;
constexpr int SRTO_PAYLOADSIZE = 49;
constexpr int SRTT_LIVE = 0;

constexpr int SRT_ERROR = -1;
constexpr int SRT_INVALID_SOCK = -1;

// Error codes, so a timeout is recognised by its CODE and never by its
// message. The first version of this file matched the string "timeout" — and
// libsrt says "Operation timed out", which does not contain it. Every receive
// timeout therefore read as a dead link, the connection was torn down, and the
// symptom was an SRT source that connected and immediately dropped.
constexpr int SRT_ETIMEOUT = 6003;   // blocking recv hit SRTO_RCVTIMEO
constexpr int SRT_EASYNCRCV = 6002;  // nothing available, non-blocking

/// SRT_TRACEBSTATS, the subset we read. The struct is large and its tail
/// changes between releases, so we allocate generously and only touch fields
/// whose offsets are stable in 1.4 and 1.5.
struct TraceStats {
  int64_t msTimeStamp;
  int64_t pktSentTotal;
  int64_t pktRecvTotal;
  int pktSndLossTotal;
  int pktRcvLossTotal;
  int pktRetransTotal;
  int pktSentACKTotal;
  int pktRecvACKTotal;
  int pktSentNAKTotal;
  int pktRecvNAKTotal;
  int64_t usSndDurationTotal;
  // The remainder is deliberately unread; see srtStats().
  char tail[4096];
};

struct Api {
  int (*startup)() = nullptr;
  int (*cleanup)() = nullptr;
  int (*create_socket)() = nullptr;
  int (*bind)(int, const sockaddr*, int) = nullptr;
  int (*connect)(int, const sockaddr*, int) = nullptr;
  int (*listen)(int, int) = nullptr;
  int (*accept)(int, sockaddr*, int*) = nullptr;
  int (*close)(int) = nullptr;
  int (*send)(int, const char*, int) = nullptr;
  int (*recv)(int, char*, int) = nullptr;
  int (*setsockflag)(int, int, const void*, int) = nullptr;
  int (*getsockflag)(int, int, void*, int*) = nullptr;
  const char* (*getlasterror_str)() = nullptr;
  int (*getlasterror)(int*) = nullptr;  // returns the code; out-param is errno
  int (*bstats)(int, TraceStats*, int) = nullptr;
  const char* (*getversion_str)() = nullptr;
  uint32_t (*getversion)() = nullptr;
};

Dylib g_lib;
Api g_api;
bool g_tried = false;
bool g_ok = false;
std::string g_error;
std::mutex g_mutex;

std::vector<std::string> runtimeCandidates() {
  std::vector<std::string> out;
  if (const char* env = std::getenv("FERRET_SRT_RUNTIME")) {
    if (*env) out.emplace_back(env);
  }
  for (const auto& dir : Dylib::localSearchPaths()) {
#ifdef _WIN32
    out.emplace_back(dir + "\\srt.dll");
#elif defined(__APPLE__)
    out.emplace_back(dir + "/libsrt.dylib");
#else
    out.emplace_back(dir + "/libsrt.so");
#endif
  }
#ifdef _WIN32
  out.emplace_back("srt.dll");
#elif defined(__APPLE__)
  out.emplace_back("/opt/homebrew/lib/libsrt.dylib");
  out.emplace_back("/usr/local/lib/libsrt.dylib");
  out.emplace_back("libsrt.dylib");
#else
  out.emplace_back("libsrt.so.1.5");
  out.emplace_back("libsrt.so.1");
  out.emplace_back("libsrt.so");
#endif
  return out;
}

bool loadRuntime(std::string& error) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_tried) {
    error = g_error;
    return g_ok;
  }
  g_tried = true;

  if (!g_lib.open(runtimeCandidates())) {
    g_error =
        "libsrt is not installed. Frame Ferret loads it at run time rather "
        "than linking it, so that a machine without SRT can still use every "
        "other transport. Install it with `brew install srt` or from "
        "https://github.com/Haivision/srt (details: " +
        g_lib.lastError() + ")";
    error = g_error;
    return false;
  }

  auto need = [&](const char* name, auto& fn) {
    if (!g_lib.symbol(name, fn)) {
      g_error = std::string("libsrt at ") + g_lib.loadedPath() +
                " is missing " + name;
      return false;
    }
    return true;
  };

  if (!need("srt_startup", g_api.startup) ||
      !need("srt_cleanup", g_api.cleanup) ||
      !need("srt_create_socket", g_api.create_socket) ||
      !need("srt_bind", g_api.bind) || !need("srt_connect", g_api.connect) ||
      !need("srt_listen", g_api.listen) || !need("srt_accept", g_api.accept) ||
      !need("srt_close", g_api.close) || !need("srt_send", g_api.send) ||
      !need("srt_recv", g_api.recv) ||
      !need("srt_setsockflag", g_api.setsockflag) ||
      !need("srt_getlasterror_str", g_api.getlasterror_str)) {
    error = g_error;
    return false;
  }

  g_lib.symbol("srt_getsockflag", g_api.getsockflag);
  g_lib.symbol("srt_getlasterror", g_api.getlasterror);
  g_lib.symbol("srt_bstats", g_api.bstats);
  g_lib.symbol("srt_getversion", g_api.getversion);

  if (g_api.startup() == SRT_ERROR) {
    g_error = std::string("srt_startup failed: ") + g_api.getlasterror_str();
    error = g_error;
    return false;
  }

  g_ok = true;
  return true;
}

std::string lastSrtError() {
  return g_api.getlasterror_str ? g_api.getlasterror_str() : "unknown";
}

/// Fills a sockaddr_in from a dotted address and port.
bool makeAddress(const std::string& host, int port, sockaddr_in* out,
                 std::string& error) {
  std::memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  out->sin_port = htons(static_cast<uint16_t>(port));

  if (host.empty() || host == "0.0.0.0" || host == "any") {
    out->sin_addr.s_addr = INADDR_ANY;
    return true;
  }
  if (inet_pton(AF_INET, host.c_str(), &out->sin_addr) != 1) {
    error = "\"" + host +
            "\" is not an IPv4 address — SRT nodes need a literal address, "
            "not a hostname";
    return false;
  }
  return true;
}

class Connection : public SrtConnection {
 public:
  Connection(int socket, SrtConfig config, std::string description)
      : socket_(socket),
        config_(std::move(config)),
        description_(std::move(description)) {}

  ~Connection() override {
    if (socket_ != SRT_INVALID_SOCK) g_api.close(socket_);
  }

  int receive(uint8_t* buffer, int capacity, int timeoutMs) override {
    if (socket_ == SRT_INVALID_SOCK) return -1;

    // Set per-call, because a receiver polled from the frame loop wants a
    // short timeout while the same socket during startup wants a long one.
    if (timeoutMs != currentRcvTimeout_) {
      g_api.setsockflag(socket_, SRTO_RCVTIMEO, &timeoutMs, sizeof(timeoutMs));
      currentRcvTimeout_ = timeoutMs;
    }

    const int n = g_api.recv(socket_, reinterpret_cast<char*>(buffer), capacity);
    if (n > 0) return n;
    if (n == 0) return 0;

    // SRT reports a timeout as an error, so a quiet moment and a dead link are
    // the same return value. Distinguished by CODE, never by message: libsrt's
    // wording is "Operation timed out", and matching the word "timeout" — as
    // this did at first — misses it, so every gap between packets tore the
    // connection down and the source connected and instantly dropped.
    // srt_getlasterror RETURNS the SRT code; its int* out-param is the system
    // errno, not the code. Reading the out-param — as this did at first —
    // yields 0 for everything, so every timeout looked like an unknown error
    // and tore the connection down. The message said "transmission timed out"
    // the whole time.
    int systemErrno = 0;
    const int code =
        g_api.getlasterror ? g_api.getlasterror(&systemErrno) : 0;
    if (code == SRT_ETIMEOUT || code == SRT_EASYNCRCV) return 0;

    // Reported once with the code, because "the SRT source keeps reconnecting"
    // is otherwise indistinguishable between a dozen causes.
    if (!reportedError_) {
      reportedError_ = true;
      std::fprintf(stderr, "srt: recv failed, code %d: %s\n", code,
                   lastSrtError().c_str());
    }
    connected_ = false;
    lastError_ = lastSrtError();
    return -1;
  }

  bool send(const uint8_t* data, int size) override {
    if (socket_ == SRT_INVALID_SOCK) return false;
    const int n =
        g_api.send(socket_, reinterpret_cast<const char*>(data), size);
    if (n == SRT_ERROR) {
      connected_ = false;
      return false;
    }
    return true;
  }

  bool connected() const override { return connected_; }
  std::string describe() const override { return description_; }

  Stats stats() const override {
    Stats out;
    if (socket_ == SRT_INVALID_SOCK || !g_api.bstats) return out;

    TraceStats raw;
    std::memset(&raw, 0, sizeof(raw));
    if (g_api.bstats(socket_, &raw, 0) == SRT_ERROR) return out;

    out.packetsSent = raw.pktSentTotal;
    out.packetsReceived = raw.pktRecvTotal;
    out.packetsLost = raw.pktSndLossTotal + raw.pktRcvLossTotal;
    out.packetsRetransmitted = raw.pktRetransTotal;
    out.negotiatedLatencyMs = config_.latencyMs;
    // Rates and RTT live further into the struct, past the point where the
    // layout is stable between libsrt releases. Left at zero rather than read
    // from a guessed offset — a wrong number here is worse than none.
    return out;
  }

 private:
  int socket_ = SRT_INVALID_SOCK;
  SrtConfig config_;
  std::string description_;
  bool connected_ = true;
  int currentRcvTimeout_ = -2;
  bool reportedError_ = false;
  std::string lastError_;
};

/// Applies the options that must be set before connect/bind.
bool applyOptions(int sock, const SrtConfig& config, std::string& error) {
  const int live = SRTT_LIVE;
  if (g_api.setsockflag(sock, SRTO_TRANSTYPE, &live, sizeof(live)) ==
      SRT_ERROR) {
    error = std::string("SRTO_TRANSTYPE: ") + lastSrtError();
    return false;
  }

  // TRANSTYPE resets several options to live-mode defaults, so everything else
  // must be set *after* it. Setting latency first silently loses it.
  const int latency = config.latencyMs;
  if (g_api.setsockflag(sock, SRTO_LATENCY, &latency, sizeof(latency)) ==
      SRT_ERROR) {
    error = std::string("SRTO_LATENCY: ") + lastSrtError();
    return false;
  }

  if (!config.passphrase.empty()) {
    if (config.passphrase.size() < 10 || config.passphrase.size() > 79) {
      error =
          "an SRT passphrase must be 10 to 79 characters; libsrt rejects "
          "anything shorter and the connection then fails with a misleading "
          "handshake error";
      return false;
    }
    if (g_api.setsockflag(sock, SRTO_PASSPHRASE, config.passphrase.c_str(),
                          static_cast<int>(config.passphrase.size())) ==
        SRT_ERROR) {
      error = std::string("SRTO_PASSPHRASE: ") + lastSrtError();
      return false;
    }
  }

  if (!config.streamId.empty()) {
    if (g_api.setsockflag(sock, SRTO_STREAMID, config.streamId.c_str(),
                          static_cast<int>(config.streamId.size())) ==
        SRT_ERROR) {
      error = std::string("SRTO_STREAMID: ") + lastSrtError();
      return false;
    }
  }

  // Blocking mode with explicit timeouts, rather than non-blocking with an
  // epoll loop. One connection per node makes the simpler shape correct.
  const int yes = 1;
  g_api.setsockflag(sock, SRTO_RCVSYN, &yes, sizeof(yes));
  g_api.setsockflag(sock, SRTO_SNDSYN, &yes, sizeof(yes));

  const int timeout = config.connectTimeoutMs;
  g_api.setsockflag(sock, SRTO_SNDTIMEO, &timeout, sizeof(timeout));

  return true;
}

/// Binds the socket to the chosen local interface, when one is named.
bool bindLocalInterface(int sock, const SrtConfig& config,
                        std::string& error) {
  if (config.interfaceSelector.empty()) return true;

  std::string listError;
  const auto interfaces = listInterfaces(listError);
  NetInterface chosen;
  if (!resolveInterface(config.interfaceSelector, interfaces, &chosen, error)) {
    return false;
  }
  if (chosen.index == 0) return true;  // "any"

  sockaddr_in local;
  if (!makeAddress(chosen.address, 0, &local, error)) return false;

  if (g_api.bind(sock, reinterpret_cast<const sockaddr*>(&local),
                 sizeof(local)) == SRT_ERROR) {
    error = "could not bind SRT to " + chosen.address + " (" + chosen.name +
            "): " + lastSrtError();
    return false;
  }
  return true;
}

}  // namespace

bool srtModeFromString(const std::string& s, SrtMode* out) {
  if (s == "caller") { *out = SrtMode::caller; return true; }
  if (s == "listener") { *out = SrtMode::listener; return true; }
  if (s == "rendezvous") { *out = SrtMode::rendezvous; return true; }
  return false;
}

const char* toString(SrtMode mode) {
  switch (mode) {
    case SrtMode::caller: return "caller";
    case SrtMode::listener: return "listener";
    case SrtMode::rendezvous: return "rendezvous";
  }
  return "caller";
}

bool SrtRuntime::available() {
  std::string ignored;
  return loadRuntime(ignored);
}

std::string SrtRuntime::loadedPath() {
  std::string ignored;
  if (!loadRuntime(ignored)) return {};
  return g_lib.loadedPath();
}

std::string SrtRuntime::unavailableReason() {
  std::string error;
  if (loadRuntime(error)) return {};
  return error;
}

std::string SrtRuntime::version() {
  std::string ignored;
  if (!loadRuntime(ignored) || !g_api.getversion) return {};
  const uint32_t v = g_api.getversion();
  // Packed as major*0x10000 + minor*0x100 + patch.
  return std::to_string((v >> 16) & 0xFF) + "." +
         std::to_string((v >> 8) & 0xFF) + "." + std::to_string(v & 0xFF);
}

bool parseSrtUrl(const std::string& url, SrtConfig* out, std::string& error) {
  if (!out) return false;

  std::string rest = url;
  const std::string scheme = "srt://";
  if (rest.rfind(scheme, 0) == 0) rest = rest.substr(scheme.size());
  if (rest.empty()) {
    error = "empty SRT address";
    return false;
  }

  std::string query;
  const size_t q = rest.find('?');
  if (q != std::string::npos) {
    query = rest.substr(q + 1);
    rest = rest.substr(0, q);
  }

  const size_t colon = rest.rfind(':');
  if (colon == std::string::npos) {
    error = "SRT address \"" + url + "\" has no port — expected host:port";
    return false;
  }
  out->host = rest.substr(0, colon);
  const std::string portText = rest.substr(colon + 1);
  char* end = nullptr;
  const long port = std::strtol(portText.c_str(), &end, 10);
  if (*end != '\0' || port <= 0 || port > 65535) {
    error = "SRT address \"" + url + "\" has an invalid port";
    return false;
  }
  out->port = static_cast<int>(port);

  size_t start = 0;
  while (start < query.size()) {
    size_t amp = query.find('&', start);
    if (amp == std::string::npos) amp = query.size();
    const std::string pair = query.substr(start, amp - start);
    start = amp + 1;
    if (pair.empty()) continue;

    const size_t eq = pair.find('=');
    if (eq == std::string::npos) {
      error = "SRT parameter \"" + pair + "\" has no value";
      return false;
    }
    const std::string key = pair.substr(0, eq);
    const std::string value = pair.substr(eq + 1);

    if (key == "mode") {
      if (!srtModeFromString(value, &out->mode)) {
        error = "unknown SRT mode \"" + value +
                "\" — expected caller, listener or rendezvous";
        return false;
      }
    } else if (key == "latency") {
      out->latencyMs = std::atoi(value.c_str());
      if (out->latencyMs < 0) {
        error = "SRT latency must not be negative";
        return false;
      }
    } else if (key == "passphrase") {
      out->passphrase = value;
    } else if (key == "streamid") {
      out->streamId = value;
    } else {
      // Rejected, never ignored. A misspelled "latencey=" that silently left
      // the buffer at its default would be diagnosed on site as a flaky link.
      error = "unknown SRT parameter \"" + key +
              "\" — supported: mode, latency, passphrase, streamid";
      return false;
    }
  }
  return true;
}

std::unique_ptr<SrtConnection> srtConnect(const SrtConfig& config,
                                          std::string& error) {
  if (!loadRuntime(error)) return nullptr;

  const int sock = g_api.create_socket();
  if (sock == SRT_INVALID_SOCK) {
    error = std::string("srt_create_socket: ") + lastSrtError();
    return nullptr;
  }

  auto fail = [&](const std::string& message) -> std::unique_ptr<SrtConnection> {
    g_api.close(sock);
    error = message;
    return nullptr;
  };

  if (!applyOptions(sock, config, error)) return fail(error);

  sockaddr_in address;
  if (!makeAddress(config.host, config.port, &address, error)) {
    return fail(error);
  }

  if (config.mode == SrtMode::listener) {
    if (g_api.bind(sock, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)) == SRT_ERROR) {
      return fail("srt_bind to " + config.host + ":" +
                  std::to_string(config.port) + ": " + lastSrtError());
    }
    if (g_api.listen(sock, 1) == SRT_ERROR) {
      return fail(std::string("srt_listen: ") + lastSrtError());
    }

    sockaddr_in peer;
    int peerLen = sizeof(peer);
    const int accepted =
        g_api.accept(sock, reinterpret_cast<sockaddr*>(&peer), &peerLen);
    g_api.close(sock);  // the listening socket has done its job
    if (accepted == SRT_INVALID_SOCK) {
      error = std::string("srt_accept: ") + lastSrtError();
      return nullptr;
    }

    char peerText[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &peer.sin_addr, peerText, sizeof(peerText));
    return std::make_unique<Connection>(
        accepted, config,
        std::string("listener on :") + std::to_string(config.port) +
            ", caller " + peerText);
  }

  // Caller. Bind the local interface first, if one was named — this is the
  // step NDI and OMT have no equivalent of.
  if (!bindLocalInterface(sock, config, error)) return fail(error);

  if (g_api.connect(sock, reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) == SRT_ERROR) {
    return fail("srt_connect to " + config.host + ":" +
                std::to_string(config.port) + ": " + lastSrtError());
  }

  return std::make_unique<Connection>(
      sock, config,
      std::string("caller to ") + config.host + ":" +
          std::to_string(config.port));
}

}  // namespace ferret

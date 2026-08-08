#include "transports/st2110.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "core/convert.h"
#include "transports/st2110_rtp.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ferret {
namespace {

using namespace st2110;

/// Parses "address:port".
bool parseEndpoint(const std::string& target, std::string* address, int* port,
                   std::string& error) {
  const size_t colon = target.rfind(':');
  if (colon == std::string::npos) {
    error = "an ST 2110 node needs \"address:port\", e.g. 239.10.10.1:20000";
    return false;
  }
  *address = target.substr(0, colon);
  char* end = nullptr;
  const long p = std::strtol(target.c_str() + colon + 1, &end, 10);
  if (*end != '\0' || p <= 0 || p > 65535) {
    error = "ST 2110 target \"" + target + "\" has an invalid port";
    return false;
  }
  *port = static_cast<int>(p);

  in_addr probe{};
  if (inet_pton(AF_INET, address->c_str(), &probe) != 1) {
    error = "\"" + *address + "\" is not an IPv4 address";
    return false;
  }
  return true;
}

/// Resolves the configured interface, or reports why it cannot.
bool chooseInterface(const std::string& selector, NetInterface* out,
                     std::string& error) {
  std::string listError;
  const auto interfaces = listInterfaces(listError);
  return resolveInterface(selector, interfaces, out, error);
}

int closeSocket(int fd) {
#ifdef _WIN32
  return ::closesocket(fd);
#else
  return ::close(fd);
#endif
}

/// A UDP socket set up to receive a multicast group on a chosen interface.
int openReceiveSocket(const std::string& group, int port,
                      const NetInterface& nic, std::string& error) {
  const int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
  if (fd < 0) {
    error = "socket() failed";
    return -1;
  }

  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&yes),
               sizeof(yes));
#ifdef SO_REUSEPORT
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<char*>(&yes),
               sizeof(yes));
#endif

  // A generous receive buffer. 1080p50 is ~2.2 Gb/s in ~1.4 kB datagrams —
  // roughly 190,000 packets a second — and the default buffer holds a few
  // milliseconds of that at best, so a scheduling hiccup becomes packet loss.
  int rcvbuf = 16 * 1024 * 1024;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&rcvbuf),
               sizeof(rcvbuf));

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(static_cast<uint16_t>(port));
  local.sin_addr.s_addr = INADDR_ANY;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    error = "could not bind UDP port " + std::to_string(port);
    closeSocket(fd);
    return -1;
  }

  if (isMulticast(group)) {
    // `ip_mreqn`, with its interface INDEX, is a Linux extension. macOS and
    // Windows both have only `ip_mreq`, which identifies the interface by
    // address. The index is the better key — two interfaces can hold the same
    // address after a DHCP reshuffle — so it is used where it exists.
    //
    // Getting this split wrong is easy: an `#else` that means "Linux" quietly
    // catches Windows too, which is exactly how this first broke.
#if defined(__linux__)
    ip_mreqn mreq{};
    inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr);
    mreq.imr_address.s_addr = INADDR_ANY;
    mreq.imr_ifindex = static_cast<int>(nic.index);
#else
    ip_mreq mreq{};
    inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr);
    if (!nic.address.empty()) {
      inet_pton(AF_INET, nic.address.c_str(), &mreq.imr_interface);
    } else {
      mreq.imr_interface.s_addr = INADDR_ANY;
    }
#endif
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                     reinterpret_cast<char*>(&mreq), sizeof(mreq)) != 0) {
      error = "could not join multicast group " + group + " on " +
              (nic.name.empty() ? "the default interface" : nic.name);
      closeSocket(fd);
      return -1;
    }
  }

  return fd;
}

/// A UDP socket set up to send to a group from a chosen interface.
int openSendSocket(const std::string& group, const NetInterface& nic,
                   std::string& error) {
  const int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
  if (fd < 0) {
    error = "socket() failed";
    return -1;
  }

  int sndbuf = 16 * 1024 * 1024;
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&sndbuf),
               sizeof(sndbuf));

  if (isMulticast(group)) {
    // Without this the OS picks the egress interface from its routing table,
    // which on a multi-homed machine is very often not the media network — the
    // stream then leaves on the wrong NIC and is diagnosed on site as a
    // network fault rather than as configuration.
    if (!nic.address.empty()) {
      in_addr local{};
      inet_pton(AF_INET, nic.address.c_str(), &local);
      if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF,
                       reinterpret_cast<char*>(&local), sizeof(local)) != 0) {
        error = "could not set the multicast egress interface to " +
                nic.address;
        closeSocket(fd);
        return -1;
      }
    }
    // 2110 is a LAN protocol but a TTL of 1 does not survive a router, and
    // plenty of plants are routed. 64 is the usual compromise.
    int ttl = 64;
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL,
                 reinterpret_cast<char*>(&ttl), sizeof(ttl));
    // Loopback on, so a receiver on this same machine can see it — which is
    // exactly how this gets tested.
    int loop = 1;
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP,
                 reinterpret_cast<char*>(&loop), sizeof(loop));
  }
  return fd;
}

// ---------------------------------------------------------------------------
// Receiver
// ---------------------------------------------------------------------------
class St2110Source final : public Source {
 public:
  St2110Source(NodeConfig config, int fd)
      : config_(std::move(config)),
        fd_(fd),
        depacketiser_(config_.width, config_.height) {
    worker_ = std::thread([this] { run(); });
  }

  ~St2110Source() override {
    stopping_.store(true);
    if (worker_.joinable()) worker_.join();
    if (fd_ >= 0) closeSocket(fd_);
  }

  const std::string& id() const override { return config_.id; }
  bool connected() const override { return connected_.load(); }

  bool poll(unsigned,
            const std::function<void(const VideoFrame&)>& onVideo) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasFrame_) return false;
    if (onVideo) onVideo(latest_.frame());
    return true;
  }

 private:
  /// Its own thread, because 1080p50 is roughly 190,000 datagrams a second and
  /// nothing that rate-limited belongs on the shared frame loop.
  void run() {
    std::vector<uint8_t> datagram(2048);
    auto lastPacket = std::chrono::steady_clock::now();

    while (!stopping_.load()) {
#ifdef _WIN32
      WSAPOLLFD p{};
      p.fd = static_cast<SOCKET>(fd_);
      p.events = POLLRDNORM;
      const int ready = ::WSAPoll(&p, 1, 100);
#else
      pollfd p{};
      p.fd = fd_;
      p.events = POLLIN;
      const int ready = ::poll(&p, 1, 100);
#endif
      if (ready <= 0) {
        // No packets for a while means the sender has gone. Reported as a
        // disconnection so the router emits black rather than holding a frozen
        // picture.
        if (std::chrono::steady_clock::now() - lastPacket >
            std::chrono::seconds(1)) {
          connected_.store(false);
        }
        continue;
      }

      // Drain what is waiting rather than one datagram per poll — at this
      // packet rate the poll alone would be most of the cost.
      for (int i = 0; i < 512; ++i) {
        const auto n = ::recv(fd_, reinterpret_cast<char*>(datagram.data()),
                                 datagram.size(), 0);
        if (n <= 0) break;
        lastPacket = std::chrono::steady_clock::now();
        if (depacketiser_.receive(datagram.data(), static_cast<int>(n))) {
          publish();
        }
      }
    }
  }

  void publish() {
    VideoFrame f;
    f.width = config_.width;
    f.height = config_.height;
    f.strideBytes = depacketiser_.strideBytes();
    f.data = depacketiser_.pixels().data();
    f.format = PixelFormat::ycbcr422_10_pgroup;
    f.colour = config_.height >= 720 ? ColourSpace::bt709 : ColourSpace::bt601;
    f.range = QuantRange::narrow;
    f.rate = config_.rate;
    // The media clock arrives on the wire, but nothing here is disciplined to
    // PTP, so this must not claim to be.
    f.timestampNs =
        static_cast<int64_t>(depacketiser_.timestamp90k()) * 1000000000 /
        kMediaClockHz;
    f.ptpLocked = false;

    std::lock_guard<std::mutex> lock(mutex_);
    latest_.assign(f);
    hasFrame_ = true;
    connected_.store(true);
  }

  NodeConfig config_;
  int fd_ = -1;
  Depacketiser depacketiser_;
  std::thread worker_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> connected_{false};
  std::mutex mutex_;
  FrameBuffer latest_;
  bool hasFrame_ = false;
};

// ---------------------------------------------------------------------------
// Sender
// ---------------------------------------------------------------------------
class St2110Sink final : public Sink {
 public:
  St2110Sink(NodeConfig config, int fd, sockaddr_in destination,
             StreamFormat format)
      : config_(std::move(config)),
        fd_(fd),
        destination_(destination),
        packetiser_(format) {}

  ~St2110Sink() override {
    if (fd_ >= 0) closeSocket(fd_);
    if (frames_ > 0) {
      std::fprintf(stderr, "st2110: %llu frames, %llu packets, %llu send errors\n",
                   (unsigned long long)frames_, (unsigned long long)packets_,
                   (unsigned long long)sendErrors_);
    }
  }

  const std::string& id() const override { return config_.id; }

  /// The 2110 wire format and nothing else. Everything upstream converts to it
  /// explicitly rather than this hiding a conversion.
  std::vector<PixelFormat> preferredFormats() const override {
    return {PixelFormat::ycbcr422_10_pgroup};
  }

  void send(const VideoFrame& frame) override {
    if (!frame.data || frame.format != PixelFormat::ycbcr422_10_pgroup) return;
    if (frame.width != config_.width || frame.height != config_.height) return;

    // The media clock. ST 2110-10 wants this disciplined to PTP on the TAI
    // timeline; this is the system monotonic clock, which is why the SDP
    // declares a wide-profile sender and frames are never marked ptpLocked.
    const auto now = std::chrono::steady_clock::now();
    const int64_t ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - epoch_)
            .count();
    const uint32_t timestamp =
        static_cast<uint32_t>((ns / 1000000000.0) * kMediaClockHz);

    packetiser_.packetise(
        frame.data, frame.strideBytes, timestamp,
        [this](const uint8_t* datagram, int size) {
          const auto sent =
              ::sendto(fd_, reinterpret_cast<const char*>(datagram), size, 0,
                       reinterpret_cast<const sockaddr*>(&destination_),
                       sizeof(destination_));
          if (sent != static_cast<decltype(sent)>(size)) {
            ++sendErrors_;
          } else {
            ++packets_;
          }
        });
    ++frames_;
  }

  void sendBlack() override {
    const int stride =
        tightStrideBytes(PixelFormat::ycbcr422_10_pgroup, config_.width);
    const size_t bytes = static_cast<size_t>(stride) * config_.height;
    if (black_.size() != bytes) {
      black_.assign(bytes, 0);
      fillBlack(PixelFormat::ycbcr422_10_pgroup, config_.width, config_.height,
                stride, QuantRange::narrow, black_.data());
    }
    VideoFrame f;
    f.width = config_.width;
    f.height = config_.height;
    f.strideBytes = stride;
    f.data = black_.data();
    f.format = PixelFormat::ycbcr422_10_pgroup;
    f.colour = ColourSpace::bt709;
    f.range = QuantRange::narrow;
    f.rate = config_.rate;
    send(f);
  }

 private:
  NodeConfig config_;
  int fd_ = -1;
  sockaddr_in destination_{};
  Packetiser packetiser_;
  std::vector<uint8_t> black_;
  std::chrono::steady_clock::time_point epoch_ =
      std::chrono::steady_clock::now();
  uint64_t frames_ = 0;
  uint64_t packets_ = 0;
  uint64_t sendErrors_ = 0;
};

StreamFormat formatFor(const NodeConfig& config) {
  StreamFormat f;
  f.width = config.width;
  f.height = config.height;
  f.rate = config.rate;
  return f;
}

}  // namespace

std::unique_ptr<Source> makeSt2110Source(const NodeConfig& config,
                                         std::string& error) {
  std::string address;
  int port = 0;
  if (!parseEndpoint(config.target, &address, &port, error)) return nullptr;
  if (config.width <= 0 || config.height <= 0) {
    error = "an ST 2110 node needs a width and height — the raster is not "
            "carried in the stream, only in the SDP";
    return nullptr;
  }

  NetInterface nic;
  if (!chooseInterface(config.interfaceSelector, &nic, error)) return nullptr;

  const int fd = openReceiveSocket(address, port, nic, error);
  if (fd < 0) return nullptr;

  std::fprintf(stderr, "st2110: receiving %s:%d on %s, %dx%d %s\n",
               address.c_str(), port,
               nic.name.empty() ? "any interface" : nic.name.c_str(),
               config.width, config.height, config.rate.label().c_str());
  return std::make_unique<St2110Source>(config, fd);
}

std::unique_ptr<Sink> makeSt2110Sink(const NodeConfig& config,
                                     std::string& error) {
  std::string address;
  int port = 0;
  if (!parseEndpoint(config.target, &address, &port, error)) return nullptr;
  if (config.width <= 0 || config.height <= 0) {
    error = "an ST 2110 node needs a width and height";
    return nullptr;
  }

  NetInterface nic;
  if (!chooseInterface(config.interfaceSelector, &nic, error)) return nullptr;

  const int fd = openSendSocket(address, nic, error);
  if (fd < 0) return nullptr;

  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, address.c_str(), &destination.sin_addr);

  std::fprintf(stderr, "st2110: sending to %s:%d from %s, %dx%d %s (wide "
                       "profile, system clock)\n",
               address.c_str(), port,
               nic.address.empty() ? "the default route" : nic.address.c_str(),
               config.width, config.height, config.rate.label().c_str());
  return std::make_unique<St2110Sink>(config, fd, destination,
                                      formatFor(config));
}

std::string st2110Sdp(const NodeConfig& config) {
  std::string address;
  int port = 0;
  std::string error;
  if (!parseEndpoint(config.target, &address, &port, error)) return {};

  NetInterface nic;
  if (!chooseInterface(config.interfaceSelector, &nic, error)) return {};

  return buildSdp(formatFor(config),
                  nic.address.empty() ? "0.0.0.0" : nic.address, address, port,
                  config.label.empty() ? config.id : config.label);
}

}  // namespace ferret

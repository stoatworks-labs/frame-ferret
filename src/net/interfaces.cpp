#include "net/interfaces.h"

#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#ifdef __APPLE__
#include <net/if_dl.h>
#include <net/if_media.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace ferret {
namespace {

std::string presentation(const sockaddr* sa, bool* isV6) {
  if (!sa) return {};
  char buf[INET6_ADDRSTRLEN] = {0};
  if (sa->sa_family == AF_INET) {
    if (isV6) *isV6 = false;
    auto* in = reinterpret_cast<const sockaddr_in*>(sa);
    if (!inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf))) return {};
    return buf;
  }
  if (sa->sa_family == AF_INET6) {
    if (isV6) *isV6 = true;
    auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
    if (!inet_ntop(AF_INET6, &in6->sin6_addr, buf, sizeof(buf))) return {};
    return buf;
  }
  return {};
}

#ifdef __APPLE__
/// Link speed via SIOCGIFMEDIA, for wired Ethernet only.
///
/// The `IFM_TYPE` gate is load-bearing, not defensive. Media subtypes are
/// numbered per media *type*, so the same small integer means 10baseT under
/// IFM_ETHER and an 802.11 modulation under IFM_IEEE80211. Switching on the
/// subtype without checking the type first will confidently report a Wi-Fi
/// interface as a wired link — a wrong number, which is worse than none, since
/// the whole point of surfacing this is to catch a 2110 stream pointed at a
/// NIC that cannot carry it.
///
/// Wi-Fi, tunnel and virtual interfaces therefore report 0, which the CLI
/// prints as "unknown" rather than as zero bandwidth.
uint64_t linkSpeedFor(const char* name) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return 0;
  ifmediareq req;
  std::memset(&req, 0, sizeof(req));
  std::strncpy(req.ifm_name, name, sizeof(req.ifm_name) - 1);
  uint64_t bps = 0;
  if (ioctl(s, SIOCGIFMEDIA, &req) == 0 && (req.ifm_status & IFM_ACTIVE) &&
      IFM_TYPE(req.ifm_active) == IFM_ETHER) {
    switch (IFM_SUBTYPE(req.ifm_active)) {
      case IFM_10_T:    bps = 10ULL * 1000000; break;
      case IFM_100_TX:  bps = 100ULL * 1000000; break;
      case IFM_1000_T:  bps = 1000ULL * 1000000; break;
      case IFM_10G_T:   bps = 10000ULL * 1000000; break;
      case IFM_2500_T:  bps = 2500ULL * 1000000; break;
      case IFM_5000_T:  bps = 5000ULL * 1000000; break;
      default: bps = 0; break;
    }
  }
  close(s);
  return bps;
}
#else
uint64_t linkSpeedFor(const char*) { return 0; }
#endif

}  // namespace

#ifdef _WIN32

std::vector<NetInterface> listInterfaces(std::string& error) {
  std::vector<NetInterface> out;

  ULONG size = 15000;
  std::vector<uint8_t> buf(size);
  ULONG rc = GetAdaptersAddresses(
      AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
  if (rc == ERROR_BUFFER_OVERFLOW) {
    buf.resize(size);
    rc = GetAdaptersAddresses(
        AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
  }
  if (rc != NO_ERROR) {
    error = "GetAdaptersAddresses failed: " + std::to_string(rc);
    return out;
  }

  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a;
       a = a->Next) {
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      NetInterface n;
      n.name = a->AdapterName;
      if (a->FriendlyName) {
        std::wstring w(a->FriendlyName);
        n.displayName.assign(w.begin(), w.end());
      }
      n.address = presentation(u->Address.lpSockaddr, &n.isV6);
      if (n.address.empty()) continue;
      n.index = n.isV6 ? a->Ipv6IfIndex : a->IfIndex;
      n.isLoopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
      n.isUp = a->OperStatus == IfOperStatusUp;
      n.supportsMulticast = !(a->Flags & IP_ADAPTER_NO_MULTICAST);
      n.linkSpeedBps = a->TransmitLinkSpeed == static_cast<ULONG64>(-1)
                           ? 0
                           : a->TransmitLinkSpeed;
      out.push_back(std::move(n));
    }
  }
  return out;
}

#else

std::vector<NetInterface> listInterfaces(std::string& error) {
  std::vector<NetInterface> out;

  ifaddrs* head = nullptr;
  if (getifaddrs(&head) != 0 || !head) {
    error = "getifaddrs failed";
    return out;
  }

  for (ifaddrs* ifa = head; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr) continue;
    const int family = ifa->ifa_addr->sa_family;
    if (family != AF_INET && family != AF_INET6) continue;

    NetInterface n;
    n.name = ifa->ifa_name ? ifa->ifa_name : "";
    n.displayName = n.name;
    n.address = presentation(ifa->ifa_addr, &n.isV6);
    if (n.address.empty()) continue;
    n.netmask = presentation(ifa->ifa_netmask, nullptr);
    n.isLoopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
    n.isUp = (ifa->ifa_flags & IFF_UP) != 0 &&
             (ifa->ifa_flags & IFF_RUNNING) != 0;
    n.supportsMulticast = (ifa->ifa_flags & IFF_MULTICAST) != 0;
    n.index = if_nametoindex(n.name.c_str());
    n.linkSpeedBps = linkSpeedFor(n.name.c_str());
    out.push_back(std::move(n));
  }

  freeifaddrs(head);
  return out;
}

#endif

bool resolveInterface(const std::string& selector,
                      const std::vector<NetInterface>& available,
                      NetInterface* out, std::string& error) {
  if (!out) return false;

  if (selector.empty()) {
    // "Any" is a real, distinct choice — it means INADDR_ANY, not "the first
    // interface we happened to enumerate". Callers check `index == 0` for it.
    *out = NetInterface{};
    out->name = "any";
    out->displayName = "Any (OS default route)";
    out->isUp = true;
    out->supportsMulticast = true;
    return true;
  }

  // Exact address first: an address is unambiguous, a name may carry several.
  for (const auto& n : available) {
    if (n.address == selector) {
      *out = n;
      return true;
    }
  }

  // Then by name, preferring an IPv4 address on that interface. IPv6 is picked
  // only when the interface has no v4 — every transport here defaults to v4,
  // and silently handing back a link-local v6 produces a bind that succeeds
  // and a stream nothing can reach.
  const NetInterface* v6Fallback = nullptr;
  for (const auto& n : available) {
    if (n.name != selector && n.displayName != selector) continue;
    if (!n.isV6) {
      *out = n;
      return true;
    }
    if (!v6Fallback) v6Fallback = &n;
  }
  if (v6Fallback) {
    *out = *v6Fallback;
    return true;
  }

  error = "no interface matches '" + selector +
          "' — run `frame-ferret interfaces` to list what this machine has";
  return false;
}

bool isMulticast(const std::string& address) {
  if (address.empty()) return false;

  in_addr v4{};
  if (inet_pton(AF_INET, address.c_str(), &v4) == 1) {
    const uint32_t host = ntohl(v4.s_addr);
    return (host >> 28) == 0xE;  // 224.0.0.0/4
  }

  in6_addr v6{};
  if (inet_pton(AF_INET6, address.c_str(), &v6) == 1) {
    return v6.s6_addr[0] == 0xFF;  // ff00::/8
  }

  return false;
}

}  // namespace ferret

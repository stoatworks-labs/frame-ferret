#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ferret {

/// One usable IP interface on this machine.
struct NetInterface {
  std::string name;         ///< "en0", "Ethernet 2".
  std::string displayName;  ///< Friendly name where the OS has one.
  std::string address;      ///< Presentation form of the bound address.
  std::string netmask;
  bool isV6 = false;
  bool isLoopback = false;
  bool isUp = false;
  bool supportsMulticast = false;

  /// The scope/adapter index. This — not the address — is what a multicast
  /// join must be keyed on: two interfaces can hold the same address after a
  /// DHCP reshuffle, and on IPv6 a link-local address is *meaningless* without
  /// it.
  uint32_t index = 0;

  /// Link speed in bits/s where the OS reports it, else 0. Surfaced because
  /// ST 2110-20 at 1080p50 needs ~2.6 Gb/s and silently selecting a 1 GbE
  /// interface for it is a configuration error worth catching at bind time
  /// rather than at showtime.
  uint64_t linkSpeedBps = 0;
};

/// Every interface the OS will let us bind, loopback included. Order is the
/// OS's own; callers that want a stable UI order should sort.
std::vector<NetInterface> listInterfaces(std::string& error);

/// Resolves a user's interface selection to a concrete interface.
///
/// Accepts an interface name ("en0"), a literal address ("10.0.0.4"), or the
/// empty string for "let the OS choose". Returns false and sets `error` if the
/// selector matches nothing — never silently falls back to the default route,
/// because a 2110 sender that quietly leaves on the wrong NIC is indis-
/// tinguishable from a network fault and is usually diagnosed as one.
bool resolveInterface(const std::string& selector,
                      const std::vector<NetInterface>& available,
                      NetInterface* out, std::string& error);

/// Whether an address is in a multicast range (224/4 or ff00::/8).
bool isMulticast(const std::string& address);

}  // namespace ferret

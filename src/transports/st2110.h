#pragma once

#include <memory>
#include <string>

#include "app/node.h"

namespace ferret {

/// ST 2110-20 uncompressed video over RTP.
///
/// `config.target` is `address:port`, usually an admin-scoped multicast group:
///     239.10.10.1:20000
///
/// **The interface selector is honoured here, and matters more than anywhere
/// else in this program.** Multicast send needs an explicit egress interface
/// (`IP_MULTICAST_IF`) and a join needs the interface *index* rather than its
/// address — two interfaces can hold the same address after a DHCP reshuffle.
/// NDI and OMT cannot bind at all; SRT can; 2110 must.
///
/// Bandwidth is not incidental: 1080p50 is about 2.2 Gb/s and 2160p50 about
/// 8.7 Gb/s. `frame-ferret interfaces` prints those next to each NIC's link
/// speed for exactly this reason.
///
/// **This is a wide-profile sender (2110TPW), declared honestly in the SDP.**
/// Narrow needs hardware transmit pacing, and macOS has no supported hardware
/// timestamping path for PTP at all — see docs/02-st2110.md. The media clock
/// here is derived from the system clock, not from a PTP servo, and frames are
/// marked `ptpLocked = false` so nothing downstream mistakes it for one.
std::unique_ptr<Source> makeSt2110Source(const NodeConfig& config,
                                         std::string& error);

std::unique_ptr<Sink> makeSt2110Sink(const NodeConfig& config,
                                     std::string& error);

/// The SDP a receiver needs for this node, or empty if the config is unusable.
/// Surfaced through the control API because pasting an SDP is how most 2110
/// equipment is configured.
std::string st2110Sdp(const NodeConfig& config);

/// 2110 binds properly, unlike NDI and OMT.
constexpr bool kSt2110SupportsInterfaceBinding = true;

}  // namespace ferret

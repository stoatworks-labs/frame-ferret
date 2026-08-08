#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/node.h"
#include "app/router.h"
#include "net/interfaces.h"

namespace {

/// Bits per second needed for uncompressed ST 2110-20, so that `interfaces`
/// can say whether a NIC is fast enough for the raster the operator wants.
/// 10-bit 4:2:2 is 20 bits per pixel of active picture; the 1.05 allows for
/// RTP/UDP/IP/Ethernet headers and the 2110-21 sender leaky-bucket margin.
double bitrate2110(int w, int h, const ferret::Rate& rate) {
  return static_cast<double>(w) * h * 20.0 * rate.approx() * 1.05;
}

std::string humanBps(uint64_t bps) {
  if (bps == 0) return "unknown";
  char buf[64];
  if (bps >= 1000000000ULL) {
    std::snprintf(buf, sizeof(buf), "%.0f Gb/s", bps / 1e9);
  } else {
    std::snprintf(buf, sizeof(buf), "%.0f Mb/s", bps / 1e6);
  }
  return buf;
}

int cmdInterfaces() {
  std::string error;
  auto found = ferret::listInterfaces(error);
  if (found.empty()) {
    std::fprintf(stderr, "could not enumerate interfaces: %s\n",
                 error.empty() ? "none found" : error.c_str());
    return 1;
  }

  // Sort so the output is stable between runs and useful first: real, up,
  // multicast-capable interfaces before loopback and down ones.
  std::sort(found.begin(), found.end(),
            [](const ferret::NetInterface& a, const ferret::NetInterface& b) {
              if (a.isLoopback != b.isLoopback) return b.isLoopback;
              if (a.isUp != b.isUp) return a.isUp;
              if (a.isV6 != b.isV6) return b.isV6;
              return a.name < b.name;
            });

  std::printf("%-10s %-40s %-8s %-10s %s\n", "NAME", "ADDRESS", "STATE",
              "MCAST", "LINK");
  for (const auto& n : found) {
    std::printf("%-10s %-40s %-8s %-10s %s%s\n", n.name.c_str(),
                n.address.c_str(), n.isUp ? "up" : "down",
                n.supportsMulticast ? "yes" : "no",
                humanBps(n.linkSpeedBps).c_str(),
                n.isLoopback ? "  (loopback)" : "");
  }

  // The 2110 headline, because it is the constraint most likely to be missed
  // until the day of the show.
  const ferret::Rate r{50, 1};
  std::printf(
      "\nST 2110-20 uncompressed needs about %.1f Gb/s for 1080p50 and "
      "%.1f Gb/s for 2160p50.\n"
      "A 1 GbE interface cannot carry either. See docs/02-st2110.md.\n",
      bitrate2110(1920, 1080, r) / 1e9, bitrate2110(3840, 2160, r) / 1e9);

  return 0;
}

int cmdKinds() {
  std::printf("%-16s %-8s %s\n", "KIND", "SOURCE", "SINK");
  for (int i = 0; i <= static_cast<int>(ferret::NodeKind::htmlOverlay); ++i) {
    auto k = static_cast<ferret::NodeKind>(i);
    std::printf("%-16s %-8s %s\n", ferret::toString(k),
                ferret::canSource(k) ? "yes" : "-",
                ferret::canSink(k) ? "yes" : "-");
  }
  return 0;
}

int usage() {
  std::printf(
      "Frame Ferret %s — a virtual capture card.\n"
      "\n"
      "Usage: frame-ferret <command>\n"
      "\n"
      "  interfaces   List the NICs available for binding, with link speed\n"
      "  kinds        List node kinds and the directions each supports\n"
      "  version      Print the version\n"
      "\n"
      "Not yet implemented: run, sources, uvc, selftest. See docs/ROADMAP.md\n"
      "for what is built and what is not — nothing here has been run against\n"
      "hardware yet.\n",
      FERRET_VERSION);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();

  const std::string cmd = argv[1];
  if (cmd == "interfaces") return cmdInterfaces();
  if (cmd == "kinds") return cmdKinds();
  if (cmd == "version") {
    std::printf("%s\n", FERRET_VERSION);
    return 0;
  }
  if (cmd == "-h" || cmd == "--help" || cmd == "help") return usage();

  std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
  usage();
  return 1;
}

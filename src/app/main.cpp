#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "app/config.h"
#include "app/engine.h"
#include "app/factory.h"
#include "app/main_loop.h"
#include "app/node.h"
#include "app/router.h"
#include "control/control_api.h"
#include "control/http_server.h"
#include "net/interfaces.h"
#include "transports/ndi.h"
#include "sinks/decklink.h"
#include "transports/omt.h"

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop.store(true); }

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

/// Builds the engine from `config` and reports what could not be built.
bool prepare(const ferret::AppConfig& config, ferret::Engine& engine,
             std::vector<ferret::NodeFailure>& failures,
             std::vector<ferret::NodeFailure>& warnings,
             std::vector<ferret::PreviewSink*>& previews) {
  std::string error;
  if (!ferret::buildNodes(config, engine, failures, warnings, previews,
                          error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return false;
  }
  for (const auto& f : failures) {
    std::fprintf(stderr, "  unavailable: %s — %s\n", f.id.c_str(),
                 f.reason.c_str());
  }
  for (const auto& w : warnings) {
    std::fprintf(stderr, "  warning: %s — %s\n", w.id.c_str(),
                 w.reason.c_str());
  }
  return true;
}

int cmdRun(const std::string& configPath) {
  ferret::AppConfig config;
  if (configPath.empty()) {
    config = ferret::selftestConfig();
    config.controlPort = 8740;
    std::printf(
        "No --config given, so running the built-in colour-bars "
        "configuration.\n");
  } else {
    std::string error;
    if (!ferret::loadConfigFile(configPath, &config, error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
  }

  ferret::Engine engine;
  std::vector<ferret::NodeFailure> failures, warnings;
  std::vector<ferret::PreviewSink*> previews;
  if (!prepare(config, engine, failures, warnings, previews)) return 1;

  std::string error;
  if (!engine.start(error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  ferret::ControlApi api(engine, config, failures, warnings, previews);
  ferret::HttpServer server;
  if (config.controlPort > 0) {
    if (!ferret::HttpServer::portAvailable(config.controlBind,
                                           config.controlPort)) {
      // Checked before binding because the usual cause is a second copy of
      // this program already running, and that is worth saying plainly rather
      // than leaving as a bind error.
      std::fprintf(stderr,
                   "control port %s:%d is already in use — is Frame Ferret "
                   "already running?\n",
                   config.controlBind.c_str(), config.controlPort);
      engine.stop();
      return 1;
    }
    if (!server.start(config.controlBind, config.controlPort,
                      config.controlToken,
                      [&api](const ferret::HttpServer::Request& request,
                             ferret::HttpServer::Response& response) {
                        api.handle(request, response);
                      },
                      error)) {
      std::fprintf(stderr, "control server: %s\n", error.c_str());
      engine.stop();
      return 1;
    }
    std::printf("Control page: http://%s:%d/\n", config.controlBind.c_str(),
                server.port());
  }

  // Signal handlers go in HERE, after prepare() has built every node — never
  // earlier. libomt embeds the .NET runtime, and .NET installs its own
  // SIGINT/SIGTERM handlers when it starts, which is on first OMT
  // sender/receiver creation inside buildNodes. Anything registered before
  // that is silently overwritten: Ctrl-C stops working and the process lingers
  // holding OMT's port, after which the *next* sender announces itself while
  // the zombie still owns it, so receivers connect to a source that never
  // sends. Moving these two lines above prepare() reintroduces all of that.
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::printf("Running at %s fps. Ctrl-C to stop.\n",
              config.rate.label().c_str());

  // waitServicingMainLoop, never sleep_for. On macOS the Syphon sink creates
  // its server with a dispatch_sync onto the main queue, and that only
  // completes if the main thread is inside a run loop — a plain sleep here
  // deadlocks the frame thread on its first Syphon frame. See app/main_loop.h.
  while (!g_stop.load()) {
    ferret::waitServicingMainLoop(0.1);
  }

  std::printf("\nStopping.\n");
  server.stop();
  engine.stop();

  const auto counters = engine.counters();
  std::printf("%llu ticks, %llu frames, %llu black, %llu late\n",
              static_cast<unsigned long long>(counters.ticks),
              static_cast<unsigned long long>(counters.framesDelivered),
              static_cast<unsigned long long>(counters.blackDelivered),
              static_cast<unsigned long long>(counters.lateTicks));
  return 0;
}

/// Drives colour bars into a preview for a few seconds and checks that frames
/// actually arrived, actually advanced, and were not black.
///
/// The "advanced" part is the one that matters: a frozen output passes any
/// test that only counts frames, and freezing is the failure mode this whole
/// program is built to avoid.
int cmdSelftest() {
  ferret::AppConfig config = ferret::selftestConfig();

  ferret::Engine engine;
  std::vector<ferret::NodeFailure> failures, warnings;
  std::vector<ferret::PreviewSink*> previews;
  if (!prepare(config, engine, failures, warnings, previews)) return 1;

  if (previews.empty()) {
    std::fprintf(stderr, "selftest: no preview sink was built\n");
    return 1;
  }

  std::string error;
  if (!engine.start(error)) {
    std::fprintf(stderr, "selftest: %s\n", error.c_str());
    return 1;
  }

  const int seconds = 3;
  std::vector<uint8_t> first, last;

  // Wait for the first real frame before sampling. Sampling immediately after
  // start() catches the engine before its first tick, and an empty buffer then
  // compares unequal to everything — which would make the "picture advanced"
  // check below pass for the wrong reason forever.
  bool gotFirst = false;
  for (int i = 0; i < 200 && !gotFirst; ++i) {
    gotFirst = previews[0]->encodeBmp(first);
    if (!gotFirst) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  const bool gotLast = previews[0]->encodeBmp(last);

  const auto counters = engine.counters();
  const auto stats = previews[0]->stats();
  engine.stop();

  std::printf("ticks           %llu\n",
              static_cast<unsigned long long>(counters.ticks));
  std::printf("frames          %llu\n",
              static_cast<unsigned long long>(counters.framesDelivered));
  std::printf("black           %llu\n",
              static_cast<unsigned long long>(counters.blackDelivered));
  std::printf("late ticks      %llu\n",
              static_cast<unsigned long long>(counters.lateTicks));
  std::printf("measured fps    %.2f\n", counters.measuredFps);
  std::printf("preview         %dx%d %s, %llu frames\n", stats.width,
              stats.height, stats.format.c_str(),
              static_cast<unsigned long long>(stats.frames));

  int failed = 0;
  auto check = [&failed](bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failed;
  };

  const double expected = config.rate.approx() * seconds;
  check(counters.framesDelivered > 0, "frames were delivered");
  check(counters.blackDelivered == 0, "no black frames were delivered");
  check(gotFirst && gotLast, "the preview produced an image");
  check(counters.framesDelivered > expected * 0.8,
        "frame rate held within 20% of nominal");
  // The moving marker means two samples three seconds apart must differ. A
  // frozen source passes every other check here, and freezing is the failure
  // mode this whole program is built to avoid — so both samples must be real.
  check(gotFirst && gotLast && first != last,
        "the picture advanced between samples");

  std::printf("%s\n", failed == 0 ? "selftest PASSED" : "selftest FAILED");
  return failed == 0 ? 0 : 1;
}

int cmdSources(const std::string& protocol) {
  int failures = 0;

  if (protocol.empty() || protocol == "ndi") {
    std::string error;
    if (!ferret::NdiRuntime::available()) {
      std::printf("ndi: unavailable — %s\n",
                  ferret::NdiRuntime::unavailableReason().c_str());
      ++failures;
    } else {
      const auto sources = ferret::ndiListSources(2000, error);
      std::printf("ndi (%s):\n", ferret::NdiRuntime::loadedPath().c_str());
      if (sources.empty()) std::printf("  (none visible)\n");
      for (const auto& s : sources) std::printf("  %s\n", s.c_str());
    }
  }

  if (protocol.empty() || protocol == "omt") {
    std::string error;
    if (!ferret::OmtRuntime::available()) {
      std::printf("omt: unavailable — %s\n",
                  ferret::OmtRuntime::unavailableReason().c_str());
      ++failures;
    } else {
      const auto sources = ferret::omtListSources(error);
      std::printf("omt (%s):\n", ferret::OmtRuntime::loadedPath().c_str());
      if (sources.empty()) {
        std::printf("  (none visible)%s\n",
                    error.empty() ? "" : (" — " + error).c_str());
        std::printf(
            "  OMT discovery lists only senders it has learned about. A "
            "receiver can always connect directly with omt://host:port.\n");
      }
      for (const auto& s : sources) std::printf("  %s\n", s.c_str());
    }
  }

  if (protocol.empty() || protocol == "decklink") {
    if (!ferret::DeckLinkRuntime::builtIn()) {
      std::printf("decklink: %s\n",
                  ferret::DeckLinkRuntime::unavailableReason().c_str());
    } else {
      const auto devices = ferret::DeckLinkRuntime::listDevices();
      std::printf("decklink:\n");
      if (devices.empty()) {
        std::printf("  (none) — %s\n",
                    ferret::DeckLinkRuntime::unavailableReason().c_str());
      }
      for (size_t i = 0; i < devices.size(); ++i) {
        std::printf("  %zu: %s\n", i, devices[i].c_str());
      }
    }
  }

  return failures > 0 && protocol.empty() ? 0 : (failures ? 1 : 0);
}

int usage() {
  std::printf(
      "Frame Ferret %s — a virtual capture card.\n"
      "\n"
      "Usage: frame-ferret <command> [options]\n"
      "\n"
      "  run [--config <file>]   Run the engine and serve the control page\n"
      "  selftest                Drive colour bars through the frame path\n"
      "  sources [--protocol X]  List NDI and OMT sources visible now\n"
      "  interfaces              List NICs available for binding, with speed\n"
      "  kinds                   List node kinds and the directions each takes\n"
      "  version                 Print the version\n"
      "\n"
      "`run` with no config serves the built-in colour-bars configuration on\n"
      "http://127.0.0.1:8740/.\n"
      "\n"
      "Only the test pattern and preview nodes exist in this build. Every\n"
      "transport, capture source and hardware output is designed but not\n"
      "implemented — see docs/ROADMAP.md.\n",
      FERRET_VERSION);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();

  const std::string cmd = argv[1];
  if (cmd == "interfaces") return cmdInterfaces();
  if (cmd == "kinds") return cmdKinds();
  if (cmd == "selftest") return cmdSelftest();
  if (cmd == "sources") {
    std::string protocol;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--protocol" && i + 1 < argc) protocol = argv[++i];
    }
    return cmdSources(protocol);
  }
  if (cmd == "run") {
    std::string configPath;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
        configPath = argv[++i];
      } else {
        std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
        return 1;
      }
    }
    return cmdRun(configPath);
  }
  if (cmd == "version") {
    std::printf("%s\n", FERRET_VERSION);
    return 0;
  }
  if (cmd == "-h" || cmd == "--help" || cmd == "help") return usage();

  std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
  usage();
  return 1;
}

#include "transports/srt_socket.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "check.h"

using namespace ferret;

namespace {

void urlsParseIntoEveryField() {
  SrtConfig c;
  std::string err;

  CHECK(parseSrtUrl("srt://10.0.0.5:9001", &c, err));
  CHECK_EQ(c.host, std::string("10.0.0.5"));
  CHECK_EQ(c.port, 9001);
  CHECK_EQ(static_cast<int>(c.mode), static_cast<int>(SrtMode::caller));
  CHECK_EQ(c.latencyMs, 120);

  // The scheme is optional.
  CHECK(parseSrtUrl("127.0.0.1:5000", &c, err));
  CHECK_EQ(c.port, 5000);

  CHECK(parseSrtUrl(
      "srt://1.2.3.4:9000?mode=listener&latency=300&streamid=live/1", &c, err));
  CHECK_EQ(static_cast<int>(c.mode), static_cast<int>(SrtMode::listener));
  CHECK_EQ(c.latencyMs, 300);
  CHECK_EQ(c.streamId, std::string("live/1"));
}

/// An unknown parameter must be an error, not something quietly dropped. A
/// misspelled "latencey=" that left the buffer at its default is diagnosed on
/// site as a flaky link, which is an expensive way to find a typo.
void unknownParametersAreRejected() {
  SrtConfig c;
  std::string err;

  CHECK(!parseSrtUrl("srt://1.2.3.4:9000?latencey=300", &c, err));
  CHECK(err.find("latencey") != std::string::npos);
  CHECK(err.find("mode, latency, passphrase, streamid") != std::string::npos);

  err.clear();
  CHECK(!parseSrtUrl("srt://1.2.3.4:9000?mode=sideways", &c, err));
  CHECK(err.find("sideways") != std::string::npos);
}

void malformedAddressesAreRejected() {
  SrtConfig c;
  std::string err;

  CHECK(!parseSrtUrl("", &c, err));
  CHECK(!parseSrtUrl("srt://", &c, err));
  CHECK(!parseSrtUrl("srt://1.2.3.4", &c, err));       // no port
  CHECK(!parseSrtUrl("srt://1.2.3.4:0", &c, err));     // port 0
  CHECK(!parseSrtUrl("srt://1.2.3.4:70000", &c, err)); // out of range
  CHECK(!parseSrtUrl("srt://1.2.3.4:abc", &c, err));
  CHECK(!parseSrtUrl("srt://1.2.3.4:9000?novalue", &c, err));
}

void modesRoundTrip() {
  SrtMode m;
  CHECK(srtModeFromString("caller", &m));
  CHECK_EQ(std::string(toString(m)), std::string("caller"));
  CHECK(srtModeFromString("listener", &m));
  CHECK_EQ(std::string(toString(m)), std::string("listener"));
  CHECK(srtModeFromString("rendezvous", &m));
  CHECK_EQ(std::string(toString(m)), std::string("rendezvous"));
  CHECK(!srtModeFromString("nonsense", &m));
}

/// A real SRT connection over loopback: a listener and a caller, with bytes
/// crossing between them. Skipped with a printed note when libsrt is absent,
/// which is the case on every CI runner.
void aRealLoopbackConnectionCarriesBytes() {
  if (!SrtRuntime::available()) {
    std::printf("  libsrt not installed — loopback test skipped (%s)\n",
                SrtRuntime::unavailableReason().substr(0, 60).c_str());
    return;
  }
  std::printf("  libsrt %s at %s\n", SrtRuntime::version().c_str(),
              SrtRuntime::loadedPath().c_str());

  // A port unlikely to collide with anything else on this machine.
  const int port = 19472;

  std::atomic<bool> listenerReady{false};
  std::atomic<int> received{0};
  std::atomic<bool> listenerFailed{false};
  std::string listenerError;

  std::thread listener([&] {
    SrtConfig config;
    config.mode = SrtMode::listener;
    config.host = "127.0.0.1";
    config.port = port;
    config.connectTimeoutMs = 5000;

    listenerReady.store(true);
    auto connection = srtConnect(config, listenerError);
    if (!connection) {
      listenerFailed.store(true);
      return;
    }

    uint8_t buffer[2048];
    for (int i = 0; i < 40 && received.load() == 0; ++i) {
      const int n = connection->receive(buffer, sizeof(buffer), 250);
      if (n > 0) {
        // Check the payload really is ours, not a partial or a stray packet.
        if (n == 7 && std::memcmp(buffer, "FERRET!", 7) == 0) {
          received.store(n);
        }
        break;
      }
      if (n < 0) break;
    }
  });

  while (!listenerReady.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  // The listener has to reach srt_accept before the caller connects.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  SrtConfig callerConfig;
  callerConfig.mode = SrtMode::caller;
  callerConfig.host = "127.0.0.1";
  callerConfig.port = port;
  callerConfig.connectTimeoutMs = 3000;

  std::string callerError;
  auto caller = srtConnect(callerConfig, callerError);

  if (!caller) {
    std::printf("  caller could not connect: %s\n", callerError.c_str());
  }
  CHECK(caller != nullptr);

  if (caller) {
    CHECK(caller->connected());
    CHECK(caller->describe().find("caller") != std::string::npos);
    CHECK_EQ(caller->payloadSize(), 1316);

    const uint8_t payload[7] = {'F', 'E', 'R', 'R', 'E', 'T', '!'};
    CHECK(caller->send(payload, sizeof(payload)));
  }

  listener.join();

  if (listenerFailed.load()) {
    std::printf("  listener failed: %s\n", listenerError.c_str());
  }
  CHECK(!listenerFailed.load());
  CHECK_EQ(received.load(), 7);
}

/// A passphrase shorter than libsrt's minimum must be refused here, with an
/// explanation. Passed through, it produces a handshake failure that says
/// nothing about the passphrase.
void aShortPassphraseIsRefusedWithAnExplanation() {
  if (!SrtRuntime::available()) return;

  SrtConfig config;
  config.mode = SrtMode::caller;
  config.host = "127.0.0.1";
  config.port = 19473;
  config.passphrase = "short";  // 5 characters, minimum is 10
  config.connectTimeoutMs = 500;

  std::string error;
  auto connection = srtConnect(config, error);
  CHECK(connection == nullptr);
  CHECK(error.find("10 to 79") != std::string::npos);
}

/// An interface selector that matches nothing must fail rather than quietly
/// leaving on the default route — the same rule the rest of the program holds,
/// and SRT is the first transport that can actually honour the setting.
void anUnknownInterfaceFailsRatherThanFallingBack() {
  if (!SrtRuntime::available()) return;

  SrtConfig config;
  config.mode = SrtMode::caller;
  config.host = "127.0.0.1";
  config.port = 19474;
  config.interfaceSelector = "en99-does-not-exist";
  config.connectTimeoutMs = 500;

  std::string error;
  auto connection = srtConnect(config, error);
  CHECK(connection == nullptr);
  CHECK(error.find("en99-does-not-exist") != std::string::npos);
}

void run() {
  urlsParseIntoEveryField();
  unknownParametersAreRejected();
  malformedAddressesAreRejected();
  modesRoundTrip();
  aRealLoopbackConnectionCarriesBytes();
  aShortPassphraseIsRefusedWithAnExplanation();
  anUnknownInterfaceFailsRatherThanFallingBack();
}

}  // namespace

TEST_MAIN("srt")

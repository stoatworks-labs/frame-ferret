#include "app/router.h"

#include "check.h"

using namespace ferret;

namespace {

Router twoByTwo() {
  Router r;
  r.addSource("cam", PixelFormat::uyvy8);
  r.addSource("desk", PixelFormat::bgra8);
  // A DeckLink takes 4:2:2 natively and nothing else.
  r.addSink("sdi", {PixelFormat::v210, PixelFormat::uyvy8});
  // A Syphon server converts internally, so it accepts anything.
  r.addSink("syphon", {});
  return r;
}

/// The invariant: one action per sink, every time, whatever the state.
void everySinkIsAlwaysPlanned() {
  Router r = twoByTwo();
  CHECK_EQ(r.plan().size(), size_t{2});

  std::string err;
  CHECK(r.route("sdi", "cam", err));
  CHECK_EQ(r.plan().size(), size_t{2});

  r.setSourceConnected("cam", true);
  CHECK_EQ(r.plan().size(), size_t{2});

  r.setMuted(true);
  CHECK_EQ(r.plan().size(), size_t{2});
}

/// The four distinct roads to black, each with its own reason. An operator
/// staring at a black output should be told which one it is.
void blackAlwaysCarriesAReason() {
  Router r = twoByTwo();
  std::string err;

  // 1. Nothing routed.
  auto p = r.plan();
  CHECK_EQ(p[0].what, RouteAction::What::black);
  CHECK(p[0].reason.find("no source routed") != std::string::npos);

  // 2. Routed but the source has no signal.
  CHECK(r.route("sdi", "cam", err));
  p = r.plan();
  for (const auto& a : p) {
    if (a.sinkId != "sdi") continue;
    CHECK_EQ(a.what, RouteAction::What::black);
    CHECK(a.reason.find("not connected") != std::string::npos);
    // The routed source is still reported, so the UI can show the intended
    // route greyed rather than showing the sink as unrouted.
    CHECK_EQ(a.sourceId, std::string("cam"));
  }

  // 3. Globally muted — and the route survives it.
  r.setSourceConnected("cam", true);
  r.setMuted(true);
  p = r.plan();
  CHECK_EQ(p[0].what, RouteAction::What::black);
  CHECK(p[0].reason.find("muted") != std::string::npos);
  CHECK_EQ(r.routedSource("sdi"), std::string("cam"));

  r.setMuted(false);
  for (const auto& a : r.plan()) {
    if (a.sinkId == "sdi") CHECK(a.what != RouteAction::What::black);
  }
}

void copyWhenTheSinkTakesTheNativeFormat() {
  Router r = twoByTwo();
  std::string err;
  CHECK(r.route("sdi", "cam", err));  // cam is uyvy8, sdi accepts uyvy8
  r.setSourceConnected("cam", true);

  for (const auto& a : r.plan()) {
    if (a.sinkId != "sdi") continue;
    CHECK_EQ(a.what, RouteAction::What::copy);
    CHECK_EQ(a.targetFormat, PixelFormat::uyvy8);
  }
}

void convertPicksTheSinksFirstPreference() {
  Router r = twoByTwo();
  std::string err;
  CHECK(r.route("sdi", "desk", err));  // desk is bgra8, sdi takes neither
  r.setSourceConnected("desk", true);

  for (const auto& a : r.plan()) {
    if (a.sinkId != "sdi") continue;
    CHECK_EQ(a.what, RouteAction::What::convert);
    // v210 is listed first, so the 10-bit path wins over the 8-bit one. If
    // this ever picks uyvy8 the router is silently throwing away two bits.
    CHECK_EQ(a.targetFormat, PixelFormat::v210);
  }
}

/// A sink with an empty accepts list converts internally, so anything is a
/// copy as far as the router is concerned.
void anAcceptEverythingSinkIsAlwaysACopy() {
  Router r = twoByTwo();
  std::string err;
  CHECK(r.route("syphon", "cam", err));
  r.setSourceConnected("cam", true);

  for (const auto& a : r.plan()) {
    if (a.sinkId != "syphon") continue;
    CHECK_EQ(a.what, RouteAction::What::copy);
    CHECK_EQ(a.targetFormat, PixelFormat::uyvy8);
  }
}

void oneSourceCanFeedManySinks() {
  Router r = twoByTwo();
  std::string err;
  CHECK(r.route("sdi", "cam", err));
  CHECK(r.route("syphon", "cam", err));
  r.setSourceConnected("cam", true);

  int fed = 0;
  for (const auto& a : r.plan()) {
    if (a.sourceId == "cam" && a.what != RouteAction::What::black) ++fed;
  }
  CHECK_EQ(fed, 2);
}

void badRoutesAreRejectedNotStored() {
  Router r = twoByTwo();
  std::string err;

  CHECK(!r.route("nosuchsink", "cam", err));
  CHECK(err.find("no such sink") != std::string::npos);

  err.clear();
  CHECK(!r.route("sdi", "nosuchsource", err));
  CHECK(err.find("no such source") != std::string::npos);

  // And the sink is still unrouted, not half-routed.
  CHECK_EQ(r.routedSource("sdi"), std::string());
}

void clearingARouteIsNotAnError() {
  Router r = twoByTwo();
  std::string err;
  CHECK(r.route("sdi", "cam", err));
  CHECK(r.route("sdi", "", err));
  CHECK_EQ(r.routedSource("sdi"), std::string());
  CHECK_EQ(r.plan().size(), size_t{2});
}

void run() {
  everySinkIsAlwaysPlanned();
  blackAlwaysCarriesAReason();
  copyWhenTheSinkTakesTheNativeFormat();
  convertPicksTheSinksFirstPreference();
  anAcceptEverythingSinkIsAlwaysACopy();
  oneSourceCanFeedManySinks();
  badRoutesAreRejectedNotStored();
  clearingARouteIsNotAnError();
}

}  // namespace

TEST_MAIN("router")

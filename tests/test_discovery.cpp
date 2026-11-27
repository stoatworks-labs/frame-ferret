#include "core/discovery.h"

#include <algorithm>
#include <string>
#include <vector>

#include "check.h"

using namespace ferret;
using std::chrono::milliseconds;

namespace {

// A finder on a fake clock that behaves like NDI's: a wait returns as soon as
// the list changes, otherwise after the time asked for. `arrivals` is when each
// source becomes known, in ms from the start.
struct FakeFinder {
  struct Arrival {
    long at;
    std::string name;
  };
  std::vector<Arrival> arrivals;
  DiscoveryClock::time_point start{};
  DiscoveryClock::time_point t{};
  int waits = 0;
  int zeroWaits = 0;

  DiscoveryClock::time_point now() const { return t; }
  long elapsed() const {
    return (long)std::chrono::duration_cast<milliseconds>(t - start).count();
  }

  std::vector<std::string> current() const {
    std::vector<std::string> out;
    for (const auto& a : arrivals)
      if (a.at <= elapsed()) out.push_back(a.name);
    return out;
  }

  void wait(milliseconds ms) {
    ++waits;
    if (ms.count() <= 0) ++zeroWaits;
    const long until = elapsed() + (long)ms.count();
    long next = until;
    for (const auto& a : arrivals)
      if (a.at > elapsed() && a.at < next) next = a.at;
    t = start + milliseconds(next);
  }

  bool has(const std::string& name) const {
    const auto c = current();
    return std::find(c.begin(), c.end(), name) != c.end();
  }
};

bool listen(FakeFinder& f, milliseconds window, std::function<bool()> satisfied) {
  return waitForDiscovery(
      window, [&] { return f.now(); }, [&](milliseconds ms) { f.wait(ms); },
      satisfied);
}

// The case that failed on the LAN: this machine's own sender is known at once,
// the one on the other machine 300 ms later.
FakeFinder localThenRemote() {
  FakeFinder f;
  f.arrivals = {{0, "MAC (macOS AV Output)"}, {300, "WIN-LAB (FFWIN NDI)"}};
  return f;
}

void oneWaitStopsAtTheFirstChange() {
  // What the code did before: one wait, then pick. Starting with nothing known,
  // the first change is the local sender, and the remote one is not there yet.
  FakeFinder f = localThenRemote();
  f.arrivals[0].at = 5;
  f.wait(milliseconds(2000));
  CHECK_EQ(f.elapsed(), 5L);
  CHECK(f.has("MAC (macOS AV Output)"));
  CHECK(!f.has("WIN-LAB (FFWIN NDI)"));
}

void aRemoteSourceAfterALocalOneIsFound() {
  FakeFinder f = localThenRemote();
  const bool found =
      listen(f, milliseconds(5000), [&] { return f.has("WIN-LAB (FFWIN NDI)"); });
  CHECK(found);
  // And as soon as it arrives, not at the end of the window.
  CHECK_EQ(f.elapsed(), 300L);
}

void aSourceAlreadyThereCostsNoWait() {
  FakeFinder f = localThenRemote();
  const bool found =
      listen(f, milliseconds(5000), [&] { return f.has("MAC (macOS AV Output)"); });
  CHECK(found);
  CHECK_EQ(f.waits, 0);
  CHECK_EQ(f.elapsed(), 0L);
}

void aMissingSourceGivesUpAtTheWindow() {
  FakeFinder f = localThenRemote();
  const bool found =
      listen(f, milliseconds(2000), [&] { return f.has("NOBODY (Here)"); });
  CHECK(!found);
  CHECK_EQ(f.elapsed(), 2000L);
}

void aListingListensForTheWholeWindow() {
  // No predicate: a listing has nothing to stop early for, and stopping at the
  // first change is exactly how `sources --protocol ndi` showed only the Mac's
  // own senders with a Windows one on the network.
  FakeFinder f = localThenRemote();
  f.arrivals.push_back({1500, "WIN-LAB (FFWIN PAD)"});
  const bool found = listen(f, milliseconds(2000), nullptr);
  CHECK(!found);
  CHECK_EQ(f.elapsed(), 2000L);
  CHECK_EQ(f.current().size(), size_t{3});
}

void aChattyNetworkStillEndsAtTheWindow() {
  // A change every millisecond makes every wait return early. The loop must
  // still finish at the window rather than run as long as the chatter lasts.
  FakeFinder f;
  for (long ms = 1; ms <= 10000; ++ms)
    f.arrivals.push_back({ms, "S" + std::to_string(ms)});
  const bool found = listen(f, milliseconds(50), nullptr);
  CHECK(!found);
  CHECK_EQ(f.elapsed(), 50L);
  CHECK_EQ(f.waits, 50);
}

void aSubMillisecondRemainderIsNotAZeroWait() {
  // Half a millisecond left must wait 1 ms, not 0: NDI answers a wait of 0 at
  // once, and the loop would spin until the clock moved past the deadline.
  FakeFinder f;
  f.t = f.start + std::chrono::microseconds(0);
  int calls = 0;
  const bool found = waitForDiscovery(
      milliseconds(2),
      [&] { return f.now(); },
      [&](milliseconds ms) {
        ++calls;
        if (ms.count() <= 0) ++f.zeroWaits;
        // Lands half a millisecond short of the deadline the first time.
        // A zero wait moves the clock 1 us, so the bug shows as a failed
        // check rather than a hung test.
        f.t += (calls == 1)        ? std::chrono::microseconds(1500)
               : (ms.count() > 0) ? std::chrono::microseconds(ms)
                                  : std::chrono::microseconds(1);
      },
      nullptr);
  CHECK(!found);
  CHECK_EQ(f.zeroWaits, 0);
  CHECK_EQ(calls, 2);
}

void anAnswerDuringTheLastWaitCounts() {
  // The remote source lands exactly as the window closes. It is checked once
  // more before giving up, so it is found rather than reported missing.
  FakeFinder f;
  f.arrivals = {{2000, "WIN-LAB (FFWIN NDI)"}};
  const bool found =
      listen(f, milliseconds(2000), [&] { return f.has("WIN-LAB (FFWIN NDI)"); });
  CHECK(found);
  CHECK_EQ(f.elapsed(), 2000L);
}

void run() {
  oneWaitStopsAtTheFirstChange();
  aRemoteSourceAfterALocalOneIsFound();
  aSourceAlreadyThereCostsNoWait();
  aMissingSourceGivesUpAtTheWindow();
  aListingListensForTheWholeWindow();
  aChattyNetworkStillEndsAtTheWindow();
  aSubMillisecondRemainderIsNotAZeroWait();
  anAnswerDuringTheLastWaitCounts();
}

}  // namespace

TEST_MAIN("discovery")

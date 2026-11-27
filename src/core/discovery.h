#pragma once

#include <chrono>
#include <functional>

namespace ferret {

/// How long to keep listening for discovery answers, and when to stop early.
///
/// NDI's `NDIlib_find_wait_for_sources(finder, ms)` returns as soon as the list
/// of sources CHANGES, not when `ms` has passed, and the first change is nearly
/// always this machine's own senders: they are known locally before any answer
/// from another machine has arrived. Waiting once and then picking from what is
/// there misses every remote sender whenever a local one exists — Frame
/// Ferret's own NDI output, NDI Tools' macOS AV Output, anything. Between a Mac
/// and a Windows VM that lost the race in 3 runs of 5 on the VM and in every run
/// on the Mac, where a local sender was always present. OMT's discovery has the
/// same shape without the wait call.
///
/// `waitForDiscovery` keeps listening until `satisfied()` says the answer is
/// enough or `window` has passed, whichever comes first. `wait(remaining)`
/// blocks for at most `remaining` and may return early (NDI's wait returns on a
/// change); `now()` reads the clock. A null `satisfied` listens for the whole
/// window, which is what a listing wants. Returns whether `satisfied()` was
/// true — checked once more after the last wait, so an answer that arrives
/// during it still counts.
using DiscoveryClock = std::chrono::steady_clock;

bool waitForDiscovery(
    std::chrono::milliseconds window,
    const std::function<DiscoveryClock::time_point()>& now,
    const std::function<void(std::chrono::milliseconds)>& wait,
    const std::function<bool()>& satisfied);

}  // namespace ferret

#include "core/discovery.h"

namespace ferret {

bool waitForDiscovery(
    std::chrono::milliseconds window,
    const std::function<DiscoveryClock::time_point()>& now,
    const std::function<void(std::chrono::milliseconds)>& wait,
    const std::function<bool()>& satisfied) {
  const auto deadline = now() + window;
  for (;;) {
    if (satisfied && satisfied()) return true;
    const auto t = now();
    if (t >= deadline) return false;
    // Rounded UP: a remainder under a millisecond truncates to a wait of 0,
    // which NDI answers at once, and the loop would spin until the clock
    // ticked past the deadline.
    wait(std::chrono::ceil<std::chrono::milliseconds>(deadline - t));
  }
}

}  // namespace ferret

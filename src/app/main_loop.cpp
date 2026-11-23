// Off macOS: a plain sleep. The macOS implementation is main_loop_mac.mm,
// because it has to pump AppKit's event queue — see there.
#include "app/main_loop.h"

#if !defined(__APPLE__)

#include <chrono>
#include <thread>

namespace ferret {

void initialiseWindowServer() {}

void waitServicingMainLoop(double seconds) {
  if (seconds <= 0) return;
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

}  // namespace ferret

#endif

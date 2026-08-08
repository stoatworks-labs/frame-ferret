#include "app/main_loop.h"

#include <chrono>
#include <thread>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace ferret {

void waitServicingMainLoop(double seconds) {
  if (seconds <= 0) return;

#if defined(__APPLE__)
  // returnAfterSourceHandled = false, so this runs the whole interval rather
  // than returning the moment one block has been handled — the caller asked
  // for a wait, not for a single event.
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
#else
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
#endif
}

}  // namespace ferret

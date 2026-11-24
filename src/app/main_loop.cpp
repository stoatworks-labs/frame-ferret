// Off macOS: a plain sleep. The macOS implementation is main_loop_mac.mm,
// because it has to pump AppKit's event queue — see there.
#include "app/main_loop.h"

#if !defined(__APPLE__)

#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#endif

namespace ferret {

void initialiseWindowServer() {
#if defined(_WIN32)
  // Per-monitor DPI aware, before anything touches a display. A process that
  // is not gets scaled numbers from the display APIs on a scaled monitor (a
  // 1920x1080 display at 150% reads as 1280x720), and the virtual display
  // would set and check the wrong raster. Desktop Duplication itself always
  // hands over real pixels.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  // A 1 ms timer. Windows sleeps in 15.6 ms steps unless a process asks, and
  // the frame loop's sleep_until then wakes on those steps, not on its
  // deadlines: a 60 Hz loop ticks unevenly and every poll at that pace lands
  // on the same few instants. Released by Windows when the process exits.
  timeBeginPeriod(1);
#endif
}

void waitServicingMainLoop(double seconds) {
  if (seconds <= 0) return;
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

}  // namespace ferret

#endif

// The main thread's wait on macOS. See main_loop.h for why every wait on the
// main thread goes through here.
//
// ## Why it pumps AppKit's event queue, not just the run loop
//
// initialiseWindowServer() calls NSApplicationLoad(), so AppKit owns this
// process's window-server connection. From then on, display reconfiguration
// reaches CoreGraphics as AppKit events, and they are only processed when
// something dequeues them. A bare CFRunLoopRunInMode services timers and the
// main dispatch queue but never dequeues an event, so a virtual display this
// process created stayed modeless forever: in the active-display list within
// 60 ms, CGDisplayCopyDisplayMode NULL six seconds later. Measured with a
// probe across every combination (2026-11-23, macOS 26.4): NSApplicationLoad
// plus a run-loop wait never got a mode; NSApplicationLoad plus an event pump,
// or no AppKit at all, got one in 40-80 ms. The same pump is what keeps
// NSScreen's list current.
//
// nextEventMatchingMask runs the run loop in the default mode while it waits,
// so the main dispatch queue is still serviced — the Syphon sink's
// dispatch_sync onto the main queue completes exactly as before.

#include "app/main_loop.h"

#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>

namespace ferret {

void initialiseWindowServer() {
  static bool done = false;
  if (done) return;
  done = true;
  NSApplicationLoad();
}

void waitServicingMainLoop(double seconds) {
  if (seconds <= 0) return;

  // No AppKit (a test, or a command that never touched the window server):
  // the run loop is all there is to service.
  if (!NSApp) {
    // returnAfterSourceHandled = false, so this runs the whole interval rather
    // than returning the moment one block has been handled — the caller asked
    // for a wait, not for a single event.
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    return;
  }

  NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
  for (;;) {
    @autoreleasepool {
      NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                          untilDate:until
                                             inMode:NSDefaultRunLoopMode
                                            dequeue:YES];
      if (event) [NSApp sendEvent:event];
    }
    if (until.timeIntervalSinceNow <= 0) break;
  }
}

}  // namespace ferret

#pragma once

namespace ferret {

/// Wait roughly `seconds`, servicing the main thread's run loop while waiting.
/// Call only from the main thread.
///
/// On macOS this is not a nicety. Syphon's discovery works through
/// NSDistributedNotificationCenter, and those notifications are delivered on
/// the **main** run loop — a worker thread running its own CFRunLoop does not
/// receive them. Measured in oxbow rather than assumed: a server created on a
/// private run-loop thread is perfectly well-formed, announces itself, and is
/// then invisible to every consumer, while identical code on the main thread is
/// found at once. That is the worst shape the failure could take, because a
/// consumer already running finds it and one started afterwards never does.
///
/// So the Syphon sink creates its server on the main thread via dispatch, and
/// that dispatch only completes if the main thread is inside a run loop rather
/// than a plain sleep.
///
/// **Every place the main thread waits must wait here.** A plain `sleep_for`
/// on the main thread deadlocks a `dispatch_sync` coming from the frame thread.
///
/// Off macOS this is exactly a sleep.
void waitServicingMainLoop(double seconds);

}  // namespace ferret

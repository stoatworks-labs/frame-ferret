#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ferret {

/// A child process with pipes on its stdin and stdout.
///
/// Exists for one purpose: running ffmpeg as a codec without linking it. That
/// choice is deliberate and worth stating, because linking would be the
/// obvious move:
///
///  - **The user points at an install rather than building one in.** ffmpeg's
///    path is configuration, so a machine that already has ffmpeg needs
///    nothing, and a machine without it gets a sentence saying so.
///  - **No ABI coupling.** libavcodec's structs change between major versions,
///    and mirroring AVCodecContext by hand — the way this repo mirrors NDI,
///    OMT and libsrt — would be far more fragile than any of those, because
///    those three expose deliberately flat, stable C ABIs and ffmpeg does not.
///  - **Licensing stays simple.** ffmpeg is LGPL, and commonly built as GPL
///    with libx264. A separate process communicating over a pipe does not
///    make this repository a derivative work; linking would raise a question
///    that has no business being in an MIT codebase.
///  - **CI keeps building.** No ffmpeg on the runners, and nothing to detect
///    at build time.
///
/// The cost is a pipe copy per frame and a process to supervise. At 1280x720
/// UYVY that is 1.8 MB a frame, which a pipe carries comfortably.
class Subprocess {
 public:
  virtual ~Subprocess() = default;

  /// Writes to the child's stdin. Returns false once the pipe is broken —
  /// which is how a child that has exited is noticed.
  virtual bool write(const uint8_t* data, size_t size) = 0;

  /// Reads up to `capacity` from the child's stdout. Returns bytes read, 0 on
  /// timeout, -1 at end of stream.
  virtual int read(uint8_t* buffer, size_t capacity, int timeoutMs) = 0;

  /// Reads exactly `size` bytes unless the stream ends. Video frames are
  /// fixed-size records, so a partial read is a torn frame rather than a short
  /// one, and every caller here wants all or nothing.
  ///
  /// `timeoutMs` is how long to wait for a record to BEGIN. A record that has
  /// begun is finished (allowing the child up to two seconds), because giving
  /// up partway throws away bytes and puts every later record out of step.
  virtual bool readExactly(uint8_t* buffer, size_t size, int timeoutMs) = 0;

  /// Closes stdin, which is how ffmpeg is told to flush and finish.
  virtual void closeInput() = 0;

  virtual bool running() = 0;

  /// Waits up to `timeoutMs` for the child to exit on its own and returns its
  /// exit code, or -1 if it is still running at the deadline or was ended by a
  /// signal. Does not terminate it. Used by the encoder trial, where "ffmpeg
  /// exited 0" is the only proof that an encoder really opened.
  virtual int waitForExit(int timeoutMs) = 0;

  /// Terminates and reaps. Safe to call more than once.
  virtual void stop() = 0;

  /// Whatever the child last wrote to stderr, for reporting a failure. ffmpeg
  /// explains itself there and nowhere else, so a start-up failure is useless
  /// without it.
  virtual std::string drainErrors() = 0;
};

/// Spawns `executable` with `arguments` (which must NOT include argv[0]).
///
/// POSIX uses posix_spawn. Windows uses CreateProcessW with three anonymous
/// pipes, a reader thread on each of the child's stdout and stderr, and an
/// explicit inherit list so that two nodes spawning at once never hand each
/// other's pipe ends to the wrong child (which would keep a pipe open after its
/// child exits, so end of stream never arrives).
std::unique_ptr<Subprocess> spawnSubprocess(
    const std::string& executable, const std::vector<std::string>& arguments,
    std::string& error);

/// The Windows command line for `arguments`, each quoted the way the MSVC C
/// runtime (and so ffmpeg) splits it back into argv: a space or tab or quote
/// forces quotes, a quote is escaped with a backslash, and backslashes are
/// doubled only where they precede a quote. Pure, so it is tested on every
/// platform, not just the one that needs it.
std::string windowsCommandLine(const std::vector<std::string>& arguments);

}  // namespace ferret

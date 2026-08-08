#pragma once

#include <string>
#include <vector>

namespace ferret {

/// Finds and describes the ffmpeg the operator already has.
///
/// Nothing is built in and nothing is vendored — see `core/subprocess.h` for
/// why ffmpeg is a process rather than a library. The whole contract here is:
/// point Frame Ferret at an ffmpeg and it uses it; point it at nothing and it
/// says so in a sentence with the paths it tried.
class Ffmpeg {
 public:
  /// Resolution order, most explicit first:
  ///   1. the node's own `ffmpeg` config field
  ///   2. `$FERRET_FFMPEG`
  ///   3. `ffmpeg` on `$PATH`
  ///   4. the usual install locations for this platform
  ///
  /// `hint` is the config field, and may be empty. Returns an empty string
  /// when nothing was found; `searchReport()` then says where it looked.
  static std::string locate(const std::string& hint);

  /// Every path tried, for an error message that can actually be acted on.
  static std::string searchReport(const std::string& hint);

  /// Runs `<ffmpeg> -version` and returns the first line. Empty if it will not
  /// run at all — which distinguishes "wrong path" from "found but broken",
  /// the two being indistinguishable from the file existing.
  static std::string version(const std::string& executable);

  /// Whether this build can run an external codec at all. False on Windows,
  /// where the subprocess layer is not implemented.
  static bool supportedOnThisPlatform();

  /// The encoders this ffmpeg actually has, out of the ones we would pick.
  /// An ffmpeg without libx264 is common — Homebrew's has it, a minimal build
  /// may not — and choosing an absent encoder fails with a wall of ffmpeg
  /// output rather than a useful message.
  static std::vector<std::string> availableVideoEncoders(
      const std::string& executable);

  /// The best available H.264 encoder, or empty. Hardware first: on Apple
  /// silicon `h264_videotoolbox` costs almost no CPU, which matters when the
  /// frame loop is already serving other sinks.
  static std::string bestH264Encoder(const std::string& executable);
};

}  // namespace ferret

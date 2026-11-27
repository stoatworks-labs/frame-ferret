#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/rational.h"

namespace ferret {

/// What an H.264 encode is asked to do. The SRT sender and the encoder trial
/// build their ffmpeg arguments from this through the same function, so a trial
/// that passes has run the exact command the sender will run, just for one
/// frame.
struct H264Encode {
  std::string encoder;
  int width = 0;
  int height = 0;
  Rate rate{30, 1};
  int64_t bitrateBps = 0;
};

/// The outcome of choosing an encoder.
struct EncoderPick {
  std::string encoder;  ///< Empty when nothing usable was found.
  std::string error;    ///< Why, when `encoder` is empty.
  /// One line per candidate that was listed but would not open, even on
  /// success — "h264_nvenc would not open (Cannot load nvcuda.dll)" is worth
  /// a line in the log on a machine that has no NVIDIA GPU.
  std::vector<std::string> skipped;
};

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
  ///   3. `ffmpeg` on `$PATH` (`ffmpeg.exe` on Windows)
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

  /// Every encoder name this ffmpeg lists in `-encoders`.
  static std::vector<std::string> listEncoders(const std::string& executable);

  /// The encoder names in an `ffmpeg -encoders` listing. Pure, so it is tested
  /// against a captured listing rather than whatever ffmpeg the machine has.
  static std::vector<std::string> parseEncoderNames(const std::string& listing);

  /// The automatic order: hardware first, because on Apple silicon
  /// `h264_videotoolbox` costs almost no CPU and the frame loop is already
  /// serving every other sink from one thread — then libx264.
  static const std::vector<std::string>& h264Preference();

  /// The arguments from the rawvideo input on stdin through the encoder's own
  /// options. The caller appends the muxer and the output.
  static std::vector<std::string> h264EncodeArguments(const H264Encode& encode);

  /// Encodes one black frame with exactly these settings into ffmpeg's null
  /// muxer. True only if ffmpeg exits 0; otherwise `why` gets ffmpeg's own
  /// first line of explanation.
  ///
  /// This is the whole fix for the hardware-encoder trap: `-encoders` lists
  /// what ffmpeg was BUILT with, not what this machine can open. The BtbN and
  /// gyan Windows builds and most distro Linux packages list h264_nvenc
  /// everywhere, and it fails at open on any machine without an NVIDIA GPU.
  static bool trialEncode(const std::string& executable,
                          const H264Encode& encode, std::string* why);

  /// The decision, separated from the processes so it can be tested.
  ///
  /// `requested` empty: the first of `h264Preference()` that is listed and
  /// passes `works`. Listed-but-broken ones are skipped and recorded.
  ///
  /// `requested` set: that encoder or nothing. An explicit choice is an
  /// instruction, the same rule as an explicit ffmpeg path — falling back to a
  /// different encoder would be diagnosed on site as the setting not working.
  /// It may be any encoder ffmpeg lists (h264_amf, h264_mf), not only the
  /// automatic ones.
  static EncoderPick chooseH264Encoder(
      const std::vector<std::string>& listed, const std::string& requested,
      const std::function<bool(const std::string& encoder, std::string* why)>&
          works);

  /// `chooseH264Encoder` against a real ffmpeg: its listing, and a trial
  /// encode with `settings` (whose `encoder` field is ignored) for each
  /// candidate.
  static EncoderPick pickH264Encoder(const std::string& executable,
                                     const std::string& requested,
                                     const H264Encode& settings);

  /// The first line of ffmpeg's stderr that says something, with the
  /// "[h264_nvenc @ 0x7f...]" context prefix removed. Pure.
  static std::string summariseError(const std::string& stderrText);
};

}  // namespace ferret

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/node.h"

namespace ferret {

/// The eight BT.709 75% colour bars, top to bottom of the pattern, in the
/// order they appear left to right. Exposed so tests can assert against the
/// same numbers the generator uses rather than restating them — a test that
/// carries its own copy of the expected colours proves only that two constants
/// match.
struct BarColour {
  const char* name;
  uint8_t r, g, b;
};

const std::vector<BarColour>& colourBars75();

/// The tone the test pattern emits alongside the bars: 1 kHz at -20 dBFS, the
/// broadcast line-up reference. Exposed so a test can assert against the same
/// numbers the generator uses rather than restating them.
constexpr double kToneHz = 1000.0;
constexpr double kToneAmplitude = 0.1;  // -20 dBFS
constexpr int kToneSampleRate = 48000;
constexpr int kToneChannels = 2;

/// A synthetic source: 75% colour bars with a moving marker, plus a 1 kHz tone.
///
/// This exists so the entire frame path — router, conversion, pacing, sinks —
/// can be proven with no SDK, no network and no hardware. Every other source in
/// this program can be unavailable and this one still runs, which makes it the
/// regression harness as well as a diagnostic. `frame-ferret selftest` uses it.
///
/// The moving marker matters: a still pattern passes a test that a frozen
/// output would also pass. The marker's position is a pure function of the
/// frame counter, so a consumer can check not merely that frames arrive but
/// that they *advance*.
std::unique_ptr<Source> makeTestPatternSource(const NodeConfig& config,
                                              std::string& error);

}  // namespace ferret

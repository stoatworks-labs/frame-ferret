#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/node.h"

namespace ferret {

/// DeckLink output.
///
/// The Blackmagic SDK is not vendored — its licence is not ours to
/// redistribute — so this is compiled only when CMake is given
/// `-DDECKLINK_SDK_DIR=/path/to/SDK`. Without it every entry point below still
/// exists and reports the card as unavailable with that explanation, so a build
/// without the SDK runs everything else unchanged.
///
/// Note the *runtime* (Desktop Video) is a separate matter: the SDK provides
/// `DeckLinkAPIDispatch.cpp`, which finds the installed driver at run time, so
/// a binary built with the SDK still starts on a machine with no card.
class DeckLinkRuntime {
 public:
  /// Whether this build has the SDK compiled in at all.
  static bool builtIn();

  /// Whether the drivers are present and at least one device exists.
  static bool available();
  static std::string unavailableReason();

  /// Display names of every device, in enumeration order — the same order the
  /// numeric `target` selector uses.
  static std::vector<std::string> listDevices();
};

/// `config.target` selects the device: a decimal string is an index, anything
/// else is matched as a substring against the device's display name, and empty
/// takes the first.
std::unique_ptr<Sink> makeDeckLinkSink(const NodeConfig& config,
                                       std::string& error);

}  // namespace ferret

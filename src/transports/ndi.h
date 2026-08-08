#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/node.h"

namespace ferret {

/// The NDI runtime, loaded at run time and shared by every NDI node.
///
/// Never linked. NDI's licence requires that whoever redistributes the SDK
/// forbid reverse engineering it, which MIT expressly permits — so this repo
/// ships no NDI binary and opens whatever the operator installed. It also
/// dodges the trap that bit openstage, where a cross-compiled target silently
/// shipped with NDI disabled because a build-time `find_package` quietly
/// failed and nobody noticed for months.
class NdiRuntime {
 public:
  /// Loads on first use. Safe to call repeatedly; the runtime is opened once
  /// per process. Returns null and sets `error` when NDI is not installed,
  /// with a message naming where to get it.
  static NdiRuntime* instance(std::string& error);

  /// Whether the runtime loaded, for the control API to report.
  static bool available();
  static std::string loadedPath();
  static std::string unavailableReason();

  /// The URL to point an operator at when NDI is missing.
  static const char* downloadUrl();
};

/// Sources visible on the network right now. `waitMs` is how long to let
/// discovery settle — NDI's finder needs a moment before its first answer is
/// meaningful.
std::vector<std::string> ndiListSources(unsigned waitMs, std::string& error);

/// A receiver. `config.target` is matched as a substring against the full NDI
/// source name ("MACHINE (Source)"), so "Mixer" finds "GALLERY (Mixer Out 1)".
/// An empty target connects to the first source found.
std::unique_ptr<Source> makeNdiSource(const NodeConfig& config,
                                      std::string& error);

/// A sender. `config.target` is the NDI source name to publish; it defaults to
/// the node's id.
std::unique_ptr<Sink> makeNdiSink(const NodeConfig& config, std::string& error);

/// Whether NDI can honour an interface selector. It cannot: see the note in
/// ndi.cpp. Exposed so the factory can warn rather than silently ignore the
/// setting.
constexpr bool kNdiSupportsInterfaceBinding = false;

}  // namespace ferret

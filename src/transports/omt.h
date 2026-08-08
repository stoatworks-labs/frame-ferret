#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/node.h"

namespace ferret {

/// The OMT (Open Media Transport) runtime, loaded at run time.
///
/// Runtime-loaded for a different reason from NDI. libomt is MIT and *could*
/// be redistributed, but it wraps a .NET assembly (`libomtnet`), ships for
/// only some platforms — **there is no published Linux build** — and pulls the
/// .NET runtime into the process. Linking it would make a Linux build
/// impossible and would drag .NET into every Frame Ferret process whether or
/// not OMT is used.
class OmtRuntime {
 public:
  static bool available();
  static std::string loadedPath();
  static std::string unavailableReason();
  static const char* downloadUrl();

  /// True once any OMT sender or receiver has been created.
  ///
  /// This exists because of a real trap: **libomt's .NET runtime replaces the
  /// process's SIGINT and SIGTERM handlers when it initialises**, which happens
  /// on first sender/receiver creation, not on dlopen. Any handler installed
  /// before that is silently overwritten — Ctrl-C stops working and the process
  /// lingers holding OMT's port. Callers must install their signal handlers
  /// *after* this returns true. See the note in omt.cpp.
  static bool dotNetInitialised();
};

/// Addresses OMT can see. OMT's discovery returns addresses rather than
/// friendly names, so these are what `target` is matched against.
std::vector<std::string> omtListSources(std::string& error);

std::unique_ptr<Source> makeOmtSource(const NodeConfig& config,
                                      std::string& error);
std::unique_ptr<Sink> makeOmtSink(const NodeConfig& config, std::string& error);

/// OMT binds to all interfaces and offers no selector, exactly like NDI.
constexpr bool kOmtSupportsInterfaceBinding = false;

}  // namespace ferret

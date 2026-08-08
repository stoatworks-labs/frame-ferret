#pragma once

#include <memory>
#include <string>

#include "app/node.h"

namespace ferret {

/// A Syphon server, macOS only.
///
/// Syphon shares a frame between applications on one machine as an IOSurface —
/// a buffer both CPU and GPU can address, handed to another process through a
/// mach port with no copy. Resolume, OBS, VDMX, MadMapper and most of the VJ
/// world consume it.
///
/// Output only. Frame Ferret has no reason to *receive* Syphon that NDI does
/// not already serve better across machines, and a Syphon client is a separate
/// piece of work; `NodeKind::sharedSurfaceIn` stays unimplemented.
///
/// The vendored Syphon subset under `third_party/syphon` is BSD-3 and is the
/// same revision oxbow uses.
class SyphonRuntime {
 public:
  /// False on every platform but macOS.
  static bool supported();
  static std::string unsupportedReason();
};

std::unique_ptr<Sink> makeSyphonSink(const NodeConfig& config,
                                     std::string& error);

}  // namespace ferret

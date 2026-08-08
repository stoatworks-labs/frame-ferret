#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/config.h"
#include "app/engine.h"
#include "app/node.h"
#include "sinks/preview.h"

namespace ferret {

/// Why a node could not be built. Distinguished from a config error because
/// these are runtime conditions — a transport whose SDK is not installed, an
/// output whose hardware is absent — and the right response is to report the
/// node as unavailable and carry on, not to refuse to start.
struct NodeFailure {
  std::string id;
  std::string reason;
};

/// Builds every enabled node in `config` into `engine`.
///
/// A node that cannot be built is recorded in `failures` and skipped; the rest
/// still run. That matters on site: an operator who has lost one DeckLink
/// should not lose the NDI output too, and a config written for a machine with
/// hardware should still start on one without it.
///
/// `warnings` collects settings that were accepted but could not be honoured —
/// distinct from `failures`, which are nodes that do not exist at all.
///
/// The distinction earns its keep immediately: NDI's C API has no interface
/// parameter of any kind, so `"interface": "en0"` on an NDI node is a setting
/// this program cannot apply. Dropping it silently would leave an operator
/// believing a stream is pinned to a NIC when it is not, which is exactly the
/// class of fault that gets diagnosed as a network problem on site.
///
/// Returns false only for a failure that leaves nothing to run at all.
bool buildNodes(const AppConfig& config, Engine& engine,
                std::vector<NodeFailure>& failures,
                std::vector<NodeFailure>& warnings,
                std::vector<PreviewSink*>& previews, std::string& error);

/// The kinds this build can actually construct today. Everything else in
/// `NodeKind` is designed but not implemented, and asking for one produces a
/// NodeFailure naming it rather than a silent no-op.
bool isImplemented(NodeKind kind);

}  // namespace ferret

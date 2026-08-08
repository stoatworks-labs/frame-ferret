#include "app/factory.h"

#include "sources/test_pattern.h"

namespace ferret {

bool isImplemented(NodeKind kind) {
  switch (kind) {
    case NodeKind::testPattern:
    case NodeKind::preview:
      return true;
    default:
      return false;
  }
}

bool buildNodes(const AppConfig& config, Engine& engine,
                std::vector<NodeFailure>& failures,
                std::vector<PreviewSink*>& previews, std::string& error) {
  engine.setRate(config.rate);

  int built = 0;

  for (const auto& node : config.nodes) {
    if (!node.enabled) continue;

    if (!isImplemented(node.kind)) {
      failures.push_back(
          {node.id, std::string("node kind \"") + toString(node.kind) +
                        "\" is designed but not implemented in this build — "
                        "see docs/ROADMAP.md"});
      continue;
    }

    std::string reason;
    switch (node.kind) {
      case NodeKind::testPattern: {
        auto source = makeTestPatternSource(node, reason);
        if (!source) {
          failures.push_back({node.id, reason});
          continue;
        }
        // The generator emits full-range BGRA. Declaring anything else here
        // would make the router plan copies that are really conversions.
        engine.addSource(std::move(source), PixelFormat::bgra8);
        ++built;
        break;
      }

      case NodeKind::preview: {
        auto sink = std::make_unique<PreviewSink>(node);
        previews.push_back(sink.get());
        engine.addSink(std::move(sink));
        ++built;
        break;
      }

      default:
        // Unreachable: isImplemented() gates this switch.
        failures.push_back({node.id, "internal: unhandled node kind"});
        break;
    }
  }

  // Routes are applied after every node exists, so a config may list them in
  // any order.
  for (const auto& [sinkId, sourceId] : config.routes) {
    std::string routeError;
    if (!engine.router().route(sinkId, sourceId, routeError)) {
      // The route named nodes that failed to build. Not fatal — the sink will
      // simply sit unrouted and say so.
      failures.push_back({sinkId, "route not applied: " + routeError});
    }
  }

  if (built == 0) {
    error =
        "no nodes could be built — every node in the config is either disabled "
        "or not implemented in this build";
    return false;
  }
  return true;
}

}  // namespace ferret

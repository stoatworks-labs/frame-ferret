#include "app/factory.h"

#include "sources/test_pattern.h"
#include "transports/ndi.h"
#include "sinks/decklink.h"
#include "sinks/syphon.h"
#include "transports/omt.h"
#include "transports/srt.h"

namespace ferret {

bool isImplemented(NodeKind kind) {
  switch (kind) {
    case NodeKind::testPattern:
    case NodeKind::preview:
    case NodeKind::ndi:
    case NodeKind::omt:
    case NodeKind::decklink:
    case NodeKind::sharedSurfaceOut:
    case NodeKind::srt:
      return true;
    default:
      return false;
  }
}

namespace {

/// Records any setting on `node` that this build accepts but cannot apply.
void checkUnhonouredSettings(const NodeConfig& node,
                             std::vector<NodeFailure>& warnings) {
  if (node.interfaceSelector.empty()) return;

  if (node.kind == NodeKind::omt && !kOmtSupportsInterfaceBinding) {
    warnings.push_back(
        {node.id,
         "\"interface\": \"" + node.interfaceSelector +
             "\" cannot be applied to an OMT node — libomt exposes no "
             "interface selector, the same limitation NDI has."});
    return;
  }

  if (node.kind == NodeKind::ndi && !kNdiSupportsInterfaceBinding) {
    warnings.push_back(
        {node.id,
         "\"interface\": \"" + node.interfaceSelector +
             "\" cannot be applied to an NDI node. The NDI C API has no "
             "interface parameter — not on send, receive or discovery — so "
             "this stream will use whatever route the OS picks. Bind it with "
             "the NDI runtime's own ndi-config.v1.json instead."});
  }
}

}  // namespace

bool buildNodes(const AppConfig& config, Engine& engine,
                std::vector<NodeFailure>& failures,
                std::vector<NodeFailure>& warnings,
                std::vector<PreviewSink*>& previews, std::string& error) {
  engine.setRate(config.rate);

  int built = 0;

  for (const auto& node : config.nodes) {
    if (!node.enabled) continue;

    checkUnhonouredSettings(node, warnings);

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

      case NodeKind::srt: {
        // Receive only for now. An SRT *sink* needs an encoder and a muxer,
        // which is the other half of this work — see docs/ROADMAP.md. A node
        // that is not used as somebody's source would therefore be a sender we
        // cannot build, so say that rather than constructing something inert.
        bool usedAsSource = false;
        for (const auto& [sinkId, sourceId] : config.routes) {
          if (sourceId == node.id) usedAsSource = true;
        }
        if (!usedAsSource) {
          failures.push_back(
              {node.id,
               "SRT sending is not implemented yet — it needs an H.264 encoder "
               "and a TS muxer. SRT receiving works; route this node into a "
               "sink to use it"});
          continue;
        }
        auto source = makeSrtSource(node, reason);
        if (!source) {
          failures.push_back({node.id, reason});
          continue;
        }
        engine.addSource(std::move(source), PixelFormat::uyvy8);
        ++built;
        break;
      }

      case NodeKind::sharedSurfaceOut: {
        auto sink = makeSyphonSink(node, reason);
        if (!sink) {
          failures.push_back({node.id, reason});
          continue;
        }
        engine.addSink(std::move(sink));
        ++built;
        break;
      }

      case NodeKind::decklink: {
        auto sink = makeDeckLinkSink(node, reason);
        if (!sink) {
          failures.push_back({node.id, reason});
          continue;
        }
        engine.addSink(std::move(sink));
        ++built;
        break;
      }

      case NodeKind::omt: {
        bool usedAsSource = false;
        for (const auto& [sinkId, sourceId] : config.routes) {
          if (sourceId == node.id) usedAsSource = true;
        }

        if (usedAsSource) {
          auto source = makeOmtSource(node, reason);
          if (!source) {
            failures.push_back({node.id, reason});
            continue;
          }
          // Asked for UYVYorBGRA, so 4:2:2 sources arrive untouched.
          engine.addSource(std::move(source), PixelFormat::uyvy8);
        } else {
          auto sink = makeOmtSink(node, reason);
          if (!sink) {
            failures.push_back({node.id, reason});
            continue;
          }
          engine.addSink(std::move(sink));
        }
        ++built;
        break;
      }

      case NodeKind::ndi: {
        // Direction comes from the routes, not from a separate field: a node
        // named as some sink's source is a receiver, anything else is a
        // sender. That keeps "a protocol is just a port on a router" true in
        // the config as well as in the code.
        bool usedAsSource = false;
        for (const auto& [sinkId, sourceId] : config.routes) {
          if (sourceId == node.id) usedAsSource = true;
        }

        if (usedAsSource) {
          auto source = makeNdiSource(node, reason);
          if (!source) {
            failures.push_back({node.id, reason});
            continue;
          }
          // The receiver is asked for UYVY_BGRA, so 4:2:2 sources arrive
          // untouched. Declaring uyvy8 here means the router plans a copy for
          // them and a conversion only where one is genuinely needed.
          engine.addSource(std::move(source), PixelFormat::uyvy8);
        } else {
          auto sink = makeNdiSink(node, reason);
          if (!sink) {
            failures.push_back({node.id, reason});
            continue;
          }
          engine.addSink(std::move(sink));
        }
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

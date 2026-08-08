#pragma once

#include <map>
#include <string>
#include <vector>

#include "app/config.h"
#include "app/engine.h"
#include "app/factory.h"
#include "control/http_server.h"
#include "sinks/preview.h"

namespace ferret {

/// Serves the operator page and the JSON control API.
///
/// Holds no state of its own: every answer is read from the Engine and the
/// Router live. A cached view here would be a second source of truth about
/// what the crosspoint is doing, and the two would disagree at exactly the
/// moment somebody was relying on the page.
class ControlApi {
 public:
  ControlApi(Engine& engine, const AppConfig& config,
             std::vector<NodeFailure> failures,
             std::vector<NodeFailure> warnings,
             std::vector<PreviewSink*> previews);

  void handle(const HttpServer::Request& request,
              HttpServer::Response& response);

 private:
  void handleState(HttpServer::Response& response) const;
  void handleRoute(const HttpServer::Request& request,
                   HttpServer::Response& response);
  void handleMute(const HttpServer::Request& request,
                  HttpServer::Response& response);
  void handleInterfaces(HttpServer::Response& response) const;
  void handlePreview(const std::string& id,
                     HttpServer::Response& response) const;

  Engine& engine_;
  AppConfig config_;
  std::vector<NodeFailure> failures_;
  std::vector<NodeFailure> warnings_;
  std::map<std::string, PreviewSink*> previews_;
};

}  // namespace ferret

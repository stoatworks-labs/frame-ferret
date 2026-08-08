#pragma once

#include <map>
#include <string>
#include <vector>

#include "app/node.h"

namespace ferret {

/// A whole configuration: what nodes exist, how they are wired, and where the
/// control server listens.
struct AppConfig {
  Rate rate{50, 1};  ///< the rate the frame loop is paced at

  std::string controlBind = "127.0.0.1";
  int controlPort = 8740;
  std::string controlToken;

  std::vector<NodeConfig> nodes;

  /// sink id -> source id. A sink absent from this map is deliberately
  /// unrouted, which is a valid state, not an error.
  std::map<std::string, std::string> routes;
};

/// Parses `text` as JSON into `out`.
///
/// Every failure is rejected with a sentence naming the offending field, never
/// defaulted around. A config that half-loads produces a show that half-works,
/// and the half that is wrong is the half nobody checked.
bool parseConfig(const std::string& text, AppConfig* out, std::string& error);

bool loadConfigFile(const std::string& path, AppConfig* out,
                    std::string& error);

/// The configuration `frame-ferret selftest` runs: colour bars into a preview,
/// with the control server off. Built in code rather than shipped as a file so
/// the self test cannot be broken by editing a config.
AppConfig selftestConfig();

}  // namespace ferret

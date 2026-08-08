#include "app/config.h"

#include <fstream>
#include <set>
#include <sstream>

#include "core/json.h"

namespace ferret {
namespace {

bool parseNode(const json::Value& v, size_t index, NodeConfig* out,
               std::string& error) {
  const std::string where = "nodes[" + std::to_string(index) + "]";

  if (!v.isObject()) {
    error = where + " is not an object";
    return false;
  }

  out->id = v["id"].asString();
  if (out->id.empty()) {
    error = where + " has no \"id\"";
    return false;
  }

  const std::string kind = v["kind"].asString();
  if (kind.empty()) {
    error = where + " (\"" + out->id + "\") has no \"kind\"";
    return false;
  }
  if (!nodeKindFromString(kind, &out->kind)) {
    error = where + " (\"" + out->id + "\") has unknown kind \"" + kind +
            "\" — run `frame-ferret kinds` for the list";
    return false;
  }

  out->label = v.has("label") ? v["label"].asString() : out->id;
  out->interfaceSelector = v["interface"].asString();
  out->target = v["target"].asString();
  out->ffmpegPath = v["ffmpeg"].asString();
  out->enabled = v.has("enabled") ? v["enabled"].asBool(true) : true;

  if (v.has("width")) out->width = v["width"].asInt(out->width);
  if (v.has("height")) out->height = v["height"].asInt(out->height);
  if (out->width <= 0 || out->height <= 0) {
    error = where + " (\"" + out->id + "\") has a non-positive width or height";
    return false;
  }

  if (v.has("rate")) {
    const std::string spelling = v["rate"].asString();
    if (!parseRate(spelling, &out->rate)) {
      error = where + " (\"" + out->id + "\") has an unrecognised rate \"" +
              spelling +
              "\" — use an integer, a broadcast decimal like 59.94, or an "
              "explicit rational like 60000/1001";
      return false;
    }
  }

  if (v.has("format")) {
    const std::string spelling = v["format"].asString();
    out->format = pixelFormatFromString(spelling);
    if (out->format == PixelFormat::unknown) {
      error = where + " (\"" + out->id + "\") has unknown format \"" +
              spelling + "\"";
      return false;
    }
  }

  return true;
}

}  // namespace

bool parseConfig(const std::string& text, AppConfig* out, std::string& error) {
  if (!out) return false;

  std::string parseError;
  auto parsed = json::parse(text, &parseError);
  if (!parsed) {
    error = "config is not valid JSON: " + parseError;
    return false;
  }
  const json::Value& root = *parsed;
  if (!root.isObject()) {
    error = "config root must be an object";
    return false;
  }

  AppConfig cfg;

  if (root.has("rate")) {
    const std::string spelling = root["rate"].asString();
    if (!parseRate(spelling, &cfg.rate)) {
      error = "top-level \"rate\" is unrecognised: \"" + spelling + "\"";
      return false;
    }
  }

  if (root.has("control")) {
    const json::Value& c = root["control"];
    if (!c.isObject()) {
      error = "\"control\" must be an object";
      return false;
    }
    if (c.has("bind")) cfg.controlBind = c["bind"].asString();
    if (c.has("port")) cfg.controlPort = c["port"].asInt(cfg.controlPort);
    if (c.has("token")) cfg.controlToken = c["token"].asString();
    if (cfg.controlPort < 0 || cfg.controlPort > 65535) {
      error = "\"control.port\" is out of range: " +
              std::to_string(cfg.controlPort);
      return false;
    }
  }

  if (!root.has("nodes") || !root["nodes"].isArray()) {
    error = "config needs a \"nodes\" array";
    return false;
  }

  std::set<std::string> ids;
  const json::Value& nodes = root["nodes"];
  for (size_t i = 0; i < nodes.size(); ++i) {
    NodeConfig n;
    if (!parseNode(nodes.at(i), i, &n, error)) return false;
    if (!ids.insert(n.id).second) {
      // Duplicate ids would make the crosspoint ambiguous and the control API
      // address the wrong node, so this is fatal rather than a warning.
      error = "duplicate node id \"" + n.id + "\"";
      return false;
    }
    cfg.nodes.push_back(std::move(n));
  }

  if (root.has("routes")) {
    const json::Value& routes = root["routes"];
    if (!routes.isObject()) {
      error = "\"routes\" must be an object mapping sink id to source id";
      return false;
    }
    for (const auto& [sinkId, sourceValue] : routes.members()) {
      const std::string sourceId = sourceValue.asString();

      auto find = [&cfg](const std::string& id) -> const NodeConfig* {
        for (const auto& n : cfg.nodes) {
          if (n.id == id) return &n;
        }
        return nullptr;
      };

      const NodeConfig* sink = find(sinkId);
      if (!sink) {
        error = "routes names unknown sink \"" + sinkId + "\"";
        return false;
      }
      if (!canSink(sink->kind)) {
        error = "\"" + sinkId + "\" is a " + toString(sink->kind) +
                ", which cannot be a sink";
        return false;
      }
      if (sourceId.empty()) continue;  // explicit "unrouted" is allowed

      const NodeConfig* source = find(sourceId);
      if (!source) {
        error = "routes points \"" + sinkId + "\" at unknown source \"" +
                sourceId + "\"";
        return false;
      }
      if (!canSource(source->kind)) {
        error = "\"" + sourceId + "\" is a " + toString(source->kind) +
                ", which cannot be a source";
        return false;
      }
      cfg.routes[sinkId] = sourceId;
    }
  }

  *out = std::move(cfg);
  return true;
}

bool loadConfigFile(const std::string& path, AppConfig* out,
                    std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open config file: " + path;
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return parseConfig(buffer.str(), out, error);
}

AppConfig selftestConfig() {
  AppConfig cfg;
  cfg.rate = Rate{50, 1};
  cfg.controlPort = 0;  // no control server

  NodeConfig bars;
  bars.id = "bars";
  bars.label = "Colour bars";
  bars.kind = NodeKind::testPattern;
  bars.width = 1280;
  bars.height = 720;
  bars.rate = cfg.rate;
  cfg.nodes.push_back(bars);

  NodeConfig preview;
  preview.id = "preview";
  preview.label = "Preview";
  preview.kind = NodeKind::preview;
  preview.width = 1280;
  preview.height = 720;
  preview.rate = cfg.rate;
  cfg.nodes.push_back(preview);

  cfg.routes["preview"] = "bars";
  return cfg;
}

}  // namespace ferret

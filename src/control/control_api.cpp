#include "control/control_api.h"

#include "control/web_assets.h"
#include "core/json.h"
#include "net/interfaces.h"

namespace ferret {

ControlApi::ControlApi(Engine& engine, const AppConfig& config,
                       std::vector<NodeFailure> failures,
                       std::vector<PreviewSink*> previews)
    : engine_(engine),
      config_(config),
      failures_(std::move(failures)) {
  for (auto* p : previews) {
    if (p) previews_[p->id()] = p;
  }
}

void ControlApi::handle(const HttpServer::Request& request,
                        HttpServer::Response& response) {
  const std::string& path = request.path;

  if (path == "/" || path == "/index.html") {
    response.contentType = "text/html; charset=utf-8";
    response.body = kControlPageHtml;
    return;
  }
  if (path == "/api/state") return handleState(response);
  if (path == "/api/interfaces") return handleInterfaces(response);
  if (path == "/api/route" && request.method == "POST") {
    return handleRoute(request, response);
  }
  if (path == "/api/mute" && request.method == "POST") {
    return handleMute(request, response);
  }

  // /preview/<id>.bmp
  const std::string prefix = "/preview/";
  if (path.rfind(prefix, 0) == 0) {
    std::string id = path.substr(prefix.size());
    const std::string suffix = ".bmp";
    if (id.size() > suffix.size() &&
        id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0) {
      id = id.substr(0, id.size() - suffix.size());
    }
    return handlePreview(id, response);
  }

  response.error(404, "no such endpoint: " + path);
}

void ControlApi::handleState(HttpServer::Response& response) const {
  auto state = json::Value::object();
  state.set("version", json::Value(FERRET_VERSION));
  state.set("rate", json::Value(engine_.rate().label()));
  state.set("running", json::Value(engine_.running()));
  state.set("muted", json::Value(engine_.router().muted()));

  const auto counters = engine_.counters();
  auto c = json::Value::object();
  c.set("ticks", json::Value(static_cast<int64_t>(counters.ticks)));
  c.set("framesDelivered",
        json::Value(static_cast<int64_t>(counters.framesDelivered)));
  c.set("blackDelivered",
        json::Value(static_cast<int64_t>(counters.blackDelivered)));
  c.set("conversions",
        json::Value(static_cast<int64_t>(counters.conversions)));
  c.set("lateTicks", json::Value(static_cast<int64_t>(counters.lateTicks)));
  c.set("measuredFps", json::Value(counters.measuredFps));
  state.set("counters", std::move(c));

  const auto reasons = engine_.sinkReasons();
  const auto plan = engine_.router().plan();

  // Sources and sinks are reported from the config rather than from the
  // engine, so a node that failed to build still appears — with its reason.
  // A UI that simply omits a broken node tells the operator nothing.
  auto sources = json::Value::array();
  auto sinks = json::Value::array();

  auto failureFor = [this](const std::string& id) -> std::string {
    for (const auto& f : failures_) {
      if (f.id == id) return f.reason;
    }
    return {};
  };

  for (const auto& node : config_.nodes) {
    auto n = json::Value::object();
    n.set("id", json::Value(node.id));
    n.set("label", json::Value(node.label.empty() ? node.id : node.label));
    n.set("kind", json::Value(toString(node.kind)));
    n.set("enabled", json::Value(node.enabled));
    n.set("width", json::Value(node.width));
    n.set("height", json::Value(node.height));
    n.set("rate", json::Value(node.rate.label()));
    if (!node.interfaceSelector.empty()) {
      n.set("interface", json::Value(node.interfaceSelector));
    }
    if (!node.target.empty()) n.set("target", json::Value(node.target));

    const std::string failure = failureFor(node.id);
    n.set("available", json::Value(failure.empty()));
    if (!failure.empty()) n.set("unavailable", json::Value(failure));

    if (canSource(node.kind)) {
      sources.push(n);
    }
    if (canSink(node.kind)) {
      auto s = n;
      s.set("routedFrom", json::Value(engine_.router().routedSource(node.id)));
      auto it = reasons.find(node.id);
      s.set("reason", json::Value(it == reasons.end() ? std::string() : it->second));

      for (const auto& action : plan) {
        if (action.sinkId != node.id) continue;
        const char* what = action.what == RouteAction::What::black    ? "black"
                           : action.what == RouteAction::What::copy   ? "copy"
                                                                     : "convert";
        s.set("action", json::Value(what));
        s.set("targetFormat", json::Value(toString(action.targetFormat)));
      }

      s.set("hasPreview",
            json::Value(previews_.find(node.id) != previews_.end()));
      sinks.push(s);
    }
  }

  state.set("sources", std::move(sources));
  state.set("sinks", std::move(sinks));

  auto fails = json::Value::array();
  for (const auto& f : failures_) {
    auto v = json::Value::object();
    v.set("id", json::Value(f.id));
    v.set("reason", json::Value(f.reason));
    fails.push(std::move(v));
  }
  state.set("failures", std::move(fails));

  response.json(state.serialize(true));
}

void ControlApi::handleRoute(const HttpServer::Request& request,
                             HttpServer::Response& response) {
  std::string sink = request.param("sink");
  std::string source = request.param("source");

  // A JSON body wins over query parameters, so the API is pleasant from both
  // curl and a browser.
  if (!request.body.empty()) {
    auto parsed = json::parse(request.body);
    if (parsed && parsed->isObject()) {
      if (parsed->has("sink")) sink = (*parsed)["sink"].asString();
      if (parsed->has("source")) source = (*parsed)["source"].asString();
    }
  }

  if (sink.empty()) {
    response.error(400, "missing \"sink\"");
    return;
  }

  std::string error;
  if (!engine_.router().route(sink, source, error)) {
    response.error(400, error);
    return;
  }

  auto ok = json::Value::object();
  ok.set("sink", json::Value(sink));
  ok.set("source", json::Value(source));
  response.json(ok.serialize());
}

void ControlApi::handleMute(const HttpServer::Request& request,
                            HttpServer::Response& response) {
  std::string value = request.param("muted");
  if (!request.body.empty()) {
    auto parsed = json::parse(request.body);
    if (parsed && parsed->isObject() && parsed->has("muted")) {
      value = (*parsed)["muted"].asBool() ? "true" : "false";
    }
  }

  if (value.empty()) {
    // No argument means toggle, which is what a Companion button wants.
    engine_.router().setMuted(!engine_.router().muted());
  } else {
    engine_.router().setMuted(value == "true" || value == "1");
  }

  auto ok = json::Value::object();
  ok.set("muted", json::Value(engine_.router().muted()));
  response.json(ok.serialize());
}

void ControlApi::handleInterfaces(HttpServer::Response& response) const {
  std::string error;
  const auto found = listInterfaces(error);

  auto arr = json::Value::array();
  for (const auto& n : found) {
    auto v = json::Value::object();
    v.set("name", json::Value(n.name));
    v.set("address", json::Value(n.address));
    v.set("isV6", json::Value(n.isV6));
    v.set("isUp", json::Value(n.isUp));
    v.set("isLoopback", json::Value(n.isLoopback));
    v.set("multicast", json::Value(n.supportsMulticast));
    v.set("index", json::Value(static_cast<int64_t>(n.index)));
    v.set("linkSpeedBps", json::Value(static_cast<int64_t>(n.linkSpeedBps)));
    arr.push(std::move(v));
  }

  auto out = json::Value::object();
  out.set("interfaces", std::move(arr));
  if (!error.empty()) out.set("error", json::Value(error));
  response.json(out.serialize(true));
}

void ControlApi::handlePreview(const std::string& id,
                               HttpServer::Response& response) const {
  auto it = previews_.find(id);
  if (it == previews_.end()) {
    response.error(404, "no preview sink with id \"" + id + "\"");
    return;
  }

  std::vector<uint8_t> bmp;
  if (!it->second->encodeBmp(bmp)) {
    response.error(503, "no frame yet");
    return;
  }

  response.useBinary = true;
  response.binaryBody = std::move(bmp);
  response.contentType = "image/bmp";
  // The preview changes every frame, so any caching at all is wrong.
  response.extraHeaders["Cache-Control"] = "no-store";
}

}  // namespace ferret

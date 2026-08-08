#include "app/config.h"

#include "check.h"

using namespace ferret;

namespace {

const char* kGood = R"({
  "rate": "59.94",
  "control": { "bind": "0.0.0.0", "port": 9000, "token": "abc" },
  "nodes": [
    { "id": "bars", "kind": "test", "width": 1280, "height": 720 },
    { "id": "prev", "kind": "preview", "label": "Monitor" }
  ],
  "routes": { "prev": "bars" }
})";

void aGoodConfigLoadsEverything() {
  AppConfig cfg;
  std::string err;
  CHECK(parseConfig(kGood, &cfg, err));

  CHECK_EQ(cfg.rate.num, int64_t{60000});
  CHECK_EQ(cfg.rate.den, int64_t{1001});
  CHECK_EQ(cfg.controlBind, std::string("0.0.0.0"));
  CHECK_EQ(cfg.controlPort, 9000);
  CHECK_EQ(cfg.controlToken, std::string("abc"));
  CHECK_EQ(cfg.nodes.size(), size_t{2});

  CHECK_EQ(cfg.nodes[0].id, std::string("bars"));
  CHECK_EQ(cfg.nodes[0].kind, NodeKind::testPattern);
  CHECK_EQ(cfg.nodes[0].width, 1280);
  // An unspecified label defaults to the id rather than to empty, so a UI
  // never has to render a nameless node.
  CHECK_EQ(cfg.nodes[0].label, std::string("bars"));
  CHECK_EQ(cfg.nodes[1].label, std::string("Monitor"));

  CHECK_EQ(cfg.routes.size(), size_t{1});
  CHECK_EQ(cfg.routes.at("prev"), std::string("bars"));
}

/// Every one of these is a config that half-loads if the parser is lenient,
/// and a show that half-works.
void badConfigsAreRejectedWithAUsefulMessage() {
  AppConfig cfg;
  std::string err;

  CHECK(!parseConfig("not json", &cfg, err));
  CHECK(err.find("valid JSON") != std::string::npos);

  CHECK(!parseConfig("[]", &cfg, err));
  CHECK(err.find("object") != std::string::npos);

  CHECK(!parseConfig(R"({"nodes": []})", &cfg, err) == false);  // empty is fine

  // Missing id.
  CHECK(!parseConfig(R"({"nodes":[{"kind":"test"}]})", &cfg, err));
  CHECK(err.find("id") != std::string::npos);

  // Missing kind.
  CHECK(!parseConfig(R"({"nodes":[{"id":"a"}]})", &cfg, err));
  CHECK(err.find("kind") != std::string::npos);

  // Unknown kind — and the message must point at how to find the real list.
  CHECK(!parseConfig(R"({"nodes":[{"id":"a","kind":"telepathy"}]})", &cfg, err));
  CHECK(err.find("telepathy") != std::string::npos);
  CHECK(err.find("frame-ferret kinds") != std::string::npos);

  // A rate that is not a real broadcast rate, rather than silently rounded.
  CHECK(!parseConfig(
      R"({"nodes":[{"id":"a","kind":"test","rate":"50.5"}]})", &cfg, err));
  CHECK(err.find("50.5") != std::string::npos);

  // Zero geometry.
  CHECK(!parseConfig(
      R"({"nodes":[{"id":"a","kind":"test","width":0,"height":100}]})", &cfg,
      err));
  CHECK(err.find("width") != std::string::npos);

  // Port out of range.
  CHECK(!parseConfig(
      R"({"control":{"port":70000},"nodes":[{"id":"a","kind":"test"}]})", &cfg,
      err));
  CHECK(err.find("port") != std::string::npos);
}

/// Duplicate ids would make the crosspoint ambiguous and the control API
/// address whichever node it found first.
void duplicateIdsAreFatal() {
  AppConfig cfg;
  std::string err;
  CHECK(!parseConfig(
      R"({"nodes":[{"id":"a","kind":"test"},{"id":"a","kind":"preview"}]})",
      &cfg, err));
  CHECK(err.find("duplicate") != std::string::npos);
  CHECK(err.find("\"a\"") != std::string::npos);
}

/// A route naming a node that cannot play that role is a config error, not
/// something to discover at the first tick.
void routesAreValidatedForDirection() {
  AppConfig cfg;
  std::string err;

  // Unknown sink.
  CHECK(!parseConfig(
      R"({"nodes":[{"id":"bars","kind":"test"}],"routes":{"ghost":"bars"}})",
      &cfg, err));
  CHECK(err.find("ghost") != std::string::npos);

  // Unknown source.
  err.clear();
  CHECK(!parseConfig(
      R"({"nodes":[{"id":"p","kind":"preview"}],"routes":{"p":"ghost"}})", &cfg,
      err));
  CHECK(err.find("ghost") != std::string::npos);

  // A source used as a sink. "test" can only source.
  err.clear();
  CHECK(!parseConfig(
      R"({"nodes":[{"id":"bars","kind":"test"},{"id":"b2","kind":"test"}],
          "routes":{"bars":"b2"}})",
      &cfg, err));
  CHECK(err.find("cannot be a sink") != std::string::npos);

  // A sink used as a source. "preview" can only sink.
  err.clear();
  CHECK(!parseConfig(
      R"({"nodes":[{"id":"p","kind":"preview"},{"id":"p2","kind":"preview"}],
          "routes":{"p":"p2"}})",
      &cfg, err));
  CHECK(err.find("cannot be a source") != std::string::npos);
}

/// An explicitly empty route means "deliberately unrouted", which is a valid
/// state and must not be an error.
void anEmptyRouteIsAllowed() {
  AppConfig cfg;
  std::string err;
  CHECK(parseConfig(
      R"({"nodes":[{"id":"p","kind":"preview"}],"routes":{"p":""}})", &cfg,
      err));
  CHECK(cfg.routes.find("p") == cfg.routes.end());
}

void defaultsAreSensible() {
  AppConfig cfg;
  std::string err;
  CHECK(parseConfig(R"({"nodes":[{"id":"a","kind":"test"}]})", &cfg, err));
  CHECK_EQ(cfg.rate.num, int64_t{50});
  CHECK_EQ(cfg.controlBind, std::string("127.0.0.1"));  // loopback, not 0.0.0.0
  CHECK_EQ(cfg.controlPort, 8740);
  CHECK(cfg.nodes[0].enabled);
  CHECK_EQ(cfg.nodes[0].width, 1920);
  CHECK_EQ(cfg.nodes[0].height, 1080);
}

/// The selftest config is built in code so that editing a file cannot break
/// the self test. Pin its shape.
void theSelftestConfigIsSelfContained() {
  const AppConfig cfg = selftestConfig();
  CHECK_EQ(cfg.nodes.size(), size_t{2});
  CHECK_EQ(cfg.controlPort, 0);  // no control server during a self test
  CHECK_EQ(cfg.routes.at("preview"), std::string("bars"));

  bool hasSource = false, hasSink = false;
  for (const auto& n : cfg.nodes) {
    if (canSource(n.kind)) hasSource = true;
    if (canSink(n.kind)) hasSink = true;
  }
  CHECK(hasSource);
  CHECK(hasSink);
}

void run() {
  aGoodConfigLoadsEverything();
  badConfigsAreRejectedWithAUsefulMessage();
  duplicateIdsAreFatal();
  routesAreValidatedForDirection();
  anEmptyRouteIsAllowed();
  defaultsAreSensible();
  theSelftestConfigIsSelfContained();
}

}  // namespace

TEST_MAIN("config")

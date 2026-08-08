#include "app/node.h"

namespace ferret {
namespace {

struct KindEntry {
  NodeKind kind;
  const char* name;
  bool source;
  bool sink;
};

const KindEntry kKinds[] = {
    {NodeKind::ndi, "ndi", true, true},
    {NodeKind::omt, "omt", true, true},
    {NodeKind::srt, "srt", true, true},
    {NodeKind::st2110, "st2110", true, true},
    {NodeKind::displayCapture, "display", true, false},
    {NodeKind::windowCapture, "window", true, false},
    {NodeKind::applicationCapture, "application", true, false},
    {NodeKind::sharedSurfaceIn, "surface-in", true, false},
    {NodeKind::virtualDisplay, "virtual-display", true, false},
    {NodeKind::uvcCamera, "uvc", false, true},
    {NodeKind::sharedSurfaceOut, "surface-out", false, true},
    {NodeKind::decklink, "decklink", false, true},
    {NodeKind::decklinkIn, "decklink-in", true, false},
    {NodeKind::htmlOverlay, "html", false, true},
};

const KindEntry* find(NodeKind k) {
  for (const auto& e : kKinds) {
    if (e.kind == k) return &e;
  }
  return nullptr;
}

}  // namespace

const char* toString(NodeKind k) {
  const auto* e = find(k);
  return e ? e->name : "unknown";
}

bool nodeKindFromString(const std::string& s, NodeKind* out) {
  for (const auto& e : kKinds) {
    if (s == e.name) {
      if (out) *out = e.kind;
      return true;
    }
  }
  return false;
}

bool canSource(NodeKind k) {
  const auto* e = find(k);
  return e && e->source;
}

bool canSink(NodeKind k) {
  const auto* e = find(k);
  return e && e->sink;
}

}  // namespace ferret

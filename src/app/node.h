#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/frame.h"
#include "net/interfaces.h"

namespace ferret {

/// What a node is made of. Kept as data rather than as a class hierarchy
/// because the control API, the config file and the web UI all need to talk
/// about a node that does not exist yet.
enum class NodeKind {
  // Transports — every one of these can be either a source or a sink, which
  // is the central claim of this program. See docs/01-architecture.md.
  ndi,
  omt,
  srt,
  st2110,

  /// A synthetic source: 75% colour bars with a moving marker. Not a
  /// debugging afterthought — it is how the whole frame path is proven with no
  /// SDK, no network and no hardware, and it is what `selftest` drives.
  testPattern,

  /// Holds the latest frame for the control page. A real sink, routed through
  /// the crosspoint like any other, so the operator sees what the router
  /// actually produced rather than a picture assembled somewhere else.
  preview,

  // Source-only: things that observe this machine.
  displayCapture,
  windowCapture,
  applicationCapture,
  sharedSurfaceIn,  ///< Syphon (macOS) / Spout (Windows) client.
  virtualDisplay,   ///< A display we present to the OS and then capture.

  // Sink-only: things that present to this machine or to hardware.
  uvcCamera,         ///< The virtual capture device other apps see.
  sharedSurfaceOut,  ///< Syphon / Spout server.
  decklink,          ///< Also a source; see `decklinkIn`.
  decklinkIn,
  htmlOverlay,  ///< A page to load as an OBS browser source.
};

const char* toString(NodeKind k);
bool nodeKindFromString(const std::string& s, NodeKind* out);

/// Whether a kind can act in each direction. Enforced at config load so an
/// impossible route ("route the UVC camera into NDI") is rejected with a
/// sentence rather than constructed and then silently doing nothing.
bool canSource(NodeKind k);
bool canSink(NodeKind k);

/// Everything needed to build one node.
struct NodeConfig {
  std::string id;  ///< Stable, user-visible, unique. Used by the control API.
  NodeKind kind = NodeKind::ndi;
  std::string label;

  /// Which NIC this node uses. Empty means the OS default route. Meaningful
  /// only for the transport kinds; ignored with a warning elsewhere rather
  /// than rejected, because a config copied between machines should not fail
  /// over a field that does not apply.
  std::string interfaceSelector;

  /// Transport-specific target: an NDI source name, an `srt://` URL, a 2110
  /// multicast group, a display index, a window title.
  std::string target;

  Rate rate{50, 1};
  int width = 1920;
  int height = 1080;
  PixelFormat format = PixelFormat::unknown;  ///< unknown = node's own default.

  /// A region of interest, in source pixels. Zero width or height means the
  /// whole thing. Only the capture sources use it, and ScreenCaptureKit takes a
  /// source rect directly — so a crop is free rather than a capture-then-discard.
  int cropX = 0;
  int cropY = 0;
  int cropW = 0;
  int cropH = 0;

  /// Which ffmpeg to use, for the nodes that need an external codec. Empty
  /// searches $FERRET_FFMPEG, then $PATH, then the usual install locations.
  /// A value containing a separator is an explicit path and is never fallen
  /// back from — see Ffmpeg::locate.
  std::string ffmpegPath;

  bool enabled = true;
};

/// A node that produces frames.
class Source {
 public:
  virtual ~Source() = default;

  virtual const std::string& id() const = 0;

  /// True once the node has a live upstream. A source that is configured but
  /// not yet connected reports false and delivers nothing — it never delivers
  /// a stale frame, because a frozen picture on an output is far harder to
  /// diagnose in a show than a black one.
  virtual bool connected() const = 0;

  /// Waits up to `timeoutMs` for the next frame. The frame passed to `onVideo`
  /// is valid only for the duration of the call.
  virtual bool poll(unsigned timeoutMs,
                    const std::function<void(const VideoFrame&)>& onVideo) = 0;

  virtual std::unique_ptr<AudioFrame> takeAudio() { return nullptr; }
  virtual std::unique_ptr<AncillaryFrame> takeAncillary() { return nullptr; }
};

/// A node that consumes frames.
class Sink {
 public:
  virtual ~Sink() = default;

  virtual const std::string& id() const = 0;

  /// The formats this sink can take without a conversion. The router consults
  /// this to decide whether a route is a copy or a convert; an empty list
  /// means "anything, I convert internally".
  virtual std::vector<PixelFormat> preferredFormats() const { return {}; }

  virtual void send(const VideoFrame& frame) = 0;
  virtual void sendAudio(const AudioFrame&) {}
  virtual void sendAncillary(const AncillaryFrame&) {}

  /// Emit a frame of black in this sink's own format and geometry.
  ///
  /// This is not a convenience. It is the mechanism behind the invariant that
  /// an output never stops: when a route's source is missing, the router calls
  /// this rather than skipping the sink. A UVC device that stops delivering
  /// gets dropped by Zoom and Teams within seconds and does not come back
  /// without the host app restarting; an SDI output that stops has to be
  /// re-locked by whatever is downstream. Black is recoverable, silence is not.
  virtual void sendBlack() = 0;
};

}  // namespace ferret

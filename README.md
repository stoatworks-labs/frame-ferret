# Frame Ferret

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. **NDI and OMT both work
> in both directions**, verified against oxbow and against macOS's own `dns-sd`
> — separate implementations, not this code. SRT's transport layer is built and
> proven over a loopback connection, but SRT carries *compressed* MPEG-TS, and
> the encoder and decoder that would let it carry frames **do not exist yet**,
> so there is no SRT node. 11 test binaries, 535 checks, all passing. ST 2110,
> screen capture, the UVC camera, DeckLink and Syphon/Spout are designed and
> documented but not implemented, and no hardware output exists on any
> platform. See [Status](#status).

A software virtual capture card. One application that is an **NDI, OMT, SRT and
ST 2110 endpoint** — transmitting and receiving — over a chosen network
interface, that can capture this machine's screen, and that presents itself to
other software as a camera, a shared GPU surface, an SDI output or a web
overlay.

The idea is that a protocol is just a port on a router. Anything can be a
source, anything can be a sink, and the crosspoint in the middle does not care
which is which.

## What it is meant to do

**As a source — things it can pick up**

- Display, window, application and region-of-interest capture
- A virtual display that other applications can be dragged onto
- Syphon (macOS) and Spout (Windows) servers published by other applications
- Incoming NDI, OMT, SRT and ST 2110 streams
- DeckLink capture input

**As a sink — things it can present to**

- A **UVC capture device**, so Zoom, Teams, OBS and anything else sees it as a
  camera
- A Syphon / Spout server for Resolume, OBS, VDMX and similar
- DeckLink cards, straight out to SDI
- An HTML page, to bring into OBS as a browser-source overlay
- Outgoing NDI, OMT, SRT and ST 2110

**Everywhere**

- Explicit network interface binding, because a stream on the wrong NIC is
  diagnosed on site as a network fault. Note NDI is the exception: its C API has
  no interface parameter at all, so Frame Ferret warns rather than pretending
  the setting applied
- A system tray launcher, configured through a web browser, in the same shape
  as [WebLinked](https://github.com/stoatworks-labs/weblinked)

## Status

Honest, and it will stay honest as this grows.

| Area | State |
|---|---|
| Node model, crosspoint router | **Built and tested.** 36 checks |
| Exact rational rates and tick deadlines | **Built and tested.** 40 checks |
| Pixel format model | **Built and tested.** 33 checks |
| Pixel conversion (BGRA/RGBA/UYVY/YUY2/v210) | **Built and tested.** 103 checks, including a colour-bar round trip verified by pixel readback |
| Frame loop / engine | **Built, tested and run.** 37 checks; 9049 ticks at 50 fps with zero late |
| Config loading | **Built and tested.** 57 checks |
| Interface enumeration and binding | **Built and runs.** 81 checks, verified against this machine's real NICs. The count varies by machine — the suite walks whatever it finds |
| Test-pattern source, preview sink | **Built and run** |
| Web control page + JSON API | **Built and driven in a browser.** Crosspoint clicks verified against the API |
| CLI (`run`, `selftest`, `interfaces`, `kinds`) | **Runs** |
| **NDI send and receive** | **Built and verified against a separate implementation.** Runtime-loaded, never linked |
| **OMT send and receive** | **Built and verified**, against oxbow and macOS `dns-sd`. Runtime-loaded |
| **SRT transport** | **Built and proven over loopback** — caller/listener, real interface binding, latency, passphrase, stream id. **No SRT node yet:** it needs an encoder |
| **SRT receive (decode)** | **Built and verified end to end** against an independent Rust SRT sender — all seven SMPTE bars decoded correctly |
| **SRT send (encode)** | **Built and verified end to end** — all eight bars within 2 code values through H.264, read back by independent tools |
| **External codec via ffmpeg** | **Built and verified.** Points at an install you already have; nothing linked, nothing bundled |
| **DeckLink output** | **Run on a real Duo 2**: 1080p50 in v210, 2531 frames with **0 late and 0 dropped** by the card's own count. Pixels on the wire not yet checked — that needs an SDI loop cable |
| **ST 2110-20 send and receive** | **Built and verified against GStreamer's RFC 4175 implementation**, both directions, within 2 code values. Wide-profile sender, system-clocked — see below |
| ST 2110-30 audio, -40 ancillary | **Not started** |
| **Display, window, application and ROI capture** (macOS) | **Built and run.** ScreenCaptureKit; region of interest is a source rect, so a crop costs nothing |
| UVC virtual camera | **Not started.** Needs an embedded provisioning profile, which the fleet's release harness does not yet produce — see [docs/03-os-extensions.md](docs/03-os-extensions.md) |
| Virtual display | **Not started.** No public macOS API exists — see the same document |
| **Syphon output** (macOS) | **Built and verified against Resolume's Syphon 5** — a different implementation from the Syphon 6 sources vendored here |
| DeckLink capture, Spout, HTML output | **Not started** |
| Tray launcher | **Not started** |
| Windows, Linux | **Built and self-tested by CI**, all three platforms green. Never run interactively, and no hardware path exists on any of them |

### What has actually been observed

Running `frame-ferret run` on this Mac: colour bars generated at 1280x720p50,
routed through the crosspoint, converted, and displayed live on the control
page. Over one 9049-tick run: **4507 frames delivered + 4542 black = 9049**,
exactly one action per sink per tick, zero late ticks, 49.99 fps measured.

Unrouting the sink from the browser turned the output black *and kept it
running at 50 fps*, reporting `no source routed` — which is the invariant this
whole program is built around, observed rather than asserted.

**OMT, both directions.** Sending is confirmed by macOS's own `dns-sd`, which
browses `_omt._tcp` and lists `MAC (FerretOMT)` on four interfaces, and by oxbow
receiving at `omt://127.0.0.1:6400` and decoding all eight bars **within 2 code
values** — much closer than NDI's 13, because OMT carried BGRA rather than
compressing it. Receiving is a Frame Ferret round trip at a clean 50 fps.

**NDI, both directions, against an independent implementation.** Sending:
[oxbow](https://github.com/stoatworks-labs/oxbow) discovers `MAC (FerretTest)`,
receives 1280x720p50, and all eight colour bars decode in the right order with
the right hues. Receiving: Frame Ferret takes oxbow's stream at 59.97 fps, 344
frames with 2 black (the ticks before connection), as a `copy` in UYVY with no
conversion.

One honest note on that measurement: saturated blue comes back 178 rather than
191. That is NDI's SpeedHQ compression, not this code — oxbow's own sender
through the same round trip lands on exactly the same 178. The control
experiment mattered; the number alone would have looked like a bug.

**How SRT uses ffmpeg.** NDI and OMT carry raw frames, so a node is a thin
wrapper over the SDK. SRT carries an MPEG transport stream of *compressed*
video, so it needs a codec. Frame Ferret runs **ffmpeg as a subprocess** rather
than linking it:

- **You point at the install you already have.** The node's `"ffmpeg"` setting,
  `$FERRET_FFMPEG`, `$PATH`, or the usual locations — in that order. An
  explicit path that is wrong is an error, never a quiet fallback to some other
  ffmpeg.
- **No ABI coupling.** libavcodec's structs change between major versions;
  mirroring them by hand would be far more fragile than NDI, OMT or libsrt,
  which all expose deliberately flat C ABIs.
- **Licensing stays simple.** ffmpeg is LGPL and commonly built as GPL with
  libx264. A separate process over a pipe raises no question that an MIT
  codebase needs to answer.
- **CI keeps building**, with no ffmpeg on the runners and nothing to detect.

Hardware encoders are preferred where present — on this Mac it selects
`h264_videotoolbox` over `libx264`.

**SRT receive, verified end to end.** Nothing already on the machine could act
as a sender — Homebrew's ffmpeg has no SRT compiled in, `srt-file-transmit`
speaks SRT's *stream* API and our live-mode receiver correctly rejects it, and
`srt-live-transmit` only bridges UDP↔SRT — so `tools/srtsend` was written
against **srt-tokio**, the pure-Rust SRT stack `srt-router` uses. That is a
completely different implementation from the libsrt Frame Ferret binds, which
is what makes the result meaningful.

Frame Ferret received all **129,156 bytes byte-for-byte**, decoded them through
an external ffmpeg, and rendered all seven SMPTE bars at the right levels in
the right channel order.

**SRT send, verified end to end.** Frame Ferret encodes with
`h264_videotoolbox`, muxes to MPEG-TS and listens on SRT;
`srt-live-transmit` pulls the stream and ffmpeg decodes it back to a still. All
eight bars land **within 2 code values** — through a lossy H.264 round trip.

Getting there fixed two real colour bugs, both of which had left hues and bar
order perfect while the values were wrong, which is the hardest way for a
colour fault to present:

- **Range.** An RGB source was encoded as full-range YCbCr and the receiver
  expanded it again — green came back at 240 instead of 191. `convert()` now
  reports the range it produced and normalises RGB sources to narrow, which is
  what every transport here expects.
- **Matrix.** rawvideo carries no colorimetry, so ffmpeg guesses **BT.601
  below 720 lines**. Sending it 709 data at 640x360 without saying so skewed
  every saturated colour while white and black stayed exact. Both input and
  output are now tagged explicitly.

SRT is the **only transport here that can genuinely bind to a chosen
interface**: `srt_bind()` takes a real address, where NDI and OMT have no such
parameter at all.

**Syphon, verified by a consumer that is not this code.** WebLinked's
`syphon_probe` links **Resolume Arena's bundled Syphon 5**, not the Syphon 6
server sources vendored here, and is started *after* Frame Ferret — which is
the exact case the main-thread trap breaks. It finds
`Frame Ferret (frame-ferret)`, receives a 1280x720 frame, and reads BGRA
`191 0 191` at x=640, which is magenta: the right colour, in the right channel
order, at the right place in the bars.

**DeckLink, on real hardware.** A Duo 2 in the 2-sub-device half-duplex profile,
output on connector 1 at 1920x1080p50 as **v210**. The card's own completion
callback reports **2531 frames, 0 late, 0 dropped** over ~50 seconds, with the
engine holding 49.98 fps. What is *not* verified is the picture on the wire:
that needs an SDI cable looping an output back to an input, and there was none.

Getting there needed the conversion made three times faster. BGRA to v210 at
1080p cost 26.6 ms — a 38 fps ceiling — so the card ran at 34 fps with a third
of its ticks late. Folding the colour coefficients and then fusing RGB to v210
past the shared intermediate took it to **12.7 ms**.

**Audio now follows the same crosspoint as video.** The test pattern emits a
1 kHz tone at -20 dBFS alongside the bars, and it travels the routes the video
does. Across NDI: 4478 audio frames sent, and a second Frame Ferret received
and routed 287 of them back through libndi's own encode and decode. Both ends
are this codebase, so that is not a fully independent check — but libndi sits
in the middle, and a wrong planar layout would not survive it.

**Screen capture, on this machine.** A display at 1280x720p30 gave 175 frames of
real desktop content; a named window gave 136 frames with zero black; and two
different regions of the same display captured as genuinely different pictures,
which is what proves the crop is applied rather than ignored.

Two things about it worth knowing. **Without Screen Recording permission a
capture starts successfully and delivers black frames forever** — no error, no
callback, nothing in any log — so permission is checked up front and reported
as the reason a node failed. And a plain C++ CLI must call `NSApplicationLoad()`
before touching CoreGraphics: *window* capture asserted with
`CGS_REQUIRE_INIT` on first run while *display* capture worked, because only
the window path reaches that code.

**ST 2110-20, both directions, against an independent implementation.**
GStreamer's `rtpvrawdepay` decoded 143 frames of Frame Ferret's stream with a
worst channel error of **1 code value**; Frame Ferret decoded 196 frames from
GStreamer's `rtpvrawpay` within **2**. ST 2110-20 is RFC 4175 with constraints,
so a conformant RFC 4175 implementation is a real check rather than this repo
agreeing with itself.

2110 is also the **only** transport here that binds an interface properly and
must: multicast send needs an explicit egress interface, and a join is keyed on
the interface *index*, because two interfaces can hold the same address after a
DHCP reshuffle. The SDP is served through the control API, since pasting one is
how most 2110 equipment is configured.

**What this is not.** It is a **wide-profile sender** (`a=TP=2110TPW`,
declared honestly), clocked from the system monotonic clock rather than a PTP
servo, and frames are never marked `ptpLocked`. Narrow profile needs hardware
transmit pacing, and macOS has no supported hardware timestamping path at all.
See [docs/02-st2110.md](docs/02-st2110.md) — that was known before a line was
written and nothing since has changed it.

Two constraints found while scoping, both written up in full because they
shape everything downstream:

- **ST 2110-20 will be a wide-profile sender.** Narrow profile needs hardware
  transmit pacing, and macOS additionally has no supported hardware
  timestamping path for PTP. Wide is in-spec and widely accepted, but it is not
  the same claim. [docs/02-st2110.md](docs/02-st2110.md)
- **macOS has no public virtual display API.** The only route is private
  CoreGraphics classes. ScreenCaptureKit window/application capture may absorb
  most of what the feature is actually wanted for.
  [docs/03-os-extensions.md](docs/03-os-extensions.md)

## Download

<!-- downloads:start -->
<!-- Generated by stoatworks-backend/release/gen-downloads.py — do not hand-edit. -->
<!-- downloads:end -->

Every transport is loaded at run time rather than linked, so a downloaded build
starts on a machine with none of them installed and reports each as unavailable
with a sentence saying where to get it. The exception is DeckLink, which needs
Blackmagic's SDK at build time and is therefore compiled out of the published
builds entirely — build from source with `-DDECKLINK_SDK_DIR=…` for SDI.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8
```

```bash
cd build && ctest --output-on-failure
```

Run it, then open <http://127.0.0.1:8740/>:

```bash
./build/frame-ferret run
```

Prove the whole frame path without a network or any hardware:

```bash
./build/frame-ferret selftest
```

```bash
./build/frame-ferret interfaces
```

Send colour bars as NDI, then find them from any NDI receiver:

```bash
./build/frame-ferret run --config config/ndi-send.json
```

`config/example.json` is a minimal working configuration.
`config/not-yet-implemented.json` shows the shape a real one will take — it
starts today and reports every unbuilt node as unavailable, which is the
intended behaviour rather than a fault.

## Documentation

- [docs/01-architecture.md](docs/01-architecture.md) — the crosspoint model, the
  plan-every-sink invariant, why pixel formats travel with frames
- [docs/02-st2110.md](docs/02-st2110.md) — what full 2110 conformance costs
- [docs/03-os-extensions.md](docs/03-os-extensions.md) — virtual camera and
  virtual display, per platform
- [AGENTS.md](AGENTS.md) — onboarding for whoever picks this up next

## Licence

MIT.

NDI® is a registered trademark of Vizrt NDI AB. This repository contains no NDI
libraries; the runtime is loaded from whatever is installed on the machine. See
[ndi.video](https://ndi.video).

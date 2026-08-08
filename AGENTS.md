# Frame Ferret — agent onboarding

A software virtual capture card: NDI / OMT / SRT / ST 2110 endpoint in both
directions over a chosen NIC, screen capture in, and a UVC camera, shared GPU
surface, DeckLink output or HTML overlay out. C++20, CMake. Public MIT repo —
"commit" means commit **and** push.

Read [docs/01-architecture.md](docs/01-architecture.md) first. It is short and
it is the whole mental model.

## What is genuinely verified vs assumed

This section is the most important one in the file. Keep it honest, and never
upgrade "compiles" to "works".

**Built, tested and run on this machine** — 535 checks across 11 binaries:

- The crosspoint `Router` and its plan-every-sink invariant — 36 checks.
- Exact rational rates, tick deadlines and the drift property — 40 checks,
  including a pin on the specific unsigned-promotion bug that cost oxbow a day.
- The pixel format model, strides and packing-group rounding — 33 checks.
- Pixel conversion between BGRA/RGBA/UYVY/YUY2/v210 — 103 checks, including a
  75% colour-bar round trip verified by pixel readback (within 2 code values),
  an explicit channel-swap check, and legal-black behaviour.
- The `Engine` frame loop — 37 checks, driven through a real running engine
  rather than through `Router::plan()` alone.
- Config parsing and its rejections — 57 checks.
- Interface enumeration and selector resolution — 81 checks (count is
  machine-dependent; the suite walks real interfaces). The `interfaces` command
  has been run and checked against `ifconfig`, including confirming a Wi-Fi
  interface reports unknown link speed rather than a fabricated one.
- **The whole application, end to end.** `frame-ferret run` generates colour
  bars, routes them, converts, and serves a live control page. One 9049-tick
  run gave 4507 frames + 4542 black = 9049 exactly — one action per sink per
  tick — at 49.99 fps with zero late ticks. The crosspoint has been clicked in
  a real browser and the resulting route verified against the REST API.

- **NDI, both directions, verified against a separate codebase.** oxbow
  discovers `MAC (FerretTest)` and decodes all eight bars in the right order;
  Frame Ferret receives oxbow's stream at 59.97 fps as a UYVY copy. Runtime
  loaded, never linked. 40 checks on the ABI and the no-runtime path.

- **OMT, both directions, verified against two things that are not this code.**
  macOS's own `dns-sd` browses `_omt._tcp` and lists `MAC (FerretOMT)`; oxbow
  receives at `omt://127.0.0.1:6400` and decodes all eight bars within 2 code
  values. Receiving is a round trip at 50 fps.
- **SRT's transport layer**, proven by a real loopback listener/caller pair
  carrying bytes. **There is no SRT node** — see below.

**SRT receive is verified end to end**, against `tools/srtsend` — ~50 lines on
**srt-tokio**, the pure-Rust SRT stack srt-router uses, and a completely
different implementation from the libsrt this binds. 129,156 bytes received
byte-for-byte, decoded through an external ffmpeg, all seven SMPTE bars correct.

Nothing already installed could act as that sender, and the reasons are worth
knowing before trying again: **Homebrew's ffmpeg has no SRT protocol**
(`-protocols` shows only `srtp`); **`srt-file-transmit` speaks SRT's stream
API** and a live-mode receiver correctly rejects it with
"MessageAPI/StreamAPI collision"; **`srt-live-transmit` only bridges UDP↔SRT**
and refuses `file://` at both ends. See `tools/srtsend/README.md`.

**SRT send is verified too**: encoded with `h264_videotoolbox`, muxed to
MPEG-TS, pulled off with `srt-live-transmit` and decoded by ffmpeg — all eight
bars within 2 code values through a lossy H.264 round trip.

Note SRT **can** bind to a chosen interface — `srt_bind()` takes a real address
and there is SRTO_BINDTODEVICE — which makes it the first transport here where
the `interface` setting does something. NDI and OMT can only warn.

**DeckLink output has been run on a real Duo 2**: 1920x1080p50 as v210 on
connector 1, with the card's own completion callback reporting **2531 frames,
0 late, 0 dropped** over ~50 seconds. **The pixels on the wire are still
unverified** — that needs an SDI cable looping an output back to an input, and
there was none. Do not upgrade "the card accepted and displayed every frame" to
"the picture is correct".

Two things about the sink worth keeping:
- **It advertises v210 and UYVY only, never BGRA.** The router treats any
  accepted format as a copy, so advertising BGRA means a BGRA source *gets*
  BGRA — and a Duo 2 will not carry 1080p50 as 8-bit BGRA at all, so the whole
  output fails having never tried the format it can do.
- The card enumerates only in the **2-sub-device half-duplex (2dhd)** profile;
  `oxbow/build/dl_profile` shows and sets it. Optional at build time via
`-DDECKLINK_SDK_DIR`; the version guard rejects anything below 11.0 and is
proven to reject the 10.11 copy inside the NDI SDK's examples.

- **Syphon output (macOS), verified against Resolume's Syphon 5** — a
  different implementation from the vendored Syphon 6 sources — from a process
  started *after* Frame Ferret, which is the exact case the main-thread trap
  breaks. Frame received, colours and channel order correct.

- **ST 2110-20, both directions, verified against GStreamer's RFC 4175
  payloader and depayloader** — 1 and 2 code values respectively. 2110-20 is
  RFC 4175 with constraints, so GStreamer is a genuine independent check.
  `st2110_rtp.*` is deliberately socket-free so the packetiser can be tested
  exhaustively; `st2110.cpp` holds the UDP and multicast side.

  It is a **wide-profile sender**, system-clocked, and says so in its SDP
  (`a=TP=2110TPW`). Frames are never marked `ptpLocked`. Do not upgrade that
  claim without hardware transmit pacing and a real PTP servo.

- **Audio routed through the crosspoint**, with a 1 kHz tone in the test
  pattern to drive it. 63 checks on the engine now. Verified across NDI as a
  Frame Ferret round trip through libndi — **not** against an independent
  receiver, because oxbow's probe has no audio support at all. Do not claim
  more than that.
- **Display, window, application and ROI capture (macOS), run on this
  machine.** ScreenCaptureKit. A display gave 175 frames of real content, a
  named window 136 with zero black, and two different regions captured as
  genuinely different pictures — which is the check that proves the crop is
  applied rather than silently ignored.

**Not written at all.** ST 2110-30 audio and -40 ancillary, DeckLink *capture*,
Spout, the HTML output, the OS extensions, and the tray launcher. Do not
describe any of it as working.

**Windows and Linux: built and self-tested by CI**, all three platforms green.
Because the CI `selftest` step drives the whole frame path — generator, router,
conversion, pacing — they have genuinely *run* it, not merely compiled. But
neither has been run interactively, neither has a window or a GPU path, and no
hardware backend exists on any platform. Treat "the synthetic path runs on all
three" as the claim; nothing stronger.

## The invariant everything rests on

`Router::plan()` returns **exactly one action per sink, every tick**, whatever
the routing, the connection state or the global mute say. Unrouted,
disconnected and muted all produce a `black` action, never an absent one.

A UVC device that stops delivering is dropped by Zoom and does not come back
without the host app restarting; an SDI output that stops has to be re-locked;
a Syphon server that stops disappears from every consumer's menu. Black is
recoverable, silence is not.

Every `black` carries a `reason` string, and it is surfaced verbatim. Do not
add a code path that produces black without one.

## Traps already known, before writing the code

Carried forward from the fleet, because these will be hit again here.

- **Pixel formats travel with frames; do not add a BGRA normalisation step.**
  It is the one architectural difference from oxbow and WebLinked and it exists
  so the 2110 and DeckLink paths keep their 10 bits. See the architecture doc.
- **The RTP marker bit ends a frame.** Get it wrong and every frame arrives one
  frame late — the picture is perfect and the latency is not.
- **RFC 4175 line numbers and pixel offsets are 1-based and in PIXELS**, while
  everything internal is 0-based and in bytes. Both conversions are in
  `st2110_rtp.cpp` and both are covered by round-trip tests at awkward widths.
- **Lost 2110 packets must tear a frame, never discard it.** A receiver that
  drops a whole frame for one missing packet produces a black flash, which is
  far more visible than a torn line.
- **v210 and the 2110-20 pgroup are different 10-bit packings** — 6 px/16 B
  little-endian versus 2 px/5 B big-endian. Confusing them gives correct
  geometry and wrong colour, which survives a long time before anyone notices.
- **Never `period * n` for a deadline.** Test pins this.
- **Cast to signed before multiplying a duration.** oxbow's card emitted valid
  black for 585 years because `uint64_t * nanoseconds` promotes the duration's
  representation to unsigned.
- **`IFM_TYPE` must be checked before `IFM_SUBTYPE`** on macOS — media subtypes
  are numbered per media type, so a Wi-Fi modulation and 10baseT can be the
  same integer. Already handled in `net/interfaces.cpp`; do not simplify it
  away.
- **libomt embeds .NET, which replaces SIGINT/SIGTERM handlers at runtime
  init.** Install signal handlers *after* creating the first OMT sender or
  receiver, or Ctrl-C is swallowed and the process lingers holding port 6400 —
  and the next sender then announces while the zombie owns the port, so
  receivers connect to a source that never sends.
- **NDI's `BGRX_BGRA_flipped` receive format is Windows-only.** Elsewhere
  normalise rows at ingest.
- **`Source::takeAudio()` is destructive.** Take it ONCE per source per tick in
  the poll loop and share it with every routed sink — calling it per sink gives
  the audio to whichever sink is served first and silence to the rest, which on
  a two-output show is a fault nobody thinks to look for. Pinned by
  `audioReachesEverySinkOnOneSource`.
- **Audio is not sent on a `black` action, and that is deliberate.** Video going
  quiet is a fault downstream equipment must recover from, which is why black
  frames are still emitted; audio going quiet *is* silence and needs no filler.
- **Screen Recording permission failing is INVISIBLE.** Without it SCStream
  starts successfully and delivers black frames forever — no error, no
  callback, no log line. `ScreenCapture::permitted()` checks up front by asking
  for shareable content, and the node fails to build with the permission named.
  Never remove that check on the grounds that the capture "starts fine".
- **Call `initialiseWindowServer()` (NSApplicationLoad) before CoreGraphics.**
  A plain C++ CLI has no Cocoa init, and *window* capture asserts with
  `CGS_REQUIRE_INIT` while *display* capture works — only the window path
  reaches that code, so this looks like a window-capture bug and is not.
- **ScreenCaptureKit sends idle samples with no image** when the screen has not
  changed. Check `SCStreamFrameInfoStatus` for `SCFrameStatusComplete`, or a
  static desktop reads as a live source delivering at full rate.
- **A Syphon server must be created on the main thread**, and **every
  main-thread wait must go through `waitServicingMainLoop`** (app/main_loop.h).
  On a private run loop the server is well-formed, announces itself, and is
  invisible to every consumer — and a plain `sleep_for` on the main thread
  deadlocks the frame thread's `dispatch_sync` on its first Syphon frame.
  `cmdRun` waits correctly; do not "simplify" it back to sleep_for.
- **Two DeckLink SDKs exist on this Mac** — 10.11 inside the NDI SDK's
  examples, 12.2 inside Unreal's BlackmagicMedia — and first-hit order picks
  the wrong one. Below 11.0 there is no `IDeckLinkProfileAttributes`.
- **A Duo 2's sub-device pairs each have their own profile manager**, and a
  pair in `1dfd` leaves its second sub-device refusing both input and output
  while still listing a full set of display modes. Reads as broken hardware.
- **Never link NDI at build time.** Runtime-load it. A cross-compiled target
  otherwise ships with NDI silently disabled, which is what happened to
  openstage for every release until someone checked.
- **NDI's audio FourCC is `FLTp`, with a LOWERCASE p**, despite every document
  writing "FLTP". Assume the obvious spelling and the SDK treats every audio
  frame as an unknown format: video works perfectly and audio silently never
  arrives. Pinned in `tests/test_ndi_abi.cpp`.
- **Mirror every SDK constant from a probe, never from memory or from reading.**
  `tools/{ndi,omt,srt}_abi.c` compile against the real headers and print sizes,
  offsets and enum values. All three have already caught something: NDI's audio
  struct is 64 bytes not 56, NDI's audio FourCC is `FLTp` not `FLTP`, and
  libsrt's SRTO_SNDTIMEO/SRTO_RCVTIMEO are 13/14 not the 38/37 written from
  memory — while SRTO_STREAMID has no explicit value in the header at all, so
  no amount of reading finds it. A wrong option number does not fail; it sets a
  different option.
- **`convert()` normalises quantisation RANGE, and its `outRange` must be
  applied to the frame.** An RGB source becomes narrow YCbCr, because every
  transport here carries narrow by convention; an RGB destination is full. Drop
  the relabelling and the receiver expands the range a second time — green came
  back at 240 instead of 191 over SRT, with hues and bar order perfect.
- **rawvideo has no colorimetry and ffmpeg guesses BT.601 below 720 lines.**
  Always tag `-colorspace`/`-color_primaries`/`-color_trc`/`-color_range` on
  BOTH the input and the output. Untagged 709 data at 640x360 encodes as 601:
  white and black stay exact while every saturated colour skews.
- **Conversion is on the critical path and must stay integer.** It was `double`
  with `std::lround` per component and cost 29.7 ms for a 1280x720 UYVY->BGRA
  frame — a 33.6 fps ceiling that showed up as an OMT receiver at 19 fps. Now
  fixed-point, 9.3 ms. Do not "simplify" it back to floating point.
- **Do not allocate per frame.** `PreviewSink` allocated 3.7 MB twice per frame
  and that cost more than the conversion did. Both the engine and the preview
  hold reusable scratch buffers; keep it that way.
- **NDI has no interface binding of any kind.** Not on send, receive or
  discovery — `grep -i interface` across the whole SDK header set returns
  nothing. `"interface"` on an NDI node therefore produces a *warning*, not a
  silent no-op; binding is done through the NDI runtime's own
  `ndi-config.v1.json`. `warnings` exists in the factory precisely for this.
- **Never gate `poll()` on `connected()` in the engine.** A network receiver
  only becomes connected as a *result* of being polled, so gating deadlocks it:
  never polls, never connects, never polls. Invisible to synthetic sources,
  which report connected from construction. Pinned by
  `aSourceThatConnectsOnlyWhenPolledStillWorks`.
- **A transport node's direction comes from the routes, not its kind.** `ndi`
  can be either, so the control API asks the engine which it was actually built
  as; inferring from the kind renders an NDI receiver as a sink too.

## Where the reusable code already is

| Need | Best existing implementation |
|---|---|
| NDI send + receive + discovery | `srt-router/crates/ndi-io/src/sys.rs` — the fullest in the fleet |
| NDI / OMT C++ runtime loading | `oxbow/src/io/ndi.cpp`, `omt.cpp` |
| Syphon server | `oxbow/src/io/syphon.mm` + its vendored subset |
| Spout server | `oxbow/src/io/spout.cpp` |
| DeckLink output, verified on a Duo 2 | `oxbow/src/io/decklink.cpp` |
| DeckLink capture | `weblinked/tools/dl_scan.mm` |
| HTTP control server + embedded page | `oxbow/src/control/` (already lifted) |
| SRT | `srt-router` |
| Tray launcher | `av-launcher`, driven by one `launcher.toml` |

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j8
cd build && ctest --output-on-failure
```

Anything decision-shaped goes in `ferret_core`, which has no I/O and no SDK
dependency, because that is the part a cheap test can reach. WebLinked shipped
an OSC bug that dropped a quarter of all messages precisely because the decoder
sat in a target no test could link against.

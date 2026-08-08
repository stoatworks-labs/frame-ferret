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

**SRT receive is written but has NEVER decoded a picture end to end.** The
socket layer is verified by loopback; the node connects, spawns ffmpeg and
reports cleanly; the decode is unproven. The reason is the test rig, not
confidence in the code: Homebrew's ffmpeg 8.1.2 has **no SRT protocol**,
`srt-file-transmit` uses SRT's *stream* API and our live-mode receiver
correctly rejects it ("MessageAPI/StreamAPI collision"), and
`srt-live-transmit` only bridges UDP↔SRT so its UDP leg could not be checked
independently. **Do not claim SRT works.** The next person needs a real SRT
encoder — OBS, vMix, a hardware unit, or an ffmpeg built with `--enable-libsrt`.

**SRT sending is not written at all** — it needs an encoder and a TS muxer.
A node not used as some sink's source therefore reports that rather than
constructing something inert.

Note SRT **can** bind to a chosen interface — `srt_bind()` takes a real address
and there is SRTO_BINDTODEVICE — which makes it the first transport here where
the `interface` setting does something. NDI and OMT can only warn.

**DeckLink output is written but has NEVER been run against a card.** The
scheduled-playback sequence is lifted faithfully from oxbow's
`src/io/decklink.cpp`, which *has* been run on a real Duo 2 — but no DeckLink
was attached to this machine while this port was written, so treat it as
unverified. What is confirmed: it compiles against SDK 12.2, the build without
the SDK reports itself unavailable, and with the SDK the dispatch layer
enumerates (drivers found, zero devices). Optional at build time via
`-DDECKLINK_SDK_DIR`; the version guard rejects anything below 11.0 and is
proven to reject the 10.11 copy inside the NDI SDK's examples.

- **Syphon output (macOS), verified against Resolume's Syphon 5** — a
  different implementation from the vendored Syphon 6 sources — from a process
  started *after* Frame Ferret, which is the exact case the main-thread trap
  breaks. Frame received, colours and channel order correct.

**Not written at all.** ST 2110, every capture source, DeckLink *capture*,
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

# Architecture

## The claim

Frame Ferret is a **crosspoint router whose ports happen to be protocols and
operating system objects.** NDI, OMT, SRT and ST 2110 are each both a source
and a sink; screen capture and Syphon/Spout clients are sources; a UVC camera,
a Syphon/Spout server, a DeckLink card and an HTML page are sinks.

Everything else follows from taking that seriously. `NodeKind` in
`src/app/node.h` is the whole port list, `canSource()` and `canSink()` say
which direction each supports, and the `Router` in `src/app/router.h` is the
only place that decides anything.

## The invariant

**Every sink is planned every tick, whatever the routing, the connection state
or the global mute say.**

`Router::plan()` returns exactly one `RouteAction` per sink — always. A sink
with no route, a sink whose source has no signal, and a sink that is muted all
get an action that says `black`, never an absent entry.

This is not tidiness. It is the difference between an output that recovers and
one that does not:

- A **UVC camera** that stops delivering frames gets dropped by Zoom and Teams
  within seconds, and does not come back until the host application restarts.
- An **SDI output** that stops has to be re-locked by whatever is downstream.
- A **Syphon or Spout server** that stops publishing disappears from every
  consumer's source menu, and the consumer does not re-scan.

Black is recoverable. Silence is not. Kestrel holds the same invariant for the
same reason and it is worth keeping the two codebases aligned on it.

The corollary: every `black` action carries a `reason` string, surfaced
verbatim through the control API. An operator looking at a black output is told
which of the four reasons it is rather than guessing.

## Formats travel with frames

oxbow and WebLinked normalise everything to BGRA at ingest. That is right for
them — both feed a GPU effect chain that wants 8-bit RGBA anyway.

Frame Ferret does not, and this is the main architectural difference from its
predecessors. Its two highest-value paths, ST 2110-20 and DeckLink, are both
natively 10-bit YCbCr 4:2:2. Routing them through a BGRA waypoint would
quantise to 8 bits *and* pay two chroma resamples for what should be a copy.

So `PixelFormat` travels on the frame, sinks declare what they accept
best-first, and `Router::plan()` marks each route `copy` or `convert`. `2110 →
DeckLink` is a repack. `2110 → Syphon` is a real conversion and is charged as
one. A sink with an empty accepts list converts internally and everything
counts as a copy to it.

`ColourSpace` and `QuantRange` travel separately because neither is implied by
the pixel format, and conflating narrow and full range is a visible black-level
shift that gets misdiagnosed as a monitor calibration problem.

## Layout

```
src/
  core/      frame, pixel formats, exact rational rates, JSON, dylib loading
  net/       interface enumeration and binding
  app/       node model, router, CLI
  sources/   capture: display, window, application, ROI, Syphon/Spout client
  transports/ ndi, omt, srt, st2110 — each implements both Source and Sink
  sinks/     uvc, Syphon/Spout server, decklink, html
  control/   HTTP server, web UI, OSC
  diag/      vendored fleet diagnostics
extensions/  the OS extensions — see docs/03-os-extensions.md
```

`ferret_core` is a separate static library holding everything with no I/O, no
GPU and no SDK dependency, because that is the part a cheap unit test can
reach. WebLinked learned this the hard way: its OSC decoder sat inside the
CEF-linked target where no test could get at it, and shipped a bug that
silently dropped a quarter of all messages. Anything decision-shaped belongs in
`ferret_core`.

## Decisions taken, worth not re-litigating

- **Exact rationals everywhere.** 59.94 is 60000/1001. `parseRate` rejects
  unrecognised decimals rather than approximating them, so a typo fails at
  config load instead of running the whole show a fraction of a percent off.
- **Deadlines computed from the rational at each tick**, never `period * n`.
  See `tickDeadlineNs` and the drift test.
- **Signed tick arithmetic.** oxbow shipped a bug where an unsigned frame
  counter multiplied a `std::chrono` duration, promoting its representation to
  unsigned, so a deadline already in the past wrapped to 585 years and the
  process sent exactly one frame while the card emitted valid black. There is a
  test pinning this.
- **An unmatched interface selector is an error**, never a fallback to the
  default route. A sender that quietly leaves on the wrong NIC is diagnosed on
  site as a network fault and costs an afternoon.
- **`""` as an interface selector means `INADDR_ANY`**, which is a real and
  distinct choice, not "the first interface we enumerated".
- **SDKs are loaded at run time, never linked.** NDI's licence forbids bundling
  under MIT and libomt has no Linux binary. This also avoids the trap that bit
  openstage, where a cross-compiled target silently shipped with NDI disabled
  because `find_package` quietly failed.
- **No GUI toolkit.** The operator UI is the same page the HTTP server serves,
  exactly as WebLinked does it, with the tray launcher from av-launcher on top.

## Where this reuses the fleet

| From | What |
|---|---|
| oxbow | `Dylib`, JSON, HTTP server, diag; the NDI/OMT/Syphon/Spout/DeckLink I/O patterns |
| WebLinked | The control-page-over-HTTP model, the DeckLink output sequence, OSC |
| srt-router | The crosspoint mental model; `ndi-io/src/sys.rs` is the fullest NDI binding in the fleet |
| Kestrel | The plan-every-output invariant; DeckLink SDK version selection |
| av-launcher | The tray launcher shell, driven by one `launcher.toml` |

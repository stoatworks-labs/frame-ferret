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

**Built, tested and run on this machine** — 416 checks across 8 binaries:

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

**Not written at all.** Every transport (NDI, OMT, SRT, ST 2110), every capture
source, every hardware output, the OS extensions, and the tray launcher. There
is no *real* video path — only the synthetic one. Do not describe any of it as
working.

**Never built or run:** Windows, Linux. The code has platform branches and CI
compiles all three from this commit onward, but only macOS has ever run.

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
- **A Syphon server must be created on the main thread.** On a private run loop
  it is perfectly well-formed, announces itself, and is invisible to every
  consumer. oxbow's `src/app/main_loop.h` is the pattern.
- **Two DeckLink SDKs exist on this Mac** — 10.11 inside the NDI SDK's
  examples, 12.2 inside Unreal's BlackmagicMedia — and first-hit order picks
  the wrong one. Below 11.0 there is no `IDeckLinkProfileAttributes`.
- **A Duo 2's sub-device pairs each have their own profile manager**, and a
  pair in `1dfd` leaves its second sub-device refusing both input and output
  while still listing a full set of display modes. Reads as broken hardware.
- **Never link NDI at build time.** Runtime-load it. A cross-compiled target
  otherwise ships with NDI silently disabled, which is what happened to
  openstage for every release until someone checked.

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

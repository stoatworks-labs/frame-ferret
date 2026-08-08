# Roadmap

Ordered by what unblocks the most, not by what is most interesting. Nothing
past phase 0 exists.

## Phase 0 — the spine ✅ done

Node model, crosspoint router, exact rational timing, pixel format model,
interface enumeration, CLI.

## Phase 1 — a running application ✅ done

A synthetic route proven through the whole program.

- Pixel conversion between BGRA/RGBA/UYVY/YUY2/v210, verified by pixel readback
- The frame loop: tick from the rational, poll sources, execute the plan
- Config loading, and the factory that builds nodes from it
- A test-pattern source and a preview sink
- The HTTP control API and the crosspoint web page
- `run --config <file>` and a `selftest` that fails on a frozen or black output

416 checks. Measured: 9049 ticks at 49.99 fps, zero late, one action per sink
per tick. Still **no real video** — the only source is synthetic.

## Phase 1b — the first real stream ✅ done

**NDI in and out**, runtime-loaded, verified against oxbow in both directions.
456 checks.

Two things this phase settled, both of which were assumptions in the plan:

- **NDI cannot be bound to an interface.** Its C API has no such parameter, on
  send, receive or discovery. The roadmap said "bound to a chosen interface";
  that is not achievable through the SDK, so the setting now produces a warning
  and binding is the NDI runtime's own `ndi-config.v1.json`. Every other
  transport still gets real interface binding.
- **Audio is plumbed but unproven.** `AudioFrame` crosses the receiver and the
  sender, but nothing has yet carried audio end to end, and the router does not
  route it separately from video. Do not claim NDI audio works.

Still to do here: a ten-minute soak, and audio actually verified.

## Phase 2 — the rest of the transports

- **OMT ✅ done.** Both directions, verified against oxbow and macOS `dns-sd`.
- **SRT — transport layer ✅ done, codec layer not started.** `srt_socket.*`
  does caller/listener, real interface binding, latency, passphrase and stream
  id, and is proven over loopback. What is missing is that **SRT carries
  compressed MPEG-TS, not frames**: a source must demux and decode, a sink must
  encode and mux. Until that exists there is deliberately no SRT node.

  Sequenced the same way as 2110, and for the same reason — receiving is far
  more forgiving than sending:
  1. TS demux + H.264 decode, so SRT can be a *source*. "Receive an SRT
     contribution feed and present it as a UVC camera" is the useful case.
  2. H.264 encode + TS mux, so SRT can be a *sink*. Needs rate control,
     keyframe interval and PCR handling to be right.

  **Resolved: ffmpeg runs as a subprocess, not as a library.** The operator
  points at an install they already have (node `"ffmpeg"` setting,
  `$FERRET_FFMPEG`, `$PATH`, then the usual locations). That avoids the ABI
  fragility of mirroring AVCodecContext, keeps ffmpeg's GPL in a separate
  process, and leaves CI with nothing to detect. `core/subprocess.*` and
  `transports/ffmpeg.*` are built and verified; hardware encoders are preferred
  where present.

  **Still open: nothing has been decoded end to end.** No reference SRT sender
  could be built on this machine — Homebrew's ffmpeg has no SRT, and neither
  srt-file-transmit (stream API) nor srt-live-transmit (UDP bridge only) works
  as one. This needs a real encoder: OBS, vMix, a hardware unit, or an ffmpeg
  built `--enable-libsrt`.

- **DeckLink out ✅ written**, optional behind `-DDECKLINK_SDK_DIR`. Lifted from
  oxbow's Duo 2-verified sequence, prefers v210. **Never run against a card
  from this codebase** — none was attached.
- **Syphon out ✅ done and verified** against Resolume's Syphon 5, from a
  process started afterwards.
- DeckLink capture; Spout out (Windows)

## Phase 3 — capture sources

- Display and region-of-interest capture via ScreenCaptureKit
- Window and application capture — **do these before the virtual display**, as
  they may absorb most of what it is wanted for
- Syphon / Spout client input

## Phase 4 — control

- HTTP control server and the embedded crosspoint page, WebLinked's model
- OSC, with the padding bug from WebLinked already covered by a test
- The av-launcher tray shell and `launcher.toml`

## Phase 5 — ST 2110

Sequenced by what the platform can actually support — see
[02-st2110.md](02-st2110.md).

1. 2110-30 audio and 2110-40 ancillary, macOS. Low bitrate, tolerant of a
   software-timestamped clock, and useful on their own.
2. 2110-20 **receive**. Far more forgiving than sending: no pacing obligation
   and the media clock arrives on the wire. A Mac that receives 2110-20 and
   presents it as a UVC camera is probably the most useful single thing in this
   application.
3. 2110-20 **send**, wide profile, labelled as such in the UI and the SDP.
4. SDP generation and parsing; NMOS IS-04 / IS-05 if a real plant needs it.

## Phase 6 — the OS extensions

Full detail in [03-os-extensions.md](03-os-extensions.md). There is **no Apple
application to make** — the System Extension capability is a checkbox on an App
ID. The prerequisite is instead a provisioning profile, which the fleet's
release harness has never produced.

1. Windows virtual camera (MFVirtualCamera) — cheapest of the four
2. Provisioning-profile support in `release-lib.sh` — fleet infrastructure, and
   the gate on everything below
3. macOS virtual camera (CMIOExtension)
4. Windows virtual display (IddCx), if attestation signing is worth opening
5. macOS virtual display — private API, off by default, experimental, and only
   if phase 3 leaves a real gap

## Unresolved, and needing a decision before the installer is designed

The macOS system extension must be activated from an app in `/Applications`.
The fleet's usual trick — unpacking into Application Support on first run, which
is how WebLinked dodges the Gatekeeper nested-helper kill — is incompatible with
that. These two pull in opposite directions and one of them has to give.

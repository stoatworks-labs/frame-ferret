# Frame Ferret

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. What exists today is the
> node model, the crosspoint router, exact-rational frame timing and network
> interface enumeration — 4 test binaries, 190 checks, all passing, and a CLI
> that lists this machine's real NICs. **No video has passed through it yet.**
> Every transport, every capture source and every output listed below is
> designed and documented but not implemented. See [Status](#status).

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
  diagnosed on site as a network fault
- A system tray launcher, configured through a web browser, in the same shape
  as [WebLinked](https://github.com/stoatworks-labs/weblinked)

## Status

Honest, and it will stay honest as this grows.

| Area | State |
|---|---|
| Node model, crosspoint router | **Built and tested.** 36 checks |
| Exact rational rates and tick deadlines | **Built and tested.** 40 checks |
| Pixel format model | **Built and tested.** 33 checks |
| Interface enumeration and binding | **Built and runs.** 81 checks, and verified against this machine's real NICs. The count varies by machine — the suite walks whatever interfaces it finds |
| CLI (`interfaces`, `kinds`, `version`) | **Runs** |
| NDI, OMT, SRT, ST 2110 | **Not started.** Designed only |
| Screen / window / application capture | **Not started** |
| UVC virtual camera | **Not started.** Needs an Apple entitlement — see [docs/03-os-extensions.md](docs/03-os-extensions.md) |
| Virtual display | **Not started.** No public macOS API exists — see the same document |
| DeckLink, Syphon/Spout, HTML output | **Not started** |
| Web control UI, tray launcher | **Not started** |
| Windows, Linux | **Never built** |

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

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8
```

```bash
cd build && ctest --output-on-failure
```

```bash
./build/frame-ferret interfaces
```

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

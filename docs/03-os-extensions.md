# The OS extensions — virtual camera and virtual display

Three of Frame Ferret's endpoints are not application code. They are operating
system extensions, they need signing paperwork that takes calendar time rather
than engineering time, and two of them constrain how the whole product is
distributed. This document is the map, written before the work starts.

The virtual display is built and experimental on macOS (2026-11-23) and on
Windows (2026-11-24, on a third-party signed driver); see their sections. The
two virtual cameras are **not started**.

## The four items and their real difficulty

| Item | Platform | Mechanism | Difficulty | Blocker |
|---|---|---|---|---|
| Virtual camera | macOS | CMIOExtension | Moderate | Provisioning profile in the release harness |
| Virtual camera | Windows | MFVirtualCamera | **Low** | None |
| Virtual display | Windows | VirtualDrivers' signed IddCx driver (VDD) | **Built, experimental** | Our own driver would need attestation signing; VDD's is signed |
| Virtual display | macOS | Private `CGVirtualDisplay` | **Built, experimental** | *No public API exists* |

Do the Windows virtual camera first. It is by far the cheapest of the four and
it validates the whole sink-side frame path before any signing paperwork is in
flight.

## macOS virtual camera — CMIOExtension

Since macOS 12.3 the supported mechanism is a **Camera Extension**: a
`CMIOExtensionProvider` bundled inside the host app, activated through
`OSSystemExtensionManager`, running as its own process outside the app.

The older **DAL plugin** route (a bundle in
`/Library/CoreMediaIO/Plug-Ins/DAL`) is what OBS's virtual camera historically
used. It is deprecated, requires disabling library validation in every client
app that loads it, and Apple has been steadily narrowing it. Do not build on
it.

### There is no application to Apple, and that was got wrong once

An earlier draft of this document said the
`com.apple.developer.system-extension.install` entitlement had to be requested
from Apple and to allow weeks for it. **That is wrong.** "System Extension"
appears in Apple's own [supported capabilities for
macOS](https://developer.apple.com/help/account/reference/supported-capabilities-macos)
as an ordinary capability — a checkbox on an App ID in Certificates,
Identifiers & Profiles. The capabilities that genuinely need a request are
DriverKit and Endpoint Security, neither of which we touch.

The one restricted entitlement in this area is
`com.apple.developer.system-extension.redistributable`, which lets a *different*
team's application install your extension. We do not need it.

### What it does require

The fleet already signs and notarises everything, and **that is necessary but
not sufficient.** The genuinely new thing is a **provisioning profile**, which
this fleet's release harness has never produced.

A Developer ID app normally needs no provisioning profile — which is exactly why
this has never come up before. Adding an extension with a capability makes one
mandatory, because some entitlements are carried in the code signature and
others are carried by the profile. macOS looks for it at
`YourApp.app/Contents/embedded.provisionprofile`.

So, against what the fleet has today:

| Already have | Must add |
|---|---|
| Developer ID Application cert (`3G7USP8N73`) | Explicit App IDs — not wildcard — for **both** the container app and the extension |
| Hardened runtime, `--timestamp`, inside-out signing, no `--deep` | The System Extension capability ticked on those App IDs |
| Notarisation and stapling | A Developer ID provisioning profile per App ID, embedded in each bundle |
| | The extension's team ID matching the container's |
| | The app installed in `/Applications` — activation from a build directory silently fails |
| | User approval in System Settings → Privacy & Security, once per install |

**This is a real change to `release-lib.sh`, not a config tweak.** `rl_mac_sign`
walks for Mach-O files and signs inside-out; it has no concept of a provisioning
profile. The profile has to be fetched and embedded *before* signing.

Other consequences that reach the rest of the product:

- The extension is a **separate process**. Frames cross to it via IOSurface, not
  by a function call. Budget for that boundary in the frame path design.
- The tray launcher cannot be the only shipping artefact. The extension has to
  be inside a properly located, properly signed `.app`.
- Ad-hoc signed builds cannot test this path at all. Every iteration needs a
  real Developer ID signature, so CI — which has no signing config and falls
  back to ad-hoc — cannot verify it.
- The fleet's "unpack into Application Support on first run" trick, which
  WebLinked uses to dodge the Gatekeeper nested-helper problem, is
  **incompatible** with system extension activation, which requires
  `/Applications`. These pull in opposite directions and the conflict needs
  resolving before the installer is designed.

## Windows virtual camera — MFVirtualCamera

Windows 11 added `MFCreateVirtualCamera`, and it is dramatically simpler than
everything else in this document: no driver, no kernel code, no attestation
signing. The app registers a Media Foundation source and the camera appears
system-wide.

Windows 10 has no equivalent. The fallback is a **DirectShow filter** — a
registered COM DLL, which works with OBS and Zoom but is a dead-end API that
modern UWP camera clients ignore.

Recommendation: **MFVirtualCamera only, Windows 11 minimum.** Adding a
DirectShow path doubles the surface area for an OS that is out of mainstream
support.

Caveat worth checking early: the Windows 11 requirement is not merely "Windows
11" but a recent enough build, and the fleet's only Windows machine is an
**ARM64 Parallels VM** running x64 under emulation. Whether MFVirtualCamera
behaves under that emulation is unknown and should be established before
committing to the design.

## Windows virtual display — IddCx

An **Indirect Display Driver** (IddCx, a UMDF driver) presents a monitor to
Windows that is backed by software. This is what every "virtual monitor" and
DisplayLink-class product uses. The API is documented and reasonably tractable.

The blocker is distribution. A driver package needs:

- test-signing enabled on the dev machine for development, and
- **attestation signing** through the Microsoft Partner Center for
  distribution, which needs an EV code-signing certificate and a Partner Center
  account.

That is a real cost and a real lead time. It is not a technical risk so much as
an administrative one, and it should be started early or dropped early.

### Built (2026-11-24): someone else's signed driver

There is no EV certificate or Partner Center account, so instead of an
unsigned driver nobody could load, the Windows `virtual-display` node drives
**VirtualDrivers' Virtual Display Driver** (VDD: an IddCx driver, MIT, signed
through the SignPath Foundation), which the operator installs once. The node
(`src/sources/virtual_display_win.cpp`) claims a VDD display, gives it the
planned mode and captures it with DXGI Desktop Duplication
(`src/sources/screen_capture_win.cpp`, which is also Windows display capture).

How VDD is driven, all measured on winlab with VDD 25.7.23:

- **Finding its displays.** VDD's EDID says maker `MTT`, product `0x1337`, so
  its monitor device path contains `#MTT1337#`. The GDI name (`DISPLAY3`) is
  not stable: every VDD reload renumbers it. The connector's `UIDnnn` in the
  device path survives a reload, so a node holds its display by that and asks
  for the GDI name again whenever the duplication is lost.
- **Modes come from its settings file**, `vdd_settings.xml`, in the folder
  `HKLM\SOFTWARE\MikeTheTech\VirtualDisplayDriver\VDDPATH` names (else
  `C:\VirtualDisplayDriver`). A mode is each `<resolution>` at its own
  `<refresh_rate>`, plus every `<g_refresh_rate>` for every listed resolution.
  A missing mode is added as a `<resolution>` block marked "added by Frame
  Ferret", the file swapped in whole, and VDD told to reload.
- **Its control pipe** is `\\.\pipe\MTTVirtualDisplayPipe`: UTF-16 commands
  in message mode, open to every user. `RELOAD_DRIVER` rereads the settings
  and takes every VDD display away for about 1.2 s.
- **The mode is set with SetDisplayConfig**, the source mode's raster and the
  path's refresh rational, target mode left for Windows to match. With
  per-monitor DPI awareness V2 set at startup, so a scaled monitor's numbers
  are real pixels.
- **No fractional rates.** With `59.9401` in the settings, Windows lists no
  59 Hz mode on the display and `ChangeDisplaySettingsEx` at 59 answers
  `BADMODE`. VDD's `CreateTargetMode` works the pixel rate out as
  `VSyncNum * Width * Height / VSyncDen` in 32-bit `UINT`s, which overflows for
  any four-place rate (599401 x 1920 x 1080); that is the likely reason. So the
  plan folds a fractional rate to the whole rate above it, as on macOS, and
  warns.
- **Session 0 has no displays.** From ssh or a service every display API
  answers empty. The node says so up front; the bench runs in the signed-in
  session through a scheduled task (`tools/winlab/s1run.ps1`).

The capture side:

- **Read back before ReleaseFrame.** The copy to the staging texture is only
  queued until Map. Mapping after the frame went back to DWM, the node took 16
  new pictures a second where raw duplication took 30; mapping first (as
  Microsoft's samples do), with a 1 ms timer, it matched raw.
- **`timeBeginPeriod(1)` at startup.** Without it Windows sleeps in 15.6 ms
  steps, so the engine's `sleep_until` woke on those steps rather than on its
  deadlines (a 2 ms poll loop ran 62 times a second).
- **The desktop's alpha is forced to 255**, HDR (non-BGRA8) and rotated
  displays are refused with a sentence, and the pointer is not drawn.

**Run, 2026-11-24, winlab** (Windows 11 Pro 26200 in QEMU, no GPU: Microsoft
Basic Render Driver, two cores). `tools/vdbench` puts a counter test card (the
omttest layout) full screen on VDD's display from a separate process and judges
what comes back:

| Route, 1920x1080p60 | Delivered | New pictures | Out of order | Bars |
| --- | --- | --- | --- | --- |
| Node in-process (`vdbench direct`) | latest frame | 28.9/s | 0 | exact |
| Node → NDI sender → `vdbench probe` in another process | 60.1 fps | 26.9/s | 0 | within 1 level |
| Raw Desktop Duplication, no Frame Ferret (`vdbench dup copy`) | — | 29.6/s | — | — |

Over two NDI runs the engine held 59.89 and 59.92 fps, with 8% and 2% of its
ticks late, and the receiver got 26.6 and 26.9 new pictures a second. The ceiling is the VM: DWM
composed VDD's display about 42 times a second whatever the card drew, and the
copy to memory competes with DWM rendering in software on two cores. 1280x720
at 50 gave 26.7/s; 1600x900 at 30, a mode VDD did not list, was added, VDD
reloaded, and the capture (19.8/s) was upright with exact bars.

Two bench traps, both fixed in vdbench: a card drawn as 16 separate GDI cells
tears (DWM composes halfway through, and the counter reads backwards), so the
counter strip goes up in one `SetDIBitsToDevice`; and an all-black desktop
decodes as counter 0, so the bench waits for the bars as well as a counter.

**A VDD reload during capture** (sent over the pipe by someone else): twice
the node logged the display gone, then "capturing \\.\DISPLAYn again" and
carried on. Once, VDD's device itself went into an error state and stayed
there (raw duplication and the card alone survived a reload each); restarting
the device (`pnputil /restart-device`, or Disable/Enable in Device Manager)
brought the display back and the node picked it up. The node now says "VDD has
stopped" with that remedy when VDD's pipe is gone.

Not done: a machine with a GPU, Resolume on Windows, OMT on Windows, the mode
re-applied if Windows restores a different one after a reload, and window and
application capture (they need Windows.Graphics.Capture).

## macOS virtual display — the one with no supported answer

**There is no public macOS API for creating a virtual display.**

The private `CGVirtualDisplay` / `CGVirtualDisplayDescriptor` classes in
CoreGraphics are what Sidecar uses internally, and what every third-party tool
in this space (BetterDisplay, Duet, and others) relies on. Using them means:

- private API, which can change or disappear in any macOS release,
- no App Store distribution, which does not matter for this fleet, and
- an application that may break on a point release with no warning and no
  supported migration.

The alternatives are all worse: there is no DriverKit display driver class, and
a DisplayLink-style USB device is hardware, not software.

**Recommendation:** treat the macOS virtual display as explicitly experimental,
implement it behind a flag that is off by default, and document that it uses
private API. Do not let it become load-bearing for any other feature.

There is a partial substitute worth considering first. Most of what people want
a virtual display for — "give me a surface to drag an application onto and then
capture it" — is served on macOS by **ScreenCaptureKit** capturing a specific
window or application directly, with no virtual display involved. That is
`NodeKind::windowCapture` and `applicationCapture`, both already in the node
model, both public API, and both far cheaper. Both are built now (cd948fd), so
the next step is seeing how much of the virtual display's purpose is left over.

### Built (2026-11-23)

What was left over turned out to be real: **applications that output to a
display, not a window.** Resolume Arena's Advanced Output is the case that drove
it. Its output devices (display, NDI, Syphon, virtual, capture) are compiled
into the app, so no plugin can add OMT to the list, and a full-screen output
window on a physical display is not something window capture can stand in for.

`virtual-display` (`src/sources/virtual_display.{h,mm}`) creates one display per
node and captures it with the existing ScreenCaptureKit path, by display ID.

- **Held at arm's length.** The four private classes are found with
  `NSClassFromString` and driven through protocols, never named in code, so the
  binary does not import `_OBJC_CLASS_$_CGVirtualDisplay`. A macOS that drops
  them fails this node with a sentence; it cannot stop Frame Ferret launching.
  `nm -u frame-ferret` shows no CGVirtualDisplay symbol.
- **Off by default** in the sense the recommendation meant: it exists only when
  a config asks for a `virtual-display` node.
- **Decisions are testable.** `planVirtualDisplay()` (name, raster, whole-hertz
  rate, stable serial, ~100 ppi physical size) is in `ferret_core`, pinned by
  `test_virtual_display`.
- **Fractional rates warn.** macOS folds 59.94 to 60 (measured in the
  decklink-display spike), so a 59.94 node runs a 60 Hz display and puts the
  expected drop rate (one frame every ~17 s) in the factory's warnings.
- **Capture asks for refresh + 1 Hz**, because ScreenCaptureKit drops a frame
  that lands a hair early against an exact minimum interval.

**The trap that cost the first run:** the display was in the active list within
60 ms but `CGDisplayCopyDisplayMode` stayed NULL indefinitely. Cause:
`initialiseWindowServer()` calls `NSApplicationLoad()`, after which display
reconfiguration reaches CoreGraphics as AppKit events, and
`waitServicingMainLoop` only ran the CFRunLoop, which never dequeues an event.
A probe across every combination: NSApplicationLoad + run loop never got a
mode; NSApplicationLoad + an NSApp event pump, or no AppKit at all, got one in
40-80 ms. The macOS wait (`src/app/main_loop_mac.mm`) now pumps events, which
still services the main dispatch queue the Syphon sink depends on.

**Run, 2026-11-23, macOS 26.4, Resolume Arena 7.29:** Arena listed the new
display live as "Display 2 (1920x1080)" (it numbers displays; it never shows the
macOS name). An Advanced Output preset with one screen on that device, fed by
an OMT test card, routed by Frame Ferret to an OMT sender and read back by an
independent receiver that decodes the card's frame counter: **601 frames in
10 s, 60.0 fps, counter +600, nothing out of order, upright, bars within 3
levels.** The same through Arena's simple full-screen output.

Not done: audio, and a run on a heavily loaded machine — the spike measured
capture losses under load whatever the capture API.

## Sequencing

1. Windows virtual camera (MFVirtualCamera) — cheapest, validates the sink path
2. macOS window/application capture via ScreenCaptureKit — public API, and may
   absorb much of the virtual display requirement. Done (cd948fd)
3. **Teach `release-lib.sh` to embed a provisioning profile.** This is the real
   macOS prerequisite, and it is fleet infrastructure work rather than Frame
   Ferret work — nothing else in the fleet needs it yet, but nothing else in
   the fleet can ship a system extension until it exists
4. macOS virtual camera (CMIOExtension)
5. Windows IddCx virtual display, if the Partner Center route is worth opening.
   **Built 2026-11-24 without that route**, on VirtualDrivers' signed VDD
6. macOS virtual display, private API, flagged experimental — last, and only if
   step 2 leaves a real gap. **Built early, 2026-11-23**: step 2 did leave one.
   Software that can only output to a display (Resolume's Advanced Output, whose
   device list is compiled in) cannot be reached by window capture

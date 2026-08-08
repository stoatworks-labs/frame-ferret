# The OS extensions — virtual camera and virtual display

Three of Frame Ferret's endpoints are not application code. They are operating
system extensions, they need signing paperwork that takes calendar time rather
than engineering time, and two of them constrain how the whole product is
distributed. This document is the map, written before the work starts.

Nothing here is implemented. Status of every item is **not started**.

## The four items and their real difficulty

| Item | Platform | Mechanism | Difficulty | Blocker |
|---|---|---|---|---|
| Virtual camera | macOS | CMIOExtension | Moderate | Provisioning profile in the release harness |
| Virtual camera | Windows | MFVirtualCamera | **Low** | None |
| Virtual display | Windows | IddCx driver | High | Attestation signing |
| Virtual display | macOS | — | **Highest** | *No public API exists* |

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
model, both public API, and both far cheaper. Build those first and see how
much of the virtual display's purpose is left over.

## Sequencing

1. Windows virtual camera (MFVirtualCamera) — cheapest, validates the sink path
2. macOS window/application capture via ScreenCaptureKit — public API, and may
   absorb much of the virtual display requirement
3. **Teach `release-lib.sh` to embed a provisioning profile.** This is the real
   macOS prerequisite, and it is fleet infrastructure work rather than Frame
   Ferret work — nothing else in the fleet needs it yet, but nothing else in
   the fleet can ship a system extension until it exists
4. macOS virtual camera (CMIOExtension)
5. Windows IddCx virtual display, if the Partner Center route is worth opening
6. macOS virtual display, private API, flagged experimental — last, and only if
   step 2 leaves a real gap

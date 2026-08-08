# The OS extensions — virtual camera and virtual display

Three of Frame Ferret's endpoints are not application code. They are operating
system extensions, they need signing paperwork that takes calendar time rather
than engineering time, and two of them constrain how the whole product is
distributed. This document is the map, written before the work starts.

Nothing here is implemented. Status of every item is **not started**.

## The four items and their real difficulty

| Item | Platform | Mechanism | Difficulty | Blocker |
|---|---|---|---|---|
| Virtual camera | macOS | CMIOExtension | Moderate | Entitlement request to Apple |
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

What CMIOExtension requires:

1. **`com.apple.developer.system-extension.install` entitlement.** This is
   requested from Apple, not enabled in Xcode. Allow weeks.
2. **A Developer ID certificate**, which this fleet already has — see the
   fleet's Apple code-signing notes. 14 repos are already signed.
3. **The app must live in `/Applications`** to activate a system extension.
   Running from a build directory silently fails to activate.
4. **User approval** in System Settings → Privacy & Security, once per install.
5. The extension is a **separate process**: frames cross to it via IOSurface,
   not by a function call. Budget for that boundary in the frame path design.

Consequences that reach the rest of the product:

- The tray launcher cannot be the only shipping artefact. The system extension
  has to be inside a properly located, properly signed `.app`.
- Ad-hoc signed builds cannot test this path at all. Every iteration needs a
  real Developer ID signature.
- The fleet's existing "unpack into Application Support on first run" trick,
  which WebLinked uses to dodge the Gatekeeper nested-helper problem, is
  **incompatible** with system extension activation, which requires
  `/Applications`. These two constraints pull in opposite directions and the
  conflict needs resolving before the installer is designed.

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
3. **Start the Apple entitlement request now**, in parallel with everything
   else, because it is calendar time rather than work
4. macOS virtual camera (CMIOExtension), once the entitlement lands
5. Windows IddCx virtual display, if the Partner Center route is worth opening
6. macOS virtual display, private API, flagged experimental — last, and only if
   step 2 leaves a real gap

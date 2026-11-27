# Frame Ferret user guide

Frame Ferret is a software virtual capture card: one application that sends and receives NDI, OMT,
SRT and ST 2110 on a network interface you choose, captures this machine's screen, and shows up to
other software as a camera, a shared GPU surface, an SDI output or a web overlay.

![Frame Ferret's control page: the crosspoint with colour bars routed to the NDI, ST 2110-20 and preview outputs and the main display to Syphon, the preview, and the frame counters](thumb.png)

*The control page at 50 fps: colour bars routed to the NDI, ST 2110-20 and preview outputs and the main display to Syphon (the ST 2110 row converting to 4:2:2 10-bit on the way), with the preview and the tick, frame and conversion counters underneath.*

The idea is that a protocol is just a port on a router. Anything can be a source, anything can be a
sink, and the crosspoint in the middle doesn't care which is which.

> **Before you rely on this:** a lot works and a lot isn't built yet, and the difference matters
> more here than in most projects. **NDI and OMT both work in both directions**, verified against
> separate implementations. **SRT send and receive are verified end to end** on macOS and have run
> on a Windows VM, and ST 2110-20 is
> verified against GStreamer's implementation in both directions. **DeckLink output has run on a
> real Duo 2**: 2531 frames with zero late and zero dropped by the card's own count, though the
> pixels on the wire haven't been checked.
>
> **Experimental:** the virtual display, on macOS (private API) and on Windows (on a signed
> driver you install); see below.
>
> **Not started:** the UVC virtual camera, DeckLink capture, Spout, HTML
> output, and ST 2110 audio and ancillary. Windows and Linux build and self-test in CI. On
> Windows, screen capture, the virtual display and SRT have run on a VM, and OMT has run both
> ways between that VM and a Mac. **Nothing else has run interactively on either, and no
> hardware path exists on them.**
>
> This codebase was created with AI assistance, directed and reviewed by a human author.

---

## The invariant

**Every sink produces a frame, every tick.** A sink with nothing routed to it goes black and
**keeps running** rather than stopping.

That's been seen happening, not just claimed: over one 9049-tick run, 4507 frames delivered plus
4542 black is exactly 9049, one action per sink per tick, with **zero late ticks and 49.99 fps
measured**. Unrouting a sink from the browser turned its output black and kept it running at
50 fps, reporting `no source routed`.

---

## Running it

```bash
frame-ferret run
```

serves the control page. The crosspoint is clickable: sources down one axis, sinks along the other.

```bash
frame-ferret interfaces     # what NICs this machine has
frame-ferret kinds          # what node types this build has
frame-ferret selftest
```

**Bind the interface explicitly.** A stream on the wrong NIC gets diagnosed on site as a network
fault, and that's an expensive hour. Every node takes an interface.

> **NDI is the exception, and it isn't one Frame Ferret can fix.** NDI's own C API has no interface
> parameter at all, so Frame Ferret **warns** rather than pretending the setting applied.

---

## What works today

| | |
|---|---|
| **NDI send and receive** | Verified against an independent implementation, both directions. |
| **OMT send and receive** | Verified against oxbow and macOS's own `dns-sd`. On Windows, run both ways against a separate OMT implementation on a Mac. |
| **SRT send and receive** | Verified end to end on macOS against an independent Rust SRT stack. Run on a Windows VM against ffmpeg's own libsrt: 40-second runs both ways, no frame out of order, bars within 1 level. On Windows since v0.5.0. On Linux it's built and unit-tested, never run. |
| **ST 2110-20 send and receive** | Verified against GStreamer's RFC 4175 implementation, within 2 code values. |
| **DeckLink output** | Run on a real Duo 2 at 1080p50 in v210. |
| **Screen, window, application and region capture** (macOS) | Built and run. A region of interest is a source rect, so a crop costs nothing. |
| **Screen and region capture** (Windows) | Built and run on a Windows VM, with Desktop Duplication. Window and application capture aren't written for Windows yet. |
| **Syphon output** (macOS) | Verified against Resolume's Syphon — a different implementation from the one vendored here. |
| **Virtual display** (macOS, experimental) | A display other software can output to, sent out over any sink. Run with Resolume Arena's Advanced Output into OMT. Private API — see below. |
| **Virtual display** (Windows, experimental) | The same, on VirtualDrivers' signed display driver, which you install once. Run on a Windows VM into NDI. New in v0.4.0 — see below. |
| **Desktop launcher** | Shipped since v0.1.1. Pick an interface and a port, press Start, and it opens the control page. Closing its window hides it to the tray (the menu bar on a Mac) and leaves the engine running. |

---

## One measurement worth understanding before you report a bug

Over an NDI round trip, **saturated blue comes back 178 rather than 191.**

That's NDI's own compression, not this code: an independent sender through the same round trip
lands on exactly the same 178. The control experiment mattered, because the number on its own would
have looked like a bug in the pixel conversion.

OMT does much better through the same test (all eight bars within 2 code values), because it
carried BGRA rather than compressing it. That difference comes from the protocols, and it's a good
reason to choose one over the other.

---

## SRT needs a codec, and that is why ffmpeg is a subprocess

NDI and OMT carry raw frames, so a node is a thin wrapper over the SDK. SRT carries an MPEG
transport stream of *compressed* video, so it needs a codec.

Frame Ferret runs **ffmpeg as a subprocess** rather than linking it:

- **You point at the install you already have:** the node's own setting, `$FERRET_FFMPEG`,
  `$PATH`, or the usual locations, in that order. An explicit path that's wrong is an error, never
  a silent fallback to some other ffmpeg.
- **No ABI coupling.** libavcodec's structs change between major versions, and mirroring them by
  hand would be far more fragile than NDI, OMT or libsrt, which all expose flat C ABIs on purpose.
- **Licensing stays simple.** A separate process over a pipe doesn't raise any question an MIT
  codebase needs to answer.

The usual locations are Homebrew's and MacPorts' on a Mac, `/usr/bin` and snap on Linux and, on
Windows, WinGet's `Links` folder, scoop's shims, Chocolatey's `bin`, `C:\Program Files\ffmpeg\bin`
and `C:\ffmpeg\bin`. Your ffmpeg doesn't need SRT support: Frame Ferret does the SRT itself, with
libsrt, and gives ffmpeg only the transport stream over a pipe. On Windows, `srt.dll` (and the
OpenSSL DLLs it needs) goes beside `frame-ferret.exe`, or `FERRET_SRT_RUNTIME` names it.

**Which H.264 encoder a sending node uses.** Hardware encoders come first, but only one that
works. ffmpeg lists `h264_nvenc` and `h264_qsv` whether or not there is a GPU behind them, so each
candidate (`h264_videotoolbox`, `h264_nvenc`, `h264_qsv`, `libx264`, `h264_v4l2m2m`, in that
order) first encodes one black frame to nowhere, and the first that succeeds is used. The log
names each one it passed over and why, for example:

```
srt: h264_nvenc is listed but would not open (Cannot load nvcuda.dll)
```

To pick one yourself, give the node an `"encoder"` setting, such as `"encoder": "libx264"`. A named
encoder is used as given: if it won't open, the node stops with ffmpeg's reason instead of quietly
using another.

---

## The virtual display (macOS, experimental)

A `virtual-display` node creates a display that macOS and every application treat as a real one:
it's listed in System Settings > Displays, windows can be dragged onto it, and software that
outputs full screen to a display can choose it. Route the node to an OMT, NDI, SRT, DeckLink or
Syphon sink and whatever is on the display goes out over that.

The case it was built for is **Resolume's Advanced Output**, whose output devices are built into
the app: there's no way to add OMT to that list, but a screen whose device is a virtual display
can be sent anywhere Frame Ferret sends. `config/virtual-display.json` is the starting point:

```json
{ "id": "screen", "kind": "virtual-display", "target": "Frame Ferret Display",
  "width": 1920, "height": 1080, "rate": "60" }
```

- `target` names the display (then `label`, then "Frame Ferret"). Resolume doesn't show that
  name: it lists displays by number, so the virtual one is the "Display 2 (1920x1080)" or
  similar that appears when Frame Ferret starts. Use Output > Identify Displays to be sure.
- The display exists while Frame Ferret runs and goes when it stops. The same name gets the same
  serial number every time, so macOS remembers where it was arranged.
- It needs Screen Recording permission, like screen capture.
- **Use a whole rate** (25, 30, 50, 60). macOS runs a virtual display at whole rates only: a 59.94
  display runs at 60, and Frame Ferret warns that its capture will drop about one frame every
  17 seconds.
- The pointer isn't captured. If you move it onto the display it won't appear in the output.
- **It is private API.** macOS has no public way to create a display; this uses the same
  CoreGraphics classes as Sidecar, BetterDisplay and DeskPad. A future macOS could remove them.
  If one does, this node fails with a sentence saying so and everything else keeps working.
- Like any screen capture, it can lose a frame when the Mac is heavily loaded, because the window
  server skips composites. Run on an idle machine it was clean.

---

## The virtual display on Windows (experimental)

**New in v0.4.0.**

Windows won't load a display driver Microsoft hasn't signed, so Frame Ferret doesn't bring one.
It uses VirtualDrivers' **Virtual Display Driver** (VDD), which is free, MIT-licensed and signed,
and which you install once:

1. Install VDD, from
   [its releases](https://github.com/VirtualDrivers/Virtual-Display-Driver/releases) or with
   `winget install --id=VirtualDrivers.Virtual-Display-Driver -e`. It adds a display called
   "VDD by MTT".
2. In Settings > System > Display, make sure the displays are set to "Extend these displays".
3. Run a config with a `virtual-display` node. `config/virtual-display.json` sends it out over
   OMT; on Windows only NDI has been tried so far, so change that sink's `kind` to `ndi` if you
   want the tested route.

When the node starts, it claims the first VDD display that no other `virtual-display` node is
using and sets it to the node's width, height and rate. Then it captures it, and keeps following
it if Windows renumbers it. If VDD doesn't list that mode, Frame Ferret adds it to VDD's settings
file (`C:\VirtualDisplayDriver\vdd_settings.xml`, marked "added by Frame Ferret") and reloads
VDD. **A reload takes every VDD display away for about a second**, so set modes up before a show,
not during one. After the first run the mode is listed and no reload is needed.

How it differs from the Mac:

- **The display is VDD's, not Frame Ferret's.** It's there whether Frame Ferret runs or not, and
  Windows and other software call it "VDD by MTT"; `target` only names it in Frame Ferret's own
  messages. For two virtual displays, raise `<count>` under `<monitors>` in VDD's settings file.
- **It has to run in the signed-in desktop.** There's no permission prompt, but there are no
  displays at all for a service or an ssh session, and none while the screen is locked.
- **Use a whole rate** (25, 30, 50, 60). VDD lists 59.94, but Windows never offers that mode, so
  a 59.94 display runs at 60 and Frame Ferret warns that its capture will drop about one frame
  every 17 seconds.
- The pointer isn't captured, and an HDR or rotated display is refused with a sentence saying so.
- **A VDD reload during capture broke VDD once in three tries** on the test VM. The log then says
  "VDD has stopped"; see the table at the end.

What it has been run on: a Windows 11 VM with no GPU, sending 1080p60 over NDI to a separate
receiver. That gave 60 frames a second delivered, nothing out of order and the bars within one
level, but only 27 *new* pictures a second, because that VM's desktop composes only about 40
times a second. A machine with a graphics card should do better; that hasn't been measured. It
hasn't been run with Resolume on Windows yet.

---

## What is not built

Designed and documented, but not implemented: the UVC virtual camera (it needs an embedded
provisioning profile the release harness doesn't produce yet), window and application capture
on Windows (they need Windows.Graphics.Capture), DeckLink capture, Spout, HTML output, and ST
2110-30 audio and -40 ancillary.

They're left out of the UI rather than sitting there failing.

---

## If something is wrong

| Symptom | Cause |
| --- | --- |
| **A stream is on the wrong NIC** | Every node takes an interface — except NDI, whose API has no such parameter. Check for the warning. |
| **A sink is black but running** | Nothing is routed to it. That is the invariant, not a fault. |
| **Blue reads 178 over NDI** | NDI's compression. Independent senders land on the same number. |
| **SRT does nothing** | It needs an ffmpeg and libsrt. Check the resolution order above; a wrong explicit path is an error rather than a silent fallback. |
| **SRT receive is black for the first few seconds** | ffmpeg examines the start of the stream before it decodes anything. About six seconds, on macOS and Windows alike. |
| **The log says an encoder "is listed but would not open"** | That machine's ffmpeg offers it, but its GPU or driver isn't there. Frame Ferret moves on to the next encoder; nothing to fix unless you wanted that one. |
| **An SRT sink with an `"encoder"` setting won't start** | The encoder you named didn't open, and a named encoder is never swapped for another. The error has ffmpeg's reason. |
| **A node type is missing** | Run `frame-ferret kinds`. Several are designed but not built — see above. |
| **The virtual display says "never made it active" or "no CGVirtualDisplay"** | The first is the window server refusing it, usually right after a macOS update; restart Frame Ferret. The second means this macOS no longer has the private classes. |
| **The virtual display is in Resolume but the output is black** | The Resolume screen isn't enabled, or the sink isn't routed to the display. A missing Screen Recording permission stops the node with a sentence rather than sending black. |
| **The Windows virtual display says VDD is not installed** | Install VirtualDrivers' Virtual Display Driver (above). Frame Ferret doesn't bring a display driver of its own. |
| **It says VDD's display "is not part of the desktop"** | Settings > System > Display: choose "Extend these displays". During a VDD reload this is normal for about a second. |
| **The log says "VDD has stopped"** | VDD's device has failed, which a VDD reload during capture did once in three tries on the test VM. In Device Manager, Display adapters > Virtual Display Driver: Disable device, then Enable device. The capture picks the display up again. |
| **A Windows capture says it needs the signed-in desktop** | It's running as a service, from an ssh session, or while the screen is locked or a UAC prompt is up. Display capture only works in the signed-in user's session. |
| **Something else does nothing on Windows or Linux** | Apart from screen capture, the virtual display, OMT and SRT on Windows, both have only built and self-tested in CI. No hardware path exists on either. |

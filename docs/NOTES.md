# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*Frame Ferret — software virtual capture card: NDI/OMT/SRT/2110 both ways, screen capture in, UVC/Syphon/DeckLink/HTML out. NDI verified both directions; everything else not built*

**Frame Ferret** (`~/Projects/frame-ferret`, started 2026-08-08) — a software
virtual capture card. One app that is an **NDI, OMT, SRT and ST 2110 endpoint in
both directions** over a chosen NIC, captures this machine's screen, and presents
as a **UVC camera, Syphon/Spout server, DeckLink output or HTML overlay**.
C++20/CMake, MIT, tray launcher + browser config in [weblinked](https://github.com/stoatworks-labs/weblinked/blob/main/docs/NOTES.md) (`weblinked`)'s shape.

Scope was confirmed with Allan before any work — he took **maximum scope on both
risk items** after they were flagged: full 2110-20/-30/-40, and all three
OS-extension endpoints (macOS UVC, Windows UVC, virtual display).

**PUBLIC at github.com/stoatworks-labs/frame-ferret, branch `main`.**

**v0.1.0 RELEASED 2026-08-08** — 3 platform zips (macOS universal / Windows x64 /
Linux x64), notarised by the autosigner, `gen-downloads.py` run after it (that
order matters), README download block written, and the website project page LIVE
at stoatworks-labs.com/software/frame-ferret/ with `scripts/shots.json` mapping
the card to `frame-ferret/docs/thumb.png`.
**VIDEO DONE 2026-08-08: YouTube `7jppEKgBDLk`** (60.8s), thumbnail set at
upload time, embedded in BOTH homes (README + projects.json), site redeployed,
`gen-downloads.py --check` exits 0. Capture/build/description live at
`stoatworks-backend/video/projects/frame-ferret/`. **Only Instagram is
Instagram Reel `instagram.com/reel/Dbybxhpijqe/` published 2026-08-08 —
`changes: []` (no black padding), `audio_codec: aac`, cover a SHA-pinned JPEG in
the public repo. **ALL SEVEN HOMES NOW AGREE.**

**Filming needs the EXTERNAL DISPLAY connected** — all 22 capture scripts carry
`refusing to film the main display`, and that guard matters: a take records the
whole display for ~50 s, so filming the main one would publish whatever else is
open. Frame Ferret's driver clicks the REAL crosspoint cells (not the API
behind them), so the State column/preview/counters follow from the running
engine. FOCUS regions are `(x,y,w,h)` and must be 16:9 to within 0.01;
THUMB_CROP is a PIL box `(l,t,r,b)` — different conventions in the same file.
**`LOGO` in 42 build.py files points at `~/Downloads/Stoatworks Labs Branding
Assets/Website Header.png`, which NO LONGER EXISTS** — use
`~/Projects/stoatworks-backend/branding/lockup.png`.

**AUDIO IS ROUTED** through the crosspoint (test pattern emits 1 kHz @ -20 dBFS).
**`Source::takeAudio()` is DESTRUCTIVE — take it ONCE per source per tick** and
share with every routed sink, or the first sink served gets it and the rest get
silence. Audio is deliberately NOT sent on a `black` action (silence needs no
filler; video black exists because downstream must re-lock). Verified across NDI
as a Frame Ferret round trip through libndi — **oxbow's probe has NO audio
support**, so it is not an independent check.

**Phases 0, 1, 1b and most of 2 done (2026-08-08). NDI AND OMT BOTH WORK BOTH
WAYS.** 535 checks / 11 binaries, CI green on all three platforms. `frame-ferret run` generates colour bars, routes them
through the crosspoint, converts, and serves a live control page with a
clickable crosspoint grid at **port 8740** (NOT 8730 — a co-session's `openrcs-s`
already listens there; the pre-bind port check caught it). Also built: pixel
conversion (BGRA/RGBA/UYVY/YUY2/v210 through one 10-bit 4:2:2 intermediate),
the Engine frame loop, config loading, a test-pattern source, a preview sink,
the HTTP control API, `selftest`, and CI on all three platforms.
**NDI send and receive are verified against oxbow — a separate codebase**: oxbow
discovers `MAC (FerretTest)` and decodes all 8 bars in the right order; Frame
Ferret receives oxbow's stream at 59.97 fps as a UYVY *copy* (no conversion).
Runtime-loaded, never linked.

**OMT also verified both ways** — against macOS's own `dns-sd` (browses
`_omt._tcp`, lists `MAC (FerretOMT)` on 4 interfaces) and oxbow receiving at
`omt://127.0.0.1:6400`, decoding all 8 bars **within 2 code values** (vs NDI's
13 — OMT carried BGRA rather than compressing). OMT states colour space
explicitly (601/709); NDI carries none. OMT frame types are a BITMASK (1/2/4),
NDI's are sequential. `omt_discovery_getaddresses` returns nothing on first call
and must be POLLED — there's no wait function like NDI's.

**DeckLink OUT RUN ON REAL HARDWARE (2026-08-08)** — Duo 2, connector 1, **1080p50
as v210**, card's own completion callback: **2531 frames, 0 late, 0 dropped** over
~50 s, engine at 49.98 fps. Optional at build time (`-DDECKLINK_SDK_DIR`); the
CMake version guard rejects <11.0 and is proven to reject the 10.11 copy in the
NDI SDK examples. **PIXELS ON THE WIRE ARE STILL UNVERIFIED** — needs an SDI loop
cable (every input read NO SIGNAL); do NOT upgrade "displayed every frame" to
"picture is correct".

**Card only enumerates in the 2dhd profile** (2 sub-devices, half duplex); use
`oxbow/build/dl_profile` to see/set it, and `oxbow/build/sdi_probe --list`.
Note the card is PCIe — on this MacBook Pro it needs a **Thunderbolt expansion
chassis**, and when the chassis is unplugged `system_profiler
SPThunderboltDataType` says "No device connected" and nothing enumerates.

**THE DECKLINK SINK MUST NOT ADVERTISE BGRA.** The router treats any accepted
format as a COPY (correctly — that's what avoids needless conversion), so
advertising BGRA means a BGRA source *gets* BGRA — and a **Duo 2 will not carry
1080p50 as 8-bit BGRA at all**, so the output dies with "will not carry that
mode" having never tried v210. Advertise `{v210, uyvy8}` only.

**Syphon OUT done and VERIFIED against Resolume's Syphon 5** (WebLinked's
`syphon_probe`), from a process started AFTERWARDS — the exact case the
main-thread trap breaks. `src/app/main_loop.h` added; **`cmdRun` must wait with
`waitServicingMainLoop`, never `sleep_for`**, or the frame thread's
`dispatch_sync` deadlocks on the first Syphon frame.

**SRT RECEIVE IS VERIFIED END TO END** (2026-08-08).
`src/transports/srt_socket.*` does caller/listener/rendezvous, latency,
passphrase, streamid — and **real interface binding**, the ONLY transport here
that can honour it (`srt_bind()` takes a real address + SRTO_BINDTODEVICE).
**BOTH DIRECTIONS VERIFIED.** Receive: 129,156 bytes byte-for-byte, decoded via
external ffmpeg, all 7 SMPTE bars correct. Send: encoded with
`h264_videotoolbox`, muxed to MPEG-TS, pulled off with `srt-live-transmit` and
decoded by ffmpeg — all 8 bars **within 2 code values** through lossy H.264.

**Nothing already installed can act as an SRT sender — this cost real time, so
use `tools/srtsend`:** ~50 lines on **srt-tokio** (the pure-Rust stack
[srt router](https://github.com/stoatworks-labs/srt-router/blob/main/docs/NOTES.md) (`srt-router`) uses — genuinely independent of libsrt), committed in the
repo. Why the obvious options fail: **Homebrew ffmpeg has NO SRT protocol**
(`-protocols` shows only `srtp`); **`srt-file-transmit` speaks SRT's STREAM
API** and a live-mode receiver correctly rejects it ("MessageAPI/StreamAPI
collision"); **`srt-live-transmit` only bridges UDP<->SRT** and refuses
`file://` as BOTH source and target.

**ffmpeg runs as a SUBPROCESS, never linked** — Allan's explicit ask: point at
an existing install. Order: node `"ffmpeg"` setting, `$FERRET_FFMPEG`, `$PATH`,
usual locations; **an explicit path that's wrong is an ERROR, never a fallback**.
Chosen over linking because libavcodec's structs shift between majors (far more
fragile than the flat NDI/OMT/libsrt ABIs), it keeps ffmpeg's GPL in its own
process, and CI has nothing to detect. Prefers hardware encoders —
picks `h264_videotoolbox` over `libx264` here. `core/subprocess.*` is POSIX-only;
**Windows needs CreateProcess + overlapped pipes and is NOT implemented**.

**TWO COLOUR BUGS FOUND BY SRT SEND — both left hues and bar ORDER perfect while
values were wrong, which reads as "nearly working":**
1. **RANGE.** `convert()` used ONE scaling for read and write, so full-range RGB
   became full-range YCbCr, which every transport treats as narrow and expands
   AGAIN — green came back **240 instead of 191**. `convert()` now returns
   `outRange`: **RGB source -> narrow YCbCr; RGB destination -> full; YCbCr to
   YCbCr untouched.** Engine AND preview must relabel the frame with it.
2. **MATRIX.** rawvideo has no colorimetry so **ffmpeg guesses BT.601 below 720
   lines**. Untagged 709 data at 640x360 encoded as 601: white/black exact,
   every saturated colour skewed. Tag `-colorspace/-color_primaries/-color_trc/
   -color_range` on **BOTH input and output**. Also made the test pattern use the
   same raster rule (>=720 -> 709) the NDI/OMT receivers use, so nothing in the
   program disagrees. Related: [ffmpeg test pattern traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffmpeg_test_pattern_traps.md).

**`poll()` TAKES A NEGATIVE TIMEOUT AS "BLOCK FOREVER".** `Subprocess::
readExactly` tracked its budget by subtracting 10 per empty read and passed the
result straight to `poll()`, so any caller asking for <10 ms **hung the thread
permanently** on the first quiet moment — the SRT node fed ffmpeg a little then
stopped reading for good. Use a real deadline; never a decremented counter.
Isolating it needed the socket layer tested standalone FIRST (it took all
129,156 bytes with TS sync byte 0x47 intact), which is what moved the search off
the transport and into the subprocess layer.

**Two SRT timeout bugs fixed, both worth remembering:** (1) a receive timeout was
detected by matching the string "timeout" — **libsrt says "Operation timed
out"**, so every gap tore the connection down and the source connected-and-
dropped on a loop. (2) **`srt_getlasterror` RETURNS the code; its `int*` is the
system errno** — reading the out-param gives 0 for everything.

**ST 2110, every capture source and every hardware output are still NOT
implemented.** NDI/OMT audio is plumbed through receiver and sender but has
NEVER been carried end to end — do not claim it.
`AGENTS.md` and `docs/ROADMAP.md` are the authority.

**Measured, not asserted:** one 9049-tick run gave **4507 frames + 4542 black =
9049 exactly** — one action per sink per tick — at 49.99 fps, zero late ticks.
Unrouting from the browser turned the output black *and kept it running*,
reporting "no source routed". Colour bars survive a YCbCr round trip within 2
code values by pixel readback.

**ST 2110-20 BOTH WAYS, VERIFIED against GStreamer's RFC 4175 implementation**
(2026-08-08): `rtpvrawdepay` decoded 143 frames of ours (worst error **1 code
value**); we decoded 196 frames from `rtpvrawpay` (worst **2**). 2110-20 IS
RFC 4175 with constraints, so GStreamer is a genuine independent check —
`gst-launch-1.0 ... ! rtpvrawpay/rtpvrawdepay ...`, format **UYVP** for 10-bit
4:2:2. **GStreamer's `videotestsrc pattern=smpte` is 100% bars (255), not 75%
(191)** — a uniform 64 offset with perfect hues is that, not a bug.
`udpsink multicast-iface=lo0` did NOT work on macOS; use unicast for that
direction.

**2110 is the ONLY transport here that binds an interface properly, and must.**
Multicast send needs `IP_MULTICAST_IF` explicitly (the routing table usually
picks the wrong NIC); the join uses the interface INDEX on Linux (`ip_mreqn`)
but only the ADDRESS on macOS/Windows (`ip_mreq`) — **`ip_mreqn` is a Linux
extension and an `#else` meaning "Linux" silently catches Windows**, which is
how CI first broke. Also: `ssize_t` doesn't exist on MSVC (recv/sendto return
int) and `poll()` is `WSAPoll` there.

`st2110_rtp.*` is deliberately SOCKET-FREE so the packetiser is exhaustively
testable (169 checks): round trip, packets spanning line boundaries at awkward
widths, odd widths, whole-pgroup payloads, sequence wrap, and two loss
behaviours — **a lost packet TEARS a frame rather than discarding it** (a
dropped frame is a black flash, far more visible than a torn line), and **a lost
marker is recovered by the next timestamp** rather than stalling forever.
Wire line numbers/offsets are **1-based and in PIXELS**; internal is 0-based
bytes. SDP served via the control API (pasting one is how 2110 gear is set up).

**It is a WIDE-profile sender** (`a=TP=2110TPW`), system-clocked, frames never
`ptpLocked`. Do not upgrade without hardware Tx pacing + a real PTP servo.

**SCREEN CAPTURE DONE (macOS, ScreenCaptureKit) 2026-08-08** — display, window,
application AND region-of-interest. Run here: display gave 175 frames of real
content, a named window 136 with 0 black, and **two different crop regions
captured as genuinely different pictures** (the check that proves a crop is
applied, not ignored). ROI is a `sourceRect`, so it crops before copying.

**THREE TRAPS, all worth remembering:**
1. **A missing Screen Recording permission is INVISIBLE** — `SCStream` starts
   *successfully* and delivers black frames forever, no error/callback/log.
   `ScreenCapture::permitted()` checks up front via shareable content and names
   the permission as the failure reason. Never remove it because "capture
   starts fine".
2. **A plain C++ CLI must call `NSApplicationLoad()` before touching
   CoreGraphics** — `initialiseWindowServer()` in `app/main_loop.h`. **Window**
   capture died on `Assertion failed: (did_initialize) ... CGS_REQUIRE_INIT`
   while **display** capture worked, because only the window path reaches that
   code — so it presents as a window bug and is not one.
3. **ScreenCaptureKit sends IDLE samples with no image** when the screen hasn't
   changed; check `SCStreamFrameInfoStatus == SCFrameStatusComplete` or a static
   desktop reads as a live source at full rate.

## The two constraints found while scoping — do not let these be forgotten

- **ST 2110-20 can only be a WIDE-profile sender here.** Narrow needs hardware
  transmit pacing (ConnectX-5+, Intel E810 Tx scheduling, or `SO_TXTIME`+ETF on
  Linux). Worse, **macOS has no supported hardware-timestamping path at all** —
  no `SO_TIMESTAMPING`, no PHC exposed — so PTP is software-timestamped. That is
  fine for 2110-30 audio and -40 ANC, marginal for -20. Hence the roadmap order:
  **-30/-40 first, then -20 RECEIVE, then -20 send last.** 2110-20 receive is
  forgiving (no pacing duty, clock arrives on the wire) and "receive 2110 →
  present as a UVC camera" is probably the single most useful route in the app.
  Bandwidth: 1080p50 = 2.2 Gb/s, 2160p50 = 8.7 Gb/s, 2160p59.94 does NOT fit
  10 GbE. Full write-up in `docs/02-st2110.md`.
- **macOS has NO public virtual display API.** Only private `CGVirtualDisplay`
  (what Sidecar and BetterDisplay use). Decision: flag it experimental, off by
  default, do it LAST — and first build ScreenCaptureKit window/application
  capture, which is public API and may absorb most of what the feature is
  actually wanted for. `docs/03-os-extensions.md`.

**Unresolved and blocking the installer design:** a macOS system extension must
activate from an app in `/Applications`, but the fleet's usual
unpack-into-Application-Support trick (how WebLinked dodges the Gatekeeper
nested-helper kill, **macos gatekeeper nested binaries** (working-practice note, kept in Claude memory)) is
incompatible with that. One of them has to give.

**Cheapest OS-extension win is the WINDOWS virtual camera** (`MFVirtualCamera`,
Win11, no driver, no attestation signing) — do it first to validate the sink
path. Windows IddCx virtual display needs Partner Center attestation signing.
Note the only Windows box is the **ARM64 Parallels VM running x64 emulated**,
and whether MFVirtualCamera works under that emulation is untested.

**macOS system extensions: there is NO application to Apple — that was my error,
corrected 2026-08-08 after Allan queried it.** "System Extension" is an ordinary
checkbox on an App ID in Certificates, Identifiers & Profiles, listed unmarked in
Apple's supported-capabilities-macos page. The capabilities that really need a
request are **DriverKit and Endpoint Security**, neither of which applies. The
one restricted entitlement here is
`com.apple.developer.system-extension.redistributable` (lets *another team's* app
install your extension) — not needed.

**Notarisation is necessary but NOT sufficient. The missing piece is a
PROVISIONING PROFILE, which `release-lib.sh` has never produced.** A Developer ID
app normally needs none — which is why this has never come up across 38 signed
repos — but an extension carrying a capability makes one mandatory, because some
entitlements ride in the signature and others ride in the profile. macOS wants it
at `App.app/Contents/embedded.provisionprofile`. Needed on top of what
[apple codesigning](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/project_apple_codesigning.md) already has: **explicit (non-wildcard) App IDs for
BOTH container and extension**, System Extension ticked on each, a Developer ID
profile per App ID embedded in each bundle, matching team IDs, and the app
installed in **`/Applications`** (activation from a build dir silently fails).
`rl_mac_sign` walks Mach-O files and knows nothing about profiles, so the profile
must be fetched and embedded BEFORE signing — real harness work, and it gates any
system extension anywhere in the fleet. Also: **ad-hoc builds cannot test this path
at all**, so CI (no signing config, ad-hoc fallback) can never verify it.

**v0.1.1 (2026-08-08) ships the DESKTOP APP** — Allan's fair pushback: v0.1.0
was a zip with a terminal binary, and the ORIGINAL brief asked for a tray app.
`launcher/` carries the av-launcher shell per-repo; `.dmg` / NSIS `.exe` /
`.deb` / `.rpm` all attach. `run` gained `--bind`/`--port` (args injection, as
oxbow does). **Two release traps:** `tauri.windows.conf.json` is a SEPARATE
resources override (kept the shell's `node.exe` and failed only on Windows),
and `upload-artifact` PRESERVES subdirectories so release globs must be
`**/*.dmg`, not `*.dmg` — a no-match silently attaches nothing.

**NEXT: completing the UI.** The control page can still only route + mute;
every node comes from the JSON config. Layer 1 is DONE (2026-08-08):
**the engine can add/remove nodes at runtime** — `addSource/addSink/removeNode`
QUEUE and `applyPending()` drains at the top of a tick before any poll, so a
tick sees one consistent set. Queued not locked (the loop walks those vectors
every tick). Removing a source clears routes pointing at it -> sinks go black
WITH A REASON. Indices are REBUILT per batch, never patched (positions shift on
erase). Still to do: **layer 2** node CRUD + discovery endpoints in the control
API (the discovery functions already exist — `ndiListSources`,
`ScreenCapture::listDisplays/listWindows`, `DeckLinkRuntime::listDevices`,
`listInterfaces` — they are just not exposed), and **layer 3** the add/edit/
remove UI on the page.

## Architecture decisions worth not re-litigating

- **A protocol is just a port on a router.** Every transport is both a `Source`
  and a `Sink`; the crosspoint doesn't care which. Same model as
  [srt router](https://github.com/stoatworks-labs/srt-router/blob/main/docs/NOTES.md) (`srt-router`).
- **Every sink is planned EVERY tick** — `Router::plan()` returns exactly one
  action per sink, always. Unrouted / disconnected / muted all yield *black with
  a reason string*, never an absent action. Same invariant as [kestrel](https://github.com/stoatworks-labs/kestrel/blob/main/docs/NOTES.md) (`kestrel`)
  and for a sharper reason: a **UVC device that stops is dropped by Zoom and does
  not return without the host app restarting**; SDI has to be re-locked; a Syphon
  server that stops vanishes from every consumer's menu. Black is recoverable,
  silence is not.
- **Pixel formats travel WITH the frame — deliberately NOT oxbow's/WebLinked's
  BGRA normalisation.** Both of those feed a GPU chain that wants RGBA anyway;
  here the two best paths (2110-20 and DeckLink) are natively 10-bit YCbCr 4:2:2
  and a BGRA waypoint would quantise to 8 bits *and* pay two chroma resamples for
  what should be a copy. Router marks each route `copy` or `convert`.
- **v210 and the 2110-20 pgroup are DIFFERENT 10-bit packings** — 6 px/16 B
  little-endian vs 2 px/5 B big-endian. Confusing them gives correct geometry and
  wrong colour, which survives a long time.
- Lifted wholesale from [oxbow](https://github.com/stoatworks-labs/oxbow/blob/main/docs/NOTES.md) (`oxbow`): `Dylib`, JSON, HTTP server, diag.

**THE BUG THE FIRST REAL TRANSPORT FOUND — do not reintroduce:** the Engine
gated `poll()` on `connected()`. A network receiver only becomes connected *as a
result of* being polled, so it never polled, never connected, never polled —
654 ticks, 0 frames, "source is not connected". **Completely invisible to the
synthetic test-pattern source**, which reports connected from construction. Poll
unconditionally; use `connected()` afterwards to decide routing. Pinned by
`aSourceThatConnectsOnlyWhenPolledStillWorks`.

Related: **a transport node's direction comes from its ROUTES, not its kind.**
`ndi` can be either, so the control API asks the Engine (`hasSource`/`hasSink`)
what was actually built — inferring from the kind rendered the NDI receiver as a
sink too. NDI-specific ABI traps are in [ndi sdk abi traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ndi_sdk_abi_traps.md).

**PERFORMANCE — conversion is the recurring bottleneck. Measured, 1920x1080:**
BGRA->v210 went **26.6 -> 23.4 -> 12.7 ms** (38 -> 43 -> 79 fps ceiling), which is
what took the DeckLink from 34 fps with a third of ticks late to a clean 50.
Two changes: (1) **fold the colour coefficients** — luma weights, range scaling
and chroma normalisation multiplied together in advance, one dot product per
component, no int64, and average chroma in RGB *before* converting (the
transform is linear, so identical, and halves the work). **Coefficients must be
`lround`ed, not cast** — truncation toward zero leaves every negative chroma term
one step short and breaks the 2-code-value colour-bar budget. (2) **fuse RGB->v210
past the Row422 intermediate** — most of the remaining cost was traffic, ~8 MB
each way per frame, not arithmetic. A deliberate exception to the
one-intermediate rule, taken only for that hot pair.

**PERFORMANCE — two earlier defects, do not reintroduce:** (1) `convert()` was per-pixel `double` + `std::lround` and cost
**29.7 ms for one 1280x720 UYVY->BGRA frame** — a 33.6 fps ceiling on EVERY
`convert` route. Now fixed-point integer, **9.3 ms**. The first integer draft
truncated luma to 8 bits before scaling and the grey round-trip test caught it
instantly — intermediates must be int64 and rounded. (2) `PreviewSink`
allocated a fresh 3.7 MB buffer TWICE per frame, which cost more than the
conversion. Both engine and preview now hold reusable scratch. Together: OMT
receiver went 19.5 -> 32.5 -> **50.0 fps**.

**A diagnosis I got WRONG and had to retract:** I first blamed the 19 fps on
libomt ignoring short receive timeouts, "measured" by stopping the sender and
seeing no change. That test did not discriminate — the engine kept
re-delivering the held frame to the preview, which kept converting. The
decisive test was unrouting the preview (50 fps) vs rerouting it (19.5). The
OMT reader thread added on the strength of the wrong diagnosis was KEPT (it is
still right for a blocking network receive) but its comment now says plainly
that it was not what fixed the frame rate.

**`Dylib::loadedPath()` used to lie** — it recorded the candidate string, not
what the loader opened. macOS `DYLD_LIBRARY_PATH` resolves by LEAF NAME first,
so it claimed libomt was at `build/libomt.dylib`, a file that does not exist.
Now resolved from the real image list (`_dyld_get_image_name` / `dlinfo` /
`GetModuleFileName`). That path is shown to operators.

**C++ trap that cost a build:** `FrameBuffer` declared its copy operations
(deleted, because `frame_.data` points into its own `storage_`), and
**user-declaring ANY copy operation suppresses the implicit move constructor** —
making the type neither copyable nor movable. It surfaces far from the cause, as
a wall of `Cpp17MoveInsertable` template errors from whatever holds one in a
`std::vector`. Fix: write out the move ctor/assignment explicitly, and re-point
`frame_.data` at the moved-in storage (it cannot be defaulted — the pointer
copied with the VideoFrame still refers to the old object's buffer address).

**macOS trap found and fixed here:** `IFM_TYPE` must be checked before
`IFM_SUBTYPE` in `SIOCGIFMEDIA` — media subtypes are numbered *per media type*,
so a Wi-Fi modulation and 10baseT can be the same integer, and switching on the
subtype alone confidently reports Wi-Fi as a wired link. A wrong link speed is
worse than none when the whole point is catching a 2110 stream aimed at a NIC
that can't carry it.

See [agents md convention](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_agents_md_convention.md), **commit means push** (working-practice note, kept in Claude memory),
[ndi distribution licensing](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ndi_distribution_licensing.md) (runtime-load NDI, never link it).

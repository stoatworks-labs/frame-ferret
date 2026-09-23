# Frame Ferret — desktop app

A small desktop app for Frame Ferret: pick a network interface + port, Start/Stop
the engine, open the control page in your browser, and leave it running in the
system tray. Built with [Tauri v2](https://tauri.app) using the fleet's reusable
[av-launcher](https://github.com/stoatworks-labs/av-launcher) shell.

Download an installer from
[Releases](https://github.com/stoatworks-labs/frame-ferret/releases):
macOS `.dmg`, Windows `-setup.exe`, Linux `.deb` + `.rpm`.

> **Fully self-contained.** Frame Ferret is a single native binary, so the bundle
> embeds just that — no runtime, no separate checkout. Every transport is loaded
> at run time rather than linked, so the bundled binary starts on a machine with
> none of them installed and reports each as unavailable.

> **Unsigned builds.** By default the installers are unsigned. On macOS,
> right-click the app → **Open** → **Open** once; on Windows, "More info" →
> "Run anyway". See [SIGNING.md](SIGNING.md) to produce signed macOS builds.

## What it does

- Lists bindable network interfaces + a port field (defaults to 8740).
- **Start/Stop** the embedded `frame-ferret` binary (`run --bind <host> --port
  <port>` — plain argument injection; with no `--config` it serves its built-in
  colour-bars configuration, so something is on screen immediately).
- **Open** the control page in your browser.
- Lives in the system tray; the panel themes itself to Frame Ferret's palette
  (carried in `src-tauri/launcher.toml`).

## Build

```bash
cd launcher
npm ci
bash scripts/prepare.sh        # cmake-build frame-ferret and stage it as src-tauri/bin/
npm run tauri build            # produces installers under src-tauri/target/release/bundle
```

The staged binary (`src-tauri/bin/frame-ferret[.exe]`) is produced by
`prepare.sh` and git-ignored; it ships inside the bundle.

## The shell

The panel/tray shell (`src/`, `src-tauri/src/`, `src-tauri/crates/`,
`Cargo.lock`) is a file-for-file copy of
[av-launcher](https://github.com/stoatworks-labs/av-launcher) at `804555c`;
only `src-tauri/launcher.toml` (config + theme), `tauri.conf.json`,
`entitlements.plist`, the icons and `scripts/prepare.sh` are app-specific.
Refresh the shell by copying those files from a newer av-launcher checkout,
not by editing them here.

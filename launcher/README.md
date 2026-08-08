# Atem Overseer — desktop app

A small menu-bar desktop app for Atem Overseer: pick a network interface + port,
Start/Stop the server, open the dashboard, and run it from the system tray.
Built with [Tauri v2](https://tauri.app) using the fleet's reusable
[av-launcher](https://github.com/stoatworks-labs/av-launcher) shell.

Download an installer from
[Releases](https://github.com/stoatworks-labs/atem-overseer/releases).

> **Fully self-contained.** Because Atem Overseer is a Node app, this bundle
> embeds a Node runtime **and** the whole app (server + built dashboard).
> Nothing needs to be installed — no Node, no separate checkout. Just download
> and run.

> **Unsigned builds.** By default the installers are unsigned. On macOS,
> right-click the app → **Open** → **Open** once; on Windows, "More info" →
> "Run anyway". See [SIGNING.md](SIGNING.md) to produce signed macOS builds.

## What it does

- Lists bindable network interfaces + a port field (defaults to 4700).
- **Start/Stop** the embedded Overseer server.
- **Open** the dashboard in your browser.
- Lives in the system tray; the panel themes itself to Atem Overseer's palette
  (carried in `src-tauri/launcher.toml`).

## Build

```bash
cd launcher
npm ci
bash scripts/prepare.sh        # build app + embed Node runtime for this platform
npm run tauri build            # produces installers under src-tauri/target/release/bundle
```

To stage a Node runtime for a different platform, set `NODE_PLATFORM`
(e.g. `NODE_PLATFORM=win-x64 bash scripts/prepare.sh`).

The embedded runtime (`src-tauri/node[.exe]`) and app tree
(`src-tauri/atem-overseer-app/`) are produced by `prepare.sh` and git-ignored;
they ship inside the bundle.

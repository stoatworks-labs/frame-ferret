# Diagnostics

Three artefacts, because a failure on site needs different things at different
moments. The module is the fleet's vendored `diag`, shared with WebLinked and
oxbow, and everything it writes carries the `stoatworks.diagnostics/1` schema —
so the same tooling reads a crash report from here, from a Rust daemon and from
a JUCE plugin.

## 1. The rotating log

```
macOS    ~/Library/Logs/FrameFerret/FrameFerret.log
Windows  %LOCALAPPDATA%\FrameFerret\logs\FrameFerret.log
Linux    ~/.local/state/FrameFerret/logs/FrameFerret.log
```

Rotates at 4 MB keeping one previous file. Every line is flushed as it is
written — a crash must not lose the line that explains it.

| Variable | Effect |
|---|---|
| `FERRET_LOG` | `trace`, `debug`, `info`, `warn`, `error`, `fatal` |
| `FERRET_LOG_DIR` | Write somewhere else |

The lines worth knowing are the ones a bug report is usually really about: every
node that could not be built and why, every node built with a warning (an
`interface` on an NDI node, say, which the SDK cannot honour), and the four
counters at shutdown. Those counters are the first question asked of any "it
went black" report, and the terminal they were printed to is long gone by the
time anybody files one.

## 2. The crash report

Written automatically when the process dies on SIGSEGV, SIGABRT, SIGBUS, SIGILL
or SIGFPE (or an unhandled Windows exception), next to the log:

```
FrameFerret-crash-20260817-174247.json
```

It carries the build identity, the platform, the redacted **effective**
configuration — the config file after `--bind` and `--port` have won, which is
the version that was actually running — the last few hundred log lines and a
backtrace.

Config keys whose names look like secrets (`token`, `password`, `secret`, `key`,
`auth`, `credential`, `cookie`) are redacted once, when the config is handed
over, so nothing downstream has to remember to do it. `control_token` is named
to be caught by that.

The handler is installed at startup and **re-installed after the nodes are
built**. libomt embeds the .NET runtime, which registers its own handlers for
SIGSEGV and SIGBUS when it starts — on first OMT sender or receiver creation —
so whatever was registered earlier may no longer be ours. This is the same trap
as the SIGINT/SIGTERM one in `app/main.cpp`, and the same fix.

## 3. The diagnostics bundle

One file with all of the above in it, so "send me your diagnostics" is one
instruction:

```bash
frame-ferret diagnostics
```

It prints the path it wrote. `--collect-diagnostics` is accepted as an alias,
because that is the spelling the issue form prints. Pass `--config <file>` to
have the bundle describe a particular configuration; without one it describes
the built-in colour-bars configuration. A config file that will not parse does
not stop the bundle — a config that will not parse is itself worth reporting,
and the reason goes in the log.

The bundle carries the app and platform identity, the redacted config, the log
directory, the recent log lines and the names of any crash reports sitting
alongside the log.

While the engine is running the control page serves the same thing:

| Route | What it gives |
|---|---|
| `GET /api/diagnostics` | Writes a bundle, returns its path and the log paths |
| `GET /api/diagnostics/bundle` | Writes a bundle and returns the file itself |
| `GET /api/log?lines=N` | The last N log lines, the level and the log path |

`/api/diagnostics/bundle` exists because a path is no use when the operator is
on a laptop and the machine with the fault is in a rack two floors down. Both
are deliberately GETs: "open this link and send me the file" is one instruction
and works from a phone.

All three need the control token if one is configured, like every other route.

The log lines come from the in-memory ring rather than by reading the file back:
the ring survives a rotation, and reading the log from the HTTP thread while the
logging thread is appending to it is a race for no benefit.

## Filing a bug

Attach the bundle to the [bug report form][bug]. Have a look at it first — the
redaction is by key name, so it catches `control_token` but cannot know that a
node's `target` is a URL you would rather not publish.

[bug]: https://github.com/stoatworks-labs/frame-ferret/issues/new?template=bug_report.yml

# srtsend — an independent SRT sender, for verifying the SRT source

Frame Ferret's SRT receive path could not be tested with anything already on
the machine, and that is worth recording because the next person will hit it:

- **Homebrew's ffmpeg has no SRT protocol.** `ffmpeg -protocols` lists `srtp`
  and nothing else; `-f mpegts srt://…` fails with "Protocol not found". An
  ffmpeg built `--enable-libsrt` would work.
- **`srt-file-transmit` speaks SRT's stream API**, and a live-mode receiver
  correctly rejects it — "MessageAPI/StreamAPI collision". That rejection is
  right: live video is the message API.
- **`srt-live-transmit` only bridges UDP↔SRT**, and refuses `file://` as either
  source or target, so it cannot be used as a self-contained sender.

So this exists. It is ~50 lines on **srt-tokio**, the pure-Rust SRT stack that
`srt-router` uses — a completely different implementation from the libsrt Frame
Ferret binds, which is exactly what makes a pass meaningful rather than this
repository agreeing with itself.

## Use

```bash
cargo build --release --manifest-path tools/srtsend/Cargo.toml
```

Make a test file, run Frame Ferret as an SRT listener, then send:

```bash
ffmpeg -f lavfi -i "smptebars=size=640x360:rate=25" -t 12 \
  -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p -f mpegts bars.ts
```

```bash
./tools/srtsend/target/release/srtsend bars.ts 127.0.0.1:9710 3
```

## What it proved

Frame Ferret received all 129,156 bytes of the file byte-for-byte, decoded it
through an external ffmpeg, and rendered all seven SMPTE bars at the right
levels and in the right channel order.

// A minimal SRT live sender, built on srt-tokio — the same pure-Rust SRT stack
// srt-router uses, and a completely different implementation from the libsrt
// that Frame Ferret binds. That independence is the whole point: it is the
// reference sender that neither Homebrew's ffmpeg (no SRT compiled in) nor
// srt-file-transmit (stream API, not live) could provide.
//
// Reads an MPEG-TS file and paces it out in 1316-byte payloads — seven 188-byte
// TS packets, which is the standard SRT live payload so a TS packet never
// straddles a boundary.
use bytes::Bytes;
use futures::SinkExt;
use srt_tokio::SrtSocket;
use std::time::{Duration, Instant};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!("usage: srtsend <file.ts> <host:port> [loops]");
        std::process::exit(2);
    }
    let data = std::fs::read(&args[1])?;
    let target = args[2].clone();
    let loops: usize = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(3);

    eprintln!("srtsend: calling {target} with {} bytes", data.len());
    let mut socket = SrtSocket::builder().call(target.as_str(), None).await?;
    eprintln!("srtsend: connected");

    const PAYLOAD: usize = 1316;
    // Pace to roughly the file's own bitrate so the receiver's buffer behaves
    // like a real feed rather than being blasted at line rate.
    let chunks = data.len().div_ceil(PAYLOAD);
    let total_secs = 12.0_f64;
    let per_chunk = Duration::from_secs_f64(total_secs / chunks as f64);

    let mut sent = 0usize;
    for pass in 0..loops {
        for chunk in data.chunks(PAYLOAD) {
            let now = Instant::now();
            socket
                .send((now.into(), Bytes::copy_from_slice(chunk)))
                .await?;
            sent += chunk.len();
            tokio::time::sleep(per_chunk).await;
        }
        eprintln!("srtsend: pass {} done, {sent} bytes sent", pass + 1);
    }
    socket.close().await?;
    Ok(())
}

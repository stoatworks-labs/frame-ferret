#pragma once

#include <memory>
#include <string>

#include "app/node.h"

namespace ferret {

/// An SRT source: an SRT connection carrying MPEG-TS, decoded to frames by an
/// external ffmpeg.
///
/// Unlike NDI and OMT, SRT carries *compressed* video, so this node is two
/// pieces bolted together — `srt_socket.*` for the transport and an ffmpeg
/// subprocess for the codec. ffmpeg is never linked; it is located at run time
/// from the node's `ffmpeg` setting, `$FERRET_FFMPEG`, `$PATH` or the usual
/// install locations. See `core/subprocess.h` for why.
///
/// `config.target` is an SRT URL:
///     srt://10.0.0.5:9000?mode=caller&latency=200&streamid=live/1
///
/// The raster is forced to `config.width` x `config.height`. That is not a
/// limitation of taste: rawvideo has no framing, so the reader can only find
/// frame boundaries if every frame is the same known size, and a contribution
/// feed that changes raster mid-stream would otherwise tear silently. ffmpeg
/// scales to fit.
std::unique_ptr<Source> makeSrtSource(const NodeConfig& config,
                                      std::string& error);

/// An SRT sink: frames encoded to H.264 and muxed to MPEG-TS by an external
/// ffmpeg, then sent over SRT.
///
/// The encoder is chosen by `Ffmpeg::bestH264Encoder`, hardware first — on
/// Apple silicon that is `h264_videotoolbox`, which matters because the frame
/// loop is already serving every other sink from one thread.
///
/// `config.target` is the same SRT URL the source takes. A sender usually
/// wants `mode=listener`, so decoders call in to it; that is independent of
/// which way the video flows and catches people out.
std::unique_ptr<Sink> makeSrtSink(const NodeConfig& config,
                                  std::string& error);

}  // namespace ferret

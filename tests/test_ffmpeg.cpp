// Choosing an H.264 encoder, and proving it opens before using it.
//
// The bug this pins: `ffmpeg -encoders` lists what ffmpeg was BUILT with. The
// common Windows builds and most distro Linux packages list h264_nvenc on every
// machine, and it fails at open without an NVIDIA GPU — so a pick by listing
// alone chose an encoder that died on every connect.
//
// Three layers, each tested on its own: the pure decision, a fake ffmpeg (this
// binary, started again as a child) that lists NVENC and QSV but cannot
// open either, and the machine's real ffmpeg when there is one.
//
// The fake is selected by an environment variable rather than an argument,
// because Ffmpeg's functions pass ffmpeg's own arguments and nothing else. The
// parent sets it after starting; children inherit it; a real ffmpeg ignores it.

#include "transports/ffmpeg.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "check.h"

using namespace ferret;

namespace {

std::string g_self;

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

bool contains(const std::vector<std::string>& list, const std::string& item) {
  for (const auto& entry : list) {
    if (entry == item) return true;
  }
  return false;
}

// A real listing's shape, trimmed: the legend (whose "V..... = Video" lines
// have the same field shape as an encoder line), then the table.
const char* kListing =
    "Encoders:\n"
    " V..... = Video\n"
    " A..... = Audio\n"
    " S..... = Subtitle\n"
    " .F.... = Frame-level multithreading\n"
    " ..S... = Slice-level multithreading\n"
    " ...X.. = Codec is experimental\n"
    " ....B. = Supports draw_horiz_band\n"
    " .....D = Supports direct rendering method 1\n"
    " ------\n"
    " V....D libx264              libx264 H.264 / AVC / MPEG-4 AVC (codec h264)\n"
    " V....D libx264rgb           libx264 H.264 RGB (codec h264)\n"
    " V....D h264_amf             AMD AMF H.264 Encoder (codec h264)\n"
    " V....D h264_mf              H264 via MediaFoundation (codec h264)\n"
    " V....D h264_nvenc           NVIDIA NVENC H.264 encoder (codec h264)\n"
    " V..... h264_qsv             H.264 / AVC (Intel Quick Sync Video) (codec h264)\n"
    " A....D aac                  AAC (Advanced Audio Coding)\n"
    " S..... srt                  SubRip subtitle\n";

// ---------------------------------------------------------------------------
// The fake ffmpeg. Behaves like a BtbN build on a PC with no GPU: lists NVENC
// and QSV, and fails to open both, NVENC before reading any input (so the
// frame being written to it hits a closed pipe) and QSV after.
// ---------------------------------------------------------------------------

int fakeFfmpeg(int argc, char** argv) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
#endif
  std::vector<std::string> args(argv + 1, argv + argc);
  auto value = [&](const char* flag) -> std::string {
    // The LAST occurrence: -pix_fmt appears for input and output.
    std::string found;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
      if (args[i] == flag) found = args[i + 1];
    }
    return found;
  };
  auto has = [&](const char* flag) {
    for (const auto& a : args) {
      if (a == flag) return true;
    }
    return false;
  };

  if (has("-version")) {
    std::printf("ffmpeg version fake-1.0 Copyright (c) the test\n");
    std::printf("built with nothing\n");
    return 0;
  }
  if (has("-encoders")) {
    std::fputs(kListing, stdout);
    return 0;
  }

  const std::string encoder = value("-c:v");
  if (encoder == "h264_nvenc") {
    std::fputs("[h264_nvenc @ 0000021c6f3e8a40] Cannot load nvcuda.dll\n"
               "[vost#0:0/h264_nvenc @ 0000021c6f3e8a80] Error while opening "
               "encoder - maybe incorrect parameters\n"
               "Conversion failed!\n",
               stderr);
    return 1;
  }

  // Everything else reads the frame it was sent, as ffmpeg does before it
  // opens an encoder.
  size_t total = 0;
  std::vector<char> buffer(65536);
  for (;;) {
    const size_t n = std::fread(buffer.data(), 1, buffer.size(), stdin);
    if (n == 0) break;
    total += n;
  }

  if (encoder == "h264_qsv") {
    std::fputs("[h264_qsv @ 0x7f8e1c004a00] Error initializing an internal "
               "MFX session: unsupported (-3)\n",
               stderr);
    return 1;
  }
  if (encoder == "libx264" || encoder == "h264_mf") {
    // A trial must send exactly one frame of the raster it names, in UYVY.
    int w = 0, h = 0;
    if (std::sscanf(value("-s").c_str(), "%dx%d", &w, &h) != 2) return 3;
    if (value("-f") != "null") return 4;
    if (total != static_cast<size_t>(w) * 2 * static_cast<size_t>(h)) return 5;
    return 0;
  }
  std::fprintf(stderr, "Unknown encoder '%s'\n", encoder.c_str());
  return 8;
}

// ---------------------------------------------------------------------------

void encoderListingsParseIntoWholeNames() {
  const auto names = Ffmpeg::parseEncoderNames(kListing);
  CHECK(contains(names, "libx264"));
  CHECK(contains(names, "libx264rgb"));
  CHECK(contains(names, "h264_nvenc"));
  CHECK(contains(names, "h264_qsv"));
  CHECK(contains(names, "h264_mf"));
  CHECK(contains(names, "aac"));
  // The legend is not a list of encoders called "=".
  CHECK(!contains(names, "="));
  CHECK(!contains(names, "Video"));
  CHECK_EQ(names.size(), static_cast<size_t>(8));

  // A listing with Windows line endings parses the same.
  std::string crlf;
  for (const char* p = kListing; *p; ++p) {
    if (*p == '\n') crlf += '\r';
    crlf += *p;
  }
  CHECK_EQ(Ffmpeg::parseEncoderNames(crlf).size(), static_cast<size_t>(8));
  CHECK(Ffmpeg::parseEncoderNames("").empty());
}

void ffmpegErrorsAreSummarisedToTheLineThatSaysWhy() {
  CHECK_EQ(Ffmpeg::summariseError(
               "[h264_nvenc @ 0000021c6f3e8a40] Cannot load nvcuda.dll\n"
               "[vost#0:0/h264_nvenc @ 0000021c6f3e8a80] Error while opening\n"),
           std::string("Cannot load nvcuda.dll"));
  CHECK_EQ(Ffmpeg::summariseError("\r\n\nUnknown encoder 'h264_foo'\r\n"),
           std::string("Unknown encoder 'h264_foo'"));
  // Brackets that are not ffmpeg's "[name @ address]" prefix are content.
  CHECK_EQ(Ffmpeg::summariseError("[error] the thing\n"),
           std::string("[error] the thing"));
  CHECK(Ffmpeg::summariseError("").empty());
}

void encodeArgumentsCarryTheEncoderRasterAndColour() {
  H264Encode e;
  e.encoder = "libx264";
  e.width = 1280;
  e.height = 720;
  e.rate = Rate{30000, 1001};
  e.bitrateBps = 2764800;
  const auto a = Ffmpeg::h264EncodeArguments(e);

  auto after = [&](const std::string& flag, int nth) -> std::string {
    int seen = 0;
    for (size_t i = 0; i + 1 < a.size(); ++i) {
      if (a[i] == flag && seen++ == nth) return a[i + 1];
    }
    return {};
  };
  CHECK_EQ(after("-c:v", 0), std::string("libx264"));
  CHECK_EQ(after("-s", 0), std::string("1280x720"));
  CHECK_EQ(after("-r", 0), std::string("30000/1001"));
  CHECK_EQ(after("-g", 0), std::string("30"));  // a keyframe a second, rounded up
  CHECK_EQ(after("-b:v", 0), std::string("2764800"));
  CHECK_EQ(after("-i", 0), std::string("pipe:0"));
  // Tagged on input and output, 709 at 720 lines and up.
  CHECK_EQ(after("-colorspace", 0), std::string("bt709"));
  CHECK_EQ(after("-colorspace", 1), std::string("bt709"));

  e.height = 360;
  e.width = 640;
  const auto sd = Ffmpeg::h264EncodeArguments(e);
  int smpte = 0;
  for (const auto& s : sd) smpte += s == "smpte170m";
  CHECK_EQ(smpte, 4);  // space and primaries, input and output
}

/// The pure decision, with a stand-in for the trial.
void theFirstEncoderThatOpensIsChosen() {
  const std::vector<std::string> listed = {"libx264", "h264_nvenc",
                                           "h264_qsv"};
  std::vector<std::string> tried;
  auto onlyX264 = [&](const std::string& enc, std::string* why) {
    tried.push_back(enc);
    if (enc == "libx264") return true;
    *why = enc == "h264_nvenc" ? "Cannot load nvcuda.dll" : "no MFX session";
    return false;
  };

  const auto pick = Ffmpeg::chooseH264Encoder(listed, "", onlyX264);
  CHECK_EQ(pick.encoder, std::string("libx264"));
  CHECK(pick.error.empty());
  // Hardware first, in order, then libx264.
  CHECK(tried == std::vector<std::string>({"h264_nvenc", "h264_qsv",
                                           "libx264"}));
  CHECK_EQ(pick.skipped.size(), static_cast<size_t>(2));
  CHECK(!pick.skipped.empty() &&
        contains(pick.skipped[0], "h264_nvenc") &&
        contains(pick.skipped[0], "Cannot load nvcuda.dll"));

  // When the hardware one opens, it wins and nothing after it is tried.
  tried.clear();
  const auto all = Ffmpeg::chooseH264Encoder(
      {"libx264", "h264_videotoolbox"}, "",
      [&](const std::string& enc, std::string*) {
        tried.push_back(enc);
        return true;
      });
  CHECK_EQ(all.encoder, std::string("h264_videotoolbox"));
  CHECK_EQ(tried.size(), static_cast<size_t>(1));
  CHECK(all.skipped.empty());

  // Unlisted encoders are never tried, however preferred.
  tried.clear();
  Ffmpeg::chooseH264Encoder({"libx264"}, "",
                            [&](const std::string& enc, std::string*) {
                              tried.push_back(enc);
                              return true;
                            });
  CHECK(tried == std::vector<std::string>({"libx264"}));
}

void nothingThatOpensIsAnErrorThatSaysWhy() {
  const auto pick = Ffmpeg::chooseH264Encoder(
      {"h264_nvenc", "mpeg4"}, "", [](const std::string&, std::string* why) {
        *why = "Cannot load nvcuda.dll";
        return false;
      });
  CHECK(pick.encoder.empty());
  CHECK(contains(pick.error, "no H.264 encoder"));
  CHECK(contains(pick.error, "Cannot load nvcuda.dll"));
}

/// An explicit encoder is an instruction: used as given, any name ffmpeg
/// lists, and never swapped for another when it fails.
void anExplicitEncoderIsUsedAsGivenOrNotAtAll() {
  const std::vector<std::string> listed = {"libx264", "h264_nvenc",
                                           "h264_mf"};
  std::vector<std::string> tried;
  auto allWork = [&](const std::string& enc, std::string*) {
    tried.push_back(enc);
    return true;
  };

  // Chosen over a working, more preferred one.
  auto pick = Ffmpeg::chooseH264Encoder(listed, "libx264", allWork);
  CHECK_EQ(pick.encoder, std::string("libx264"));
  CHECK(tried == std::vector<std::string>({"libx264"}));

  // Not in the automatic list at all, but ffmpeg has it.
  tried.clear();
  pick = Ffmpeg::chooseH264Encoder(listed, "h264_mf", allWork);
  CHECK_EQ(pick.encoder, std::string("h264_mf"));

  // Listed but will not open: an error naming why, and no fallback.
  tried.clear();
  pick = Ffmpeg::chooseH264Encoder(
      listed, "h264_nvenc", [&](const std::string& enc, std::string* why) {
        tried.push_back(enc);
        *why = "Cannot load nvcuda.dll";
        return enc != "h264_nvenc";
      });
  CHECK(pick.encoder.empty());
  CHECK(tried == std::vector<std::string>({"h264_nvenc"}));
  CHECK(contains(pick.error, "Cannot load nvcuda.dll"));
  CHECK(contains(pick.error, "never fallen back"));

  // Not listed: an error, and nothing is run.
  tried.clear();
  pick = Ffmpeg::chooseH264Encoder(listed, "h264_amf", allWork);
  CHECK(pick.encoder.empty());
  CHECK(tried.empty());
  CHECK(contains(pick.error, "h264_amf"));
}

/// The same decision through real processes: the fake ffmpeg lists NVENC and
/// QSV and cannot open either. This is the case that used to pick NVENC.
void aListedHardwareEncoderThatCannotOpenIsSkipped() {
  CHECK_EQ(Ffmpeg::version(g_self),
           std::string("ffmpeg version fake-1.0 Copyright (c) the test"));
  CHECK_EQ(Ffmpeg::listEncoders(g_self).size(), static_cast<size_t>(8));

  H264Encode settings;
  settings.width = 1280;
  settings.height = 720;
  settings.rate = Rate{30, 1};
  settings.bitrateBps = 2764800;

  const auto pick = Ffmpeg::pickH264Encoder(g_self, "", settings);
  CHECK_EQ(pick.encoder, std::string("libx264"));
  if (pick.encoder != "libx264") {
    std::printf("  picked \"%s\": %s\n", pick.encoder.c_str(),
                pick.error.c_str());
  }
  CHECK_EQ(pick.skipped.size(), static_cast<size_t>(2));
  std::string skipped;
  for (const auto& line : pick.skipped) skipped += line + "\n";
  CHECK(contains(skipped, "Cannot load nvcuda.dll"));
  CHECK(contains(skipped, "MFX session"));

  // Asked for by name, the broken one is an error, not libx264.
  const auto explicitNvenc =
      Ffmpeg::pickH264Encoder(g_self, "h264_nvenc", settings);
  CHECK(explicitNvenc.encoder.empty());
  CHECK(contains(explicitNvenc.error, "Cannot load nvcuda.dll"));

  // And a trial that passes really did send one whole frame (the fake exits
  // non-zero on a short or long one).
  H264Encode mf = settings;
  mf.encoder = "h264_mf";
  std::string why;
  CHECK(Ffmpeg::trialEncode(g_self, mf, &why));
}

/// Against the machine's own ffmpeg, when it has one. CI runners do not, so
/// this prints a note and checks nothing there.
void theRealFfmpegPicksAnEncoderThatOpens() {
  const std::string ffmpeg = Ffmpeg::locate("");
  if (ffmpeg.empty()) {
    std::printf("  no ffmpeg on this machine — real-ffmpeg checks skipped\n");
    return;
  }
  std::printf("  %s\n", Ffmpeg::version(ffmpeg).c_str());

  H264Encode settings;
  settings.width = 640;
  settings.height = 360;
  settings.rate = Rate{25, 1};
  settings.bitrateBps = 576000;

  const auto pick = Ffmpeg::pickH264Encoder(ffmpeg, "", settings);
  for (const auto& line : pick.skipped) std::printf("  skipped: %s\n", line.c_str());
  if (pick.encoder.empty()) {
    std::printf("  no encoder opened: %s\n", pick.error.c_str());
  } else {
    std::printf("  picked %s\n", pick.encoder.c_str());
  }
  // An ffmpeg with no H.264 encoder at all is possible, but then the listing
  // says so too; anything listed and picked must have opened.
  bool listsAny = false;
  for (const auto& name : Ffmpeg::listEncoders(ffmpeg)) {
    for (const auto& preferred : Ffmpeg::h264Preference()) {
      listsAny = listsAny || name == preferred;
    }
  }
  if (listsAny && pick.encoder.empty()) {
    // Every listed one failing is a real result on some machines; say so
    // rather than fail, since the choice was still made correctly.
    std::printf("  (every listed H.264 encoder failed its trial here)\n");
  }

  H264Encode bogus = settings;
  bogus.encoder = "ferret_no_such_encoder";
  std::string why;
  CHECK(!Ffmpeg::trialEncode(ffmpeg, bogus, &why));
  CHECK(!why.empty());
  std::printf("  bogus encoder refused: %s\n", why.c_str());
}

void run() {
  encoderListingsParseIntoWholeNames();
  ffmpegErrorsAreSummarisedToTheLineThatSaysWhy();
  encodeArgumentsCarryTheEncoderRasterAndColour();
  theFirstEncoderThatOpensIsChosen();
  nothingThatOpensIsAnErrorThatSaysWhy();
  anExplicitEncoderIsUsedAsGivenOrNotAtAll();
  aListedHardwareEncoderThatCannotOpenIsSkipped();
  theRealFfmpegPicksAnEncoderThatOpens();
}

}  // namespace

constexpr const char* kFakeVariable = "FERRET_TEST_FAKE_FFMPEG";

int main(int argc, char** argv) {
  if (const char* fake = std::getenv(kFakeVariable); fake && *fake) {
    return fakeFfmpeg(argc, argv);
  }
  // CMake passes the full path, because argv[0] is not reliably one.
  g_self = argc > 1 ? argv[1] : argv[0];
#if defined(_WIN32)
  _putenv_s(kFakeVariable, "1");  // also sets the OS block CreateProcess copies
#else
  ::setenv(kFakeVariable, "1", 1);
#endif

  std::printf("ffmpeg\n");
  run();
  std::printf("ffmpeg: %d checks, %d failed\n", ferret_test::g_checks,
              ferret_test::g_failures);
  return ferret_test::g_failures == 0 ? 0 : 1;
}

#include "transports/ffmpeg.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "core/subprocess.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ferret {
namespace {

bool isRunnable(const std::string& path) {
  if (path.empty()) return false;
#if defined(_WIN32)
  return false;
#else
  struct stat info;
  if (::stat(path.c_str(), &info) != 0) return false;
  if (!S_ISREG(info.st_mode)) return false;
  return ::access(path.c_str(), X_OK) == 0;
#endif
}

std::vector<std::string> candidates(const std::string& hint) {
  std::vector<std::string> out;

  if (!hint.empty()) out.push_back(hint);

  if (const char* env = std::getenv("FERRET_FFMPEG")) {
    if (*env) out.emplace_back(env);
  }

  // $PATH, walked by hand rather than shelling out — invoking a shell to find
  // a binary is a quoting bug waiting to happen.
  if (const char* pathEnv = std::getenv("PATH")) {
    std::string path(pathEnv);
    size_t start = 0;
    while (start <= path.size()) {
      size_t colon = path.find(':', start);
      if (colon == std::string::npos) colon = path.size();
      const std::string dir = path.substr(start, colon - start);
      if (!dir.empty()) {
        out.push_back(dir + "/" + (hint.empty() ? "ffmpeg" : hint));
        if (!hint.empty()) out.push_back(dir + "/ffmpeg");
      }
      start = colon + 1;
    }
  }

  // The usual install locations, for a GUI launch that inherits a bare PATH —
  // which is exactly what a tray app gets.
#if defined(__APPLE__)
  out.emplace_back("/opt/homebrew/bin/ffmpeg");
  out.emplace_back("/usr/local/bin/ffmpeg");
  out.emplace_back("/opt/local/bin/ffmpeg");
#elif !defined(_WIN32)
  out.emplace_back("/usr/bin/ffmpeg");
  out.emplace_back("/usr/local/bin/ffmpeg");
  out.emplace_back("/snap/bin/ffmpeg");
#endif

  return out;
}

/// Runs the executable and returns its combined output, or empty.
std::string runAndCapture(const std::string& executable,
                          const std::vector<std::string>& arguments) {
  std::string error;
  auto process = spawnSubprocess(executable, arguments, error);
  if (!process) return {};

  process->closeInput();

  std::string out;
  uint8_t buffer[8192];
  for (int i = 0; i < 200; ++i) {
    const int n = process->read(buffer, sizeof(buffer), 50);
    if (n < 0) break;
    if (n > 0) out.append(reinterpret_cast<char*>(buffer),
                          static_cast<size_t>(n));
    if (out.size() > 512 * 1024) break;
  }
  // ffmpeg writes -version to stdout but much else to stderr, so take both.
  out += process->drainErrors();
  process->stop();
  return out;
}

}  // namespace

bool Ffmpeg::supportedOnThisPlatform() {
#if defined(_WIN32)
  return false;
#else
  return true;
#endif
}

std::string Ffmpeg::locate(const std::string& hint) {
  // An explicit path is an instruction, not a suggestion. If the operator
  // named a file and it is not there, fail — never quietly fall back to some
  // other ffmpeg. This is the same rule the interface selector holds, and for
  // the same reason: silently using something other than what was configured
  // is diagnosed on site as the configuration not working, which is an
  // expensive way to find a typo.
  //
  // A bare name (no separator) is treated as "find this on PATH", so
  // `"ffmpeg7"` still searches.
  if (!hint.empty() && hint.find('/') != std::string::npos) {
    return isRunnable(hint) ? hint : std::string();
  }

  for (const auto& candidate : candidates(hint)) {
    if (isRunnable(candidate)) return candidate;
  }
  return {};
}

std::string Ffmpeg::searchReport(const std::string& hint) {
  if (!hint.empty() && hint.find('/') != std::string::npos) {
    return "the configured ffmpeg \"" + hint +
           "\" is not there, or is not executable. An explicit path is used as "
           "given and never fallen back from — correct it, or remove it to "
           "search $PATH instead.";
  }
  const auto tried = candidates(hint);
  std::string out =
      "no usable ffmpeg was found. Frame Ferret does not bundle one — point it "
      "at the install you already have, with the node's \"ffmpeg\" setting or "
      "$FERRET_FFMPEG. Tried: ";
  // Only the first few, or the whole of $PATH lands in an error message.
  for (size_t i = 0; i < tried.size() && i < 6; ++i) {
    out += (i ? ", " : "") + tried[i];
  }
  if (tried.size() > 6) {
    out += ", and " + std::to_string(tried.size() - 6) + " more";
  }
  return out;
}

std::string Ffmpeg::version(const std::string& executable) {
  const std::string output = runAndCapture(executable, {"-version"});
  if (output.empty()) return {};
  const size_t newline = output.find('\n');
  return output.substr(0, newline == std::string::npos ? output.size()
                                                       : newline);
}

std::vector<std::string> Ffmpeg::availableVideoEncoders(
    const std::string& executable) {
  const std::string output = runAndCapture(executable, {"-hide_banner",
                                                        "-encoders"});
  std::vector<std::string> found;
  // Only the ones we would ever choose. Parsing ffmpeg's whole encoder table
  // is pointless when the question is "which of these four exist".
  for (const char* name : {"h264_videotoolbox", "libx264", "h264_nvenc",
                           "h264_qsv", "h264_v4l2m2m", "mpeg2video"}) {
    if (output.find(name) != std::string::npos) found.emplace_back(name);
  }
  return found;
}

std::string Ffmpeg::bestH264Encoder(const std::string& executable) {
  const auto available = availableVideoEncoders(executable);
  // Hardware first. On Apple silicon h264_videotoolbox costs almost no CPU,
  // and the frame loop is already serving every other sink from one thread.
  for (const char* preferred : {"h264_videotoolbox", "h264_nvenc", "h264_qsv",
                                "libx264", "h264_v4l2m2m"}) {
    if (std::find(available.begin(), available.end(), preferred) !=
        available.end()) {
      return preferred;
    }
  }
  return {};
}

}  // namespace ferret

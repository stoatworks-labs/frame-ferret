#include "transports/ffmpeg.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "core/subprocess.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ferret {
namespace {

#if defined(_WIN32)
constexpr char kPathListSeparator = ';';
constexpr const char* kExecutableName = "ffmpeg.exe";

std::wstring widen(const std::string& text) {
  if (text.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                      static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), n);
  return out;
}

std::string narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                      static_cast<int>(text.size()), nullptr, 0,
                                      nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), n, nullptr, nullptr);
  return out;
}

/// The environment as UTF-8. getenv on Windows is the ANSI code page, which
/// mangles any path with a character outside it — a user folder named in
/// Polish, say.
std::string environment(const char* name) {
  const std::wstring wide = widen(name);
  const DWORD needed = ::GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
  if (needed == 0) return {};
  std::wstring value(needed, L'\0');
  const DWORD got =
      ::GetEnvironmentVariableW(wide.c_str(), value.data(), needed);
  value.resize(got);
  return narrow(value);
}

bool isRunnable(const std::string& path) {
  if (path.empty()) return false;
  const DWORD attributes = ::GetFileAttributesW(widen(path).c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool hasExtension(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  const size_t sep = path.find_last_of("\\/");
  return dot != std::string::npos && (sep == std::string::npos || dot > sep);
}

/// A path names a file when it has a separator or a drive.
bool isExplicitPath(const std::string& hint) {
  return hint.find_first_of("\\/:") != std::string::npos;
}

std::string joinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  const char last = dir.back();
  return (last == '\\' || last == '/') ? dir + name : dir + "\\" + name;
}
#else
constexpr char kPathListSeparator = ':';
constexpr const char* kExecutableName = "ffmpeg";

std::string environment(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

bool isRunnable(const std::string& path) {
  if (path.empty()) return false;
  struct stat info;
  if (::stat(path.c_str(), &info) != 0) return false;
  if (!S_ISREG(info.st_mode)) return false;
  return ::access(path.c_str(), X_OK) == 0;
}

bool isExplicitPath(const std::string& hint) {
  return hint.find('/') != std::string::npos;
}

std::string joinPath(const std::string& dir, const std::string& name) {
  return dir + "/" + name;
}
#endif

/// The names to look for in each directory: the hint as given and, on Windows,
/// with `.exe` added — CreateProcess needs the extension, and nobody types it.
std::vector<std::string> namesFor(const std::string& hint) {
  std::vector<std::string> names;
  if (hint.empty()) {
    names.emplace_back(kExecutableName);
    return names;
  }
  names.push_back(hint);
#if defined(_WIN32)
  if (!hasExtension(hint)) names.push_back(hint + ".exe");
#endif
  names.emplace_back(kExecutableName);
  return names;
}

std::vector<std::string> candidates(const std::string& hint) {
  std::vector<std::string> out;

  if (!hint.empty()) out.push_back(hint);

  const std::string fromEnv = environment("FERRET_FFMPEG");
  if (!fromEnv.empty()) out.push_back(fromEnv);

  // $PATH, walked by hand rather than shelling out — invoking a shell to find
  // a binary is a quoting bug waiting to happen.
  const std::string path = environment("PATH");
  size_t start = 0;
  while (!path.empty() && start <= path.size()) {
    size_t end = path.find(kPathListSeparator, start);
    if (end == std::string::npos) end = path.size();
    std::string dir = path.substr(start, end - start);
#if defined(_WIN32)
    // PATH entries may be quoted on Windows, and often are when they hold a
    // space ("C:\Program Files\ffmpeg\bin").
    if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') {
      dir = dir.substr(1, dir.size() - 2);
    }
#endif
    if (!dir.empty()) {
      for (const auto& name : namesFor(hint)) {
        out.push_back(joinPath(dir, name));
      }
    }
    start = end + 1;
  }

  // The usual install locations, for a GUI launch that inherits a bare PATH —
  // which is exactly what a tray app gets.
#if defined(__APPLE__)
  out.emplace_back("/opt/homebrew/bin/ffmpeg");
  out.emplace_back("/usr/local/bin/ffmpeg");
  out.emplace_back("/opt/local/bin/ffmpeg");
#elif defined(_WIN32)
  // winget (Gyan.FFmpeg / BtbN.FFmpeg) links here, Scoop and Chocolatey put a
  // shim in their own bin, and the long-standing manual install is C:\ffmpeg.
  const std::string local = environment("LOCALAPPDATA");
  if (!local.empty()) {
    out.push_back(local + "\\Microsoft\\WinGet\\Links\\ffmpeg.exe");
  }
  const std::string profile = environment("USERPROFILE");
  if (!profile.empty()) out.push_back(profile + "\\scoop\\shims\\ffmpeg.exe");
  const std::string programData = environment("ProgramData");
  if (!programData.empty()) {
    out.push_back(programData + "\\chocolatey\\bin\\ffmpeg.exe");
  }
  const std::string programFiles = environment("ProgramFiles");
  if (!programFiles.empty()) {
    out.push_back(programFiles + "\\ffmpeg\\bin\\ffmpeg.exe");
  }
  out.emplace_back("C:\\ffmpeg\\bin\\ffmpeg.exe");
#else
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
  process->waitForExit(1000);
  out += process->drainErrors();
  process->stop();
  return out;
}

std::string trim(const std::string& text) {
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

}  // namespace

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
  if (!hint.empty() && isExplicitPath(hint)) {
    if (isRunnable(hint)) return hint;
#if defined(_WIN32)
    // "C:\ffmpeg\bin\ffmpeg" means ffmpeg.exe; nothing else could run.
    if (!hasExtension(hint) && isRunnable(hint + ".exe")) return hint + ".exe";
#endif
    return {};
  }

  for (const auto& candidate : candidates(hint)) {
    if (isRunnable(candidate)) return candidate;
  }
  return {};
}

std::string Ffmpeg::searchReport(const std::string& hint) {
  if (!hint.empty() && isExplicitPath(hint)) {
    return "the configured ffmpeg \"" + hint +
           "\" is not there, or is not executable. An explicit path is used as "
           "given and never fallen back from — correct it, or remove it to "
           "search the PATH instead.";
  }
  const auto tried = candidates(hint);
  std::string out =
      "no usable ffmpeg was found. Frame Ferret does not bundle one — point it "
      "at the install you already have, with the node's \"ffmpeg\" setting or "
      "the FERRET_FFMPEG environment variable. Tried: ";
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
  const size_t newline = output.find_first_of("\r\n");
  return output.substr(0, newline == std::string::npos ? output.size()
                                                       : newline);
}

std::vector<std::string> Ffmpeg::parseEncoderNames(const std::string& listing) {
  // Each encoder line is a six-character capability field, the name, and a
  // description:
  //     V....D libx264              libx264 H.264 / AVC / MPEG-4 AVC ...
  // The legend above the table uses the same field shape ("V..... = Video"),
  // which the "=" in the name position rules out. Matching whole names rather
  // than substrings matters once any encoder can be asked for: "libx264" is a
  // substring of "libx264rgb", and "h264_mf" of nothing useful at all.
  std::vector<std::string> names;
  std::istringstream lines(listing);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    std::string capabilities, name;
    if (!(fields >> capabilities >> name)) continue;
    // Six flag characters today; a range, so one more flag in a later ffmpeg
    // does not make every encoder vanish.
    if (capabilities.size() < 5 || capabilities.size() > 8) continue;
    bool flags = true;
    for (const char c : capabilities) {
      if (!(c == '.' || (c >= 'A' && c <= 'Z'))) flags = false;
    }
    if (!flags) continue;
    const char kind = capabilities[0];
    if (kind != 'V' && kind != 'A' && kind != 'S') continue;
    if (name.empty() || name == "=") continue;
    bool plausible = true;
    for (const char c : name) {
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
            c == '-' || c == '.')) {
        plausible = false;
        break;
      }
    }
    if (plausible) names.push_back(name);
  }
  return names;
}

std::vector<std::string> Ffmpeg::listEncoders(const std::string& executable) {
  return parseEncoderNames(
      runAndCapture(executable, {"-hide_banner", "-encoders"}));
}

const std::vector<std::string>& Ffmpeg::h264Preference() {
  static const std::vector<std::string> order = {
      "h264_videotoolbox", "h264_nvenc", "h264_qsv", "libx264",
      "h264_v4l2m2m"};
  return order;
}

std::vector<std::string> Ffmpeg::h264EncodeArguments(const H264Encode& e) {
  const std::string rate =
      std::to_string(e.rate.num) + "/" + std::to_string(e.rate.den);

  // Tag the colour space explicitly, on the input and again on the output.
  //
  // rawvideo carries no colorimetry, so ffmpeg guesses from the raster —
  // **BT.601 below 720 lines, BT.709 at or above**. Send it 709 data at
  // 640x360 without saying so and it encodes as 601: white and black stay
  // exactly right while saturated colours skew, green high and red low. That
  // reads as "nearly working", which is the worst way for a colour fault to
  // present. The same trap is recorded fleet-wide for ffmpeg's rgb24 handling.
  //
  // The rule below is the one the NDI and OMT receivers already use to infer
  // an untagged source, so every path in this program agrees.
  const bool hd = e.height >= 720;
  const char* space = hd ? "bt709" : "smpte170m";
  const char* primaries = hd ? "bt709" : "smpte170m";

  return {
      "-hide_banner", "-loglevel", "error",
      "-f", "rawvideo",
      "-pix_fmt", "uyvy422",
      "-colorspace", space,
      "-color_primaries", primaries,
      "-color_trc", "bt709",
      "-color_range", "mpeg",
      "-s", std::to_string(e.width) + "x" + std::to_string(e.height),
      "-r", rate,
      "-i", "pipe:0",
      "-an",
      "-c:v", e.encoder,
      "-b:v", std::to_string(e.bitrateBps),
      // One keyframe a second. A decoder joining mid-stream shows nothing
      // until the next one, so a long GOP is a long black hold on connect.
      "-g", std::to_string((e.rate.num + e.rate.den - 1) / e.rate.den),
      "-pix_fmt", "yuv420p",
      "-colorspace", space,
      "-color_primaries", primaries,
      "-color_trc", "bt709",
      "-color_range", "mpeg",
  };
}

bool Ffmpeg::trialEncode(const std::string& executable,
                         const H264Encode& encode, std::string* why) {
  auto arguments = h264EncodeArguments(encode);
  // The null muxer opens no output, so nothing comes back on stdout and the
  // only thing to wait for is the exit code.
  arguments.insert(arguments.end(), {"-f", "null", "-"});

  std::string error;
  auto process = spawnSubprocess(executable, arguments, error);
  if (!process) {
    if (why) *why = error;
    return false;
  }

  // One narrow-range black UYVY frame: U Y V Y = 128 16 128 16.
  const size_t bytes =
      static_cast<size_t>(encode.width) * 2 * static_cast<size_t>(encode.height);
  std::vector<uint8_t> frame(bytes);
  for (size_t i = 0; i + 3 < bytes; i += 4) {
    frame[i] = 128;
    frame[i + 1] = 16;
    frame[i + 2] = 128;
    frame[i + 3] = 16;
  }
  // A write failure is not the verdict: an encoder that refuses to open makes
  // ffmpeg exit while the frame is still going in. The exit code decides.
  process->write(frame.data(), frame.size());
  process->closeInput();

  // Hardware encoders can take a moment to bring a session up; ten seconds is
  // far beyond any that works and short enough not to look like a hang.
  const int code = process->waitForExit(10000);
  const std::string errors = process->drainErrors();
  process->stop();

  if (code == 0) return true;
  if (why) {
    const std::string summary = summariseError(errors);
    if (!summary.empty()) {
      *why = summary;
    } else if (code == -1) {
      *why = "a one-frame encode did not finish within 10 seconds";
    } else {
      *why = "ffmpeg exited with code " + std::to_string(code);
    }
  }
  return false;
}

EncoderPick Ffmpeg::chooseH264Encoder(
    const std::vector<std::string>& listed, const std::string& requested,
    const std::function<bool(const std::string&, std::string*)>& works) {
  EncoderPick pick;
  const auto isListed = [&](const std::string& name) {
    return std::find(listed.begin(), listed.end(), name) != listed.end();
  };

  if (!requested.empty()) {
    if (!isListed(requested)) {
      pick.error = "this ffmpeg has no encoder named \"" + requested +
                   "\" (run `ffmpeg -encoders` to see what it has)";
      return pick;
    }
    std::string why;
    if (!works(requested, &why)) {
      pick.error = "the configured encoder \"" + requested +
                   "\" would not open: " + why +
                   ". An explicit encoder is used as given and never fallen "
                   "back from — correct it, or remove it to let Frame Ferret "
                   "choose";
      return pick;
    }
    pick.encoder = requested;
    return pick;
  }

  for (const auto& candidate : h264Preference()) {
    if (!isListed(candidate)) continue;
    std::string why;
    if (works(candidate, &why)) {
      pick.encoder = candidate;
      return pick;
    }
    pick.skipped.push_back(candidate + " is listed but would not open (" +
                           why + ")");
  }

  std::string looked;
  for (const auto& name : h264Preference()) {
    looked += (looked.empty() ? "" : ", ") + name;
  }
  pick.error = "no H.264 encoder in this ffmpeg will open (looked for " +
               looked + ")";
  for (const auto& line : pick.skipped) pick.error += "; " + line;
  return pick;
}

EncoderPick Ffmpeg::pickH264Encoder(const std::string& executable,
                                    const std::string& requested,
                                    const H264Encode& settings) {
  const auto listed = listEncoders(executable);
  return chooseH264Encoder(
      listed, requested,
      [&](const std::string& encoder, std::string* why) {
        H264Encode trial = settings;
        trial.encoder = encoder;
        return trialEncode(executable, trial, why);
      });
}

std::string Ffmpeg::summariseError(const std::string& stderrText) {
  std::istringstream lines(stderrText);
  std::string line;
  while (std::getline(lines, line)) {
    line = trim(line);
    // "[h264_nvenc @ 0000021c6f3e8a40] Cannot load nvcuda.dll" -> the message.
    // Only a bracket that holds " @ " is ffmpeg's context prefix; anything
    // else in brackets is part of what it is saying.
    while (!line.empty() && line.front() == '[') {
      const size_t close = line.find(']');
      if (close == std::string::npos ||
          line.substr(0, close).find(" @ ") == std::string::npos) {
        break;
      }
      line = trim(line.substr(close + 1));
    }
    if (line.empty()) continue;
    if (line.size() > 240) line = line.substr(0, 240) + "...";
    return line;
  }
  return {};
}

}  // namespace ferret

#include "core/dylib.h"

#include <utility>

#if defined(_WIN32)
// See the NOMINMAX note in CMakeLists.txt: windows.h's max/min macros
// collide with std::numeric_limits<>::max() in CEF's headers.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif !defined(_WIN32)
#include <link.h>
#endif
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#elif defined(__linux__)
#include <unistd.h>
#include <climits>
#endif

namespace ferret {
namespace {

std::string executableDirectory() {
#if defined(_WIN32)
  char buffer[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  if (length == 0) {
    return {};
  }
  std::string path(buffer, length);
  const auto slash = path.find_last_of("\\/");
  return slash == std::string::npos ? std::string{} : path.substr(0, slash);
#elif defined(__APPLE__)
  char buffer[PATH_MAX] = {};
  uint32_t size = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &size) != 0) {
    return {};
  }
  std::string path(buffer);
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string{} : path.substr(0, slash);
#else
  char buffer[PATH_MAX] = {};
  const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length <= 0) {
    return {};
  }
  std::string path(buffer, static_cast<size_t>(length));
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string{} : path.substr(0, slash);
#endif
}

std::string lastLoaderError() {
#if defined(_WIN32)
  const DWORD code = GetLastError();
  if (code == 0) {
    return "no error";
  }
  char* buffer = nullptr;
  const DWORD length = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
  std::string message = length && buffer ? std::string(buffer, length) : "unknown error";
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
    message.pop_back();
  }
  return message;
#else
  const char* error = dlerror();
  return error != nullptr ? std::string(error) : std::string("unknown error");
#endif
}

}  // namespace

Dylib::~Dylib() { close(); }

Dylib::Dylib(Dylib&& other) noexcept
    : handle_(other.handle_),
      loadedPath_(std::move(other.loadedPath_)),
      lastError_(std::move(other.lastError_)) {
  other.handle_ = nullptr;
}

Dylib& Dylib::operator=(Dylib&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    loadedPath_ = std::move(other.loadedPath_);
    lastError_ = std::move(other.lastError_);
    other.handle_ = nullptr;
  }
  return *this;
}

bool Dylib::open(const std::vector<std::string>& candidates) {
  close();
  lastError_.clear();

  for (const auto& candidate : candidates) {
    if (candidate.empty()) {
      continue;
    }
#if defined(_WIN32)
    handle_ = static_cast<void*>(LoadLibraryA(candidate.c_str()));
#else
    handle_ = dlopen(candidate.c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
    if (handle_ != nullptr) {
      // Provisional. This is the path we *asked* for, which is not necessarily
      // the file the loader opened — see resolveRealPath().
      loadedPath_ = candidate;
      resolveRealPath();
      return true;
    }
    if (!lastError_.empty()) {
      lastError_ += "; ";
    }
    lastError_ += candidate + ": " + lastLoaderError();
  }
  return false;
}

void Dylib::resolveRealPath() {
  // The candidate string is what we asked for; it can differ from what the
  // loader actually opened. On macOS, DYLD_LIBRARY_PATH is searched by *leaf
  // name* ahead of the path given to dlopen, so asking for
  // "<exedir>/libomt.dylib" can legitimately open
  // "~/.local/lib/omt/libomt.dylib" — and reporting the first one sends any
  // "which runtime is this actually using?" investigation to a file that does
  // not exist. That is exactly what happened when OMT was first wired up.
  //
  // The path is reported through the control API, so it has to be the truth.
#if defined(_WIN32)
  char buffer[MAX_PATH] = {0};
  if (GetModuleFileNameA(static_cast<HMODULE>(handle_), buffer,
                         sizeof(buffer)) > 0) {
    loadedPath_ = buffer;
  }
#elif defined(__APPLE__)
  // dladdr needs an address inside the image, and at this point no symbol has
  // been resolved. Scanning the loaded-image list by leaf name is the reliable
  // way to answer before the first dlsym.
  const size_t slash = loadedPath_.find_last_of('/');
  const std::string leaf =
      slash == std::string::npos ? loadedPath_ : loadedPath_.substr(slash + 1);
  const uint32_t count = _dyld_image_count();
  for (uint32_t i = 0; i < count; ++i) {
    const char* name = _dyld_get_image_name(i);
    if (!name) continue;
    const std::string image(name);
    const size_t imageSlash = image.find_last_of('/');
    const std::string imageLeaf =
        imageSlash == std::string::npos ? image : image.substr(imageSlash + 1);
    if (imageLeaf == leaf) {
      loadedPath_ = image;
      return;
    }
  }
#else
  link_map* map = nullptr;
  if (dlinfo(handle_, RTLD_DI_LINKMAP, &map) == 0 && map && map->l_name &&
      *map->l_name) {
    loadedPath_ = map->l_name;
  }
#endif
}

void Dylib::close() {
  if (handle_ == nullptr) {
    return;
  }
#if defined(_WIN32)
  FreeLibrary(static_cast<HMODULE>(handle_));
#else
  // Deliberately *not* dlclosed on POSIX. Both libndi and libomt start worker
  // threads; unloading the code those threads are executing during shutdown is
  // a race we cannot win, and leaking one handle for the life of the process
  // costs nothing.
#endif
  handle_ = nullptr;
  loadedPath_.clear();
}

void* Dylib::rawSymbol(const char* name) const {
  if (handle_ == nullptr) {
    return nullptr;
  }
#if defined(_WIN32)
  return reinterpret_cast<void*>(
      GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
  return dlsym(handle_, name);
#endif
}

std::vector<std::string> Dylib::localSearchPaths() {
  std::vector<std::string> paths;
  const std::string exeDir = executableDirectory();
  if (exeDir.empty()) {
    return paths;
  }
  paths.push_back(exeDir);
#if defined(__APPLE__)
  // .../WebLinked.app/Contents/MacOS -> .../Contents/Frameworks
  const auto slash = exeDir.find_last_of('/');
  if (slash != std::string::npos) {
    paths.push_back(exeDir.substr(0, slash) + "/Frameworks");
  }
#endif
  return paths;
}

}  // namespace ferret

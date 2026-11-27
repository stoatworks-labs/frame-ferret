#include "core/subprocess.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <thread>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace ferret {
namespace {

/// Reads exactly `size` bytes through `process.read`. Shared by both
/// platforms, because the bugs it exists to avoid are not platform-specific.
///
/// A real deadline, not a counter decremented by a guess. The first version
/// subtracted 10 per empty read and passed the result straight to poll() —
/// which takes a NEGATIVE timeout to mean "block forever", so any caller asking
/// for less than 10 ms hung the thread permanently on the first quiet moment.
/// That is a deadlock, not a slow path, and it is invisible until the far end
/// goes briefly quiet.
///
/// **The deadline is for the record to START.** Once its first bytes are in,
/// the rest is waited for (up to kFinishRecordMs). The version before this one
/// gave up at the caller's deadline whatever had arrived, and the bytes it had
/// read were simply gone — so a decoder that paused for more than 5 ms halfway
/// through writing a frame (routine on a busy two-core machine) knocked every
/// later record out of step. The next "frame" was the tail of one picture and
/// the head of the next, rolled hundreds of rows; the timeout after it landed
/// in the gap before the following frame and silently realigned. One rolled
/// picture and two lost ones per stall, found on winlab as a counter card that
/// read 255 between 513 and 514. Finishing a record cannot deadlock: a child
/// writing out a frame it has already made needs nothing more from us.
bool readExactlyWithDeadline(Subprocess& process, uint8_t* buffer, size_t size,
                             int timeoutMs) {
  constexpr int kFinishRecordMs = 2000;
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeoutMs);
  bool started = false;
  size_t got = 0;
  while (got < size) {
    const auto now = std::chrono::steady_clock::now();
    if (got > 0 && !started) {
      started = true;
      deadline = std::max(deadline,
                          now + std::chrono::milliseconds(kFinishRecordMs));
    }
    // Past this with a partial record, the child has stalled mid-record for
    // seconds and the stream is out of step whatever happens next.
    if (now >= deadline) return false;
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count();
    // Never negative, and never zero-with-work-left: poll(0) spins.
    const int slice = static_cast<int>(left > 0 ? left : 1);

    const int n = process.read(buffer + got, size - got, slice);
    if (n < 0) return false;
    if (n == 0) continue;
    got += static_cast<size_t>(n);
  }
  return true;
}

void appendQuoted(std::string& out, const std::string& argument) {
  // No quotes needed: pass it through untouched, so the common case reads
  // exactly as typed in a process list.
  if (!argument.empty() &&
      argument.find_first_of(" \t\n\v\"") == std::string::npos) {
    out += argument;
    return;
  }

  out += '"';
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < argument.size() && argument[i] == '\\') {
      ++i;
      ++backslashes;
    }
    if (i == argument.size()) {
      // Backslashes before the closing quote are doubled, or the last one
      // would escape it.
      out.append(backslashes * 2, '\\');
      break;
    }
    if (argument[i] == '"') {
      // Each backslash doubled, then one more to escape the quote itself.
      out.append(backslashes * 2 + 1, '\\');
      out += '"';
    } else {
      // Backslashes not before a quote are literal: C:\Program Files stays.
      out.append(backslashes, '\\');
      out += argument[i];
    }
  }
  out += '"';
}

#if !defined(_WIN32)

class PosixSubprocess final : public Subprocess {
 public:
  PosixSubprocess(pid_t pid, int stdinFd, int stdoutFd, int stderrFd)
      : pid_(pid), stdin_(stdinFd), stdout_(stdoutFd), stderr_(stderrFd) {}

  ~PosixSubprocess() override { stop(); }

  bool write(const uint8_t* data, size_t size) override {
    if (stdin_ < 0) return false;
    size_t written = 0;
    while (written < size) {
      const ssize_t n =
          ::write(stdin_, data + written, size - written);
      if (n > 0) {
        written += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      // EPIPE means the child has gone. Reported as a write failure rather
      // than raised as SIGPIPE, which is ignored for this reason at spawn.
      return false;
    }
    return true;
  }

  int read(uint8_t* buffer, size_t capacity, int timeoutMs) override {
    if (stdout_ < 0) return -1;

    pollfd p{};
    p.fd = stdout_;
    p.events = POLLIN;
    const int ready = ::poll(&p, 1, timeoutMs < 0 ? 0 : timeoutMs);
    if (ready == 0) return 0;
    if (ready < 0) return errno == EINTR ? 0 : -1;

    const ssize_t n = ::read(stdout_, buffer, capacity);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return -1;  // end of stream
    return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
  }

  bool readExactly(uint8_t* buffer, size_t size, int timeoutMs) override {
    return readExactlyWithDeadline(*this, buffer, size, timeoutMs);
  }

  void closeInput() override {
    if (stdin_ >= 0) {
      ::close(stdin_);
      stdin_ = -1;
    }
  }

  bool running() override {
    if (pid_ <= 0) return false;
    return !reap(WNOHANG);
  }

  int waitForExit(int timeoutMs) override {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (pid_ > 0) {
      if (reap(WNOHANG)) break;
      if (std::chrono::steady_clock::now() >= deadline) return -1;
      ::usleep(5000);
    }
    return exitCode_;
  }

  void stop() override {
    closeInput();
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      // Reap so the child does not linger as a zombie. ffmpeg exits promptly
      // once its stdin closes, so the terminate is a backstop.
      for (int i = 0; i < 50 && pid_ > 0; ++i) {
        if (reap(WNOHANG)) break;
        ::usleep(10000);
      }
      if (pid_ > 0) {
        ::kill(pid_, SIGKILL);
        reap(0);
      }
      pid_ = -1;
    }
    if (stdout_ >= 0) {
      ::close(stdout_);
      stdout_ = -1;
    }
    if (stderr_ >= 0) {
      ::close(stderr_);
      stderr_ = -1;
    }
  }

  std::string drainErrors() override {
    if (stderr_ < 0) return {};
    std::string out;
    char buffer[4096];
    for (;;) {
      pollfd p{};
      p.fd = stderr_;
      p.events = POLLIN;
      if (::poll(&p, 1, 0) <= 0) break;
      const ssize_t n = ::read(stderr_, buffer, sizeof(buffer));
      if (n <= 0) break;
      out.append(buffer, static_cast<size_t>(n));
      if (out.size() > 64 * 1024) break;
    }
    return out;
  }

 private:
  /// True once the child has been reaped, recording how it ended.
  bool reap(int flags) {
    int status = 0;
    const pid_t result = ::waitpid(pid_, &status, flags);
    if (result == pid_) {
      exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      pid_ = -1;
      return true;
    }
    if (result < 0 && errno == ECHILD) {
      pid_ = -1;
      return true;
    }
    return false;
  }

  pid_t pid_ = -1;
  int stdin_ = -1;
  int stdout_ = -1;
  int stderr_ = -1;
  int exitCode_ = -1;
};

#else  // _WIN32

std::wstring widen(const std::string& text) {
  if (text.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                      static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), n);
  return out;
}

std::string narrow(const wchar_t* text) {
  if (!text || !*text) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                      nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<size_t>(n - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
  return out;
}

std::string systemMessage(DWORD code) {
  wchar_t* message = nullptr;
  ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&message), 0,
                   nullptr);
  std::string text = narrow(message);
  if (message) ::LocalFree(message);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                           text.back() == ' ' || text.back() == '.')) {
    text.pop_back();
  }
  return text + " (Windows error " + std::to_string(code) + ")";
}

/// A child with its three standard handles on anonymous pipes.
///
/// Anonymous pipes cannot do overlapped I/O, and `read()` needs a timeout, so a
/// thread owns each of the child's output pipes and blocks in ReadFile on it:
///
///  - **stdout** fills a fixed ring that `read()` drains under a condition
///    variable, which is what gives `read()` its timeout. The ring is bounded;
///    when it is full the thread stops reading, the pipe fills, and the child
///    blocks — the same back-pressure a POSIX pipe gives.
///  - **stderr** is read continuously and the newest 64 KB kept. On POSIX
///    stderr is only read when something asks, which is fine with a 64 KB pipe
///    buffer; a Windows pipe can be a few KB, and a decoder printing a warning
///    per packet would stall on a full stderr and look like a dead link.
///
/// The copy through the ring is a memcpy per byte — about 55 MB/s at 720p30
/// UYVY, which costs nothing next to the codec on the other end of the pipe.
class WindowsSubprocess final : public Subprocess {
 public:
  WindowsSubprocess(HANDLE process, HANDLE stdinWrite, HANDLE stdoutRead,
                    HANDLE stderrRead)
      : process_(process),
        stdin_(stdinWrite),
        stdout_(stdoutRead),
        stderr_(stderrRead),
        ring_(kRingBytes),
        scratch_(64 * 1024) {
    outThread_ = std::thread([this] { pumpStdout(); });
    errThread_ = std::thread([this] { pumpStderr(); });
  }

  ~WindowsSubprocess() override { stop(); }

  bool write(const uint8_t* data, size_t size) override {
    if (!stdin_) return false;
    size_t written = 0;
    while (written < size) {
      const DWORD chunk =
          static_cast<DWORD>(std::min<size_t>(size - written, 1u << 20));
      DWORD n = 0;
      // A child that has gone makes this fail with ERROR_NO_DATA or
      // ERROR_BROKEN_PIPE — reported as a write failure, as on POSIX.
      if (!::WriteFile(stdin_, data + written, chunk, &n, nullptr)) {
        return false;
      }
      if (n == 0) return false;
      written += n;
    }
    return true;
  }

  int read(uint8_t* buffer, size_t capacity, int timeoutMs) override {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ready = dataReady_.wait_for(
        lock, std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs),
        [&] { return size_ > 0 || stdoutEnded_; });
    if (!ready) return 0;
    if (size_ == 0) return -1;  // end of stream, and everything delivered

    const size_t cap = ring_.size();
    const size_t n = std::min<size_t>({capacity, size_, 0x7fffffff});
    const size_t first = std::min(n, cap - head_);
    std::memcpy(buffer, ring_.data() + head_, first);
    if (n > first) std::memcpy(buffer + first, ring_.data(), n - first);
    head_ = (head_ + n) % cap;
    size_ -= n;
    spaceFree_.notify_all();
    return static_cast<int>(n);
  }

  bool readExactly(uint8_t* buffer, size_t size, int timeoutMs) override {
    return readExactlyWithDeadline(*this, buffer, size, timeoutMs);
  }

  void closeInput() override {
    if (stdin_) {
      ::CloseHandle(stdin_);
      stdin_ = nullptr;
    }
  }

  bool running() override {
    if (!process_) return false;
    if (::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) return true;
    recordExit();
    return false;
  }

  int waitForExit(int timeoutMs) override {
    if (!process_) return exitCode_;
    if (::WaitForSingleObject(process_, static_cast<DWORD>(
                                            timeoutMs < 0 ? 0 : timeoutMs)) !=
        WAIT_OBJECT_0) {
      return -1;
    }
    recordExit();
    // The child's last stderr is still in flight to the reader thread when
    // the process handle signals. Callers read the verdict and the reason
    // together ("exited 1: Cannot load nvcuda.dll"), so wait for the pipe to
    // break — which, the child being gone, takes only as long as the copy.
    for (int i = 0; i < 100 && !errDone_.load(); ++i) ::Sleep(5);
    return exitCode_;
  }

  void stop() override {
    closeInput();
    if (process_) {
      // From here on stdout is thrown away rather than queued. A child blocked
      // writing to a full stdout pipe would never read the end of its stdin,
      // and so never exit on its own.
      {
        std::lock_guard<std::mutex> lock(mutex_);
        discarding_ = true;
      }
      spaceFree_.notify_all();

      // No SIGTERM on Windows. ffmpeg flushes and exits once its stdin ends;
      // TerminateProcess is the backstop, as SIGKILL is on POSIX.
      if (::WaitForSingleObject(process_, 500) == WAIT_TIMEOUT) {
        ::TerminateProcess(process_, 1);
        ::WaitForSingleObject(process_, 2000);
      }
      recordExit();
      ::CloseHandle(process_);
      process_ = nullptr;
    }

    // The child held the only other ends of these pipes, so with it gone both
    // ReadFile calls return ERROR_BROKEN_PIPE and the threads finish.
    joinReader(outThread_, outDone_);
    joinReader(errThread_, errDone_);
    if (stdout_) {
      ::CloseHandle(stdout_);
      stdout_ = nullptr;
    }
    if (stderr_) {
      ::CloseHandle(stderr_);
      stderr_ = nullptr;
    }
  }

  std::string drainErrors() override {
    std::lock_guard<std::mutex> lock(errorsMutex_);
    std::string out;
    out.swap(errors_);
    return out;
  }

 private:
  static constexpr size_t kRingBytes = 4u << 20;
  static constexpr size_t kErrorBytes = 64u << 10;

  void recordExit() {
    DWORD code = 0;
    if (process_ && ::GetExitCodeProcess(process_, &code) &&
        code != STILL_ACTIVE) {
      exitCode_ = static_cast<int>(code);
    }
  }

  void pumpStdout() {
    for (;;) {
      uint8_t* target = nullptr;
      size_t span = 0;
      bool intoRing = false;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        spaceFree_.wait(lock,
                        [&] { return discarding_ || size_ < ring_.size(); });
        if (discarding_) {
          target = scratch_.data();
          span = scratch_.size();
        } else {
          // Only the free region is written, outside the lock: `read()` only
          // ever touches the filled region, and the free region can only grow
          // while this thread is in ReadFile.
          const size_t cap = ring_.size();
          const size_t tail = (head_ + size_) % cap;
          span = std::min(cap - size_, cap - tail);
          target = ring_.data() + tail;
          intoRing = true;
        }
      }

      DWORD got = 0;
      const DWORD ask = static_cast<DWORD>(std::min<size_t>(span, 1u << 20));
      if (!::ReadFile(stdout_, target, ask, &got, nullptr)) break;
      if (got == 0) continue;

      if (intoRing) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          size_ += got;
        }
        dataReady_.notify_all();
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stdoutEnded_ = true;
    }
    dataReady_.notify_all();
    outDone_.store(true);
  }

  void pumpStderr() {
    char buffer[4096];
    for (;;) {
      DWORD got = 0;
      if (!::ReadFile(stderr_, buffer, sizeof(buffer), &got, nullptr)) break;
      if (got == 0) continue;
      std::lock_guard<std::mutex> lock(errorsMutex_);
      errors_.append(buffer, got);
      if (errors_.size() > kErrorBytes) {
        errors_.erase(0, errors_.size() - kErrorBytes);
      }
    }
    errDone_.store(true);
  }

  /// Joins a reader thread, cancelling its ReadFile if it has not finished on
  /// its own within a second. That only happens if something other than the
  /// child still holds the pipe's write end — a grandchild that inherited it,
  /// say — and a stuck join would hang node shutdown for good.
  static void joinReader(std::thread& thread, const std::atomic<bool>& done) {
    if (!thread.joinable()) return;
    for (int i = 0; i < 100 && !done.load(); ++i) ::Sleep(10);
    for (int i = 0; i < 200 && !done.load(); ++i) {
      ::CancelSynchronousIo(static_cast<HANDLE>(thread.native_handle()));
      ::Sleep(10);
    }
    thread.join();
  }

  HANDLE process_ = nullptr;
  HANDLE stdin_ = nullptr;
  HANDLE stdout_ = nullptr;
  HANDLE stderr_ = nullptr;
  int exitCode_ = -1;

  std::mutex mutex_;
  std::condition_variable dataReady_;
  std::condition_variable spaceFree_;
  std::vector<uint8_t> ring_;
  size_t head_ = 0;
  size_t size_ = 0;
  bool stdoutEnded_ = false;
  bool discarding_ = false;
  std::vector<uint8_t> scratch_;  // where discarded output lands; reader only

  std::mutex errorsMutex_;
  std::string errors_;

  std::thread outThread_;
  std::thread errThread_;
  std::atomic<bool> outDone_{false};
  std::atomic<bool> errDone_{false};
};

#endif  // _WIN32

}  // namespace

std::string windowsCommandLine(const std::vector<std::string>& arguments) {
  std::string out;
  for (size_t i = 0; i < arguments.size(); ++i) {
    if (i) out += ' ';
    appendQuoted(out, arguments[i]);
  }
  return out;
}

std::unique_ptr<Subprocess> spawnSubprocess(
    const std::string& executable, const std::vector<std::string>& arguments,
    std::string& error) {
#if defined(_WIN32)
  HANDLE inRead = nullptr, inWrite = nullptr;
  HANDLE outRead = nullptr, outWrite = nullptr;
  HANDLE errRead = nullptr, errWrite = nullptr;

  auto closeAll = [&] {
    for (HANDLE h : {inRead, inWrite, outRead, outWrite, errRead, errWrite}) {
      if (h) ::CloseHandle(h);
    }
  };

  // Created NOT inheritable; only the child's three ends are then marked
  // inheritable, and the child is handed exactly those through
  // PROC_THREAD_ATTRIBUTE_HANDLE_LIST. Without the list, bInheritHandles hands
  // over every inheritable handle in the process — so the SRT source and sink
  // starting together would each give their ffmpeg a copy of the other's pipe,
  // and that pipe would never report end of stream after its own child died.
  // The lock closes the remaining window between marking and spawning.
  static std::mutex spawnLock;
  std::lock_guard<std::mutex> guard(spawnLock);

  if (!::CreatePipe(&inRead, &inWrite, nullptr, 1u << 20) ||
      !::CreatePipe(&outRead, &outWrite, nullptr, 1u << 20) ||
      !::CreatePipe(&errRead, &errWrite, nullptr, 64u << 10)) {
    error = "could not create a pipe: " + systemMessage(::GetLastError());
    closeAll();
    return nullptr;
  }
  for (HANDLE childEnd : {inRead, outWrite, errWrite}) {
    ::SetHandleInformation(childEnd, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  }

  SIZE_T attributeBytes = 0;
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
  std::vector<uint8_t> attributeStorage(attributeBytes);
  auto* attributes =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
  HANDLE inherit[3] = {inRead, outWrite, errWrite};
  if (!::InitializeProcThreadAttributeList(attributes, 1, 0,
                                           &attributeBytes) ||
      !::UpdateProcThreadAttribute(attributes, 0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                   sizeof(inherit), nullptr, nullptr)) {
    error = "could not set up the child's handles: " +
            systemMessage(::GetLastError());
    closeAll();
    return nullptr;
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = inRead;
  startup.StartupInfo.hStdOutput = outWrite;
  startup.StartupInfo.hStdError = errWrite;
  startup.lpAttributeList = attributes;

  std::vector<std::string> argv;
  argv.reserve(arguments.size() + 1);
  argv.push_back(executable);
  argv.insert(argv.end(), arguments.begin(), arguments.end());
  std::wstring commandLine = widen(windowsCommandLine(argv));

  // A path is run exactly as given, with no search. A bare name ("cmd.exe")
  // is left for CreateProcess to find, which needs a null application name.
  const bool isPath =
      executable.find_first_of("\\/:") != std::string::npos;
  const std::wstring application = widen(executable);

  PROCESS_INFORMATION info{};
  // CREATE_NO_WINDOW: ffmpeg is a console program, and a tray-launched Frame
  // Ferret would otherwise flash a console window per codec. It also keeps a
  // Ctrl+C in Frame Ferret's own console from reaching the child, so the
  // parent stays the one that decides when the codec ends.
  const BOOL created = ::CreateProcessW(
      isPath ? application.c_str() : nullptr, commandLine.data(), nullptr,
      nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
      nullptr, &startup.StartupInfo, &info);
  const DWORD createError = ::GetLastError();
  ::DeleteProcThreadAttributeList(attributes);

  if (!created) {
    error = "could not run \"" + executable + "\": " +
            systemMessage(createError);
    closeAll();
    return nullptr;
  }
  ::CloseHandle(info.hThread);

  // Close the child's ends here, or reading stdout never sees end of stream.
  ::CloseHandle(inRead);
  ::CloseHandle(outWrite);
  ::CloseHandle(errWrite);

  return std::make_unique<WindowsSubprocess>(info.hProcess, inWrite, outRead,
                                             errRead);
#else
  int inPipe[2] = {-1, -1};
  int outPipe[2] = {-1, -1};
  int errPipe[2] = {-1, -1};

  auto closeAll = [&] {
    for (int* p : {inPipe, outPipe, errPipe}) {
      if (p[0] >= 0) ::close(p[0]);
      if (p[1] >= 0) ::close(p[1]);
    }
  };

  // No pipe end may leak into a child spawned by another node at the same
  // moment: an inherited copy of this child's stdout write end would keep the
  // pipe open after this child exits, and end of stream would never arrive.
  // So every end is close-on-exec (dup2 onto 0/1/2 clears it for the child's
  // own three), and the lock covers the gap between pipe() and fcntl(), since
  // macOS has no pipe2.
  static std::mutex spawnLock;
  std::lock_guard<std::mutex> guard(spawnLock);

  if (::pipe(inPipe) != 0 || ::pipe(outPipe) != 0 || ::pipe(errPipe) != 0) {
    error = std::string("pipe: ") + std::strerror(errno);
    closeAll();
    return nullptr;
  }
  for (int* p : {inPipe, outPipe, errPipe}) {
    for (int fd : {p[0], p[1]}) {
      // A pipe that landed on 0, 1 or 2 (a parent started with them closed)
      // would be dup2'ed onto itself, which does not clear the flag.
      if (fd > STDERR_FILENO) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
  }

  // A dead child must make write() fail rather than kill this process. Set
  // once, globally, because posix_spawn cannot set it per child.
  static std::once_flag ignorePipe;
  std::call_once(ignorePipe, [] { ::signal(SIGPIPE, SIG_IGN); });

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, inPipe[1]);
  posix_spawn_file_actions_addclose(&actions, outPipe[0]);
  posix_spawn_file_actions_addclose(&actions, errPipe[0]);

  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(executable.c_str()));
  for (const auto& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);

  pid_t pid = -1;
  const int rc =
      ::posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(),
                    environ);
  posix_spawn_file_actions_destroy(&actions);

  if (rc != 0) {
    error = "could not run \"" + executable + "\": " + std::strerror(rc);
    closeAll();
    return nullptr;
  }

  // Close the child's ends here, or reading stdout never sees end of stream.
  ::close(inPipe[0]);
  ::close(outPipe[1]);
  ::close(errPipe[1]);

  // Non-blocking stderr so drainErrors never stalls the frame loop.
  ::fcntl(errPipe[0], F_SETFL, O_NONBLOCK);

  return std::make_unique<PosixSubprocess>(pid, inPipe[1], outPipe[0],
                                           errPipe[0]);
#endif
}

}  // namespace ferret

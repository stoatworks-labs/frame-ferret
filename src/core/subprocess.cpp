#include "core/subprocess.h"

#include <chrono>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
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
    const int ready = ::poll(&p, 1, timeoutMs);
    if (ready == 0) return 0;
    if (ready < 0) return errno == EINTR ? 0 : -1;

    const ssize_t n = ::read(stdout_, buffer, capacity);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return -1;  // end of stream
    return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
  }

  bool readExactly(uint8_t* buffer, size_t size, int timeoutMs) override {
    // A real deadline for the whole record, not a counter decremented by a
    // guess. The first version subtracted 10 per empty read and passed the
    // result straight to poll() — which takes a NEGATIVE timeout to mean
    // "block forever", so any caller asking for less than 10 ms hung the
    // thread permanently on the first quiet moment. That is a deadlock, not a
    // slow path, and it is invisible until the far end goes briefly quiet.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    size_t got = 0;
    while (got < size) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return false;
      const auto left =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
              .count();
      // Never negative, and never zero-with-work-left: poll(0) spins.
      const int slice = static_cast<int>(left > 0 ? left : 1);

      const int n = read(buffer + got, size - got, slice);
      if (n < 0) return false;
      if (n == 0) continue;
      got += static_cast<size_t>(n);
    }
    return true;
  }

  void closeInput() override {
    if (stdin_ >= 0) {
      ::close(stdin_);
      stdin_ = -1;
    }
  }

  bool running() override {
    if (pid_ <= 0) return false;
    int status = 0;
    const pid_t result = ::waitpid(pid_, &status, WNOHANG);
    if (result == pid_) {
      pid_ = -1;
      return false;
    }
    return result == 0;
  }

  void stop() override {
    closeInput();
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      // Reap so the child does not linger as a zombie. ffmpeg exits promptly
      // once its stdin closes, so the terminate is a backstop.
      int status = 0;
      for (int i = 0; i < 50; ++i) {
        if (::waitpid(pid_, &status, WNOHANG) == pid_) break;
        ::usleep(10000);
      }
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, &status, WNOHANG);
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
  pid_t pid_ = -1;
  int stdin_ = -1;
  int stdout_ = -1;
  int stderr_ = -1;
};

#endif  // !_WIN32

}  // namespace

std::unique_ptr<Subprocess> spawnSubprocess(
    const std::string& executable, const std::vector<std::string>& arguments,
    std::string& error) {
#if defined(_WIN32)
  // Not implemented. Windows needs CreateProcess with overlapped pipes, which
  // is a different shape entirely, and no Windows machine here can test it —
  // saying so is better than shipping an untested guess.
  (void)executable;
  (void)arguments;
  error =
      "running an external codec is not implemented on Windows yet; the POSIX "
      "path uses posix_spawn and Windows needs CreateProcess with overlapped "
      "pipes";
  return nullptr;
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

  if (::pipe(inPipe) != 0 || ::pipe(outPipe) != 0 || ::pipe(errPipe) != 0) {
    error = std::string("pipe: ") + std::strerror(errno);
    closeAll();
    return nullptr;
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

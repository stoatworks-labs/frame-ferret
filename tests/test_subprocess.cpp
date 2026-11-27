// The subprocess layer, driven against a real child: this test binary itself,
// started again in a child mode. That keeps the test independent of whatever
// tools the machine has (CI runners have no ffmpeg) while still exercising the
// real posix_spawn or CreateProcess path, pipes and all.

#include "core/subprocess.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "check.h"

using namespace ferret;

namespace {

std::string g_self;

void binaryStdio() {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
}

/// The child side. Returns the exit code.
int child(int argc, char** argv) {
  binaryStdio();
  const std::string mode = argc > 2 ? argv[2] : "";

  if (mode == "echo") {
    std::vector<char> buffer(65536);
    for (;;) {
      const size_t n = std::fread(buffer.data(), 1, buffer.size(), stdin);
      if (n == 0) break;
      std::fwrite(buffer.data(), 1, n, stdout);
    }
    std::fflush(stdout);
    return 0;
  }
  if (mode == "exit") return argc > 3 ? std::atoi(argv[3]) : 0;
  if (mode == "stderr") {
    std::fputs("hello on stderr\n", stderr);
    return 0;
  }
  if (mode == "args") {
    // One argument per line, bracketed so an empty one is visible.
    for (int i = 3; i < argc; ++i) std::printf("[%s]\n", argv[i]);
    return 0;
  }
  if (mode == "halves") {
    // One record in two halves with a pause between, like a decoder that is
    // descheduled partway through writing a frame. Bytes count up from 0.
    const size_t half = argc > 3 ? static_cast<size_t>(std::atol(argv[3])) : 0;
    std::vector<unsigned char> bytes(half * 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<unsigned char>(i * 7);
    }
    std::fwrite(bytes.data(), 1, half, stdout);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::fwrite(bytes.data() + half, 1, half, stdout);
    std::fflush(stdout);
    return 0;
  }
  if (mode == "sleep") {
    std::this_thread::sleep_for(std::chrono::seconds(30));
    return 0;
  }
  return 99;
}

std::unique_ptr<Subprocess> spawnChild(const std::vector<std::string>& args) {
  std::vector<std::string> all = {"--child"};
  all.insert(all.end(), args.begin(), args.end());
  std::string error;
  auto process = spawnSubprocess(g_self, all, error);
  if (!process) std::printf("  spawn failed: %s\n", error.c_str());
  return process;
}

std::string readAll(Subprocess& process, int timeoutMs) {
  std::string out;
  uint8_t buffer[4096];
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    const int n = process.read(buffer, sizeof(buffer), 50);
    if (n < 0) break;
    out.append(reinterpret_cast<char*>(buffer), static_cast<size_t>(n));
  }
  return out;
}

/// The quoting rules MSVC's runtime undoes. Every case here is one that a
/// naive "wrap it in quotes" gets wrong.
void commandLinesQuoteLikeTheCRuntimeExpects() {
  CHECK_EQ(windowsCommandLine({"ffmpeg", "-i", "pipe:0"}),
           std::string("ffmpeg -i pipe:0"));
  CHECK_EQ(windowsCommandLine({"C:\\Program Files\\ffmpeg\\bin\\ffmpeg.exe"}),
           std::string("\"C:\\Program Files\\ffmpeg\\bin\\ffmpeg.exe\""));
  // An empty argument must survive as one, not vanish.
  CHECK_EQ(windowsCommandLine({"a", "", "b"}), std::string("a \"\" b"));
  // A quote inside is backslash-escaped.
  CHECK_EQ(windowsCommandLine({"say \"hi\""}),
           std::string("\"say \\\"hi\\\"\""));
  // Backslashes before the closing quote are doubled, or the last one would
  // escape it and swallow the rest of the line.
  CHECK_EQ(windowsCommandLine({"dir with space\\"}),
           std::string("\"dir with space\\\\\""));
  // Backslashes anywhere else are literal, quoted or not.
  CHECK_EQ(windowsCommandLine({"\\\\server\\share"}),
           std::string("\\\\server\\share"));
  // Backslashes before an inner quote: each doubled, plus one for the quote.
  CHECK_EQ(windowsCommandLine({"a\\\"b"}), std::string("\"a\\\\\\\"b\""));
  // A filter graph with no spaces passes through untouched.
  CHECK_EQ(windowsCommandLine({"scale=1280:720"}),
           std::string("scale=1280:720"));
}

void argumentsArriveExactlyAsGiven() {
  const std::vector<std::string> tricky = {
      "plain",          "with space",        "",
      "quote\"inside",  "trailing\\",        "trailing space\\ ",
      "space then\\",   "\\\\server\\share", "a\\\\\"b",
      "C:\\Program Files\\x", "tab\there",   "scale=640:360"};
  std::vector<std::string> args = {"args"};
  args.insert(args.end(), tricky.begin(), tricky.end());
  auto process = spawnChild(args);
  CHECK(process != nullptr);
  if (!process) return;
  process->closeInput();

  std::string expected;
  for (const auto& a : tricky) expected += "[" + a + "]\n";
  const std::string got = readAll(*process, 5000);
  CHECK_EQ(got, expected);
  if (got != expected) std::printf("  got:\n%s\n", got.c_str());
  CHECK_EQ(process->waitForExit(5000), 0);
}

/// Several MB both ways at once, which only works if neither side's pipe
/// filling up can wedge the other — the shape of a codec on a frame loop.
void bytesRoundTripThroughAChild() {
  auto process = spawnChild({"echo"});
  CHECK(process != nullptr);
  if (!process) return;

  const size_t size = 6u << 20;  // bigger than the Windows ring and any pipe
  std::vector<uint8_t> sent(size);
  for (size_t i = 0; i < size; ++i) {
    sent[i] = static_cast<uint8_t>((i * 2654435761u) >> 13);
  }

  bool wroteAll = false;
  std::thread writer([&] {
    // In pieces of a 720p UYVY frame, as the SRT sender writes.
    const size_t piece = 1280 * 720 * 2;
    bool ok = true;
    for (size_t off = 0; off < size && ok; off += piece) {
      ok = process->write(sent.data() + off, std::min(piece, size - off));
    }
    wroteAll = ok;
    process->closeInput();
  });

  std::vector<uint8_t> received(size);
  const bool readAllOfIt =
      process->readExactly(received.data(), received.size(), 20000);
  writer.join();

  CHECK(wroteAll);
  CHECK(readAllOfIt);
  CHECK(received == sent);

  // Then end of stream, and a clean exit.
  uint8_t extra[16];
  int n = 0;
  for (int i = 0; i < 100; ++i) {
    n = process->read(extra, sizeof(extra), 50);
    if (n != 0) break;
  }
  CHECK_EQ(n, -1);
  CHECK_EQ(process->waitForExit(5000), 0);
  CHECK(!process->running());
}

/// A record that has started is finished, however short the deadline. The
/// version before gave up at the deadline and lost what it had read, which put
/// every later frame out of step — seen on winlab as a rolled picture whenever
/// the decoder paused mid-frame.
void aRecordThatHasStartedIsFinished() {
  const size_t half = 300000;
  auto process = spawnChild({"halves", std::to_string(half)});
  CHECK(process != nullptr);
  if (!process) return;
  process->closeInput();

  std::vector<uint8_t> record(half * 2);
  // 20 ms to begin; the second half comes 200 ms after the first.
  const bool whole = process->readExactly(record.data(), record.size(), 20);
  CHECK(whole);
  bool inOrder = whole;
  for (size_t i = 0; i < record.size() && inOrder; ++i) {
    inOrder = record[i] == static_cast<uint8_t>(i * 7);
  }
  CHECK(inOrder);

  // And with nothing more coming, the deadline still holds: the child has
  // exited, so this is end of stream, not a wait.
  const auto start = std::chrono::steady_clock::now();
  CHECK(!process->readExactly(record.data(), record.size(), 20));
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
  CHECK_EQ(process->waitForExit(5000), 0);
}

/// A record that never starts still times out on the caller's deadline.
void aRecordThatNeverStartsTimesOut() {
  auto process = spawnChild({"sleep"});
  CHECK(process != nullptr);
  if (!process) return;
  uint8_t record[64];
  const auto start = std::chrono::steady_clock::now();
  CHECK(!process->readExactly(record, sizeof(record), 50));
  const auto took = std::chrono::steady_clock::now() - start;
  CHECK(took >= std::chrono::milliseconds(40));
  CHECK(took < std::chrono::seconds(1));
  process->stop();
}

void exitCodesAreReported() {
  auto process = spawnChild({"exit", "3"});
  CHECK(process != nullptr);
  if (!process) return;
  process->closeInput();
  CHECK_EQ(process->waitForExit(5000), 3);
  CHECK(!process->running());
  // Asking again gives the same answer rather than "still running".
  CHECK_EQ(process->waitForExit(0), 3);
}

/// The reason has to be there together with the verdict: the encoder trial
/// reads both straight after the child exits.
void stderrIsCapturedByTheTimeTheChildHasExited() {
  auto process = spawnChild({"stderr"});
  CHECK(process != nullptr);
  if (!process) return;
  process->closeInput();
  CHECK_EQ(process->waitForExit(5000), 0);
  const std::string errors = process->drainErrors();
  CHECK(errors.find("hello on stderr") != std::string::npos);
}

void waitingTimesOutOnALiveChildAndStopEndsIt() {
  auto process = spawnChild({"sleep"});
  CHECK(process != nullptr);
  if (!process) return;
  CHECK(process->running());
  CHECK_EQ(process->waitForExit(100), -1);

  const auto start = std::chrono::steady_clock::now();
  process->stop();
  const auto took = std::chrono::steady_clock::now() - start;
  CHECK(!process->running());
  CHECK(took < std::chrono::seconds(5));
  process->stop();  // twice is fine
}

/// A child started later must not inherit the pipe ends of one started
/// earlier. If it does, it holds the first child's stdin open, so closing
/// stdin never reaches the first child, which then never finishes — exactly
/// how an encoder trial would time out while an SRT decoder was running.
void aLaterChildDoesNotHoldAnEarlierChildsPipes() {
  auto first = spawnChild({"echo"});
  auto second = spawnChild({"sleep"});
  CHECK(first != nullptr);
  CHECK(second != nullptr);
  if (!first || !second) return;

  CHECK(first->write(reinterpret_cast<const uint8_t*>("abc"), 3));
  first->closeInput();
  const std::string got = readAll(*first, 5000);
  CHECK_EQ(got, std::string("abc"));
  CHECK_EQ(first->waitForExit(5000), 0);
  second->stop();
}

void aMissingExecutableIsAnErrorNotACrash() {
  std::string error;
#if defined(_WIN32)
  const std::string missing = "C:\\no\\such\\ffmpeg.exe";
#else
  const std::string missing = "/no/such/ffmpeg";
#endif
  auto process = spawnSubprocess(missing, {"-version"}, error);
  // posix_spawn may report a missing file either way: as a spawn error, or as
  // a child that exits 127 at once. Both must be an error, not a hang.
  if (process) {
    process->closeInput();
    CHECK(process->waitForExit(5000) != 0);
  } else {
    CHECK(error.find(missing) != std::string::npos);
  }
}

void run() {
  commandLinesQuoteLikeTheCRuntimeExpects();
  argumentsArriveExactlyAsGiven();
  bytesRoundTripThroughAChild();
  aRecordThatHasStartedIsFinished();
  aRecordThatNeverStartsTimesOut();
  exitCodesAreReported();
  stderrIsCapturedByTheTimeTheChildHasExited();
  waitingTimesOutOnALiveChildAndStopEndsIt();
  aLaterChildDoesNotHoldAnEarlierChildsPipes();
  aMissingExecutableIsAnErrorNotACrash();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--child") == 0) {
    return child(argc, argv);
  }
  // CMake passes the full path, because argv[0] is not reliably one.
  g_self = argc > 1 ? argv[1] : argv[0];

  std::printf("subprocess\n");
  run();
  std::printf("subprocess: %d checks, %d failed\n", ferret_test::g_checks,
              ferret_test::g_failures);
  return ferret_test::g_failures == 0 ? 0 : 1;
}

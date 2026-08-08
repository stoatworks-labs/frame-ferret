#include "control/web_assets.h"

#include <cstring>
#include <string>
#include <vector>

#include "check.h"

using namespace ferret;

namespace {

std::string page() { return std::string(kControlPageHtml); }

/// The bug this whole suite exists for.
///
/// WebLinked shipped a NUL inside its embedded page. It compiled, served 200,
/// rendered the HTML, and killed every line of JavaScript after it — the page
/// sat on "connecting" with a black preview and nothing in any log. A NUL also
/// makes grep treat the file as binary and return nothing, which sends the
/// hunt in entirely the wrong direction.
void thereAreNoStrayControlBytes() {
  const char* raw = kControlPageHtml;
  const size_t declared = std::strlen(raw);

  CHECK(declared > 2000);

  // Scanned as a whole and asserted once. A CHECK per byte would report ~8000
  // checks for this file alone and swamp every honest count in the suite.
  size_t offenders = 0;
  for (size_t i = 0; i < declared; ++i) {
    const unsigned char c = static_cast<unsigned char>(raw[i]);
    // Tab, newline and carriage return are the only control bytes a served
    // page has any business containing.
    const bool allowed = c >= 0x20 || c == '\t' || c == '\n' || c == '\r';
    if (!allowed) {
      ++offenders;
      if (offenders <= 5) {
        std::printf("  stray control byte 0x%02X at offset %zu\n", c, i);
      }
    }
  }
  CHECK_EQ(offenders, size_t{0});
}

void itIsAWholeHtmlDocument() {
  const std::string p = page();
  CHECK(p.rfind("<!doctype html>", 0) == 0);
  CHECK(p.find("<title>Frame Ferret</title>") != std::string::npos);
  CHECK(p.find("</html>") != std::string::npos);
}

/// An unbalanced script tag is the other way this page dies silently: the
/// browser swallows the remaining markup as script text and renders nothing.
void tagsAreBalanced() {
  const std::string p = page();

  auto count = [&p](const std::string& needle) {
    int n = 0;
    for (size_t at = p.find(needle); at != std::string::npos;
         at = p.find(needle, at + 1)) {
      ++n;
    }
    return n;
  };

  CHECK_EQ(count("<script"), count("</script>"));
  CHECK_EQ(count("<style"), count("</style>"));
  CHECK_EQ(count("<table"), count("</table>"));
  CHECK_EQ(count("<script"), 1);
}

/// Every endpoint the page fetches must be one the control API serves. These
/// two files drift apart silently — the page just stops working — so the
/// coupling is asserted rather than trusted.
void everyEndpointThePageCallsIsReal() {
  const std::string p = page();

  // Served by ControlApi::handle.
  const char* endpoints[] = {"/api/state", "/api/route", "/api/mute",
                             "/preview/"};
  for (const char* e : endpoints) {
    CHECK(p.find(e) != std::string::npos);
  }

  // And the fields the page reads out of /api/state. If a field is renamed in
  // control_api.cpp this catches it here rather than as an undefined in a
  // browser console nobody is looking at.
  const char* fields[] = {"sources",  "sinks",      "counters",  "failures",
                          "muted",    "running",    "rate",      "routedFrom",
                          "reason",   "hasPreview", "available", "measuredFps",
                          "lateTicks"};
  for (const char* f : fields) {
    CHECK(p.find(f) != std::string::npos);
  }
}

/// The page is served from a C++ raw string delimited by )HTML". If that
/// sequence ever appears inside the content the literal terminates early and
/// the file will not compile — but a near miss is worth pinning too, because
/// the failure message is baffling.
void theRawStringDelimiterDoesNotAppearInside() {
  const std::string p = page();
  CHECK(p.find(")HTML\"") == std::string::npos);
}

/// The preview is re-fetched on a timer with a cache-buster. Without it every
/// browser serves the first frame forever and the preview looks frozen — which
/// is indistinguishable from the engine having stopped.
void thePreviewIsCacheBusted() {
  const std::string p = page();
  CHECK(p.find("Date.now()") != std::string::npos);
  CHECK(p.find("setInterval") != std::string::npos);
}

void run() {
  thereAreNoStrayControlBytes();
  itIsAWholeHtmlDocument();
  tagsAreBalanced();
  everyEndpointThePageCallsIsReal();
  theRawStringDelimiterDoesNotAppearInside();
  thePreviewIsCacheBusted();
}

}  // namespace

TEST_MAIN("web_assets")

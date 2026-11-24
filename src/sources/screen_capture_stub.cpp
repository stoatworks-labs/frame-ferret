// Screen capture is implemented on macOS (screen_capture.mm, ScreenCaptureKit)
// and Windows (screen_capture_win.cpp, Desktop Duplication). This supplies the
// same symbols elsewhere so the factory and control API need no #ifdefs, and
// an operator on another platform gets a sentence rather than a missing node
// kind.
#include "sources/screen_capture.h"

#if !defined(__APPLE__) && !defined(_WIN32)

namespace ferret {

bool ScreenCapture::supported() { return false; }

std::string ScreenCapture::unsupportedReason() {
  return "screen capture is implemented on macOS and Windows only so far — "
         "Linux needs PipeWire, which is not written";
}

bool ScreenCapture::permitted() { return false; }
std::string ScreenCapture::permissionReason() { return unsupportedReason(); }
std::vector<ScreenCapture::Display> ScreenCapture::listDisplays() { return {}; }
std::vector<ScreenCapture::Window> ScreenCapture::listWindows() { return {}; }
std::vector<std::string> ScreenCapture::listApplications() { return {}; }

std::unique_ptr<Source> makeDisplaySource(const NodeConfig&,
                                          std::string& error) {
  error = ScreenCapture::unsupportedReason();
  return nullptr;
}
std::unique_ptr<Source> makeDisplaySourceForId(const NodeConfig&, uint32_t,
                                               int, bool, std::string& error) {
  error = ScreenCapture::unsupportedReason();
  return nullptr;
}
std::unique_ptr<Source> makeWindowSource(const NodeConfig&,
                                         std::string& error) {
  error = ScreenCapture::unsupportedReason();
  return nullptr;
}
std::unique_ptr<Source> makeApplicationSource(const NodeConfig&,
                                              std::string& error) {
  error = ScreenCapture::unsupportedReason();
  return nullptr;
}

}  // namespace ferret

#endif

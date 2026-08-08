// Syphon is macOS-only. This keeps every entry point present on other
// platforms so the factory and the control API need no #ifdefs, and an
// operator on Windows or Linux gets a sentence rather than a missing node.
#include "sinks/syphon.h"

#if !defined(__APPLE__)

namespace ferret {

bool SyphonRuntime::supported() { return false; }

std::string SyphonRuntime::unsupportedReason() {
  return "Syphon is macOS only — it shares frames as an IOSurface, which has "
         "no equivalent elsewhere. The Windows counterpart is Spout, which is "
         "not implemented yet";
}

std::unique_ptr<Sink> makeSyphonSink(const NodeConfig&, std::string& error) {
  error = SyphonRuntime::unsupportedReason();
  return nullptr;
}

}  // namespace ferret

#endif

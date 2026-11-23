// The virtual display is macOS-only. The real implementation is in
// virtual_display.mm, compiled only on Apple; this supplies the symbol
// elsewhere so the factory needs no #ifdef and an operator gets a sentence.
#include "sources/virtual_display.h"

#if !defined(__APPLE__)

namespace ferret {

std::unique_ptr<Source> makeVirtualDisplaySource(const NodeConfig&,
                                                 std::string& error) {
  error =
      "the virtual display is implemented on macOS only so far — Windows "
      "needs an IddCx display driver, which needs attestation signing; see "
      "docs/03-os-extensions.md";
  return nullptr;
}

}  // namespace ferret

#endif

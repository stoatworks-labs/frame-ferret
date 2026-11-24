// The virtual display is implemented on macOS (virtual_display.mm) and
// Windows (virtual_display_win.cpp); this supplies the symbol elsewhere so the
// factory needs no #ifdef and an operator gets a sentence.
#include "sources/virtual_display.h"

#if !defined(__APPLE__) && !defined(_WIN32)

namespace ferret {

std::unique_ptr<Source> makeVirtualDisplaySource(const NodeConfig&,
                                                 std::string& error) {
  error =
      "the virtual display is implemented on macOS and Windows only so far; "
      "see docs/03-os-extensions.md";
  return nullptr;
}

}  // namespace ferret

#endif

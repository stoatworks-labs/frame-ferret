#pragma once

// Desktop Duplication: the capture behind display capture and the virtual
// display on Windows. Windows only.

#include <functional>
#include <memory>
#include <string>

#include "app/node.h"

namespace ferret::win {

/// Captures one display with DXGI Desktop Duplication.
///
/// `resolve` returns the display's current GDI name ("\\.\DISPLAY2"), or an
/// empty string while it has none, saying why in `*why` if it can (that
/// sentence is what the log shows). It is asked again whenever the
/// duplication is lost, because a display can be renumbered while it is being
/// captured: VDD's reload does exactly that, and a source holding the old name
/// would go black for good.
///
/// Frames are the display's own raster (or the node's crop of it), BGRA with
/// alpha forced opaque. The pointer is not in them: Desktop Duplication hands
/// it over separately and this does not draw it.
///
/// Fails, with a sentence, when the first duplication cannot be opened.
std::unique_ptr<Source> makeDuplicationSource(
    const NodeConfig& config,
    std::function<std::wstring(std::string* why)> resolve, std::string& error);

}  // namespace ferret::win

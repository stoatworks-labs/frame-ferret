#include "core/frame.h"

#include <cstring>

namespace ferret {

void FrameBuffer::assign(const VideoFrame& src) {
  frame_ = src;
  frame_.data = nullptr;

  if (!src.data || src.height <= 0 || src.strideBytes <= 0) {
    storage_.clear();
    return;
  }

  const size_t bytes = static_cast<size_t>(src.strideBytes) *
                       static_cast<size_t>(src.height);
  if (storage_.size() != bytes) storage_.resize(bytes);

  // Copied whole rather than row by row: every producer in the fleet hands
  // over a contiguous buffer, and the row loop measured no faster while making
  // a padded-stride bug easy to write.
  std::memcpy(storage_.data(), src.data, bytes);
  frame_.data = storage_.data();
}

}  // namespace ferret

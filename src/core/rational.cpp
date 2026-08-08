#include "core/rational.h"

#include <cstdio>
#include <cstdlib>

namespace ferret {
namespace {

struct NamedRate {
  const char* spelling;
  Rate rate;
};

// The decimal spellings the industry uses, and what they actually mean. A
// literal decimal parse of "59.94" gives 59.94 exactly, which is *not* the rate
// any equipment runs at, and the difference accumulates to a frame every ~28
// minutes against a real 60000/1001 source.
const NamedRate kNamed[] = {
    {"23.98", {24000, 1001}}, {"23.976", {24000, 1001}},
    {"29.97", {30000, 1001}}, {"47.95", {48000, 1001}},
    {"59.94", {60000, 1001}}, {"119.88", {120000, 1001}},
};

int64_t gcd(int64_t a, int64_t b) {
  while (b) {
    int64_t t = a % b;
    a = b;
    b = t;
  }
  return a < 0 ? -a : a;
}

}  // namespace

std::string Rate::label() const {
  if (!valid()) return "invalid";
  if (den == 1) return std::to_string(num);
  for (const auto& n : kNamed) {
    if (*this == n.rate) return n.spelling;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%lld/%lld", static_cast<long long>(num),
                static_cast<long long>(den));
  return buf;
}

bool parseRate(const std::string& s, Rate* out) {
  if (s.empty() || !out) return false;

  for (const auto& n : kNamed) {
    if (s == n.spelling) {
      *out = n.rate;
      return true;
    }
  }

  auto slash = s.find('/');
  if (slash != std::string::npos) {
    char* end = nullptr;
    long long n = std::strtoll(s.c_str(), &end, 10);
    if (end != s.c_str() + slash) return false;
    long long d = std::strtoll(s.c_str() + slash + 1, &end, 10);
    if (*end != '\0' || n <= 0 || d <= 0) return false;
    int64_t g = gcd(n, d);
    *out = Rate{n / g, d / g};
    return true;
  }

  // A bare integer. Anything else — including an unrecognised decimal — is
  // rejected rather than approximated, so a typo'd rate fails loudly at config
  // load instead of quietly running the whole show a fraction of a percent off.
  char* end = nullptr;
  long long n = std::strtoll(s.c_str(), &end, 10);
  if (*end != '\0' || n <= 0) return false;
  *out = Rate{n, 1};
  return true;
}

}  // namespace ferret

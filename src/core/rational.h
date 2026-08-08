#pragma once

#include <cstdint>
#include <string>

namespace ferret {

/// An exact frame rate. Never a double: 59.94 is 60000/1001 everywhere in this
/// codebase, and 30000/1001 is not 29.97.
struct Rate {
  int64_t num = 60;
  int64_t den = 1;

  constexpr Rate() = default;
  constexpr Rate(int64_t n, int64_t d) : num(n), den(d) {}

  constexpr bool valid() const { return num > 0 && den > 0; }
  constexpr bool operator==(const Rate& o) const {
    return num * o.den == o.num * den;
  }
  constexpr bool operator!=(const Rate& o) const { return !(*this == o); }

  double approx() const {
    return den ? static_cast<double>(num) / static_cast<double>(den) : 0.0;
  }

  /// "59.94", "50", "23.98" — for logs and the control API only. Never parse a
  /// rate back out of this; use `parseRate`.
  std::string label() const;
};

/// Accepts "50", "59.94", "60000/1001", "29.97". The decimal spellings map to
/// their exact broadcast rationals rather than to a literal decimal, because
/// "59.94" on a spec sheet has always meant 60000/1001.
bool parseRate(const std::string& s, Rate* out);

/// The deadline for tick `n`, in nanoseconds from tick zero.
///
/// Computed from the rational every time, never `period * n`.
///
/// At 59.94 the true period is 16683333.33 ns, so a period truncated to whole
/// nanoseconds is out by a third of a nanosecond per frame. That is 72 µs over
/// an hour and 720 µs over ten — small, but strictly linear in uptime and
/// therefore unbounded, which is exactly the shape that survives every short
/// test and then shows up as a slow walk against genlock on a long show. Round
/// the period to microseconds instead, as is common, and the same arithmetic
/// gives 72 ms an hour.
///
/// `tick` is signed on purpose. oxbow shipped a bug where an unsigned frame
/// counter multiplied a std::chrono duration, promoting its representation to
/// unsigned, so a deadline already in the past wrapped to ~585 years and the
/// process sent exactly one frame — see the fleet notes on that hunt. Signed
/// arithmetic here means a late tick produces a negative offset, which is what
/// callers expect and can act on.
constexpr int64_t tickDeadlineNs(const Rate& rate, int64_t tick) {
  // (tick * den * 1e9) / num, ordered to keep the intermediate exact for any
  // sane tick count. int64 overflows at ~1e18 ns, i.e. ~29 years of runtime.
  return (tick * rate.den * 1000000000LL) / rate.num;
}

/// The ST 2110-10 media clock: a 90 kHz counter for video, locked to the
/// PTP-disciplined TAI epoch rather than to process start, because every
/// receiver on the network reconstructs alignment from it. `taiNs` must come
/// from the PTP servo, not from the system clock — see docs/02-st2110.md for
/// why a free-running system clock makes a sender that looks fine on a scope
/// and is rejected by conformant receivers.
constexpr uint32_t rtpTimestamp90k(int64_t taiNs) {
  // Deliberately wraps at 2^32, which is what RTP specifies.
  return static_cast<uint32_t>((taiNs / 100000LL) * 9LL);
}

/// The audio equivalent, running at the sample rate (48 kHz for 2110-30).
constexpr uint32_t rtpTimestampAudio(int64_t taiNs, int sampleRate) {
  return static_cast<uint32_t>((taiNs * sampleRate) / 1000000000LL);
}

}  // namespace ferret

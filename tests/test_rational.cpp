#include "core/rational.h"

#include "check.h"

using namespace ferret;

namespace {

void decimalSpellingsMeanTheirBroadcastRationals() {
  Rate r;
  CHECK(parseRate("59.94", &r));
  CHECK_EQ(r.num, int64_t{60000});
  CHECK_EQ(r.den, int64_t{1001});

  CHECK(parseRate("29.97", &r));
  CHECK_EQ(r.num, int64_t{30000});
  CHECK_EQ(r.den, int64_t{1001});

  CHECK(parseRate("23.98", &r));
  CHECK_EQ(r.num, int64_t{24000});
  CHECK_EQ(r.den, int64_t{1001});

  // And that is genuinely not 59.94 — the gap is what drifts a frame every
  // ~28 minutes if anyone ever parses this as a decimal.
  CHECK(parseRate("59.94", &r));
  CHECK(r.approx() != 59.94);
}

void explicitRationalsAreReducedNotStored() {
  Rate r;
  CHECK(parseRate("60000/1001", &r));
  CHECK_EQ(r.num, int64_t{60000});
  CHECK_EQ(r.den, int64_t{1001});

  CHECK(parseRate("50/2", &r));
  CHECK_EQ(r.num, int64_t{25});
  CHECK_EQ(r.den, int64_t{1});
}

void unrecognisedRatesAreRejectedNotApproximated() {
  Rate r;
  CHECK(!parseRate("", &r));
  CHECK(!parseRate("fifty", &r));
  CHECK(!parseRate("50.1", &r));   // not a broadcast rate — fail, don't guess
  CHECK(!parseRate("-50", &r));
  CHECK(!parseRate("50/0", &r));
  CHECK(!parseRate("50/", &r));
}

void equalityIsByValueNotByRepresentation() {
  CHECK(Rate(50, 1) == Rate(100, 2));
  CHECK(Rate(60000, 1001) != Rate(60, 1));
}

/// Deadlines come from the rational every tick, so error never accumulates.
///
/// The property under test is not "the drift is large" — with nanosecond
/// truncation it is only 72 µs an hour at 59.94. It is that the drift is
/// *linear in uptime and therefore unbounded*, while the exact form has no
/// error term at all. A test that only checked one hour would pass on a naive
/// implementation too, so this checks the growth.
void deadlinesDoNotDrift() {
  const Rate r{60000, 1001};
  const int64_t hour = 60000LL * 3600 / 1001;  // 215784 ticks

  // The exact form is the closed-form rational, at any tick count.
  CHECK_EQ(tickDeadlineNs(r, hour), (hour * 1001LL * 1000000000LL) / 60000LL);
  CHECK_EQ(tickDeadlineNs(r, hour), int64_t{3599996400000});

  // The naive form: one truncated period, multiplied out.
  const int64_t period = tickDeadlineNs(r, 1);
  CHECK_EQ(period, int64_t{16683333});  // true value is 16683333.33…

  auto drift = [&](int64_t ticks) {
    return tickDeadlineNs(r, ticks) - period * ticks;
  };

  // 72 µs at one hour, and exactly ten times that at ten hours. Linear growth
  // with no ceiling is the failure mode; the magnitude at any one duration is
  // not the point.
  CHECK_EQ(drift(hour), int64_t{71928});
  CHECK_EQ(drift(hour * 10), int64_t{719280});
  CHECK_EQ(drift(hour * 10), drift(hour) * 10);

  // An integer rate has no truncation, so the two forms agree exactly. This is
  // why the bug never shows up at 50 or 60 and only bites on the 1001 rates.
  const Rate fifty{50, 1};
  const int64_t p50 = tickDeadlineNs(fifty, 1);
  CHECK_EQ(tickDeadlineNs(fifty, 180000), p50 * 180000);
}

/// oxbow's 585-year bug, guarded. A deadline already in the past must produce
/// a negative offset, not an enormous positive one.
void aLateTickGoesNegativeNotEnormous() {
  const Rate r{50, 1};
  const int64_t now = tickDeadlineNs(r, 100);
  const int64_t target = tickDeadlineNs(r, 10);  // deliberately in the past
  const int64_t wait = target - now;
  // 90 ticks at 50 fps = 1.8 s in the past.
  CHECK_EQ(wait, int64_t{-1800000000});
  CHECK(wait < 0);
  // The failure mode being guarded: an unsigned representation turns exactly
  // this subtraction into ~1.8e19 ns, i.e. 585 years, and the process sends
  // one frame and then waits forever while the card emits valid black.
  CHECK(wait > -2000000000LL);
}

void labelsRoundTripThroughTheirSpelling() {
  Rate r;
  CHECK(parseRate("59.94", &r));
  CHECK_EQ(r.label(), std::string("59.94"));

  CHECK(parseRate("50", &r));
  CHECK_EQ(r.label(), std::string("50"));
}

/// The 90 kHz media clock wraps at 2^32, which RTP requires. A receiver that
/// assumes monotonic timestamps breaks about every 13 hours; ours must wrap.
void theMediaClockWraps() {
  // Just below the wrap point.
  const int64_t nsAtWrap = (4294967296LL / 9LL) * 100000LL;
  const uint32_t before = rtpTimestamp90k(nsAtWrap - 100000000LL);
  const uint32_t after = rtpTimestamp90k(nsAtWrap + 100000000LL);
  CHECK(after < before);
}

void run() {
  decimalSpellingsMeanTheirBroadcastRationals();
  explicitRationalsAreReducedNotStored();
  unrecognisedRatesAreRejectedNotApproximated();
  equalityIsByValueNotByRepresentation();
  deadlinesDoNotDrift();
  aLateTickGoesNegativeNotEnormous();
  labelsRoundTripThroughTheirSpelling();
  theMediaClockWraps();
}

}  // namespace

TEST_MAIN("rational")

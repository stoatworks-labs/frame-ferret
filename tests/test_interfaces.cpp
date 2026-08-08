#include "net/interfaces.h"

#include "check.h"

using namespace ferret;

namespace {

std::vector<NetInterface> fixture() {
  std::vector<NetInterface> v;

  NetInterface lo;
  lo.name = "lo0";
  lo.address = "127.0.0.1";
  lo.isLoopback = true;
  lo.isUp = true;
  lo.index = 1;
  v.push_back(lo);

  NetInterface en0v6;
  en0v6.name = "en0";
  en0v6.address = "fe80::1";
  en0v6.isV6 = true;
  en0v6.isUp = true;
  en0v6.index = 4;
  v.push_back(en0v6);

  NetInterface en0;
  en0.name = "en0";
  en0.address = "10.0.0.4";
  en0.isUp = true;
  en0.supportsMulticast = true;
  en0.index = 4;
  en0.linkSpeedBps = 10000000000ULL;
  v.push_back(en0);

  NetInterface en6;
  en6.name = "en6";
  en6.address = "192.168.1.20";
  en6.isUp = true;
  en6.supportsMulticast = true;
  en6.index = 9;
  v.push_back(en6);

  return v;
}

void emptySelectorMeansAnyNotFirst() {
  NetInterface got;
  std::string err;
  CHECK(resolveInterface("", fixture(), &got, err));
  // The distinction that matters: "any" is INADDR_ANY, not lo0 because lo0
  // happened to be enumerated first.
  CHECK_EQ(got.name, std::string("any"));
  CHECK_EQ(got.index, uint32_t{0});
}

void anAddressResolvesExactly() {
  NetInterface got;
  std::string err;
  CHECK(resolveInterface("192.168.1.20", fixture(), &got, err));
  CHECK_EQ(got.name, std::string("en6"));
  CHECK_EQ(got.index, uint32_t{9});
}

/// en0 carries both a link-local v6 and a v4, and v6 is enumerated first. The
/// v4 must win: every transport here defaults to v4, and binding a link-local
/// v6 succeeds and then reaches nothing.
void aNameWithBothFamiliesPrefersV4() {
  NetInterface got;
  std::string err;
  CHECK(resolveInterface("en0", fixture(), &got, err));
  CHECK_EQ(got.address, std::string("10.0.0.4"));
  CHECK(!got.isV6);
}

void aV6OnlyInterfaceStillResolves() {
  std::vector<NetInterface> only;
  NetInterface n;
  n.name = "en9";
  n.address = "2001:db8::5";
  n.isV6 = true;
  n.isUp = true;
  only.push_back(n);

  NetInterface got;
  std::string err;
  CHECK(resolveInterface("en9", only, &got, err));
  CHECK(got.isV6);
}

/// An unmatched selector must fail loudly. Falling back to the default route
/// gives a sender that leaves on the wrong NIC — which on site is diagnosed as
/// a network fault, not as a config error, and can cost an afternoon.
void anUnknownSelectorFailsRatherThanFallingBack() {
  NetInterface got;
  std::string err;
  CHECK(!resolveInterface("en99", fixture(), &got, err));
  CHECK(err.find("en99") != std::string::npos);
  CHECK(err.find("frame-ferret interfaces") != std::string::npos);
}

void multicastRangesAreRecognised() {
  CHECK(isMulticast("239.255.0.1"));   // the usual 2110 admin-scoped range
  CHECK(isMulticast("224.0.1.129"));   // PTP
  CHECK(isMulticast("232.10.10.1"));   // SSM
  CHECK(isMulticast("ff02::1"));
  CHECK(isMulticast("ff15::abcd"));

  CHECK(!isMulticast("10.0.0.4"));
  CHECK(!isMulticast("223.255.255.255"));  // just below 224/4
  CHECK(!isMulticast("240.0.0.1"));        // reserved, above 224/4
  CHECK(!isMulticast("2001:db8::1"));
  CHECK(!isMulticast(""));
  CHECK(!isMulticast("not an address"));
}

/// Not an assertion about this machine — enumeration must simply work and
/// find loopback, on any machine this test runs on, including CI.
void enumerationFindsSomething() {
  std::string err;
  auto found = listInterfaces(err);
  CHECK(!found.empty());

  bool sawLoopback = false;
  for (const auto& n : found) {
    if (n.isLoopback) sawLoopback = true;
    CHECK(!n.address.empty());
    CHECK(!n.name.empty());
  }
  CHECK(sawLoopback);
}

void run() {
  emptySelectorMeansAnyNotFirst();
  anAddressResolvesExactly();
  aNameWithBothFamiliesPrefersV4();
  aV6OnlyInterfaceStillResolves();
  anUnknownSelectorFailsRatherThanFallingBack();
  multicastRangesAreRecognised();
  enumerationFindsSomething();
}

}  // namespace

TEST_MAIN("interfaces")

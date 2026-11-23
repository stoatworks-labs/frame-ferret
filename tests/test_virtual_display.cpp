// The virtual display's decisions: name, raster, rate and serial. The display
// itself needs a window server and is proven by running it (docs/NOTES.md).
#include "sources/virtual_display.h"

#include "check.h"

using namespace ferret;

namespace {

NodeConfig base() {
  NodeConfig c;
  c.id = "vd";
  c.kind = NodeKind::virtualDisplay;
  c.width = 1920;
  c.height = 1080;
  c.rate = Rate{60, 1};
  return c;
}

void theNameComesFromTargetThenLabelThenADefault() {
  VirtualDisplayPlan plan;
  std::string error;

  NodeConfig c = base();
  CHECK(planVirtualDisplay(c, &plan, error));
  CHECK_EQ(plan.name, std::string("Frame Ferret"));

  c.label = "Stage left";
  CHECK(planVirtualDisplay(c, &plan, error));
  CHECK_EQ(plan.name, std::string("Stage left"));

  c.target = "Arena Out";
  CHECK(planVirtualDisplay(c, &plan, error));
  CHECK_EQ(plan.name, std::string("Arena Out"));
}

void aWholeRateIsExactAndSilent() {
  VirtualDisplayPlan plan;
  std::string error;
  NodeConfig c = base();
  c.rate = Rate{50, 1};
  CHECK(planVirtualDisplay(c, &plan, error));
  CHECK_EQ(plan.refreshHz, 50);
  CHECK(plan.warning.empty());
}

void aFractionalRateRunsAtTheWholeRateAndSaysSo() {
  // Measured: macOS runs a 59.94 virtual display at 60. Asking for 59.94 and
  // saying nothing would hide a dropped frame every ~17 s.
  VirtualDisplayPlan plan;
  std::string error;
  NodeConfig c = base();
  c.rate = Rate{60000, 1001};
  CHECK(planVirtualDisplay(c, &plan, error));
  CHECK_EQ(plan.refreshHz, 60);
  CHECK(!plan.warning.empty());
  CHECK(plan.warning.find("17 seconds") != std::string::npos);

  c.rate = Rate{24000, 1001};
  CHECK(planVirtualDisplay(c, &plan, error));
  CHECK_EQ(plan.refreshHz, 24);
  CHECK(!plan.warning.empty());
}

void theSerialIsStableForANameAndDiffersBetweenNames() {
  VirtualDisplayPlan a, b, c2;
  std::string error;
  NodeConfig c = base();
  c.target = "One";
  CHECK(planVirtualDisplay(c, &a, error));
  CHECK(planVirtualDisplay(c, &b, error));
  CHECK_EQ(a.serial, b.serial);
  CHECK(a.serial != 0u);
  c.target = "Two";
  CHECK(planVirtualDisplay(c, &c2, error));
  CHECK(a.serial != c2.serial);
}

void impossibleDisplaysAreRefusedWithASentence() {
  VirtualDisplayPlan plan;
  std::string error;

  NodeConfig c = base();
  c.width = 1921;
  CHECK(!planVirtualDisplay(c, &plan, error));
  CHECK(error.find("even") != std::string::npos);

  c = base();
  c.width = 320;
  c.height = 240;
  error.clear();
  CHECK(!planVirtualDisplay(c, &plan, error));
  CHECK(!error.empty());

  c = base();
  c.rate = Rate{240, 1};
  error.clear();
  CHECK(!planVirtualDisplay(c, &plan, error));
  CHECK(!error.empty());

  c = base();
  c.target = std::string(60, 'x');
  error.clear();
  CHECK(!planVirtualDisplay(c, &plan, error));
  CHECK(error.find("48") != std::string::npos);
}

void thePhysicalSizeIsAnOrdinaryMonitor() {
  // About 100 ppi. A display reported far denser is offered HiDPI modes, and
  // macOS may then pick a scaled one that is not the raster asked for.
  VirtualDisplayPlan plan;
  std::string error;
  CHECK(planVirtualDisplay(base(), &plan, error));
  const double ppi = plan.width / (plan.widthMm / 25.4);
  CHECK(ppi > 90 && ppi < 110);
}

void run() {
  theNameComesFromTargetThenLabelThenADefault();
  aWholeRateIsExactAndSilent();
  aFractionalRateRunsAtTheWholeRateAndSaysSo();
  theSerialIsStableForANameAndDiffersBetweenNames();
  impossibleDisplaysAreRefusedWithASentence();
  thePhysicalSizeIsAnOrdinaryMonitor();
}

}  // namespace

TEST_MAIN("virtual_display")

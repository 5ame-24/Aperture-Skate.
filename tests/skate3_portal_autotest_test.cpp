// Unit test for the AUTOTEST sequencer (no game: a fake host).
// clang++ -std=c++23 -Wall -Wextra -Werror -Isrc tests/skate3_portal_autotest_test.cpp src/skate3_portal_autotest.cpp -o /tmp/skate3_portal_autotest_test && /tmp/skate3_portal_autotest_test
#include "skate3_portal_autotest.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

using namespace skate3::portal_autotest;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

struct Fake : Host {
  std::map<std::string, std::string> cvars{{"skate3_portal_gate_fit", "true"}, {"skate3_portal_fling", "false"}, {"skate3_portal_fling_mode", "ollie"},
                                           {"skate3_portal_carry_position_s", "0.25"}, {"skate3_portal_carry_short", "0.4"}};
  int clears = 0, places = 0, teleports = 0, refuse_next = 1;
  float last_feet[3] = {}, last_normal_b[3] = {};
  std::vector<std::string> log;
  std::string file;
  void SetCvar(const std::string& n, const std::string& v) override { cvars[n] = v; }
  std::string GetCvar(const std::string& n) override { return cvars[n]; }
  void ClearPortals() override { ++clears; }
  void PlacePortal(int which, const float[3], const float normal[3], const float[3]) override {
    ++places;
    if (which == 1) std::copy(normal, normal + 3, last_normal_b);
  }
  bool Teleport(const float feet[3], float, float) override {
    if (refuse_next > 0) {  // the first attempt is refused (the game was busy): the sequencer must try again
      --refuse_next;
      return false;
    }
    if (feet[1] > 2.5f) {  // a drop (the placing teleport to A's centre is not counted)
      ++teleports;
      std::copy(feet, feet + 3, last_feet);
    }
    return true;
  }
  void Log(const std::string& l) override { log.push_back(l); }
  void WriteFile(const std::string& name, const std::string& text) override {
    CHECK(name == "skate3_portal_autotest.json");
    file = text;
  }
};

int main() {
  Fake h;
  const float ground[3] = {10, 2, 30}, heading[2] = {0, 1};
  Start(h, ground, heading);
  CHECK(Running() && h.cvars["skate3_portal_gate_fit"] == "false");
  const auto plan = Plan();
  CHECK(plan.size() == 6 * Settings().size());
  double t = 0;
  int trial = 0;
  while (Running() && t < 2000) {
    const float on_a[3] = {10, 2, 36};  // the skater stands on portal A (6 m ahead of the start)
    Step(h, t, false, on_a, true);
    // while a trial waits, report a result as the probe would: setting 0 flies well, setting 1 flies half, setting 2 times out on walls
    if (h.teleports > trial) {
      const Trial& tr = plan[size_t(trial)];
      CHECK(std::abs(h.last_feet[1] - (2.0f + tr.height)) < 1e-4f && std::abs(h.last_feet[2] - 36.0f) < 1e-4f);  // above portal A (6 m ahead)
      if (tr.scenario == Scenario::kFloorWall) CHECK(std::abs(h.last_normal_b[1]) < 1e-4f && std::abs(h.last_normal_b[0] - 1.0f) < 1e-4f);
      Result r;
      if (tr.scenario == Scenario::kFloorFloor) {
        r.launch = true;
        r.rule_rise = tr.height;
        r.rose = tr.setting == 0 ? 0.95f * tr.height : tr.setting == 1 ? 0.5f * tr.height : 0.8f * tr.height;
        Step(h, t + 0.1, true);
        Report(r);
      } else if (tr.setting != 2) {
        r.kept = tr.setting == 0 ? 92.0f : 60.0f;
        r.heading = tr.setting == 0 ? 5.0f : 30.0f;
        Step(h, t + 0.1, true);
        Report(r);
      }  // setting 2, walls: no report -> timeout
      ++trial;
    }
    t += 0.05;
  }
  CHECK(!Running());
  CHECK(trial == int(plan.size()) && h.teleports == int(plan.size()));
  CHECK(h.places == 2 * int(plan.size()));
  CHECK(h.clears >= int(plan.size()));
  // cvars restored
  CHECK(h.cvars["skate3_portal_fling"] == "false" && h.cvars["skate3_portal_gate_fit"] == "true" && h.cvars["skate3_portal_fling_mode"] == "ollie" && h.cvars["skate3_portal_carry_position_s"] == "0.25");
  const std::string sum = Summary();
  CHECK(sum.find("best: " + Settings()[0].name) != std::string::npos);
  CHECK(sum.find("2 without a result") != std::string::npos);  // setting 2's two wall trials timed out
  CHECK(h.file.find("\"trials\":[") != std::string::npos && h.file.find("\"ok\":false") != std::string::npos);
  bool logged_best = false;
  for (const auto& l : h.log) logged_best |= l.find("AUTOTEST best: ") != std::string::npos;
  CHECK(logged_best);
  {
    // a drop that did not land over portal A is skipped and says so (not "no result")
    Fake h2;
    h2.refuse_next = 0;
    Start(h2, ground, heading);
    const float far[3] = {50, 2, 30};
    for (double t2 = 0; t2 < 40.0; t2 += 0.05) Step(h2, t2, false, far, true);
    bool skipped = false;
    for (const auto& l : h2.log) skipped |= l.find("would not settle on the drop point") != std::string::npos;
    CHECK(skipped);
    Stop(h2, "test");
    CHECK(!Running());
  }
  std::puts("skate3_portal_autotest_test: ok");
  return 0;
}

#pragma once

// AUTOTEST (Ctrl+Shift+T): the portal mechanics measured without a human doing the drops. From where the skater stands (flat open ground):
// it places its own portals ahead, teleports the skater up to a set height above the entry portal, lets it fall in, waits for the measured
// result of the crossing (FLING result line for upward launches, CROSSCHECK +20 for the rest), clears the portals, and goes on, for a set of
// scenarios and heights, under each of a few carry settings. At the end: one summary line per setting, the best one, and a JSON file.
// Pure sequencing: the game side is reached through `Host` (tests/skate3_portal_autotest_test.cpp drives it with a fake).

#include <string>
#include <vector>

namespace skate3::portal_autotest {

enum class Scenario { kFloorFloor, kFloorWall };

struct Trial {
  Scenario scenario;
  float height;   // metres above the entry portal the skater is dropped from
  int setting;    // index into Settings()
};

// A setting = cvar values applied for its trials (restored at the end).
struct Setting {
  std::string name;
  std::vector<std::pair<std::string, std::string>> cvars;
};
const std::vector<Setting>& Settings();
std::vector<Trial> Plan();

// What the sequencer asks of the game.
struct Host {
  virtual ~Host() = default;
  virtual void SetCvar(const std::string& name, const std::string& value) = 0;
  virtual std::string GetCvar(const std::string& name) = 0;
  virtual void ClearPortals() = 0;
  // which 0 blue (entry) / 1 orange (exit); hit = point on the surface, normal out of it, shot = the shooting direction (floor "up")
  virtual void PlacePortal(int which, const float hit[3], const float normal[3], const float shot[3]) = 0;
  // relocate the skater: feet at `feet`, facing (hx, hz); false = not possible this frame (try again)
  virtual bool Teleport(const float feet[3], float hx, float hz) = 0;
  virtual void Log(const std::string& line) = 0;
  virtual void WriteFile(const std::string& name, const std::string& text) = 0;
};

// Results reported by the probe while a trial waits.
struct Result {
  bool launch = false;          // an upward launch (FLING result line)
  float rule_rise = 0, rose = 0;
  float kept = 0, heading = -1;  // CROSSCHECK +20 (horizontal exits)
};

bool Running();
// one line for the overlay while running (empty when idle)
std::string Status();
// Start from the skater's ground position and heading (unit horizontal).
void Start(Host& host, const float ground[3], const float heading[2]);
void Stop(Host& host, const char* why);
// Once per human update: now = seconds; crossed = a crossing armed this update.
void Step(Host& host, double now, bool crossed);
void Report(const Result& r);

// For tests: the summary of the results so far (one line per setting).
std::string Summary();

}  // namespace skate3::portal_autotest

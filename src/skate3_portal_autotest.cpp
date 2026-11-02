#include "skate3_portal_autotest.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace skate3::portal_autotest {
namespace {

enum class Phase { kIdle, kPlace, kSettle, kDrop, kWait, kRest };

struct State {
  Phase phase = Phase::kIdle;
  std::vector<Trial> plan;
  size_t index = 0;
  int applied_setting = -1;
  double phase_t = 0;
  bool have_result = false;
  Result result;
  bool crossed = false;
  float origin[3] = {}, fwd[2] = {0, 1};
  std::map<std::string, std::string> saved;  // cvars to restore
  struct Row {
    Trial trial;
    bool ok;
    Result r;
  };
  std::vector<Row> rows;
} g;

const char* ScenarioName(Scenario s) { return s == Scenario::kFloorFloor ? "floor->floor" : "floor->wall"; }

// Portal A (entry, floor) 6 m ahead; portal B 16 m ahead: a floor (upward launch) or a wall standing on the ground facing sideways (right).
void Layout(Scenario s, float a[3], float b[3], float bn[3]) {
  const float* o = g.origin;
  const float f[2] = {g.fwd[0], g.fwd[1]};
  a[0] = o[0] + f[0] * 6.0f, a[1] = o[1], a[2] = o[2] + f[1] * 6.0f;
  b[0] = o[0] + f[0] * 16.0f, b[1] = o[1], b[2] = o[2] + f[1] * 16.0f;
  if (s == Scenario::kFloorFloor) {
    bn[0] = 0, bn[1] = 1, bn[2] = 0;
  } else {
    bn[0] = f[1], bn[1] = 0, bn[2] = -f[0];  // to the right of the line: the exit flies sideways, away from both portals
    b[1] += 2.2f;                            // the wall portal's centre, standing on the ground
  }
}

void ApplySetting(Host& host, int s) {
  const Setting& st = Settings()[size_t(s)];
  for (const auto& [name, value] : st.cvars) {
    if (!g.saved.count(name)) g.saved[name] = host.GetCvar(name);
    host.SetCvar(name, value);
  }
  g.applied_setting = s;
}

void Finish(Host& host, const char* why) {
  for (const auto& [name, value] : g.saved) host.SetCvar(name, value);
  g.saved.clear();
  host.ClearPortals();
  const std::string sum = Summary();
  host.Log(std::string("AUTOTEST done (") + why + "): " + std::to_string(g.rows.size()) + " trials");
  size_t p0 = 0;
  while (p0 < sum.size()) {
    const size_t q = sum.find('\n', p0);
    host.Log("AUTOTEST " + sum.substr(p0, q == std::string::npos ? std::string::npos : q - p0));
    if (q == std::string::npos) break;
    p0 = q + 1;
  }
  // JSON: every trial
  std::string js = "{\"trials\":[";
  for (size_t i = 0; i < g.rows.size(); ++i) {
    const auto& r = g.rows[i];
    char b[320];
    std::snprintf(b, sizeof(b),
                  "%s{\"setting\":\"%s\",\"scenario\":\"%s\",\"height\":%.1f,\"ok\":%s,\"launch\":%s,\"rule_rise\":%.2f,\"rose\":%.2f,\"kept\":%.1f,"
                  "\"heading\":%.1f}",
                  i ? "," : "", Settings()[size_t(r.trial.setting)].name.c_str(), ScenarioName(r.trial.scenario), r.trial.height,
                  r.ok ? "true" : "false", r.r.launch ? "true" : "false", r.r.rule_rise, r.r.rose, r.r.kept, r.r.heading);
    js += b;
  }
  js += "]}\n";
  host.WriteFile("skate3_portal_autotest.json", js);
  g.phase = Phase::kIdle;
}

}  // namespace

const std::vector<Setting>& Settings() {
  static const std::vector<Setting> s = {
      {"carry (default: 0.25 s arc, 0.4 s short)", {{"skate3_portal_fling", "true"}, {"skate3_portal_fling_mode", "carry"}, {"skate3_portal_carry_position_s", "0.25"},
                                                     {"skate3_portal_carry_short", "0.4"}}},
      {"carry, no arc move", {{"skate3_portal_fling", "true"}, {"skate3_portal_fling_mode", "carry"}, {"skate3_portal_carry_position_s", "0"}, {"skate3_portal_carry_short", "0.4"}}},
      {"carry, 0.5 s arc, 0.8 s short", {{"skate3_portal_fling", "true"}, {"skate3_portal_fling_mode", "carry"}, {"skate3_portal_carry_position_s", "0.5"},
                                         {"skate3_portal_carry_short", "0.8"}}},
  };
  return s;
}

std::vector<Trial> Plan() {
  std::vector<Trial> p;
  for (int s = 0; s < int(Settings().size()); ++s) {
    for (float h : {2.0f, 4.0f, 7.0f, 10.0f}) p.push_back({Scenario::kFloorFloor, h, s});
    for (float h : {3.0f, 7.0f}) p.push_back({Scenario::kFloorWall, h, s});
  }
  return p;
}

bool Running() { return g.phase != Phase::kIdle; }

std::string Status() {
  if (g.phase == Phase::kIdle || g.index >= g.plan.size()) return {};
  const Trial& t = g.plan[g.index];
  char b[200];
  std::snprintf(b, sizeof(b), "AUTOTEST trial %zu/%zu: %s from %.0f m, setting \"%s\" (keep clear; Ctrl+Shift+T stops)", g.index + 1, g.plan.size(),
                ScenarioName(t.scenario), t.height, Settings()[size_t(t.setting)].name.c_str());
  return b;
}

void Start(Host& host, const float ground[3], const float heading[2]) {
  g = State{};
  g.plan = Plan();
  std::copy(ground, ground + 3, g.origin);
  const float l = std::hypot(heading[0], heading[1]);
  g.fwd[0] = l > 1e-3f ? heading[0] / l : 0.0f;
  g.fwd[1] = l > 1e-3f ? heading[1] / l : 1.0f;
  // the fitter would push mid-air test portals around: off while testing (restored at the end)
  g.saved["skate3_portal_gate_fit"] = host.GetCvar("skate3_portal_gate_fit");
  host.SetCvar("skate3_portal_gate_fit", "false");
  g.phase = Phase::kPlace;
  host.Log("AUTOTEST started: " + std::to_string(g.plan.size()) + " trials (" + std::to_string(Settings().size()) +
           " settings); keep clear of the area ahead, it takes about " + std::to_string(int(g.plan.size()) * 6) + " s");
}

void Stop(Host& host, const char* why) {
  if (Running()) Finish(host, why);
}

void Report(const Result& r) {
  if (g.phase != Phase::kWait) return;
  g.result = r;
  g.have_result = true;
}

void Step(Host& host, double now, bool crossed) {
  if (g.phase == Phase::kIdle) return;
  if (crossed) g.crossed = true;
  if (g.index >= g.plan.size()) {
    Finish(host, "all trials run");
    return;
  }
  const Trial& t = g.plan[g.index];
  float a[3], b[3], bn[3];
  Layout(t.scenario, a, b, bn);
  switch (g.phase) {
    case Phase::kPlace: {
      if (g.applied_setting != t.setting) ApplySetting(host, t.setting);
      host.ClearPortals();
      const float up[3] = {0, 1, 0}, shot[3] = {g.fwd[0], -0.3f, g.fwd[1]};
      host.PlacePortal(0, a, up, shot);
      const float bshot[3] = {-bn[0], -0.1f, -bn[2]};
      host.PlacePortal(1, b, bn, t.scenario == Scenario::kFloorFloor ? shot : bshot);
      g.phase = Phase::kSettle;
      g.phase_t = now;
      break;
    }
    case Phase::kSettle:
      if (now - g.phase_t > 0.5) g.phase = Phase::kDrop;
      break;
    case Phase::kDrop: {
      const float feet[3] = {a[0], a[1] + t.height, a[2]};
      if (host.Teleport(feet, g.fwd[0], g.fwd[1])) {
        g.phase = Phase::kWait;
        g.phase_t = now;
        g.have_result = false;
        g.crossed = false;
      }
      break;
    }
    case Phase::kWait: {
      const bool timeout = now - g.phase_t > 6.0;
      // a launch result comes ~1.5 s after its exit; a horizontal exit's CROSSCHECK after 20 updates
      if (g.have_result || timeout) {
        g.rows.push_back({t, g.have_result, g.result});
        char b2[200];
        if (!g.have_result) {
          std::snprintf(b2, sizeof(b2), "AUTOTEST trial %zu/%zu [%s] %s from %.0f m: no result (%s)", g.index + 1, g.plan.size(),
                        Settings()[size_t(t.setting)].name.c_str(), ScenarioName(t.scenario), t.height,
                        g.crossed ? "crossed, no measurement" : "never reached the portal");
        } else if (g.result.launch) {
          std::snprintf(b2, sizeof(b2), "AUTOTEST trial %zu/%zu [%s] %s from %.0f m: rose %.2f m of the rule's %.2f (%.0f%%)", g.index + 1,
                        g.plan.size(), Settings()[size_t(t.setting)].name.c_str(), ScenarioName(t.scenario), t.height, g.result.rose,
                        g.result.rule_rise, g.result.rule_rise > 0.1f ? 100.0f * g.result.rose / g.result.rule_rise : 0.0f);
        } else {
          std::snprintf(b2, sizeof(b2), "AUTOTEST trial %zu/%zu [%s] %s from %.0f m: kept %.0f%%, heading off %.0f deg", g.index + 1,
                        g.plan.size(), Settings()[size_t(t.setting)].name.c_str(), ScenarioName(t.scenario), t.height, g.result.kept,
                        g.result.heading);
        }
        host.Log(b2);
        host.ClearPortals();  // (a floor-floor pair would loop the skater forever)
        g.phase = Phase::kRest;
        g.phase_t = now;
      }
      break;
    }
    case Phase::kRest:
      if (now - g.phase_t > 1.0) {
        ++g.index;
        g.phase = Phase::kPlace;
      }
      break;
    case Phase::kIdle:
      break;
  }
}

std::string Summary() {
  std::string out;
  int best = -1;
  double best_score = -1e9;
  for (int s = 0; s < int(Settings().size()); ++s) {
    double launch_sum = 0, kept_sum = 0, head_sum = 0;
    int ln = 0, kn = 0, hn = 0, fails = 0, total = 0;
    for (const auto& r : g.rows) {
      if (r.trial.setting != s) continue;
      ++total;
      if (!r.ok) {
        ++fails;
        continue;
      }
      if (r.r.launch && r.r.rule_rise > 0.1f) {
        launch_sum += std::min(1.5, double(r.r.rose / r.r.rule_rise));
        ++ln;
      } else if (!r.r.launch) {
        kept_sum += r.r.kept;
        ++kn;
        if (r.r.heading >= 0) head_sum += r.r.heading, ++hn;
      }
    }
    if (total == 0) continue;
    const double lr = ln ? 100.0 * launch_sum / ln : 0.0, kp = kn ? kept_sum / kn : 0.0, hd = hn ? head_sum / hn : 0.0;
    // score: launches reaching the rule's height and exits keeping speed, each up to 100; heading error and failures cost
    const double score = (ln ? lr : 0.0) + (kn ? std::min(100.0, kp) : 0.0) - hd - 25.0 * fails;
    char b[256];
    std::snprintf(b, sizeof(b), "%s: launches %.0f%% of the rule's rise (n %d), exits kept %.0f%% heading off %.0f deg (n %d), %d without a result, "
                  "score %.0f", Settings()[size_t(s)].name.c_str(), lr, ln, kp, hd, kn, fails, score);
    out += std::string(out.empty() ? "" : "\n") + b;
    if (score > best_score) best_score = score, best = s;
  }
  if (best >= 0) out += "\nbest: " + Settings()[size_t(best)].name;
  return out;
}

}  // namespace skate3::portal_autotest

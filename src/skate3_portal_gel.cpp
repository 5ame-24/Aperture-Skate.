#include "skate3_portal_gel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "skate3_portal_gate.h"

REXCVAR_DEFINE_BOOL(skate3_portal_gel, true, "Skate 3",
                    "Portal 2 style gels: Ctrl+H (or D-pad down + B) switches the portal gun to painting gel. Blue bounces you (and props) "
                    "back out with the speed you hit it with, orange speeds the board up while you ride on it. Clear (Ctrl+3, middle click, "
                    "Y) removes the gel while gel mode is on. Log prefix GEL.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(skate3_portal_gel_bounce_height, 5.3, "Skate 3",
                      "Smallest height (m) blue gel on a floor launches you to (rolling onto it, or a small drop). Portal 2: 5.3 m "
                      "(bounce_paint_min_speed 500 under sv_gravity 600). Turned into a speed with the skater's measured gravity.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(skate3_portal_gel_bounce_max_height, 6.6, "Skate 3",
                      "Highest a blue floor bounce launches you (m): a big drop onto the paint comes back at most this high (0 = no cap, "
                      "Portal's restitution 1 keeps the whole fall).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(skate3_portal_gel_wall_up, 7.0, "Skate 3",
                      "Upward speed (m/s) an airborne bounce off blue wall paint gives at least (Portal 2 wall jump: 275 u/s = 7.0); riding "
                      "into the wall gives none.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(skate3_portal_gel_prop_bounce, 5.0, "Skate 3",
                      "Smallest speed (m/s) a prop falling onto blue paint comes back up with (Portal cubes keep bouncing).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(skate3_portal_gel_boost_cap, 20.3, "Skate 3",
                      "Top speed (m/s) orange gel pushes the board up to (Portal 2 sv_speed_paint_max 800 = 20.3 m/s).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::portal_gel {
namespace pm = skate3::portal_math;
namespace {

constexpr int kMax = 32;
std::mutex g_mutex;
pm::GelPatch g_list[kMax];
int g_n = 0;
uint32_t g_next_id = 1;
std::atomic<int> g_count{0};
std::atomic<bool> g_mode{false};
pm::GelRules g_rules;

int64_t NowNs() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

// human state (game thread only)
int64_t g_last_bounce_ns = 0;
struct BounceWatch {
  bool on = false;
  uint32_t id = 0, frame = 0;
  float y0 = 0, peak = 0, expect = 0, t = 0;
  bool left_ground = false;
} g_watch;
struct BoostWatch {
  bool on = false;
  uint32_t id = 0, frame = 0;
  float in = 0, peak = 0, last = 0, t = 0;
  int miss = 0;
} g_boost;

const char* Surface(const pm::GelPatch& g) { return g.n[1] > pm::kFlatNy ? "floor" : g.n[1] < -pm::kFlatNy ? "ceiling" : "wall"; }

void SyncRules(float gravity) {
  const float g = gravity > 1.0f ? gravity : 9.81f;
  g_rules.min_bounce = std::sqrt(2.0f * g * std::max(0.0f, float(REXCVAR_GET(skate3_portal_gel_bounce_height))));
  g_rules.boost_cap = float(REXCVAR_GET(skate3_portal_gel_boost_cap));
  const float hmax = float(REXCVAR_GET(skate3_portal_gel_bounce_max_height));
  g_rules.max_bounce = hmax > 0.0f ? std::sqrt(2.0f * g * hmax) : 0.0f;
  g_rules.wall_up = float(REXCVAR_GET(skate3_portal_gel_wall_up));
}

}  // namespace

bool Enabled() { return REXCVAR_GET(skate3_portal_gel) && skate3::portal_gate::Enabled(); }
bool Mode() { return Enabled() && g_mode.load(); }

void ToggleMode() {
  if (!Enabled()) {
    REXLOG_INFO("portal-gel: GEL mode key ignored (skate3_portal_gel or the portal gate is off)");
    return;
  }
  static std::atomic<int64_t> last{0};
  const int64_t now = NowNs();
  if (now - last.load() < 400'000'000) return;  // the bind auto-repeats while held
  last = now;
  const bool on = !g_mode.load();
  g_mode = on;
  REXLOG_INFO("portal-gel: GEL mode {} ({} patches painted)", on ? "ON: the gun paints gel (blue bounce, orange speed)" : "off: the gun places portals",
              Count());
}

void Paint(int kind, const float hit[3], const float normal[3], uint32_t frame) {
  pm::GelPatch p;
  const float nl = std::sqrt(pm::Dot(normal, normal));
  if (nl < 0.5f) return;
  for (int k = 0; k < 3; ++k) {
    p.c[k] = hit[k];
    p.n[k] = normal[k] / nl;
  }
  p.kind = kind ? 1 : 0;
  int covered, n;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    p.id = g_next_id++;
    covered = pm::GelPaint(g_list, &g_n, kMax, p);
    n = g_n;
    g_count = g_n;
  }
  const uint32_t id = p.id;
  REXLOG_INFO("portal-gel: GEL [f {}] painted {} #{} on a {} at ({:.2f}, {:.2f}, {:.2f}) normal ({:.2f}, {:.2f}, {:.2f}), covered {}, {} patches",
              frame, p.kind ? "orange" : "blue", id, Surface(p), p.c[0], p.c[1], p.c[2], p.n[0], p.n[1], p.n[2], covered, n);
}

void Clear() {
  int was;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    was = g_n;
    g_n = 0;
    g_count = 0;
  }
  REXLOG_INFO("portal-gel: GEL cleared {} patches", was);
}

int Count() { return g_count.load(std::memory_order_relaxed); }

int Snapshot(pm::GelPatch* out, int max) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int n = std::min(max, g_n);
  std::copy(g_list, g_list + n, out);
  return n;
}

const pm::GelRules& Rules() { return g_rules; }

std::atomic<int64_t> g_drawn_ns{0};
void NoteDrawn() { g_drawn_ns = NowNs(); }
bool DrawnRecently() { return NowNs() - g_drawn_ns.load() < 250'000'000; }

Effect HumanStep(const float feet[3], const float centre[3], const float v[3], bool grounded, float dt, float gravity, uint32_t frame) {
  Effect e;
  // the result of the last bounce: how high did it really go?
  if (g_watch.on) {
    g_watch.t += dt;
    g_watch.peak = std::max(g_watch.peak, feet[1]);
    if (!grounded) g_watch.left_ground = true;
    if ((g_watch.left_ground && grounded && g_watch.t > 0.3f) || g_watch.t > 4.0f) {
      g_watch.on = false;
      REXLOG_INFO("portal-gel: GEL [f {}] bounce on #{} (f {}) result: rose {:.2f} m (expected {:.2f} m), {}", frame, g_watch.id, g_watch.frame,
                  g_watch.peak - g_watch.y0, g_watch.expect, g_watch.left_ground ? "landed" : "never left the ground");
    }
  }
  if (Count() == 0 || !Enabled()) {
    if (g_boost.on) g_boost.on = false;
    return e;
  }
  SyncRules(gravity);
  pm::GelPatch list[kMax];
  int n;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    n = g_n;
    std::copy(g_list, g_list + n, list);
  }
  const int64_t now = NowNs();
  bool on_orange = false;
  for (int i = n - 1; i >= 0; --i) {  // newest paint first
    const pm::GelPatch& g = list[i];
    const bool flat = g.n[1] > pm::kFlatNy;
    float h;
    if (!pm::GelTouch(g, flat ? feet : centre, g_rules, &h)) continue;
    if (g.kind == 0) {
      if (now - g_last_bounce_ns < 350'000'000) continue;
      float out[3];
      if (!pm::GelBounce(g, v, grounded, g_rules, out)) continue;
      g_last_bounce_ns = now;
      e.kind = Effect::kBounce;
      e.patch = int(g.id);
      e.lift = grounded;  // the game drops velocity added while the board is on the ground (run 197): the probe lifts it off first
      for (int k = 0; k < 3; ++k) {
        e.v_out[k] = out[k];
        e.dv[k] = out[k] - v[k];
      }
      const float vn = pm::Dot(v, g.n), on = pm::Dot(out, g.n);
      REXLOG_INFO("portal-gel: GEL [f {}] bounce on blue #{} ({}): v ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f}), {:.1f} m/s into it -> {:.1f} out "
                  "({}), {}", frame, g.id, Surface(g), v[0], v[1], v[2], out[0], out[1], out[2], -vn, on,
                  on >= g_rules.max_bounce - 0.01f && g_rules.max_bounce > 0.0f ? "capped" : -vn * g_rules.restitution >= on - 0.01f ? "speed kept" : "minimum bounce",
                  grounded ? "riding: lift" : "airborne");
      if (flat && gravity > 1.0f) {
        g_watch = BounceWatch{};
        g_watch.on = true;
        g_watch.id = g.id;
        g_watch.frame = frame;
        g_watch.y0 = feet[1];
        g_watch.peak = feet[1];
        g_watch.expect = out[1] * out[1] / (2.0f * gravity);
      }
      return e;
    }
    // orange: only riding on floor-ish paint pushes (a wall or a jump over it does nothing)
    if (!flat || !grounded || on_orange) continue;
    const float hs = std::hypot(v[0], v[2]);
    const float hs2 = pm::GelBoost(hs, dt, g_rules);
    on_orange = true;
    if (!g_boost.on || g_boost.id != g.id) {
      if (g_boost.on) {
        REXLOG_INFO("portal-gel: GEL [f {}] moved from orange #{} to #{} at {:.1f} m/s", frame, g_boost.id, g.id, hs);
      } else {
        REXLOG_INFO("portal-gel: GEL [f {}] on orange #{} at {:.1f} m/s", frame, g.id, hs);
        g_boost = BoostWatch{};
        g_boost.in = hs;
        g_boost.frame = frame;
      }
      g_boost.on = true;
      g_boost.id = g.id;
    }
    g_boost.miss = 0;
    g_boost.peak = std::max(g_boost.peak, hs);
    g_boost.last = hs;
    g_boost.t += dt;
    if (hs2 > hs) {
      e.kind = Effect::kBoost;
      e.patch = int(g.id);
      const float add = hs2 - hs;
      e.dv[0] = v[0] / hs * add;
      e.dv[2] = v[2] / hs * add;
    }
  }
  if (!on_orange && g_boost.on && ++g_boost.miss >= 3) {  // a short gap (a bump, a pop) does not end the ride
    g_boost.on = false;
    REXLOG_INFO("portal-gel: GEL [f {}] left orange #{} after {:.2f} s: speed in {:.1f}, peak {:.1f}, out {:.1f} m/s (boost {:.1f} m/s^2 to {:.0f})",
                frame, g_boost.id, g_boost.t, g_boost.in, g_boost.peak, g_boost.last, g_rules.boost_accel, g_rules.boost_cap);
  }
  return e;
}

bool BodyStep(const float p[3], const float v[3], float out_v[3]) {
  if (Count() == 0) return false;
  pm::GelPatch list[kMax];
  int n;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    n = g_n;
    std::copy(g_list, g_list + n, list);
  }
  for (int i = n - 1; i >= 0; --i) {
    if (list[i].kind != 0 || !pm::GelTouch(list[i], p, g_rules, nullptr)) continue;
    // Props: falling onto the paint at 1.5 m/s or more comes back at least skate3_portal_gel_prop_bounce (run 197: with no minimum a prop
    // settling at 1-2 m/s rose a few cm, invisible); one lying on it stays put (it moves into the paint slower than that).
    pm::GelRules r = g_rules;
    r.min_bounce = float(REXCVAR_GET(skate3_portal_gel_prop_bounce));
    r.wall_min = 0.5f * r.min_bounce;
    r.min_into = 1.5f;
    if (pm::GelBounce(list[i], v, false, r, out_v)) return true;
  }
  return false;
}

}  // namespace skate3::portal_gel

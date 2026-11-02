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

void PaintImpl(int kind, const float hit[3], const float normal[3], uint32_t frame, bool quiet) {
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
  if (quiet) return;
  REXLOG_INFO("portal-gel: GEL [f {}] painted {} #{} on a {} at ({:.2f}, {:.2f}, {:.2f}) normal ({:.2f}, {:.2f}, {:.2f}), covered {}, {} patches",
              frame, p.kind ? "orange" : "blue", id, Surface(p), p.c[0], p.c[1], p.c[2], p.n[0], p.n[1], p.n[2], covered, n);
}

void Paint(int kind, const float hit[3], const float normal[3], uint32_t frame) { PaintImpl(kind, hit, normal, frame, false); }

void ClearStreams();
void Clear() {
  ClearStreams();
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


// ---- droppers and blobs ----
namespace {
struct Dropper {
  float p[3];
  int kind;
  int64_t next_ns;
};
struct Blob {
  uint32_t id;
  float p[3], v[3];
  int kind;
  int64_t born_ns;
  bool hit_known = false, ray_out = false;
  float hit[3], hit_n[3];
  int64_t ray_ns = 0;
  int hops = 0;
};
constexpr int kDroppers = 4, kBlobs = 24;
std::mutex g_stream_mutex;
std::vector<Dropper> g_droppers;
std::vector<Blob> g_blobs;
uint32_t g_next_blob = 1;
std::atomic<bool> g_dropper_mode{false};
int g_blob_paints = 0, g_blob_hops = 0, g_blob_lost = 0;
}  // namespace

void ClearStreams() {
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  if (!g_droppers.empty()) REXLOG_INFO("portal-gel: GEL droppers cleared ({}), {} blobs dropped", g_droppers.size(), g_blobs.size());
  g_droppers.clear();
  g_blobs.clear();
}

bool DropperMode() { return Mode() && g_dropper_mode.load(); }

void ToggleDropperMode() {
  static std::atomic<int64_t> last{0};
  const int64_t now = NowNs();
  if (now - last.load() < 400'000'000) return;
  last = now;
  if (!Mode()) {
    REXLOG_INFO("portal-gel: GEL dropper key ignored: gel mode is off (Ctrl+H first)");
    return;
  }
  const bool on = !g_dropper_mode.load();
  g_dropper_mode = on;
  REXLOG_INFO("portal-gel: GEL gun sub-mode: {}", on ? "DROPPER (a shot places a gel dropper)" : "paint");
}

void PlaceDropper(int kind, const float hit[3], const float normal[3], uint32_t frame) {
  Dropper d;
  const bool ceiling = normal[1] < -pm::kFlatNy;
  for (int k = 0; k < 3; ++k) d.p[k] = hit[k] + (ceiling ? normal[k] * 0.25f : 0.0f);
  if (!ceiling) d.p[1] += 4.0f;  // a floor or wall hit: the dropper hangs 4 m above it
  d.kind = kind ? 1 : 0;
  d.next_ns = NowNs();
  size_t n;
  {
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    if (g_droppers.size() >= size_t(kDroppers)) g_droppers.erase(g_droppers.begin());
    g_droppers.push_back(d);
    n = g_droppers.size();
  }
  REXLOG_INFO("portal-gel: GEL [f {}] dropper placed: {} at ({:.2f}, {:.2f}, {:.2f}) ({}), {} droppers", frame, d.kind ? "orange" : "blue", d.p[0], d.p[1],
              d.p[2], ceiling ? "under the ceiling" : "4 m above the hit", n);
}

void StreamStep(float dt, float gravity, const pm::Frame* portals, uint32_t frame) {
  if (!Enabled()) return;
  struct PaintAt {
    int kind;
    float p[3], n[3];
  };
  std::vector<PaintAt> paints;
  const int64_t now = NowNs();
  {
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    if (g_droppers.empty() && g_blobs.empty()) return;
    for (Dropper& d : g_droppers) {
      if (now < d.next_ns || g_blobs.size() >= size_t(kBlobs)) continue;
      d.next_ns = now + 200'000'000;
      Blob b;
      b.id = g_next_blob++;
      std::copy(d.p, d.p + 3, b.p);
      // a small wobble so the stream is a stream, not a line
      const float a = float(b.id % 16) * 0.3927f;
      b.v[0] = 0.25f * std::cos(a);
      b.v[1] = -1.0f;
      b.v[2] = 0.25f * std::sin(a);
      b.kind = d.kind;
      b.born_ns = now;
      g_blobs.push_back(b);
    }
    for (size_t i = 0; i < g_blobs.size();) {
      Blob& b = g_blobs[i];
      const float before[3] = {b.p[0], b.p[1], b.p[2]};
      const int through = pm::BlobAdvance(portals, b.p, b.v, gravity, dt, 5.7f);
      if (through >= 0) {
        b.hit_known = false;  // a new path: ask again
        b.ray_out = false;
        ++b.hops;
        if (g_blob_hops++ < 10) {
          REXLOG_INFO("portal-gel: GEL [f {}] blob {} went through the {} portal, out at ({:.2f}, {:.2f}, {:.2f}) v ({:.1f}, {:.1f}, {:.1f})", frame, b.id,
                      through ? "orange" : "blue", b.p[0], b.p[1], b.p[2], b.v[0], b.v[1], b.v[2]);
        }
      }
      bool landed = false;
      if (b.hit_known) {
        // landed once this step crossed the surface the ray found (or came within a few cm of the hit)
        const float a0 = (before[0] - b.hit[0]) * b.hit_n[0] + (before[1] - b.hit[1]) * b.hit_n[1] + (before[2] - b.hit[2]) * b.hit_n[2];
        const float a1 = (b.p[0] - b.hit[0]) * b.hit_n[0] + (b.p[1] - b.hit[1]) * b.hit_n[1] + (b.p[2] - b.hit[2]) * b.hit_n[2];
        landed = a0 >= 0.0f && a1 <= 0.03f;
      }
      if (landed) {
        paints.push_back({b.kind, {b.hit[0], b.hit[1], b.hit[2]}, {b.hit_n[0], b.hit_n[1], b.hit_n[2]}});
        g_blobs.erase(g_blobs.begin() + long(i));
        continue;
      }
      if (now - b.born_ns > 5'000'000'000ll || b.p[1] < -100.0f) {
        ++g_blob_lost;
        g_blobs.erase(g_blobs.begin() + long(i));
        continue;
      }
      ++i;
    }
  }
  for (const PaintAt& pa : paints) {
    PaintImpl(pa.kind, pa.p, pa.n, frame, true);
    if (g_blob_paints++ % 50 == 0) {
      REXLOG_INFO("portal-gel: GEL [f {}] blob paint #{} ({}) at ({:.2f}, {:.2f}, {:.2f}); {} lost (no surface in 5 s)", frame, g_blob_paints,
                  pa.kind ? "orange" : "blue", pa.p[0], pa.p[1], pa.p[2], g_blob_lost);
    }
  }
}

bool NextBlobRay(uint32_t* blob, float o[3], float d[3], float* len) {
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  // the blob that has waited longest for an answer (or never had one)
  Blob* pick = nullptr;
  const int64_t now = NowNs();
  for (Blob& b : g_blobs) {
    if (b.ray_out && now - b.ray_ns < 300'000'000) continue;  // one in flight (answer lost after 0.3 s: ask again)
    if (b.hit_known && now - b.ray_ns < 250'000'000) continue;  // a fresh answer: the path is a curve, refresh it every 0.25 s
    if (!pick || b.ray_ns < pick->ray_ns) pick = &b;
  }
  if (!pick) return false;
  const float sp = std::sqrt(pm::Dot(pick->v, pick->v));
  if (sp < 1e-3f) return false;
  for (int k = 0; k < 3; ++k) o[k] = pick->p[k], d[k] = pick->v[k] / sp;
  // far enough for the next ~0.4 s along the current direction (a chord of the falling curve)
  *len = std::clamp(sp * 0.4f + 1.5f, 2.0f, 40.0f);
  pick->ray_out = true;
  pick->ray_ns = now;
  *blob = pick->id;
  return true;
}

void OnBlobRay(uint32_t blob, bool hit, const float pos[3], const float normal[3]) {
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  for (Blob& b : g_blobs) {
    if (b.id != blob) continue;
    b.ray_out = false;
    b.ray_ns = NowNs();
    b.hit_known = hit;
    if (hit) {
      std::copy(pos, pos + 3, b.hit);
      std::copy(normal, normal + 3, b.hit_n);
    }
    return;
  }
}

int BlobSnapshot(float (*p)[3], int* kind, int max) {
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  const int n = std::min(max, int(g_blobs.size()));
  for (int i = 0; i < n; ++i) std::copy(g_blobs[size_t(i)].p, g_blobs[size_t(i)].p + 3, p[i]), kind[i] = g_blobs[size_t(i)].kind;
  return n;
}

int DropperSnapshot(float (*p)[3], int* kind, int max) {
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  const int n = std::min(max, int(g_droppers.size()));
  for (int i = 0; i < n; ++i) std::copy(g_droppers[size_t(i)].p, g_droppers[size_t(i)].p + 3, p[i]), kind[i] = g_droppers[size_t(i)].kind;
  return n;
}

}  // namespace skate3::portal_gel

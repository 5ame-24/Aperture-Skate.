#include "skate3_portal_cube.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

#include <rex/logging.h>

#include "skate3_portal_gate.h"

namespace skate3::portal_cube {
namespace {

std::mutex g_mutex;
std::atomic<bool> g_spawn_request{false};
std::atomic<bool> g_grab_request{false};

struct Cube {
  bool active = false;
  float p[3] = {}, v[3] = {};
  float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};  // world direction of local x, y, z
  float w[3] = {};                                          // angular velocity, world frame
  float spawn[3] = {};
  // Last answers from the game's world
  bool ground_valid = false;
  float ground_y = 0.0f, ground_xz[2] = {};
  bool wall_valid = false;
  float wall_n[3] = {}, wall_pos[3] = {};
  int wall_age = 99;
  int frame = 0;
  float ray_dir[3] = {};
  float last_ground_y = -1e9f;
  bool held = false;
  float last_skater[3] = {};
  bool have_skater = false;
} g_cube;

constexpr float kGravity = 16.0f;

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Rotate the cube's axes about the world axis w by |w| dt (Rodrigues), then re-orthonormalise.
void Spin(Cube& c, float dt) {
  const float mag = std::sqrt(Dot(c.w, c.w));
  if (mag * dt < 1e-5f) return;
  const float k[3] = {c.w[0] / mag, c.w[1] / mag, c.w[2] / mag};
  const float a = mag * dt, ca = std::cos(a), sa = std::sin(a);
  for (auto& ax : c.axes) {
    const float kx[3] = {k[1] * ax[2] - k[2] * ax[1], k[2] * ax[0] - k[0] * ax[2], k[0] * ax[1] - k[1] * ax[0]};
    const float kd = Dot(k, ax);
    for (int i = 0; i < 3; ++i) ax[i] = ax[i] * ca + kx[i] * sa + k[i] * kd * (1.0f - ca);
  }
}

void Orthonormalise(float ax[3][3]) {
  auto norm = [](float v[3]) {
    const float l = std::sqrt(Dot(v, v));
    if (l > 1e-6f) for (int i = 0; i < 3; ++i) v[i] /= l;
  };
  norm(ax[0]);
  const float d = Dot(ax[0], ax[1]);
  for (int i = 0; i < 3; ++i) ax[1][i] -= d * ax[0][i];
  norm(ax[1]);
  ax[2][0] = ax[0][1] * ax[1][2] - ax[0][2] * ax[1][1];
  ax[2][1] = ax[0][2] * ax[1][0] - ax[0][0] * ax[1][2];
  ax[2][2] = ax[0][0] * ax[1][1] - ax[0][1] * ax[1][0];
}

// How far the cube reaches below its centre (its extent along world y).
float Reach(const Cube& c) { return kHalf * (std::abs(c.axes[0][1]) + std::abs(c.axes[1][1]) + std::abs(c.axes[2][1])); }

// Settle toward the nearest flat orientation (every local axis snapped to the nearest world axis), keeping the yaw.
void Settle(Cube& c, float rate) {
  float snap[3][3];
  for (int a = 0; a < 3; ++a) {
    int best = 0;
    for (int i = 1; i < 3; ++i) if (std::abs(c.axes[a][i]) > std::abs(c.axes[a][best])) best = i;
    for (int i = 0; i < 3; ++i) snap[a][i] = 0.0f;
    snap[a][best] = c.axes[a][best] >= 0 ? 1.0f : -1.0f;
  }
  for (int a = 0; a < 3; ++a) for (int i = 0; i < 3; ++i) c.axes[a][i] += (snap[a][i] - c.axes[a][i]) * rate;
  Orthonormalise(c.axes);
}

}  // namespace

void RequestSpawn() { g_spawn_request = true; }
void RequestGrab() { g_grab_request = true; }
bool TakeGrab() {
  bool want = true;
  return g_grab_request.compare_exchange_strong(want, false);
}
bool Held() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_cube.held;
}
bool ToggleHold(const float skater[3], const float skater_v[2]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Cube& c = g_cube;
  if (!c.active) return false;
  if (c.held) {
    c.held = false;
    c.v[0] = skater_v[0];
    c.v[2] = skater_v[1];
    REXLOG_INFO("portal-cube: dropped at ({:.2f}, {:.2f}, {:.2f}) v ({:.1f}, {:.1f})", c.p[0], c.p[1], c.p[2], c.v[0], c.v[2]);
    return false;
  }
  const float d = std::sqrt((c.p[0] - skater[0]) * (c.p[0] - skater[0]) + (c.p[1] - skater[1]) * (c.p[1] - skater[1]) +
                            (c.p[2] - skater[2]) * (c.p[2] - skater[2]));
  if (d > 3.6f) {
    REXLOG_INFO("portal-cube: too far to grab ({:.1f} m)", d);
    return false;
  }
  c.held = true;
  REXLOG_INFO("portal-cube: picked up ({:.1f} m away)", d);
  return true;
}
bool TakeSpawn() {
  bool want = true;
  return g_spawn_request.compare_exchange_strong(want, false);
}

void Spawn(const float at[3]) {
  skate3::portal_gate::Sfx(4);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_cube = Cube{};
  g_cube.active = true;
  for (int k = 0; k < 3; ++k) {
    g_cube.p[k] = at[k];
    g_cube.spawn[k] = at[k];
  }
  REXLOG_INFO("portal-cube: spawned at ({:.2f}, {:.2f}, {:.2f})", at[0], at[1], at[2]);
}

bool Active() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_cube.active;
}

bool NextRay(RayWant* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Cube& c = g_cube;
  if (!c.active) return false;
  const float hs = std::hypot(c.v[0], c.v[2]);
  const bool wall = (c.frame++ & 1) && hs > 0.8f;
  if (wall) {
    out->kind = 1;
    out->d[0] = c.v[0] / hs;
    out->d[1] = 0.0f;
    out->d[2] = c.v[2] / hs;
    for (int k = 0; k < 3; ++k) out->o[k] = c.p[k];
    out->len = kHalf * 1.8f + hs * 0.12f;
  } else {
    out->kind = 0;
    out->d[0] = 0.0f;
    out->d[1] = -1.0f;
    out->d[2] = 0.0f;
    out->o[0] = c.p[0];
    out->o[1] = c.p[1] + 0.3f;
    out->o[2] = c.p[2];
    out->len = 30.0f;
  }
  return true;
}

void OnRay(int kind, bool hit, const float pos[3], const float normal[3]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Cube& c = g_cube;
  if (!c.active) return;
  // A hit inside an open portal's opening is not there: the cube falls (and walks) through.
  if (hit && skate3::portal_gate::InsideOpening(pos, 0.9f)) hit = false;
  if (kind == 0) {
    c.ground_valid = hit;
    if (hit) {
      c.ground_y = pos[1];
      c.ground_xz[0] = pos[0];
      c.ground_xz[1] = pos[2];
      c.last_ground_y = pos[1];
    }
  } else {
    c.wall_valid = hit;
    c.wall_age = 0;
    if (hit) {
      for (int k = 0; k < 3; ++k) {
        c.wall_n[k] = normal[k];
        c.wall_pos[k] = pos[k];
      }
    }
  }
}

void Step(float dt, const float skater[3], const float skater_v[2], const float hand[3]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Cube& c = g_cube;
  if (!c.active) return;
  dt = std::clamp(dt, 0.001f, 0.05f);
  const float prev[3] = {c.p[0], c.p[1], c.p[2]};
  // Held: it floats in front of the skater (a spring toward a point ahead, no gravity). The skater crossing a portal moves the
  // point far away in one frame: the cube is brought along. Held too far from its point (it went through a portal alone, or
  // got stuck on something) it is let go.
  float hold_target[3] = {};
  if (c.held) {
    for (int k = 0; k < 3; ++k) hold_target[k] = hand[k];
    const float jump = c.have_skater ? std::hypot(skater[0] - c.last_skater[0], std::hypot(skater[1] - c.last_skater[1], skater[2] - c.last_skater[2])) : 0.0f;
    if (jump > 3.0f) {
      for (int k = 0; k < 3; ++k) c.p[k] = hold_target[k];
      c.v[0] = c.v[1] = c.v[2] = 0.0f;
    }
    const float off = std::hypot(hold_target[0] - c.p[0], std::hypot(hold_target[1] - c.p[1], hold_target[2] - c.p[2]));
    if (off > 4.5f) {
      c.held = false;
      REXLOG_INFO("portal-cube: let go, it is {:.1f} m from the hand", off);
    }
  }
  for (int k = 0; k < 3; ++k) c.last_skater[k] = skater[k];
  c.have_skater = true;
  if (c.held) {
    float vel[3];
    for (int k = 0; k < 3; ++k) vel[k] = (hold_target[k] - c.p[k]) * 14.0f;
    const float sp = std::sqrt(Dot(vel, vel));
    if (sp > 30.0f) for (float& x : vel) x *= 30.0f / sp;
    for (int k = 0; k < 3; ++k) c.v[k] = vel[k];
    for (float& x : c.w) x *= std::exp(-6.0f * dt);
    Settle(c, 1.0f - std::exp(-6.0f * dt));
  } else {
    c.v[1] -= kGravity * dt;
  }
  for (int k = 0; k < 3; ++k) c.p[k] += c.v[k] * dt;
  Spin(c, dt);
  Orthonormalise(c.axes);
  bool grounded = false;
  // Ground, from the last downward ray if it was cast close to here.
  const float reach = Reach(c);
  if (c.ground_valid && std::hypot(c.p[0] - c.ground_xz[0], c.p[2] - c.ground_xz[1]) < 1.5f &&
      c.p[1] - reach <= c.ground_y && c.p[1] > c.ground_y - 2.0f) {
    c.p[1] = c.ground_y + reach;
    if (c.v[1] < 0.0f) c.v[1] = c.v[1] < -3.0f ? -c.v[1] * 0.18f : 0.0f;
    const float damp = std::exp(-2.2f * dt);
    c.v[0] *= damp;
    c.v[2] *= damp;
    const float adamp = std::exp(-5.0f * dt);
    for (float& x : c.w) x *= adamp;
    Settle(c, 1.0f - std::exp(-9.0f * dt));
    grounded = true;
  }
  (void)grounded;
  // Wall: a recent horizontal ray that hit close ahead.
  if (c.wall_valid && c.wall_age++ < 4) {
    const float dist = (c.wall_pos[0] - c.p[0]) * -c.wall_n[0] + (c.wall_pos[2] - c.p[2]) * -c.wall_n[2];
    const float vn = Dot(c.v, c.wall_n);
    if (dist < kHalf * 1.5f && vn < 0.0f) {
      for (int k = 0; k < 3; ++k) c.v[k] -= 1.3f * vn * c.wall_n[k];
      const float push = kHalf * 1.5f - dist;
      if (push > 0.0f && push < 1.0f) for (int k = 0; k < 3; ++k) c.p[k] += c.wall_n[k] * push * 0.5f;
    }
  } else if (c.wall_age >= 4) {
    c.wall_valid = false;
  }
  // The skater pushes it.
  {
    const float dx = c.p[0] - skater[0], dz = c.p[2] - skater[2];
    const float dist = std::hypot(dx, dz);
    const float touch = kHalf * 1.25f + 0.5f;
    const float dy = c.p[1] - (skater[1] - 0.4f);
    if (!c.held && dist < touch && std::abs(dy) < 1.2f && dist > 1e-3f) {
      const float nx = dx / dist, nz = dz / dist;
      const float approach = (skater_v[0] - c.v[0]) * nx + (skater_v[1] - c.v[2]) * nz;
      if (approach > 0.0f) {
        c.v[0] += nx * (approach * 1.15f + 0.4f);
        c.v[2] += nz * (approach * 1.15f + 0.4f);
        // a push off-centre spins it a little about the vertical
        c.w[1] += (nz * skater_v[0] - nx * skater_v[1]) * 0.15f;
      }
      c.p[0] = skater[0] + nx * touch;
      c.p[2] = skater[2] + nz * touch;
    }
  }
  // Portals.
  {
    float np[3], nv[3], R[9];
    if (skate3::portal_gate::CubeCross(prev, c.p, c.v, np, nv, R)) {
      for (auto& ax : c.axes) {
        const float o[3] = {ax[0], ax[1], ax[2]};
        for (int k = 0; k < 3; ++k) ax[k] = R[k * 3] * o[0] + R[k * 3 + 1] * o[1] + R[k * 3 + 2] * o[2];
      }
      const float ow[3] = {c.w[0], c.w[1], c.w[2]};
      for (int k = 0; k < 3; ++k) c.w[k] = R[k * 3] * ow[0] + R[k * 3 + 1] * ow[1] + R[k * 3 + 2] * ow[2];
      REXLOG_INFO("portal-cube: crossed ({:.2f},{:.2f},{:.2f}) v ({:.1f},{:.1f},{:.1f}) -> ({:.2f},{:.2f},{:.2f}) v ({:.1f},{:.1f},{:.1f})",
                  c.p[0], c.p[1], c.p[2], c.v[0], c.v[1], c.v[2], np[0], np[1], np[2], nv[0], nv[1], nv[2]);
      for (int k = 0; k < 3; ++k) {
        c.p[k] = np[k];
        c.v[k] = nv[k];
      }
      c.ground_valid = false;  // the old ground answer belongs to the other side
      c.wall_valid = false;
    }
  }
  // Fell out of the world: back to where it was spawned.
  if (c.p[1] < std::min(c.last_ground_y, c.spawn[1]) - 60.0f || !std::isfinite(c.p[1])) {
    REXLOG_INFO("portal-cube: fell out of the world, back to the spawn point");
    const float at[3] = {c.spawn[0], c.spawn[1], c.spawn[2]};
    c = Cube{};
    c.active = true;
    for (int k = 0; k < 3; ++k) c.p[k] = c.spawn[k] = at[k];
  }
}

namespace {
struct External {
  float c[3] = {};
  float axes[3][3] = {};
  int64_t ns = 0;
} g_ext;
int64_t NowNs() { return std::chrono::steady_clock::now().time_since_epoch().count(); }
bool ExtFresh() { return g_ext.ns != 0 && NowNs() - g_ext.ns < 200'000'000ll; }
}  // namespace

void SetExternal(const float c[3], const float axes[3][3]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (int k = 0; k < 3; ++k) {
    g_ext.c[k] = c[k];
    for (int a = 0; a < 3; ++a) g_ext.axes[a][k] = axes[a][k];
  }
  g_ext.ns = NowNs();
}
void ClearExternal() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_ext.ns = 0;
}
bool ExternalActive() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return ExtFresh();
}
bool ExternalPos(float out[3]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ExtFresh()) return false;
  for (int k = 0; k < 3; ++k) out[k] = g_ext.c[k];
  return true;
}
bool HidesItemAt(const float p[3]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ExtFresh()) return false;
  const float d[3] = {p[0] - g_ext.c[0], p[1] - g_ext.c[1], p[2] - g_ext.c[2]};
  return d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 0.9f * 0.9f;
}

bool Snapshot(Draw* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (ExtFresh()) {
    for (int k = 0; k < 3; ++k) {
      out->c[k] = g_ext.c[k];
      for (int a = 0; a < 3; ++a) out->axes[a][k] = g_ext.axes[a][k];
    }
    out->has_ground = false;
    return true;
  }
  if (!g_cube.active) return false;
  for (int k = 0; k < 3; ++k) {
    out->c[k] = g_cube.p[k];
    for (int a = 0; a < 3; ++a) out->axes[a][k] = g_cube.axes[a][k];
  }
  out->has_ground = g_cube.ground_valid;
  out->ground[0] = g_cube.ground_xz[0];
  out->ground[1] = g_cube.ground_y;
  out->ground[2] = g_cube.ground_xz[1];
  return true;
}

}  // namespace skate3::portal_cube

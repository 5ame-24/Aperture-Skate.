#include "skate3_portal_cube.h"
#include "skate3_portal_gate.h"
#include "skate3_portal_gel.h"
#include "skate3_portal_math.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <vector>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <string>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include "skate3_native_scene.h"
#include "skate3_native_scene_state.h"

REXCVAR_DEFINE_BOOL(skate3_portal_gate, false, "Skate 3",
                    "Test portal pair: Ctrl+1 places the blue ring ahead of the camera, Ctrl+2 the orange "
                    "one, Ctrl+3 clears. Jump through one while airborne to come out of the other.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_gate_mouse, false, "Skate 3",
                    "Portal gun aims at the mouse cursor (reticle) and fires with the mouse: left button blue, "
                    "right button orange, middle button clears. Off: shots go level along the camera heading.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_fpv, false, "Skate 3",
                    "Portal first-person view on/off (Ctrl+4, or D-pad down + R3 on a pad).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(skate3_portal_gate_wall_exit, 0.0, "Skate 3",
                      "Hole mode: metres in front of a wall portal's plane the skater comes out (run 120: at 0.02 m the "
                      "turned rates were cancelled one frame later, as if a wall probe saw the wall). 0 = on the plane.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_gate_hole, true, "Skate 3",
                    "Make the wall/floor non-solid inside an open portal (body collision is skipped there) and cross at the "
                    "real portal plane, exiting at the other portal's plane. Off = old behaviour: solid surface, crossing "
                    "triggers in front of it and the exit is lifted.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_portal_keep_skeleton_state, false, "Skate 3",
                    "Portal experiment: put back the skeleton's state flags/timers (skeleton +0x4000..0x4080: trick, foot and grab state) that the "
                    "game's teleport reset (82BE3508) clears, as the velocity vectors already are.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_gun_model, true, "Skate 3", "Draw the real Portal gun (from your own Portal 2 install) in 3D.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_portal_gun_2d, false, "Skate 3", "Also draw the old flat 2D gun and the green skater dot on the overlay.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_portal_sfx, true, "Skate 3", "Portal gun sounds (synthesised: shots, portal travel, clear, companion cube).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_portal_sfx_dir, "", "Skate 3",
                      "Folder with Portal 2's pak01_dir.vpk (the 'portal2' folder of your install) for the real portal sounds; empty = the Steam "
                      "default on macOS. Missing files fall back to synthesised sounds.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_portal_sfx_volume, 0.35, "Skate 3", "Portal sound volume, 0..1.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_keep_tricks, true, "Skate 3",
                    "Portal experiment: hide the game's 'just teleported' processed-input flag (0x04000000 in the word at +2484) from the "
                    "rest of the frame, so a trick grab is not reset by a portal crossing.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_nobail, false, "Skate 3",
                    "Portal 'boots': experimental no-bail mode. Clears the game's wipeout flag every frame so landings "
                    "and crashes do not throw the skater off the board.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_fpv_autocentre, true, "Skate 3",
                    "Portal first person: with no look input for a moment, the camera eases toward the direction you "
                    "are travelling (the right stick is the flick stick while skating, so it cannot always look).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_fpv_rider, false, "Skate 3",
                    "Portal first-person view sits on the rider's head (follows the skater through a bail) instead "
                    "of on the board.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(skate3_portal_pad_modifier, "dpad_down", "Skate 3",
                      "Pad button(s) held for portal mode (RT blue, LT orange, Y clear, R3 first person, right stick "
                      "looks). Buttons joined by '+': a b x y lb rb l3 r3 back start dpad_up dpad_down dpad_left "
                      "dpad_right.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_gate_world, true, "Skate 3",
                    "Draw the portals inside the game world (depth-tested, behind objects, opaque) instead of as an "
                    "ImGui overlay on top of everything.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_gate_props, true, "Skate 3",
                    "Loose physics bodies (props, a dropped board) that reach an open portal moving into it come out of the "
                    "other one, velocity turned.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_gate_clones, true, "Skate 3",
                    "Draw a copy of the skater, board and props that are partly through a portal coming out of the other "
                    "portal.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_gate_clock, true, "Skate 3",
                    "Show the wall-clock time (same clock as the log's timestamps) in the top right, so screenshots can be matched to the log.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_gate_seethru, true, "Skate 3",
                    "Draw the view through each portal (a second pass from a camera on the far side of the other portal).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::portal_gate {
namespace {

constexpr float kFlatNy = 0.7f;  // |normal.y| above this (within ~45 degrees of flat) behaves as a floor/ceiling portal
constexpr float kHalfWidth = 2.6f;  // metres: portals are tall rounded rectangles, easier to hit than rings
constexpr float kHalfHeight = 2.0f;
constexpr float kWallTrigger = 1.1f;  // metres in front of a wall portal (was 0.8: fast or angled approaches hit the wall first)
constexpr float kTriggerMargin = 0.5f;  // the crossing test is a little bigger than the drawn rectangle, to count the board's width
constexpr float kFlatTrigger = 0.95f;  // metres above a floor / below a ceiling portal; also the exit offset of both
// A falling skater must cross while still airborne: at 0.95 m (riding height is ~0.8) the board had already touched
// the floor, which grounded the skater, cushioned the fall and ended tricks. Floors trigger higher.
constexpr float kFloorTrigger = 2.2f;
constexpr float kAhead = 7.0f;      // fallback placement distance in front of the skater
constexpr float kEntityAboveGround = 0.8f;  // skater entity height above the ground while riding
constexpr int kFlatCooldownFrames = 2;  // the endless fall outruns longer: 8 frames is 9 m at 60 m/s and the skater fell through (run 143)
constexpr int kCooldownFrames = 45;  // ~0.75 s: after exiting, do not re-enter a portal on the way out

struct Gate {
  bool active = false;
  float c[3] = {};
  // Frame, right-handed: r x u = n. n is the front normal (the side you see the portal from), u is "up" within
  // the portal plane (world up on a wall; the shooter's heading on a floor or ceiling), r is across.
  float n[3] = {0, 0, 1}, u[3] = {0, 1, 0}, r[3] = {1, 0, 0};
  // How far in front of the drawn portal the crossing triggers. A wall is solid, so the skater's position
  // never reaches the wall plane (it stops ~0.5 m short); the trigger sits where the skater can actually get.
  float trigger = 0.0f;
  bool Flat() const { return std::abs(n[1]) > kFlatNy; }  // floor, ceiling or gentle slope
};

void SetFrame(Gate& g, const float n[3], const float u[3]) {
  std::copy(n, n + 3, g.n);
  std::copy(u, u + 3, g.u);
  g.r[0] = u[1] * n[2] - u[2] * n[1];  // r = u x n
  g.r[1] = u[2] * n[0] - u[0] * n[2];
  g.r[2] = u[0] * n[1] - u[1] * n[0];
}

// Portals are ovals (like the real ones): `grow` widens the oval by that many metres all round.
bool InOval(float lat, float vert, float grow) {
  const float a = kHalfWidth + grow, b = kHalfHeight + grow;
  return lat * lat / (a * a) + vert * vert / (b * b) < 1.0f;
}

float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

std::mutex g_mutex;
Gate g_gates[2];
std::atomic<bool> g_both_placed{false};  // mirrors g_gates[0/1].active for the collision hot path
std::atomic<int> g_pending{-1};
float g_prev[3] = {};
bool g_have_prev = false;
int g_cooldown = 0;
int g_standing_block = 0;  // frames after a crossing in which merely standing in a floor opening does not cross (overlapping floor portals ping-ponged 20x a second, run 170)
float g_tracked[3] = {};
bool g_tracked_valid = false;
// Mouse reticle, written by the overlay (UI thread), read by the game thread: aim as normalised device coordinates.
std::atomic<float> g_aim_x{0.0f}, g_aim_y{0.0f};
std::atomic<float> g_pad_x{0.0f}, g_pad_y{0.1f};  // the pad's reticle (right stick, while portal mode is held), normalised device coordinates
std::atomic<bool> g_aim_free{false};
std::atomic<int64_t> g_fire_ns{0};  // last accepted shot, for the muzzle flash
std::atomic<int> g_fire_which{0};
std::atomic<bool> g_fpv{false};
std::atomic<int64_t> g_surface_ns{0};
std::atomic<float> g_travel_x{0.0f}, g_travel_z{0.0f}, g_travel_speed{0.0f};
std::atomic<int64_t> g_look_ns{0};  // last time the player moved the look (mouse or stick)
float g_head[3] = {};
bool g_head_valid = false;
std::chrono::steady_clock::time_point g_head_t{};
std::atomic<float> g_look_dx{0.0f}, g_look_dy{0.0f};  // pixels of relative mouse motion not yet consumed
std::atomic<float> g_wall_clear{1.5f};  // metres between a wall portal's trigger plane and where the skater comes out

int64_t now_ns() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

std::shared_ptr<const native_scene::FrameScene> Scene() {
  std::lock_guard<std::mutex> lock(native_scene::g_scene_mutex);
  return native_scene::g_scene;
}

// General 4x4 inverse (row-major), false if singular.
bool Invert4(const float m[16], float out[16]) {
  double a[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      a[r][c] = m[r * 4 + c];
      a[r][4 + c] = r == c ? 1.0 : 0.0;
    }
  }
  for (int col = 0; col < 4; ++col) {
    int piv = col;
    for (int r = col + 1; r < 4; ++r) {
      if (std::abs(a[r][col]) > std::abs(a[piv][col])) piv = r;
    }
    if (std::abs(a[piv][col]) < 1e-12) return false;
    for (int c = 0; c < 8; ++c) std::swap(a[col][c], a[piv][c]);
    const double inv = 1.0 / a[col][col];
    for (int c = 0; c < 8; ++c) a[col][c] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == col) continue;
      const double f = a[r][col];
      for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
    }
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = float(a[r][4 + c]);
  }
  return true;
}

// World point under a normalised-device coordinate (row-vector convention: clip = [p 1] * view_proj).
bool Unproject(const float inv[16], float x, float y, float z, float out[3]) {
  float v[4];
  for (int c = 0; c < 4; ++c) v[c] = x * inv[c] + y * inv[4 + c] + z * inv[8 + c] + inv[12 + c];
  if (std::abs(v[3]) < 1e-9f) return false;
  for (int k = 0; k < 3; ++k) out[k] = v[k] / v[3];
  return true;
}

}  // namespace

uint16_t ModifierMask() {
  static const struct {
    const char* name;
    uint16_t bit;
  } kTokens[] = {{"a", 0x1000},  {"b", 0x2000},  {"x", 0x4000},   {"y", 0x8000},       {"lb", 0x0100},
                 {"rb", 0x0200}, {"l3", 0x0040}, {"r3", 0x0080},  {"back", 0x0020},    {"select", 0x0020},
                 {"view", 0x0020}, {"start", 0x0010}, {"menu", 0x0010}, {"dpad_up", 0x0001},
                 {"dpad_down", 0x0002}, {"dpad_left", 0x0004}, {"dpad_right", 0x0008}};  // XInput bits
  const std::string spec = REXCVAR_GET(skate3_portal_pad_modifier);
  uint16_t mask = 0;
  size_t i = 0;
  while (i <= spec.size()) {
    size_t j = spec.find('+', i);
    if (j == std::string::npos) j = spec.size();
    const std::string tok = spec.substr(i, j - i);
    for (const auto& t : kTokens) {
      if (tok == t.name) mask |= t.bit;
    }
    i = j + 1;
  }
  return mask ? mask : 0x0002;  // D-pad down
}

bool Enabled() { return REXCVAR_GET(skate3_portal_gate); }

void Request(int what) {
  // The bind auto-repeats while the key is held; one press is enough.
  static std::atomic<int64_t> last_ns{0};
  const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
  REXLOG_INFO("portal-gate: key bind {} received (gate {})", what, Enabled() ? "enabled" : "DISABLED");
  if (Enabled() && now - last_ns.load() > 500'000'000) {
    last_ns = now;
    if (what == 2 && portal_gel::Mode()) {  // in gel mode "clear" removes the paint, the portals stay
      portal_gel::Clear();
      return;
    }
    g_pending = what;
    if (what == 0 || what == 1) {
      g_fire_which = what;
      g_fire_ns = now;
    }
  }
}

bool TakeFire(int* which) {
  int p = g_pending.load();
  if ((p == 0 || p == 1) && g_pending.compare_exchange_strong(p, -1)) {
    *which = p;
    return true;
  }
  return false;
}

bool AimRay(float origin[3], float dir[3]) {
  const auto scene = Scene();
  float inv[16], near_pt[3], far_pt[3];
  if (!scene || !Invert4(scene->view_proj, inv) || !Unproject(inv, g_aim_x, g_aim_y, 0.1f, near_pt) ||
      !Unproject(inv, g_aim_x, g_aim_y, 0.9f, far_pt)) {
    return false;
  }
  float len = 0;
  for (int k = 0; k < 3; ++k) {
    dir[k] = far_pt[k] - near_pt[k];
    len += dir[k] * dir[k];
  }
  len = std::sqrt(len);
  if (!(len > 1e-6f)) return false;
  for (int k = 0; k < 3; ++k) {
    dir[k] /= len;
    origin[k] = scene->cam_pos[k];
  }
  return true;
}

bool AimFree() { return g_aim_free; }

bool InsideOpening(const float p[3], float slack) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_gates[0].active || !g_gates[1].active) return false;
  for (const Gate& g : g_gates) {
    const float rel[3] = {p[0] - g.c[0], p[1] - g.c[1], p[2] - g.c[2]};
    if (std::abs(Dot3(rel, g.n)) < slack && InOval(Dot3(rel, g.r), Dot3(rel, g.u), 0.0f)) {
      return true;
    }
  }
  return false;
}

bool NearPortal(const float p[3], float radius) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const Gate& g : g_gates) {
    const float d[3] = {p[0] - g.c[0], p[1] - g.c[1], p[2] - g.c[2]};
    if (g.active && Dot3(d, d) < radius * radius) return true;
  }
  return false;
}

static float g_props[24][6];
static int g_prop_n = 0;
static float g_chars[48][3];
static int g_char_n = 0;

void SetCharacterPositions(const float (*p)[3], int n) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_char_n = std::min(n, 48);
  for (int i = 0; i < g_char_n; ++i) std::copy(p[i], p[i] + 3, g_chars[i]);
}

static bool NearCharacterLocked(const float p[3], float radius) {
  for (int i = 0; i < g_char_n; ++i) {
    const float d[3] = {p[0] - g_chars[i][0], p[1] - g_chars[i][1], p[2] - g_chars[i][2]};
    if (Dot3(d, d) < radius * radius) return true;
  }
  return false;
}

bool NearCharacter(const float p[3], float radius) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return NearCharacterLocked(p, radius);
}

void SetPropPositions(const float (*pv)[6], int n) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_prop_n = std::min(n, 24);
  for (int i = 0; i < g_prop_n; ++i) std::copy(pv[i], pv[i] + 6, g_props[i]);
}

bool PropRescue(const float p[3], float out[3]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_gates[0].active || !g_gates[1].active) return false;
  const Gate& low = g_gates[0].c[1] < g_gates[1].c[1] ? g_gates[0] : g_gates[1];
  constexpr float kBelow = 3.0f;  // well under both portals: a legitimate loop would have crossed long before
  // a wall portal's centre is ~1.7 m above the ground it stands on
  if (p[1] > low.c[1] - kBelow - (low.Flat() ? 0.0f : 1.7f)) return false;
  float dx = p[0] - low.c[0], dz = p[2] - low.c[2];
  const float len = std::hypot(dx, dz);
  if (len < 0.01f) dx = 1.0f, dz = 0.0f;
  else dx /= len, dz /= len;
  const float rim = kHalfWidth + 0.8f;  // beside the opening, on the surface it fell off
  out[0] = low.c[0] + dx * std::max(len, rim);
  out[1] = low.c[1] + (low.Flat() ? 0.6f : -0.9f);
  out[2] = low.c[2] + dz * std::max(len, rim);
  return true;
}

static std::atomic<int64_t> g_grind_capture_until{0};
void RequestGrindCapture() {
  g_grind_capture_until = now_ns() + 8'000'000'000ll;
  REXLOG_INFO("portal-gate: grind capture on for 8 s");
}
bool GrindCaptureOn() { return now_ns() < g_grind_capture_until.load(std::memory_order_relaxed); }
static std::atomic<bool> g_adopt{false};
void RequestAdopt() { g_adopt.store(true); }
bool TakeAdopt() { return g_adopt.exchange(false); }
static std::atomic<bool> g_wake{false};
void RequestWake() { g_wake.store(true); }
bool TakeWake() { return g_wake.exchange(false); }
static std::atomic<bool> g_selftest{false};
void RequestSelfTest() { g_selftest.store(true); }
bool TakeSelfTest() { return g_selftest.exchange(false); }

bool SelfTestSpot(int scenario, float pos[3], float vel[3], float* low_y, bool* flat) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_gates[0].active || !g_gates[1].active) return false;
  const Gate& e = g_gates[0].n[1] >= g_gates[1].n[1] ? g_gates[0] : g_gates[1];  // a floor portal first (it falls in), then a wall (tossed)
  const float back = scenario == 2 ? 3.5f : 2.5f;
  const float side = scenario == 1 ? 0.8f : 0.0f;
  // Into a flat portal it just falls; into a wall portal it is thrown.
  const float speed = e.Flat() ? (scenario == 2 ? 8.0f : 0.0f) : (scenario == 2 ? 8.0f : 5.0f);
  for (int k = 0; k < 3; ++k) {
    pos[k] = e.c[k] + e.n[k] * back + e.r[k] * side;
    vel[k] = -e.n[k] * speed;
  }
  *low_y = std::min(g_gates[0].c[1], g_gates[1].c[1]);
  *flat = e.Flat();
  return true;
}

// ---- Sounds: small synthesised effects through their own SDL stream (the game's audio goes through the emulated XMA path) ----
namespace {
std::mutex g_sfx_mutex;
std::vector<float> g_sfx_bank[6];
SDL_AudioStream* g_sfx_stream[6] = {};
bool g_sfx_failed = false, g_sfx_ready = false;
constexpr int kSfxRate = 44100;

std::vector<float> SynthSfx(int kind) {
  const float durs[6] = {0.26f, 0.26f, 0.32f, 0.38f, 0.16f, 0.2f};
  const int n = int(durs[kind] * kSfxRate);
  std::vector<float> out(n);
  float ph = 0.0f;
  uint32_t rng = 12345u + kind;
  float lp = 0.0f;
  for (int i = 0; i < n; ++i) {
    const float t = float(i) / kSfxRate, u = t / durs[kind];
    rng = rng * 1664525u + 1013904223u;
    const float noise = float(int32_t(rng)) / 2147483648.0f;
    float f = 0.0f, v = 0.0f;
    const float attack = std::min(1.0f, t / 0.008f);
    switch (kind) {
      case 0:  // blue shot: rising chirp
        f = 520.0f + 640.0f * std::pow(u, 0.7f);
        ph += 6.2831853f * f / kSfxRate;
        v = (std::sin(ph) * 0.6f + std::sin(2.0f * ph) * 0.25f) * std::exp(-5.0f * t);
        break;
      case 1:  // orange shot: falling chirp
        f = 780.0f - 430.0f * u;
        ph += 6.2831853f * f / kSfxRate;
        v = (std::sin(ph) * 0.6f + std::sin(1.5f * ph) * 0.25f) * std::exp(-5.0f * t);
        break;
      case 2:  // through a portal: whoosh
        lp += (noise - lp) * (0.25f - 0.2f * u);
        f = 190.0f - 110.0f * u;
        ph += 6.2831853f * f / kSfxRate;
        v = (lp * 1.6f + std::sin(ph) * 0.3f) * std::sin(3.1415927f * u);
        break;
      case 3:  // clear: two falling sweeps with a tremolo
        f = 920.0f - 480.0f * u;
        ph += 6.2831853f * f / kSfxRate;
        v = std::sin(ph) * (0.6f + 0.4f * std::sin(6.2831853f * 18.0f * t)) * std::exp(-4.0f * t);
        break;
      case 4:  // companion cube pop
        f = 260.0f - 150.0f * u;
        ph += 6.2831853f * f / kSfxRate;
        v = std::sin(ph) * std::exp(-16.0f * t) + noise * 0.15f * std::exp(-60.0f * t);
        break;
      default:  // no surface: low fizzle
        lp += (noise - lp) * 0.08f;
        v = lp * 2.0f * std::exp(-9.0f * t);
        break;
    }
    out[i] = v * attack * 0.8f;
  }
  return out;
}
}  // namespace

// The real Portal 2 effects, read at startup from the user's own install (never copied or bundled): the directory holding pak01_dir.vpk.
// Source VPK v1 (12-byte header: sig, version, tree size) or v2 (28 bytes), then the tree (extension / path / name strings and a 18-byte record each),
// file data in pak01_NNN.vpk (archive index) or, for 0x7FFF, after the tree in the directory file.
std::vector<uint8_t> ReadVpkFile(const std::string& dir, const std::string& want) {
  std::ifstream f(dir + "/pak01_dir.vpk", std::ios::binary);
  if (!f) return {};
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 28) return {};
  auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= d.size()) std::memcpy(&v, &d[o], 4); return v; };
  auto u16 = [&](size_t o) { uint16_t v = 0; if (o + 2 <= d.size()) std::memcpy(&v, &d[o], 2); return v; };
  const uint32_t version = u32(4);
  if (u32(0) != 0x55AA1234u || (version != 1 && version != 2)) return {};  // the macOS Steam copy is version 1 (12-byte header)
  const size_t header = version == 2 ? 28 : 12;
  const size_t tree_end = header + u32(8);
  size_t pos = header;
  auto cstr = [&]() { std::string r; while (pos < tree_end && d[pos]) r += char(d[pos++]); ++pos; return r; };
  while (pos < tree_end) {
    const std::string ext = cstr();
    if (ext.empty()) break;
    for (std::string path = cstr(); !path.empty(); path = cstr()) {
      for (std::string name = cstr(); !name.empty(); name = cstr()) {
        const uint16_t pre = u16(pos + 4), arch = u16(pos + 6);
        const uint32_t off = u32(pos + 8), len = u32(pos + 12);
        const size_t rec = pos;
        pos += 18 + pre;
        if (path + "/" + name + "." + ext != want) continue;
        std::vector<uint8_t> out(d.begin() + rec + 18, d.begin() + rec + 18 + pre);
        if (len == 0) return out;
        out.resize(pre + len);
        if (arch == 0x7FFF) {
          if (tree_end + off + len > d.size()) return {};
          std::memcpy(&out[pre], &d[tree_end + off], len);
        } else {
          char nm[32];
          std::snprintf(nm, sizeof(nm), "/pak01_%03d.vpk", arch);
          std::ifstream a(dir + nm, std::ios::binary);
          if (!a) return {};
          a.seekg(off);
          a.read(reinterpret_cast<char*>(&out[pre]), len);
          if (!a) return {};
        }
        return out;
      }
    }
  }
  return {};
}

std::vector<float> LoadPortalWav(const std::string& dir, const char* vpk_path) {
  const std::vector<uint8_t> raw = ReadVpkFile(dir, vpk_path);
  if (raw.empty()) {
    REXLOG_INFO("portal-gate: sound '{}' not found or unreadable in the Portal 2 archive", vpk_path);
    return {};
  }
  SDL_AudioSpec spec;
  uint8_t* data = nullptr;
  uint32_t len = 0;
  if (!SDL_LoadWAV_IO(SDL_IOFromConstMem(raw.data(), raw.size()), true, &spec, &data, &len)) {
    REXLOG_INFO("portal-gate: sound '{}' read ({} bytes) but SDL could not decode it: {}", vpk_path, raw.size(), SDL_GetError());
    return {};
  }
  const SDL_AudioSpec want = {SDL_AUDIO_F32, 1, kSfxRate};
  uint8_t* conv = nullptr;
  int conv_len = 0;
  std::vector<float> out;
  if (SDL_ConvertAudioSamples(&spec, data, int(len), &want, &conv, &conv_len)) {
    out.assign(reinterpret_cast<float*>(conv), reinterpret_cast<float*>(conv) + conv_len / sizeof(float));
    SDL_free(conv);
  } else {
    REXLOG_INFO("portal-gate: sound '{}' decoded ({} bytes, {} Hz, {} ch) but conversion failed: {}", vpk_path, len, spec.freq, spec.channels, SDL_GetError());
  }
  SDL_free(data);
  return out;
}

struct SfxVoice {
  int kind;
  size_t pos;
};
std::vector<SfxVoice> g_sfx_voices;  // under g_sfx_mutex
std::mutex g_sfx_init_mutex;

// SDL asks for more audio on its own thread: mix every playing sound into one block. (Several streams on the default device stayed silent.)
void SDLCALL SfxMix(void*, SDL_AudioStream* stream, int additional, int) {
  const int frames = additional / int(sizeof(float));
  if (frames <= 0) return;
  std::vector<float> mix(size_t(frames), 0.0f);
  const float vol = float(std::clamp(REXCVAR_GET(skate3_portal_sfx_volume), 0.0, 1.0));
  {
    std::lock_guard<std::mutex> lock(g_sfx_mutex);
    for (SfxVoice& v : g_sfx_voices) {
      const std::vector<float>& s = g_sfx_bank[v.kind];
      for (int i = 0; i < frames && v.pos < s.size(); ++i, ++v.pos) mix[size_t(i)] += s[v.pos] * vol;
    }
    g_sfx_voices.erase(std::remove_if(g_sfx_voices.begin(), g_sfx_voices.end(), [](const SfxVoice& v) { return v.pos >= g_sfx_bank[v.kind].size(); }),
                       g_sfx_voices.end());
  }
  for (float& x : mix) x = std::clamp(x, -1.0f, 1.0f);
  SDL_PutAudioStreamData(stream, mix.data(), int(mix.size() * sizeof(float)));
}

void Sfx(int kind) {
  if (kind < 0 || kind > 5 || !REXCVAR_GET(skate3_portal_sfx)) return;
  {
    std::lock_guard<std::mutex> init(g_sfx_init_mutex);  // not under g_sfx_mutex: the audio thread takes that one while SDL holds its device lock
    if (g_sfx_failed) return;
    if (!g_sfx_ready) {
      SDL_InitSubSystem(SDL_INIT_AUDIO);
      // blue shot, orange shot, enter, clear, companion cube, no surface (the cube borrows the gun's power-up)
      static const char* const kReal[6] = {"sound/weapons/portalgun/portalgun_shoot_blue1.wav", "sound/weapons/portalgun/portalgun_shoot_red1.wav",
                                           "sound/weapons/portalgun/portal_enter_02.wav",       "sound/weapons/portalgun/portal_close1.wav",
                                           "sound/weapons/portalgun_powerup1.wav",              "sound/weapons/portalgun/portal_invalid_surface3.wav"};
      std::string dir = REXCVAR_GET(skate3_portal_sfx_dir);
      if (dir.empty()) {
        const char* home = std::getenv("HOME");
        if (home) dir = std::string(home) + "/Library/Application Support/Steam/steamapps/common/Portal 2/portal2";
      }
      int real = 0;
      std::vector<float> bank[6];
      for (int k = 0; k < 6; ++k) {
        bank[k] = LoadPortalWav(dir, kReal[k]);
        if (bank[k].empty()) {
          REXLOG_INFO("portal-gate: could not load {} from '{}' ({}); using a synthesised sound", kReal[k], dir, SDL_GetError());
          bank[k] = SynthSfx(k);
        } else {
          ++real;
        }
      }
      {
        std::lock_guard<std::mutex> lock(g_sfx_mutex);
        for (int k = 0; k < 6; ++k) g_sfx_bank[k] = std::move(bank[k]);
      }
      const SDL_AudioSpec spec = {SDL_AUDIO_F32, 1, kSfxRate};
      g_sfx_stream[0] = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, SfxMix, nullptr);
      if (!g_sfx_stream[0]) {
        g_sfx_failed = true;
        REXLOG_INFO("portal-gate: sounds unavailable: {}", SDL_GetError());
        return;
      }
      SDL_ResumeAudioStreamDevice(g_sfx_stream[0]);
      g_sfx_ready = true;
      REXLOG_INFO("portal-gate: sounds: {} of 6 are the Portal 2 effects from '{}', the rest synthesised", real, dir);
    }
  }
  std::lock_guard<std::mutex> lock(g_sfx_mutex);
  if (g_sfx_voices.size() < 12) g_sfx_voices.push_back({kind, 0});
}

bool GunVisible() { return Enabled() && REXCVAR_GET(skate3_portal_gun_model); }

void AimNdc(float* x, float* y) {
  if (REXCVAR_GET(skate3_portal_fpv)) {
    *x = *y = 0.0f;
  } else if (REXCVAR_GET(skate3_portal_gate_mouse) && g_aim_free.load()) {
    *x = g_aim_x.load();
    *y = g_aim_y.load();
  } else {
    *x = g_pad_x.load();
    *y = g_pad_y.load();
  }
}

float GunKick(int* which) {
  *which = g_fire_which;
  const int64_t t = g_fire_ns.load();
  return t == 0 ? 0.0f : std::exp(-float(now_ns() - t) * 1e-9f / 0.12f);
}

bool HoleArmed() { return g_both_placed.load(std::memory_order_relaxed) && REXCVAR_GET(skate3_portal_gate_hole); }

bool HoleSkips(const float mover[3], const float tri_normal[3], const float tri_point[3]) {
  constexpr float kPlane = 0.4f;   // how far a triangle may sit from the portal plane and still be "the portal's surface"
  constexpr float kDepth = 2.5f;   // the shape may be this far in front of or behind the plane
  constexpr float kEdge = 0.3f;    // the shape's own radius, so its rim does not catch on the hole's edge
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_gates[0].active || !g_gates[1].active) return false;
  // Only the skater and their board get the hole: props and NPCs keep a solid floor until they can cross (step 2).
  // shortcut: identity is "within kSkater of the tracked skater", so a prop right beside the skater falls in too.
  constexpr float kSkater = 3.0f;
  // A loose prop (matched to the mover by position; the probe knows which bodies are the skater's own) gets the hole only while it is
  // entering an open portal, so it passes through the opening and crosses at the real plane. Otherwise it keeps a solid floor.
  const float* prop = nullptr;
  const float to_skater[3] = {mover[0] - g_tracked[0], mover[1] - g_tracked[1], mover[2] - g_tracked[2]};
  const bool at_skater = g_tracked_valid && Dot3(to_skater, to_skater) < 0.8f * 0.8f;  // the board and feet: the skater's own hole rules
  // Another character's body (an NPC skater standing or riding over the opening, a pedestrian): solid floor. Runs 180/181: NPCs dropped
  // into the hole and were then moved as props. The 1.3 m reaches from the entity's origin to its feet and board.
  if (!at_skater && NearCharacterLocked(mover, 1.3f)) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 10) {
      REXLOG_INFO("portal-gate: HOLE kept solid under another character at ({:.2f}, {:.2f}, {:.2f})", mover[0], mover[1], mover[2]);
    }
    return false;
  }
  for (int i = 0; i < g_prop_n && !prop && !at_skater; ++i) {
    const float d[3] = {mover[0] - g_props[i][0], mover[1] - g_props[i][1], mover[2] - g_props[i][2]};
    if (Dot3(d, d) < 0.7f * 0.7f) prop = g_props[i];
  }
  if (prop) {
    bool entering = false;
    for (const Gate& g : g_gates) {
      const float rel[3] = {prop[0] - g.c[0], prop[1] - g.c[1], prop[2] - g.c[2]};
      const float vn = Dot3(prop + 3, g.n);
      if (std::abs(Dot3(rel, g.n)) < kDepth && InOval(Dot3(rel, g.r), Dot3(rel, g.u), kEdge + 0.2f) && (g.Flat() ? vn <= 0.3f : vn < -0.5f)) {
        entering = true;
      }
    }
    if (!entering) return false;
  } else {
    const float from_skater[3] = {mover[0] - g_tracked[0], mover[1] - g_tracked[1], mover[2] - g_tracked[2]};
    if (!g_tracked_valid || Dot3(from_skater, from_skater) > kSkater * kSkater) return false;
  }
  for (const Gate& g : g_gates) {
    if (std::abs(Dot3(tri_normal, g.n)) < 0.9f) continue;
    const float to_tri[3] = {tri_point[0] - g.c[0], tri_point[1] - g.c[1], tri_point[2] - g.c[2]};
    if (std::abs(Dot3(to_tri, g.n)) > kPlane) continue;
    const float rel[3] = {mover[0] - g.c[0], mover[1] - g.c[1], mover[2] - g.c[2]};
    if (std::abs(Dot3(rel, g.n)) < kDepth && InOval(Dot3(rel, g.r), Dot3(rel, g.u), kEdge)) {
      return true;
    }
  }
  return false;
}

int SurfaceShapes(SurfaceShape out[2]) {
  if (!Enabled() || !REXCVAR_GET(skate3_portal_gate_world)) return 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  int n = 0;
  constexpr int kPoints = 28;
  constexpr float kRim = 0.35f;
  for (int i = 0; i < 2; ++i) {
    const Gate& g = g_gates[i];
    if (!g.active) continue;
    SurfaceShape& sh = out[n++];
    sh.portal_index = i;
    sh.count = kPoints;
    std::copy(g.c, g.c + 3, sh.centre);
    std::copy(g.n, g.n + 3, sh.normal);
    for (int layer = 0; layer < 2; ++layer) {
      const float hw = layer == 0 ? kHalfWidth : kHalfWidth - kRim;
      const float hh = layer == 0 ? kHalfHeight : kHalfHeight - kRim;
      const float lift = layer == 0 ? 0.02f : 0.03f;  // keep the core just in front of the rim
      for (int k = 0; k < kPoints; ++k) {
        const float a = 6.2831853f * float(k) / float(kPoints);
        const float u = std::cos(a) * hw, v = std::sin(a) * hh;
        float* p = layer == 0 ? sh.rim[k] : sh.core[k];
        for (int c = 0; c < 3; ++c) p[c] = g.c[c] + u * g.r[c] + v * g.u[c] + g.n[c] * lift;
      }
    }
  }
  return n;
}

// Entry portal `e` -> exit portal `x`: fwd takes a point near e to the same point near x (row-vector 4x4), inv is the
// reverse. The crossing rule: (lat, vert, z) in the entry frame comes out at (-lat, vert, -z) in the exit frame.
static void PairMatrices(const Gate& e, const Gate& x, float inv[16], float fwd[16]) {
  // R (column-vector form) = -x.r e.r^T + x.u e.u^T - x.n e.n^T.
  float R[3][3];
  for (int k = 0; k < 3; ++k) {
    for (int j = 0; j < 3; ++j) R[k][j] = -x.r[k] * e.r[j] + x.u[k] * e.u[j] - x.n[k] * e.n[j];
  }
  std::fill(fwd, fwd + 16, 0.0f);
  std::fill(inv, inv + 16, 0.0f);
  for (int j = 0; j < 3; ++j) {
    for (int k = 0; k < 3; ++k) {
      fwd[j * 4 + k] = R[k][j];  // p' = x.c + R (p - e.c)
      inv[j * 4 + k] = R[j][k];  // p = e.c + R^T (p' - x.c)
    }
  }
  for (int k = 0; k < 3; ++k) {
    float tf = x.c[k], ti = e.c[k];
    for (int j = 0; j < 3; ++j) {
      tf -= e.c[j] * R[k][j];
      ti -= x.c[j] * R[j][k];
    }
    fwd[12 + k] = tf;
    inv[12 + k] = ti;
  }
  fwd[15] = inv[15] = 1.0f;
}

bool ViewThrough(int look_at, const float cam[3], float inv[16], float fwd[16], float plane[4]) {
  if (!Enabled() || !REXCVAR_GET(skate3_portal_gate_seethru)) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_gates[0].active || !g_gates[1].active) return false;
  const Gate& e = g_gates[look_at];
  const Gate& x = g_gates[1 - look_at];
  const float rel[3] = {cam[0] - e.c[0], cam[1] - e.c[1], cam[2] - e.c[2]};
  if (Dot3(rel, e.n) <= 0.02f) return false;  // behind (or in) the portal: the view is the other side's problem
  PairMatrices(e, x, inv, fwd);
  constexpr float kBehind = 0.04f;  // the wall's own face sits at the plane: clip just in front of it
  for (int k = 0; k < 3; ++k) plane[k] = x.n[k];
  plane[3] = -Dot3(x.n, x.c) - kBehind;
  return true;
}

bool PortalFrame(int from, float fwd[16], float c[3], float n[3], float r[3], float u[3]) {
  if (!Enabled() || !REXCVAR_GET(skate3_portal_gate_clones)) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_gates[0].active || !g_gates[1].active) return false;
  const Gate& e = g_gates[from];
  float inv[16];
  PairMatrices(e, g_gates[1 - from], inv, fwd);
  std::copy(e.c, e.c + 3, c);
  std::copy(e.n, e.n + 3, n);
  std::copy(e.r, e.r + 3, r);
  std::copy(e.u, e.u + 3, u);
  return true;
}

bool CubeCross(const float prev[3], const float now[3], const float v[3], float out_p[3], float out_v[3], float R[9]) {
  if (!g_both_placed.load(std::memory_order_relaxed)) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_gates[0].active || !g_gates[1].active) return false;
  for (int i = 0; i < 2; ++i) {
    const Gate& e = g_gates[i];
    const Gate& x = g_gates[1 - i];
    const float rel[3] = {now[0] - e.c[0], now[1] - e.c[1], now[2] - e.c[2]};
    const float prel[3] = {prev[0] - e.c[0], prev[1] - e.c[1], prev[2] - e.c[2]};
    const float d = Dot3(rel, e.n), pd = Dot3(prel, e.n);
    const float lat = Dot3(rel, e.r), vert = Dot3(rel, e.u);
    if (!(pd > 0.0f && d <= 0.0f) || !InOval(lat, vert, 0.1f)) continue;
    const float out_dist = -d + 0.02f;
    for (int k = 0; k < 3; ++k) out_p[k] = x.c[k] - lat * x.r[k] + vert * x.u[k] + out_dist * x.n[k];
    for (int k = 0; k < 3; ++k) {
      for (int j = 0; j < 3; ++j) R[k * 3 + j] = -x.r[k] * e.r[j] + x.u[k] * e.u[j] - x.n[k] * e.n[j];
    }
    for (int k = 0; k < 3; ++k) out_v[k] = R[k * 3] * v[0] + R[k * 3 + 1] * v[1] + R[k * 3 + 2] * v[2];
    const float sp = std::sqrt(Dot3(out_v, out_v));
    if (sp > 45.0f) {
      for (int k = 0; k < 3; ++k) out_v[k] *= 45.0f / sp;
    }
    return true;
  }
  return false;
}

bool TrackedPos(float out[3]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_tracked_valid) return false;
  std::copy(g_tracked, g_tracked + 3, out);
  return true;
}

bool BodiesArmed() { return g_both_placed.load(std::memory_order_relaxed) && REXCVAR_GET(skate3_portal_gate_props); }

// Bodies seen close to the skater (the board and skeleton parts are rigid bodies too). A body keeps the label for a while,
// so the skater's parts stay excluded through a crossing, when they are briefly far from the tracked position.
std::unordered_map<uint32_t, int64_t> g_skater_bodies;
// Where the skater last crossed (entry and exit points): the game's teleport relocates the skater's whole skeleton there, bodies
// that were never near the skater before, so they get a no-props zone for a few seconds.
float g_zone[2][3] = {};
int64_t g_zone_ns = 0;
float g_cross_R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // the latest human crossing's velocity rotation (camera carry)
unsigned g_cross_seq = 0;
// The same zone for NPC actor crossings (skate3_portal_npc), a few at once.
struct ActorZone {
  float p[2][3] = {};
  int64_t ns = 0;
};
constexpr int kActorZones = 8;
ActorZone g_actor_zones[kActorZones];
int g_actor_zone_next = 0;

// Called with g_mutex held. True for the skater's own bodies (close to the tracked position, inside the post-crossing zone, or
// labelled so a moment ago); labels what it finds.
static bool SkaterBodyLocked(uint32_t id, const float p[3], int64_t now) {
  // Only the zone around the skater's last crossing: the game's teleport relocates the skater's skeleton there, bodies that were never
  // near the skater before. The skater's ordinary bodies are told apart by the probe (updates spent within 1.5 m of the skater); a
  // 3 m radius and a 10 s label here (run 138) kept a bin dropped or carried near the skater from ever crossing.
  if (g_zone_ns != 0 && now - g_zone_ns < 4'000'000'000ll) {
    for (const auto& z : g_zone) {
      const float s[3] = {p[0] - z[0], p[1] - z[1], p[2] - z[2]};
      if (Dot3(s, s) < 5.0f * 5.0f) {
        g_skater_bodies[id] = now;
        return true;
      }
    }
  }
  for (const ActorZone& z : g_actor_zones) {
    if (z.ns == 0 || now - z.ns >= 4'000'000'000ll) continue;
    for (const auto& q : z.p) {
      const float s[3] = {p[0] - q[0], p[1] - q[1], p[2] - q[2]};
      if (Dot3(s, s) < 5.0f * 5.0f) return true;
    }
  }
  (void)id;
  return false;
}

unsigned LastCrossing(float R[9], long long* ns, float entry[3], float exit[3]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::copy(g_cross_R, g_cross_R + 9, R);
  *ns = g_zone_ns;
  std::copy(g_zone[0], g_zone[0] + 3, entry);
  std::copy(g_zone[1], g_zone[1] + 3, exit);
  return g_cross_seq;
}

void NoteActorCrossing(const float entry[3], const float exit[3]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  ActorZone& z = g_actor_zones[g_actor_zone_next++ % kActorZones];
  std::copy(entry, entry + 3, z.p[0]);
  std::copy(exit, exit + 3, z.p[1]);
  z.ns = now_ns();
}

bool PortalPairFrames(portal_math::Frame out[2]) {
  if (!g_both_placed.load(std::memory_order_relaxed)) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_gates[0].active || !g_gates[1].active) return false;
  for (int i = 0; i < 2; ++i) portal_math::SetFrame(out[i], g_gates[i].c, g_gates[i].n, g_gates[i].u, g_gates[i].trigger);
  return true;
}

bool BodyIsSkaters(uint32_t id, const float p[3]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return SkaterBodyLocked(id, p, now_ns());
}

bool BodyCross(uint32_t id, const float p[3], const float v[3], float out_p[3], float out_v[3], float R[9],
               float exit_plane[4]) {
  if (!g_both_placed.load(std::memory_order_relaxed) || !REXCVAR_GET(skate3_portal_gate_props)) return false;
  constexpr float kTrigger = 0.25f;   // a prop crosses when its centre is this close to the plane: the opening is a hole for it (was 0.7 on a solid surface)
  constexpr float kBack = 1.5f;       // ... and no further behind it than this
  constexpr float kMinInto = 0.5f;    // m/s along -normal: resting things do not cross
  constexpr float kMaxSpeed = 60.0f;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_gates[0].active || !g_gates[1].active) return false;
  if (SkaterBodyLocked(id, p, now_ns())) return false;
  for (int i = 0; i < 2; ++i) {
    const Gate& e = g_gates[i];
    const Gate& x = g_gates[1 - i];
    const float rel[3] = {p[0] - e.c[0], p[1] - e.c[1], p[2] - e.c[2]};
    const float d = Dot3(rel, e.n);
    const float lat = Dot3(rel, e.r), vert = Dot3(rel, e.u);
    if (d > kTrigger || d < -kBack || !InOval(lat, vert, 0.2f)) continue;
    // Something lying on a floor portal falls in (it never gets a speed into a solid floor); elsewhere it has to be moving in.
    if (Dot3(v, e.n) > -((e.n[1] > kFlatNy) ? 0.05f : kMinInto)) continue;
    // Same rule as the skater: (lat, vert, depth) -> (-lat, vert, ...) in the exit frame; the trigger plane sits kTrigger
    // in front of the real one, so the exit sits kTrigger in front of its plane plus however far past the trigger it was.
    const float out_dist = kTrigger + (kTrigger - d);
    for (int k = 0; k < 3; ++k) out_p[k] = x.c[k] - lat * x.r[k] + vert * x.u[k] + out_dist * x.n[k];
    if (!x.Flat()) out_p[1] = std::max(out_p[1], x.c[1] - 1.0f);
    for (int k = 0; k < 3; ++k) {
      for (int j = 0; j < 3; ++j) R[k * 3 + j] = -x.r[k] * e.r[j] + x.u[k] * e.u[j] - x.n[k] * e.n[j];
    }
    for (int k = 0; k < 3; ++k) out_v[k] = R[k * 3] * v[0] + R[k * 3 + 1] * v[1] + R[k * 3 + 2] * v[2];
    const float sp = std::sqrt(Dot3(out_v, out_v));
    if (sp > kMaxSpeed) {
      for (int k = 0; k < 3; ++k) out_v[k] *= kMaxSpeed / sp;
    }
    for (int k = 0; k < 3; ++k) exit_plane[k] = x.n[k];
    exit_plane[3] = Dot3(x.n, x.c);
    return true;
  }
  return false;
}

void NoteSurfaceDrawn() { g_surface_ns = now_ns(); }

bool SurfaceDrawnRecently() { return now_ns() - g_surface_ns.load() < 400'000'000 && g_surface_ns.load() != 0; }

bool FpvActive() { return Enabled() && REXCVAR_GET(skate3_portal_fpv); }

void ToggleFpv() {
  if (!Enabled()) return;
  const bool on = !REXCVAR_GET(skate3_portal_fpv);
  rex::cvar::SetFlagByName("skate3_portal_fpv", on ? "true" : "false");
  REXLOG_INFO("portal-gate: first-person view {}", on ? "ON (mouse looks, Ctrl+4 to leave)" : "off");
}

void FpvConsumeLook(double* dyaw, double* dpitch) {
  constexpr double kRadPerPixel = 0.0022;  // same as the freecam's mouse look
  *dyaw = double(g_look_dx.exchange(0.0f)) * kRadPerPixel;
  *dpitch = -double(g_look_dy.exchange(0.0f)) * kRadPerPixel;
  if (*dyaw != 0.0 || *dpitch != 0.0) g_look_ns = now_ns();
}

void SetTravel(float dx, float dz, float speed) {
  g_travel_x = dx;
  g_travel_z = dz;
  g_travel_speed = speed;
}

bool FpvAutoYaw(double* yaw) {
  if (!REXCVAR_GET(skate3_portal_fpv_autocentre)) return false;
  const float x = g_travel_x, z = g_travel_z;
  if (g_travel_speed < 1.5f || std::hypot(x, z) < 0.001f) return false;
  if (now_ns() - g_look_ns.load() < 1'200'000'000) return false;  // the player is looking around: leave it
  *yaw = std::atan2(double(x), double(z));
  return true;
}

std::atomic<unsigned> g_game_frame{0};
void SetGameFrame(unsigned frame) { g_game_frame.store(frame, std::memory_order_relaxed); }

void SetHead(const float p[3]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::copy(p, p + 3, g_head);
  g_head_valid = true;
  g_head_t = std::chrono::steady_clock::now();
}

bool FpvHeadPos(float out[3]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  // The rider's own position when it was seen in the last few frames (the board can be flipping under them);
  // otherwise the board's ground point plus the usual height.
  if (REXCVAR_GET(skate3_portal_fpv_rider) && g_head_valid &&
      std::chrono::steady_clock::now() - g_head_t < std::chrono::milliseconds(150)) {
    out[0] = g_head[0];
    out[1] = g_head[1] + 0.7f;
    out[2] = g_head[2];
    return true;
  }
  if (!g_tracked_valid) return false;
  out[0] = g_tracked[0];
  out[1] = g_tracked[1] + 0.85f;
  out[2] = g_tracked[2];
  return true;
}

void FilterPad(uint16_t* buttons, uint8_t* left_trigger, uint8_t* right_trigger, int16_t* rx, int16_t* ry) {
  constexpr uint16_t kY = 0x8000, kR3 = 0x0080, kX = 0x4000, kR1 = 0x0200, kB = 0x2000;  // XInput button bits
  static bool was_rt = false, was_lt = false, was_y = false, was_r3 = false, was_x = false, was_r1 = false, was_b = false;
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  const float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
  last = now;
  if (!Enabled()) return;
  // Diagnostics for pad bring-up: every change of the button mask, and a note when the filter first runs.
  static uint16_t last_buttons = 0xFFFF;
  static int logged = 0;
  if (logged == 0 || (*buttons != last_buttons && logged < 40)) {
    ++logged;
    REXLOG_INFO("portal-gate: pad buttons {:04X} (modifier mask {:04X}) lt {} rt {} rstick ({}, {})", *buttons,
                ModifierMask(), *left_trigger, *right_trigger, *rx, *ry);
  }
  last_buttons = *buttons;
  const uint16_t mod = ModifierMask();
  // L1 + D-pad down is the game's own "return to marker": L1 held means that, not portal mode.
  constexpr uint16_t kL1 = 0x0100;
  if ((*buttons & mod) != mod || (*buttons & kL1)) {
    was_rt = was_lt = was_y = was_r3 = was_x = was_r1 = was_b = false;
    return;
  }
  if (!FpvActive()) {
    // Portal mode: the right stick moves the reticle (the shot and a carried object follow it) instead of flicking the board.
    const auto axis = [](int16_t v) {
      const float f = float(v) / 32767.0f;
      return std::abs(f) < 0.2f ? 0.0f : f;
    };
    g_pad_x = std::clamp(g_pad_x.load() + axis(*rx) * 1.4f * dt, -0.95f, 0.95f);
    g_pad_y = std::clamp(g_pad_y.load() + axis(*ry) * 1.4f * dt, -0.95f, 0.95f);
    *rx = 0;
    *ry = 0;
  }
  const bool rt = *right_trigger > 128, lt = *left_trigger > 128, y = *buttons & kY, r3 = *buttons & kR3, x = *buttons & kX, r1 = *buttons & kR1;
  const bool b = *buttons & kB;
  if (b && !was_b) portal_gel::ToggleMode();
  was_b = b;
  if (rt && !was_rt) Request(0);
  if (lt && !was_lt) Request(1);
  if (y && !was_y) Request(2);
  if (r3 && !was_r3) ToggleFpv();
  if (x && !was_x) skate3::portal_cube::RequestSpawn();
  if (r1 && !was_r1) skate3::portal_cube::RequestGrab();
  was_x = x;
  was_r1 = r1;
  was_rt = rt;
  was_lt = lt;
  was_y = y;
  was_r3 = r3;
  if (FpvActive()) {
    // right stick looks: 900 "pixels" per second at full deflection (~2 rad/s), with a dead zone
    auto axis = [](int16_t v) {
      const float f = float(v) / 32767.0f;
      return std::abs(f) < 0.2f ? 0.0f : f;
    };
    g_look_dx = g_look_dx + axis(*rx) * 900.0f * dt;
    g_look_dy = g_look_dy + -axis(*ry) * 900.0f * dt;  // stick up is positive; screen-down pixels are positive
    *rx = 0;
    *ry = 0;
  }
  *buttons = uint16_t(*buttons & ~(mod | kY | kR3 | kX | kR1 | kB));
  *left_trigger = 0;
  *right_trigger = 0;
}

void SetWallClear(float metres) { g_wall_clear = metres; }

bool PlaceOn(int which, const float hit[3], const float normal[3], const float shot[3]) {
  const float nl = std::sqrt(Dot3(normal, normal));
  if (!(nl > 0.5f)) {
    REXLOG_INFO("portal-gate: no usable surface normal, no portal");
    return false;
  }
  // The portal lies on the true surface (angled portals), except that a normal within ~6 degrees of vertical or
  // horizontal snaps to it: the world's triangles are slightly off-true, and a wobbling portal looks worse than a
  // slightly misaligned one.
  constexpr float kSnap = 0.9945f;  // cos 6 degrees
  float n[3] = {normal[0] / nl, normal[1] / nl, normal[2] / nl};
  if (std::abs(n[1]) >= kSnap) {
    n[0] = n[2] = 0.0f;
    n[1] = n[1] > 0 ? 1.0f : -1.0f;
  } else if (std::hypot(n[0], n[2]) >= kSnap) {
    const float hl = std::hypot(n[0], n[2]);
    n[0] /= hl;
    n[2] /= hl;
    n[1] = 0.0f;
  }
  float u[3] = {0, 1, 0};
  float trigger = kWallTrigger;
  const bool floorish = std::abs(n[1]) > kFlatNy;
  if (floorish) {  // floor, ceiling or a gentle slope: "up" in the plane is the shooter's heading
    const float sl = std::hypot(shot[0], shot[2]);
    const float h[3] = {sl > 0.05f ? shot[0] / sl : 0.0f, 0.0f, sl > 0.05f ? shot[2] / sl : 1.0f};
    const float d = Dot3(h, n);
    for (int k = 0; k < 3; ++k) u[k] = h[k] - d * n[k];
    // The entity centre rides ~0.85 m above the floor, so a floor portal is entered at that height; a ceiling
    // portal when the head reaches it.
    trigger = n[1] > 0 ? kFloorTrigger : kFlatTrigger;
  } else {  // wall, steep slope or overhang: "up" in the plane is world up
    const float d = n[1];
    for (int k = 0; k < 3; ++k) u[k] = (k == 1 ? 1.0f : 0.0f) - d * n[k];
  }
  const float ul = std::sqrt(Dot3(u, u));
  if (!(ul > 1e-3f)) return false;
  for (int k = 0; k < 3; ++k) u[k] /= ul;
  std::lock_guard<std::mutex> lock(g_mutex);
  Gate& g = g_gates[which];
  g.active = true;
  SetFrame(g, n, u);
  for (int k = 0; k < 3; ++k) g.c[k] = hit[k] + n[k] * 0.05f;
  g.trigger = trigger;
  g_have_prev = false;
  g_both_placed = g_gates[0].active && g_gates[1].active;
  const bool axis = std::abs(n[1]) >= 0.9999f || std::abs(n[1]) < 1e-4f;
  Sfx(which ? 1 : 0);
  REXLOG_INFO("portal-gate: {} portal on a {} at ({:.2f}, {:.2f}, {:.2f}), front ({:.2f}, {:.2f}, {:.2f}){}",
              which ? "orange" : "blue",
              g.Flat() ? (n[1] > 0 ? "floor" : "ceiling") : "wall", g.c[0], g.c[1], g.c[2], n[0], n[1], n[2],
              axis ? "" : " (angled)");
  return true;
}

void PlaceAhead(int which, const float skater[3]) {
  const auto scene = Scene();
  if (!scene) return;
  float fx = skater[0] - scene->cam_pos[0], fz = skater[2] - scene->cam_pos[2];
  const float len = std::hypot(fx, fz);
  if (len < 0.01f) return;
  fx /= len;
  fz /= len;
  std::lock_guard<std::mutex> lock(g_mutex);
  Gate& g = g_gates[which];
  g.active = true;
  const float n[3] = {-fx, 0.0f, -fz};  // faces the skater
  const float up[3] = {0, 1, 0};
  SetFrame(g, n, up);
  g.trigger = 0.0f;
  g.c[0] = skater[0] + fx * kAhead;
  g.c[1] = skater[1] - kEntityAboveGround + kHalfHeight;
  g.c[2] = skater[2] + fz * kAhead;
  g_have_prev = false;
  REXLOG_INFO("portal-gate: {} portal placed {} m ahead (no wall found) at ({:.2f}, {:.2f}, {:.2f})",
              which ? "orange" : "blue", kAhead, g.c[0], g.c[1], g.c[2]);
}

bool Step(const float pos[3], bool allowed, unsigned flags, Cross* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::copy(pos, pos + 3, g_tracked);
  g_tracked_valid = true;
  if (g_pending.load() == 2) {
    g_pending = -1;
    g_gates[0].active = g_gates[1].active = false;
    g_have_prev = false;
    g_both_placed = false;
    Sfx(3);
    REXLOG_INFO("portal-gate: cleared");
  }
  bool crossed = false;
  bool flat_pair = false;
  const bool hole = REXCVAR_GET(skate3_portal_gate_hole);
  if (g_standing_block > 0) --g_standing_block;
  if (g_cooldown > 0) {
    --g_cooldown;
  } else if (g_have_prev && g_gates[0].active && g_gates[1].active) {
    for (int i = 0; i < 2 && !crossed; ++i) {
      const Gate& g = g_gates[i];
      // A portal is entered through its front, moving against its normal: the signed distance in front of the
      // portal goes from positive to non-positive.
      const float rel[3] = {pos[0] - g.c[0], pos[1] - g.c[1], pos[2] - g.c[2]};
      const float prel[3] = {g_prev[0] - g.c[0], g_prev[1] - g.c[1], g_prev[2] - g.c[2]};
      // With the hole the skater can reach the real plane, so the crossing happens there.
      const float trig = hole ? 0.0f : g.trigger;
      const float prev_d = Dot3(prel, g.n) - trig;
      const float d = Dot3(rel, g.n) - trig;
      const float lat = Dot3(rel, g.r), vert = Dot3(rel, g.u);  // across / up within the portal plane
      const bool inside = InOval(lat, vert, kTriggerMargin);
      // Standing in a flat portal's opening counts too: rolling onto a floor portal, or landing in it from a wall exit
      // that came out below the (high) floor trigger plane. Only while not moving away, or a fling out of a floor
      // portal would re-enter it at once.
      bool crossing = prev_d > 0.0f && d <= 0.0f && inside;
      // (hole: also catch a skater already sinking through the open surface, up to 3 m below the plane)
      // (hole: a walker standing in a floor portal sits kFlatTrigger above the plane and never sinks, so count that too)
      if (!crossing && g_standing_block == 0 && g.Flat() && inside && d <= (hole ? kFlatTrigger : 0.0f) && Dot3(rel, g.n) > (hole ? -3.0f : -0.5f)) {
        const float away[3] = {pos[0] - g_prev[0], pos[1] - g_prev[1], pos[2] - g_prev[2]};
        crossing = Dot3(away, g.n) <= 0.01f;
      }
      // Hole: already below a flat portal's plane and still sinking into it. This also counts during the standing block: a crossing refused
      // for one frame (mid-relocation) used to leave the skater falling through the open floor for the whole block (floor fall-through,
      // run 181). Sinking (moving into the portal) cannot be the standing ping-pong the block is for.
      bool sinking = false;
      if (!crossing && hole && g.Flat() && inside && Dot3(rel, g.n) < 0.0f && Dot3(rel, g.n) > -3.0f) {
        const float away[3] = {pos[0] - g_prev[0], pos[1] - g_prev[1], pos[2] - g_prev[2]};
        sinking = crossing = Dot3(away, g.n) < -0.01f;
      }
      if (!crossing) continue;
      if (!allowed) {
        REXLOG_INFO("portal-gate: went through the {} portal but ignored it: not airborne (flags {:08X})",
                    i ? "orange" : "blue", flags);
        g_cooldown = hole ? 0 : 30;
        break;
      }
      const Gate& to = g_gates[1 - i];
      flat_pair = g.Flat() || to.Flat();
      // Portal transform: in the entry frame the point is (lat, vert, depth in front); it comes out at
      // (-lat, vert, depth) in the exit frame, facing out. Velocity goes the same way: (a, b, c) along
      // (r, u, n) becomes -a r' + b u' - c n'.
      const float depth = std::max(0.0f, -d);
      const float clear = to.Flat() ? 0.3f : g_wall_clear.load();  // a wall's exit also stays clear of the wall itself
      // Hole: come out of the other portal's plane by exactly as far as you were past this one's.
      const float out_dist = hole ? depth + 0.02f + (to.Flat() ? 0.0f : float(REXCVAR_GET(skate3_portal_gate_wall_exit))) : (to.Flat() ? kFlatTrigger : to.trigger) + depth + clear;
      for (int k = 0; k < 3; ++k) {
        out->exit_pos[k] = to.c[k] - lat * to.r[k] + vert * to.u[k] + out_dist * to.n[k];
      }
      // A floor portal's "up" is the aim heading, so a sideways offset there becomes a vertical one on a wall: keep the
      // exit above the ground a wall portal is usually placed on (centre ~1.7 m up), or the skater lands inside it.
      if (!to.Flat()) out->exit_pos[1] = std::max(out->exit_pos[1], to.c[1] - 1.0f);
      // M = [-r' u' -n'] * [r u n]^T, i.e. M[k][j] = -r'[k] r[j] + u'[k] u[j] - n'[k] n[j]
      for (int k = 0; k < 3; ++k) {
        for (int j = 0; j < 3; ++j) {
          out->vel[k * 3 + j] = -to.r[k] * g.r[j] + to.u[k] * g.u[j] - to.n[k] * g.n[j];
        }
      }
      out->exit_dir[0] = to.n[0];
      out->exit_dir[1] = to.n[2];
      out->lift = (hole || to.n[1] < -0.5f) ? 0.0f : 1.0f;  // a ceiling exit starts at head height, not lifted
      std::copy(to.c, to.c + 3, out->exit_c);
      std::copy(to.n, to.n + 3, out->exit_n);
      crossed = true;
      Sfx(2);
      REXLOG_INFO("portal-gate: crossed {} portal at ({:.2f}, {:.2f}, {:.2f}) -> ({:.2f}, {:.2f}, {:.2f}){}",
                  i ? "orange" : "blue", pos[0], pos[1], pos[2], out->exit_pos[0], out->exit_pos[1],
                  out->exit_pos[2], sinking ? " (sinking through the opening)" : "");
    }
  }
  if (crossed) {
    std::copy(pos, pos + 3, g_zone[0]);
    std::copy(out->exit_pos, out->exit_pos + 3, g_zone[1]);
    g_zone_ns = now_ns();
    std::copy(out->vel, out->vel + 9, g_cross_R);
    ++g_cross_seq;
    g_have_prev = false;
    g_standing_block = 60;
    // A floor/ceiling pair is the endless fall: the way back into the portal is only ~2 m away, so a 0.75 s lockout
    // would let the skater land before the next crossing could fire.
    g_cooldown = flat_pair ? kFlatCooldownFrames : kCooldownFrames;
  } else {
    std::copy(pos, pos + 3, g_prev);
    g_have_prev = true;
  }
  return crossed;
}

void Overlay::OnDraw(ImGuiIO& io) {
  if (!Enabled()) {
    return;
  }
  // Same pattern as the renderer indicator, the one overlay known to show in-game: a real window (here
  // full-screen, transparent, click-through) with a text widget, and the rings drawn into its draw list.
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
  ImGui::Begin("##portal_gates", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);
  struct EndGuard {
    ~EndGuard() { ImGui::End(); }
  } end_guard;
  g_aim_free = false;
  g_aim_x = g_aim_y = 0.0f;
  ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                     REXCVAR_GET(skate3_portal_gate_mouse)
                         ? "PORTAL GUN ON  (aim with the mouse: left click blue, right click orange, middle clear)"
                         : "PORTAL GUN ON  (hold D-pad down: right stick aims, RT blue, LT orange, Y clear, R1 grab, X cube; or Ctrl+1/2/3, Ctrl+G, Ctrl+5)");
  if (portal_gel::Mode()) {
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "GEL MODE: blue = bounce, orange = speed, clear removes gel (%d painted; Ctrl+H or D-pad down + B for portals)",
                       portal_gel::Count());
  } else if (portal_gel::Enabled()) {
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Ctrl+H or D-pad down + B: gel mode");
  }
  if (REXCVAR_GET(skate3_portal_gate_clock)) {
    // Wall-clock time to the millisecond, in the log's own (local) time, so a screenshot says which log lines it matches.
    const auto wall = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(wall);
    std::tm local{};
    localtime_r(&secs, &local);
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(wall.time_since_epoch()).count() % 1000);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec, ms);
    const ImVec2 sz = ImGui::CalcTextSize(stamp);
    ImGui::GetWindowDrawList()->AddText(ImVec2(io.DisplaySize.x - sz.x - 10.0f, 4.0f), IM_COL32(255, 255, 0, 255), stamp);
    // the [f N] of the FPVTRACK / FLING log lines
    char frame[24];
    std::snprintf(frame, sizeof(frame), "f %u", g_game_frame.load(std::memory_order_relaxed));
    const ImVec2 fs = ImGui::CalcTextSize(frame);
    ImGui::GetWindowDrawList()->AddText(ImVec2(io.DisplaySize.x - fs.x - 10.0f, 6.0f + sz.y), IM_COL32(255, 255, 0, 255), frame);
  }
  std::shared_ptr<const native_scene::FrameScene> scene;
  {
    std::lock_guard<std::mutex> lock(native_scene::g_scene_mutex);
    scene = native_scene::g_scene;
  }
  if (!scene) {
    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "no scene yet");
    return;
  }
  float x0 = 0, y0 = 0, w = io.DisplaySize.x, h = io.DisplaySize.y;
  if (rex::ui::Presenter* presenter = imgui_drawer()->presenter()) {
    const auto rect = presenter->GetLastGuestOutputPaintRect();
    if (rect.width > 0 && rect.height > 0) {
      x0 = float(rect.x);
      y0 = float(rect.y);
      w = float(rect.width);
      h = float(rect.height);
    }
  }
  const float* vp = scene->view_proj;
  // view_proj is row-vector convention (translation in row 3, like the scene's world matrices)
  auto project = [&](const float p[3], ImVec2* out) {
    const float cw = p[0] * vp[3] + p[1] * vp[7] + p[2] * vp[11] + vp[15];
    if (cw < 0.1f) {
      return false;
    }
    const float cx = p[0] * vp[0] + p[1] * vp[4] + p[2] * vp[8] + vp[12];
    const float cy = p[0] * vp[1] + p[1] * vp[5] + p[2] * vp[9] + vp[13];
    *out = ImVec2(x0 + (cx / cw * 0.5f + 0.5f) * w, y0 + (0.5f - cy / cw * 0.5f) * h);
    return true;
  };
  ImDrawList* dl = ImGui::GetWindowDrawList();
  Gate gates[2];
  float tracked[3];
  bool tracked_valid;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::copy(g_gates, g_gates + 2, gates);
    std::copy(g_tracked, g_tracked + 3, tracked);
    tracked_valid = g_tracked_valid;
  }
  const ImU32 colors[2] = {IM_COL32(40, 150, 255, 255), IM_COL32(255, 140, 20, 255)};
  {
    // gel paint: translucent discs in their surface plane (screen overlay, so not hidden by walls; only within 60 m)
    portal_math::GelPatch gels[32];
    const int ng = portal_gel::Snapshot(gels, 32);
    for (int i = 0; i < ng; ++i) {
      const portal_math::GelPatch& g = gels[i];
      if (tracked_valid && std::hypot(g.c[0] - tracked[0], g.c[2] - tracked[2]) > 60.0f) continue;
      const float ref[3] = {std::abs(g.n[1]) < 0.9f ? 0.0f : 1.0f, std::abs(g.n[1]) < 0.9f ? 1.0f : 0.0f, 0.0f};
      float t[3] = {ref[1] * g.n[2] - ref[2] * g.n[1], ref[2] * g.n[0] - ref[0] * g.n[2], ref[0] * g.n[1] - ref[1] * g.n[0]};
      const float tl = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
      if (tl < 1e-4f) continue;
      for (float& c : t) c /= tl;
      const float bt[3] = {g.n[1] * t[2] - g.n[2] * t[1], g.n[2] * t[0] - g.n[0] * t[2], g.n[0] * t[1] - g.n[1] * t[0]};
      constexpr int kGelSeg = 20;
      ImVec2 gp[kGelSeg];
      int m = 0;
      for (int k = 0; k < kGelSeg; ++k) {
        const float a = 6.2831853f * float(k) / float(kGelSeg);
        // a slightly uneven rim, so it reads as a splat and not a portal
        const float rr = g.radius * (0.88f + 0.12f * std::sin(a * 5.0f + float(g.id)));
        const float q[3] = {g.c[0] + g.n[0] * 0.03f + (std::cos(a) * t[0] + std::sin(a) * bt[0]) * rr,
                            g.c[1] + g.n[1] * 0.03f + (std::cos(a) * t[1] + std::sin(a) * bt[1]) * rr,
                            g.c[2] + g.n[2] * 0.03f + (std::cos(a) * t[2] + std::sin(a) * bt[2]) * rr};
        if (project(q, &gp[m])) ++m;
      }
      if (m != kGelSeg) continue;
      const ImU32 fill = g.kind ? IM_COL32(255, 106, 0, 120) : IM_COL32(0, 165, 255, 120);  // Portal 2's speed / bounce paint colours
      const ImU32 rim = g.kind ? IM_COL32(255, 170, 60, 220) : IM_COL32(90, 180, 255, 220);
      // the splat is convex enough for a fan from the centre
      ImVec2 cc;
      if (project(g.c, &cc)) {
        for (int k = 0; k < kGelSeg; ++k) dl->AddTriangleFilled(cc, gp[k], gp[(k + 1) % kGelSeg], fill);
      }
      dl->AddPolyline(gp, kGelSeg, rim, ImDrawFlags_Closed, 3.0f);
    }
  }
  const bool world_surfaces = SurfaceDrawnRecently();
  for (int i = 0; i < 2; ++i) {
    if (!gates[i].active || world_surfaces) {
      continue;
    }
    const Gate& g = gates[i];
    // rounded rectangle in the portal plane: u across (g.r), v along g.u
    constexpr int kArc = 6;
    constexpr float kCorner = 0.7f;
    constexpr int kSeg = 4 * (kArc + 1);
    ImVec2 pts[kSeg];
    int n = 0;
    for (int corner = 0; corner < 4; ++corner) {
      const float cu = (corner == 0 || corner == 3) ? kHalfWidth - kCorner : -(kHalfWidth - kCorner);
      const float cv = (corner < 2) ? kHalfHeight - kCorner : -(kHalfHeight - kCorner);
      const float a0 = 1.5707963f * float(corner == 0 ? 0 : corner == 1 ? 1 : corner == 2 ? 2 : 3);
      for (int k = 0; k <= kArc; ++k) {
        const float a = a0 + 1.5707963f * float(k) / float(kArc);
        const float u = cu + std::cos(a) * kCorner, v = cv + std::sin(a) * kCorner;
        const float p[3] = {g.c[0] + u * g.r[0] + v * g.u[0], g.c[1] + u * g.r[1] + v * g.u[1],
                            g.c[2] + u * g.r[2] + v * g.u[2]};
        if (project(p, &pts[n])) {
          ++n;
        }
      }
    }
    if (n == kSeg) {
      // Opaque for now (the wall behind is not cut away): a dark core in the portal's colour with a bright rim.
      const ImU32 core = IM_COL32(((colors[i] >> IM_COL32_R_SHIFT) & 0xFF) / 4, ((colors[i] >> IM_COL32_G_SHIFT) & 0xFF) / 4,
                                  ((colors[i] >> IM_COL32_B_SHIFT) & 0xFF) / 4, 255);
      dl->AddConvexPolyFilled(pts, n, core);
      dl->AddPolyline(pts, n, colors[i], ImDrawFlags_Closed, 7.0f);
    }
    // depth cues, since the ring is drawn over the scene: an arrow along the travel direction (on the ground
    // for a wall, with a footprint line), and the distance from the skater
    ImVec2 a, b, c2, d, e;
    float base[3] = {g.c[0], g.c[1], g.c[2]};
    float dir[3] = {-g.n[0], -g.n[1], -g.n[2]};  // entry direction: against the front normal
    if (!g.Flat()) {
      base[1] = g.c[1] - kHalfHeight;
      const float foot_a[3] = {g.c[0] - g.r[0] * kHalfWidth, base[1], g.c[2] - g.r[2] * kHalfWidth};
      const float foot_b[3] = {g.c[0] + g.r[0] * kHalfWidth, base[1], g.c[2] + g.r[2] * kHalfWidth};
      if (project(foot_a, &a) && project(foot_b, &b)) {
        dl->AddLine(a, b, colors[i], 6.0f);
      }
    }
    float tip[3], head_l[3], head_r[3];
    for (int k = 0; k < 3; ++k) {
      tip[k] = base[k] + dir[k] * 3.0f;
      head_l[k] = tip[k] - dir[k] * 0.8f + g.r[k] * 0.6f;
      head_r[k] = tip[k] - dir[k] * 0.8f - g.r[k] * 0.6f;
    }
    if (project(base, &c2) && project(tip, &d)) {
      dl->AddLine(c2, d, colors[i], 4.0f);
      if (project(head_l, &a)) dl->AddLine(d, a, colors[i], 4.0f);
      if (project(head_r, &e)) dl->AddLine(d, e, colors[i], 4.0f);
    }
    if (tracked_valid && project(g.c, &a)) {
      const float dist = std::hypot(tracked[0] - g.c[0], tracked[2] - g.c[2]);
      char label[48];
      std::snprintf(label, sizeof(label), "%s  %.0f m", i ? "ORANGE" : "BLUE", dist);
      dl->AddText(ImVec2(a.x - 30.0f, a.y - 8.0f), IM_COL32(255, 255, 255, 255), label);
    }
  }
  // Portal gun: reticle at the mouse cursor (the shot goes from the camera through it), a gun held at the
  // bottom right pointing at the reticle, and a flash in the colour of the portal just fired.
  // First person: capture the mouse for looking (SDL relative mode) and aim from the screen centre.
  static bool fpv_captured = false;
  const bool fpv = FpvActive();
  if (fpv != fpv_captured) {
    fpv_captured = fpv;
    if (SDL_Window* win = SDL_GetKeyboardFocus()) SDL_SetWindowRelativeMouseMode(win, fpv);
    g_look_dx = g_look_dy = 0.0f;
  }
  if (fpv) {
    float dx = 0.0f, dy = 0.0f;
    SDL_GetRelativeMouseState(&dx, &dy);
    g_look_dx = g_look_dx + dx;
    g_look_dy = g_look_dy + dy;
  }
  static ImVec2 last_m(-1.0f, -1.0f);
  const bool mouse_in = io.MousePos.x >= x0 && io.MousePos.x < x0 + w && io.MousePos.y >= y0 && io.MousePos.y < y0 + h;
  if (fpv) last_m = ImVec2(x0 + w * 0.5f, y0 + h * 0.5f);
  else if (REXCVAR_GET(skate3_portal_gate_mouse) && mouse_in) last_m = io.MousePos;
  else if (!REXCVAR_GET(skate3_portal_gate_mouse)) last_m = ImVec2(x0 + (g_pad_x.load() + 1.0f) * 0.5f * w, y0 + (1.0f - g_pad_y.load()) * 0.5f * h);
  if (last_m.x < 0.0f) last_m = ImVec2(x0 + w * 0.5f, y0 + h * 0.5f);
  {
    // the reticle and gun stay where the cursor last was inside the view when it leaves the window (or sit where the pad put them)
    const ImVec2 m = last_m;
    g_aim_x = (m.x - x0) / w * 2.0f - 1.0f;
    g_aim_y = 1.0f - (m.y - y0) / h * 2.0f;
    g_aim_free = true;
    if (REXCVAR_GET(skate3_portal_gate_mouse)) {
      if ((mouse_in || fpv) && io.MouseClicked[0]) Request(0);
      if ((mouse_in || fpv) && io.MouseClicked[1]) Request(1);
      if ((mouse_in || fpv) && io.MouseClicked[2]) Request(2);
    }
    const float r = h * 0.022f;
    const ImU32 white = IM_COL32(255, 255, 255, 230);
    dl->AddCircle(m, r, IM_COL32(0, 0, 0, 160), 32, 5.0f);
    dl->PathArcTo(m, r, 1.5707963f, 4.712389f, 16);  // left half: blue
    dl->PathStroke(colors[0], 0, 3.0f);
    dl->PathArcTo(m, r, -1.5707963f, 1.5707963f, 16);  // right half: orange
    dl->PathStroke(colors[1], 0, 3.0f);
    if (gates[0].active) dl->AddCircleFilled(ImVec2(m.x - r * 0.45f, m.y), r * 0.22f, colors[0]);
    if (gates[1].active) dl->AddCircleFilled(ImVec2(m.x + r * 0.45f, m.y), r * 0.22f, colors[1]);
    dl->AddCircleFilled(m, 2.0f, white);

    if (REXCVAR_GET(skate3_portal_gun_2d) || !GunVisible()) {
    const ImVec2 hand(x0 + w * 0.86f, y0 + h * 0.97f);
    float dx = m.x - hand.x, dy = m.y - hand.y;
    const float dl_len = std::max(1.0f, std::hypot(dx, dy));
    dx /= dl_len;
    dy /= dl_len;
    const float px = -dy, py = dx;
    const float gl = h * 0.2f, gw = h * 0.028f;  // barrel length and half width
    const ImVec2 tip(hand.x + dx * gl, hand.y + dy * gl);
    const ImVec2 body[4] = {ImVec2(hand.x + px * gw, hand.y + py * gw), ImVec2(tip.x + px * gw * 0.6f, tip.y + py * gw * 0.6f),
                            ImVec2(tip.x - px * gw * 0.6f, tip.y - py * gw * 0.6f), ImVec2(hand.x - px * gw, hand.y - py * gw)};
    dl->AddConvexPolyFilled(body, 4, IM_COL32(235, 235, 240, 235));
    dl->AddPolyline(body, 4, IM_COL32(40, 40, 50, 255), ImDrawFlags_Closed, 3.0f);
    const int last = g_fire_which;
    const ImU32 glow = colors[last];
    dl->AddCircleFilled(tip, gw * 0.9f, (glow & 0x00FFFFFF) | IM_COL32(0, 0, 0, 255));
    const float since = float(now_ns() - g_fire_ns.load()) * 1e-9f;
    if (g_fire_ns.load() != 0 && since < 0.2f) {
      dl->AddCircleFilled(tip, gw * (1.6f + since * 10.0f), (glow & 0x00FFFFFF) | IM_COL32(0, 0, 0, 120));
    }
    dl->AddLine(tip, m, (glow & 0x00FFFFFF) | IM_COL32(0, 0, 0, 70), 2.0f);
    }
  }
  // projection check: a green dot should sit on the skater
  ImVec2 dot;
  const bool dot_ok = tracked_valid && project(tracked, &dot);
  if (dot_ok && REXCVAR_GET(skate3_portal_gun_2d)) {
    dl->AddCircleFilled(dot, 5.0f, IM_COL32(60, 255, 90, 255));
  }
  static std::chrono::steady_clock::time_point last_log{};
  const auto now = std::chrono::steady_clock::now();
  if (now - last_log > std::chrono::seconds(3)) {
    last_log = now;
    REXLOG_INFO("portal-gate: overlay draw: display {}x{} rect ({:.0f},{:.0f}) {:.0f}x{:.0f}; mouse ({:.0f},{:.0f}); tracked ({:.1f},{:.1f},{:.1f}) {} -> screen ({:.0f},{:.0f}); cam ({:.1f},{:.1f},{:.1f}); rings {}{}",
                io.DisplaySize.x, io.DisplaySize.y, x0, y0, w, h, io.MousePos.x, io.MousePos.y, tracked[0], tracked[1], tracked[2],
                tracked_valid ? "valid" : "invalid", dot_ok ? dot.x : -1.0f, dot_ok ? dot.y : -1.0f,
                scene->cam_pos[0], scene->cam_pos[1], scene->cam_pos[2], gates[0].active ? "blue " : "",
                gates[1].active ? "orange" : "");
  }
}

}  // namespace skate3::portal_gate

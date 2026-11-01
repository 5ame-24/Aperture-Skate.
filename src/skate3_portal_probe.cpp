// Temporary reverse-engineering probe for the portal gun; delete once the
// skater's position/velocity fields are known. Hooks the actor-update
// candidates named in skate-core and, four times a second, compares the
// skater's world position (from its presentation entity) with what those
// objects hold, then looks for a vector matching the skater's measured velocity.
// Known so far: r3 of sub_82BFB1E0/82BFB3F8 is skate-core's checkpoint History,
// whose current_position (the physical COM) sits at +0x110.

#include "generated/skate3_init.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <random>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "native/skate3_native_entity.h"
#include "skate3_portal_cube.h"
#include "skate3_portal_gate.h"
#include "native/skate3_native_guest_read.h"

REXCVAR_DEFINE_BOOL(skate3_portal_probe, true, "Skate 3",
                    "Portal gun reverse-engineering probe: hooks the actor update and logs "
                    "where the skater's position and velocity live. Off to rule it out when "
                    "debugging.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(bool, skate3_portal_gate_hole);
REXCVAR_DEFINE_BOOL(skate3_portal_gate_ground, true, "Skate 3",
                    "Allow portal gate crossings while riding on the ground too (the game's teleport loses speed "
                    "there and can trigger its recovery warp). Off = airborne crossings only.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Run 092: 13,000 ray hits dropped at a floor portal and the skater still stood on it (the body collision is not
// ray based), while the missing foot-test hits probably set the recovery flag that made crossings 'not airborne'.
// So this is OFF by default; it only matters for experiments.
REXCVAR_DEFINE_BOOL(skate3_portal_gate_cutout, false, "Skate 3",
                    "Portal gate: with both portals placed, the world's ray answers (the composite ray service, results "
                    "function 8276EE88) inside a portal opening are reported as misses, so the opening is not solid to "
                    "the queries that use rays (skeleton line tests and anything else). Experiment.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_gate_ab, false, "Skate 3",
                    "Portal gate A/B test: cycle through 4 crossing setups (see kProfiles) and log which one is used, "
                    "so one session shows which setup keeps the speed.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_gate_airmode, true, "Skate 3",
                    "Portal gate: present a rolling crossing to the game's teleport as an airborne one (ground bit "
                    "cleared). The airborne teleport keeps the skeleton's velocities; the grounded one zeroes them.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_gate_turn, true, "Skate 3",
                    "Portal gate: turn the board's velocity from the entry portal's frame to the exit portal's, so "
                    "portals on different walls, floors and ceilings redirect your speed like Portal. Applies to the vectors the restore puts back.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_portal_gate_restore, false, "Skate 3",
                    "Portal gate crossings also write back the board/skeleton velocities the game's teleport "
                    "zeroes (the speed-restore experiment). Off = the game's native behaviour.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(skate3_portal_probe_teleport, 0.0, "Skate 3",
                      "Test: non-zero = every 6 s while riding on the ground, ask the game's own teleport (the path a "
                      "session-marker relocation takes: its flag bit plus our target matrix) to put the "
                      "skater back where they were 3 s ago, facing their direction of travel, keeping speed.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Defined with the integrator hook, further down (global scope); called from GateStep.
void BodiesFrame(uint8_t* base, const float skater[3], const float skater_v[2], const float hand[3], float dt);
bool BodiesGrabToggle(uint8_t* base, const float skater[3], const float skater_v[2], const float aim_o[3], const float aim_d[3],
                      bool aim_ok, const float hand[3], float cube_score);

namespace {

constexpr uint32_t kScanBytes = 0x1000;
constexpr float kTolerance = 1.5f;  // the actor's COM sits above the entity root
constexpr float kMinSpeed = 2.0f;   // below this the direction is too noisy to match
constexpr float kMaxSpeed = 60.0f;  // faster is a teleport (session marker, respawn), not skating
constexpr int kMaxHits = 600;       // total logged hit lines; scanning is cheap, the log is not
constexpr int kMaxHitsPerScan = 8;
constexpr int64_t kTickMs = 250;
constexpr uint32_t kHooked[] = {0x82BFB1E0, 0x82BFB3F8, 0x82BFC038, 0x82BFC378};

std::atomic<uint32_t> g_calls[4];
std::atomic<int64_t> g_next_ms{0};
std::atomic<int> g_hits{0};

float BeFloat(uint32_t raw) { return std::bit_cast<float>(__builtin_bswap32(raw)); }

// Reads the first kScanBytes of a guest object; false for bad pointers.
bool ReadObject(uint8_t* base, uint32_t addr, uint32_t* raw) {
  return addr >= 0x10000 && !(addr & 3) &&
         skate3::native_scene::GuestTryCopy(raw, base + addr, kScanBytes);
}

// Float triples in the object that sit within kTolerance of the skater's position.
void ScanPosition(uint8_t* base, uint32_t fn, const char* arg, uint32_t addr, const float pos[3]) {
  uint32_t raw[kScanBytes / 4];
  if (!ReadObject(base, addr, raw)) {
    return;
  }
  int hits = 0;
  for (uint32_t i = 0; i + 3 <= kScanBytes / 4 && hits < kMaxHitsPerScan; ++i) {
    const float v[3] = {BeFloat(raw[i]), BeFloat(raw[i + 1]), BeFloat(raw[i + 2])};
    if (std::abs(v[0] - pos[0]) < kTolerance && std::abs(v[1] - pos[1]) < kTolerance &&
        std::abs(v[2] - pos[2]) < kTolerance) {
      ++hits;
      g_hits.fetch_add(1, std::memory_order_relaxed);
      REXLOG_INFO("portal-probe: pos sub_{:08X} {}={:08X}+0x{:X} = ({:.2f}, {:.2f}, {:.2f})", fn,
                  arg, addr, i * 4, v[0], v[1], v[2]);
    }
  }
}

// Float triples with the skater's measured speed (+-20%) and heading (cos > 0.97).
void ScanVelocity(uint8_t* base, uint32_t fn, const char* arg, uint32_t addr, const float vel[3],
                  float speed) {
  uint32_t raw[kScanBytes / 4];
  if (!ReadObject(base, addr, raw)) {
    return;
  }
  int hits = 0;
  for (uint32_t i = 0; i + 3 <= kScanBytes / 4 && hits < kMaxHitsPerScan; ++i) {
    const float w[3] = {BeFloat(raw[i]), BeFloat(raw[i + 1]), BeFloat(raw[i + 2])};
    const float mag = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (!(mag > 0.8f * speed && mag < 1.2f * speed)) {
      continue;
    }
    const float cosine = (w[0] * vel[0] + w[1] * vel[1] + w[2] * vel[2]) / (mag * speed);
    if (cosine > 0.97f) {
      ++hits;
      g_hits.fetch_add(1, std::memory_order_relaxed);
      REXLOG_INFO("portal-probe: vel sub_{:08X} {}={:08X}+0x{:X} = ({:.2f}, {:.2f}, {:.2f}) |v|={:.2f}",
                  fn, arg, addr, i * 4, w[0], w[1], w[2], mag);
    }
  }
}

void Probe(uint8_t* base, int slot, uint32_t a3, uint32_t a4, uint32_t a5) {
  if (!REXCVAR_GET(skate3_portal_probe)) {
    return;
  }
  g_calls[slot].fetch_add(1, std::memory_order_relaxed);
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  int64_t due = g_next_ms.load(std::memory_order_relaxed);
  if (now < due || !g_next_ms.compare_exchange_strong(due, now + kTickMs)) {
    return;
  }
  // Only the thread that won the tick gets here, so these need no lock.
  static int tick = 0;
  static float prev[3];
  static int64_t prev_ms = 0;
  static bool have_prev = false;

  float pos[3] = {};
  const bool have = skate3::native_entity::SkaterWorldPos(base, pos);
  float history[3] = {};  // History.current_position at r3+0x110 (slots 0 and 1)
  uint32_t raw[kScanBytes / 4];
  const bool have_history = slot < 2 && ReadObject(base, a3, raw);
  if (have_history) {
    for (int k = 0; k < 3; ++k) {
      history[k] = BeFloat(raw[0x110 / 4 + k]);
    }
  }
  if (++tick % 4 == 0) {
    REXLOG_INFO(
        "portal-probe: calls/s 82BFB1E0={} 82BFB3F8={} 82BFC038={} 82BFC378={} skater={} "
        "({:.2f}, {:.2f}, {:.2f}) history+0x110=({:.2f}, {:.2f}, {:.2f})",
        g_calls[0].exchange(0), g_calls[1].exchange(0), g_calls[2].exchange(0),
        g_calls[3].exchange(0), have ? "yes" : "no", pos[0], pos[1], pos[2], history[0], history[1],
        history[2]);
  }

  float vel[3] = {};
  float speed = 0.0f;
  if (have && have_prev && now > prev_ms) {
    const float dt = float(now - prev_ms) / 1000.0f;
    for (int k = 0; k < 3; ++k) {
      vel[k] = (pos[k] - prev[k]) / dt;
    }
    speed = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
  }
  have_prev = have;
  prev_ms = now;
  std::memcpy(prev, pos, sizeof(prev));
  if (!have || g_hits.load(std::memory_order_relaxed) >= kMaxHits) {
    return;
  }

  const char* const names[3] = {"r3", "r4", "r5"};
  const uint32_t args[3] = {a3, a4, a5};
  for (int i = 0; i < 3; ++i) {
    ScanPosition(base, kHooked[slot], names[i], args[i], pos);
    if (speed > kMinSpeed && speed < kMaxSpeed) {
      ScanVelocity(base, kHooked[slot], names[i], args[i], vel, speed);
    }
  }
}

// The game's own "put the player here" path (skate-host input_teleport.rs: reset_player 82DB8998,
// whose skeleton stages are 82BE3508 and 82BDF7C0). Logged with raw arguments before the call so the
// target-transform argument can be told apart; only the args themselves are read, no pointer chasing.
std::atomic<int> g_teleports{0};

// Ring of recent calls to the functions UpdateSessionMarker (82898FC8) calls, each with a snapshot of the
// 4x4 matrix r4/r5 point at. When the teleport executor fires, any ring entry whose matrix translation sits
// within 2 units of the executor's target is the function that asked for it: the request to reuse.
struct RingCall {
  int64_t ms = 0;
  uint32_t fn = 0;
  uint32_t r[4] = {};
  float m[2][16] = {};
  bool has[2] = {};
};
constexpr int kRing = 512;
RingCall g_ring[kRing];
uint32_t g_ring_next = 0;
std::mutex g_ring_mu;

bool ReadMatrix(uint8_t* base, uint32_t addr, float (&out)[16]) {
  uint32_t raw[16];
  if (addr < 0x10000 || addr >= 0xC0000000 || (addr & 3) ||
      !skate3::native_scene::GuestTryCopy(raw, base + addr, sizeof(raw))) {
    return false;
  }
  for (int i = 0; i < 16; ++i) out[i] = BeFloat(raw[i]);
  return true;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void RecordCall(uint8_t* base, uint32_t fn, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6) {
  if (!REXCVAR_GET(skate3_portal_probe)) return;
  RingCall c;
  c.ms = NowMs();
  c.fn = fn;
  c.r[0] = r3, c.r[1] = r4, c.r[2] = r5, c.r[3] = r6;
  c.has[0] = ReadMatrix(base, r4, c.m[0]);
  c.has[1] = ReadMatrix(base, r5, c.m[1]);
  std::lock_guard<std::mutex> lock(g_ring_mu);
  g_ring[g_ring_next++ % kRing] = c;
}

// At the executor: which recent call carried a matrix with the executor's target translation?
void FindRequest(uint8_t* base, uint32_t target_addr) {
  float target[16];
  if (!ReadMatrix(base, target_addr, target)) return;
  const int64_t now = NowMs();
  std::lock_guard<std::mutex> lock(g_ring_mu);
  int found = 0;
  for (int i = 0; i < kRing && found < 12; ++i) {
    const RingCall& c = g_ring[(g_ring_next + kRing - 1 - i) % kRing];
    if (!c.fn || now - c.ms > 3000) continue;
    for (int k = 0; k < 2; ++k) {
      if (!c.has[k]) continue;
      const float* m = c.m[k];
      if (std::abs(m[12] - target[12]) < 2.0f && std::abs(m[13] - target[13]) < 2.0f &&
          std::abs(m[14] - target[14]) < 2.0f) {
        ++found;
        REXLOG_INFO(
            "portal-probe: REQUEST CANDIDATE sub_{:08X} r{} matrix translation ({:.2f}, {:.2f}, {:.2f}) "
            "{} ms before executor; args r3..r6={:08X} {:08X} {:08X} {:08X}",
            c.fn, k + 4, m[12], m[13], m[14], now - c.ms, c.r[0], c.r[1], c.r[2], c.r[3]);
      }
    }
  }
  if (!found) REXLOG_INFO("portal-probe: no recent call carried the executor's target matrix");
}

void TeleportBefore(uint8_t* base, uint32_t fn, const uint32_t (&a)[8]) {
  if (fn == 0x82DB8998) FindRequest(base, a[1]);
  float pos[3] = {};
  const bool have = skate3::native_entity::SkaterWorldPos(base, pos);
  REXLOG_INFO(
      "portal-probe: teleport sub_{:08X} BEFORE skater={} ({:.2f}, {:.2f}, {:.2f}) r3..r10={:08X} "
      "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
      fn, have ? "yes" : "no", pos[0], pos[1], pos[2], a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
  for (int i = 0; i < 8; ++i) {
    uint32_t raw[24];
    if (a[i] < 0x10000 || a[i] >= 0xC0000000 || (a[i] & 3) ||
        !skate3::native_scene::GuestTryCopy(raw, base + a[i], sizeof(raw))) {
      continue;
    }
    char text[2048];  // 24 values x up to 45 chars; garbage floats (1e38) overflowed the old 256-byte buffer
    int n = 0;
    for (int k = 0; k < 24; ++k) {
      n += std::snprintf(text + n, sizeof(text) - n, "%s%.2f", k % 4 ? " " : (k ? " | " : ""),
                         BeFloat(raw[k]));
    }
    REXLOG_INFO("portal-probe: teleport sub_{:08X} arg r{}={:08X}: {}", fn, i + 3, a[i], text);
  }
}

void TeleportAfter(uint8_t* base, uint32_t fn) {
  float pos[3] = {};
  const bool have = skate3::native_entity::SkaterWorldPos(base, pos);
  REXLOG_INFO("portal-probe: teleport sub_{:08X} AFTER skater={} ({:.2f}, {:.2f}, {:.2f})", fn,
              have ? "yes" : "no", pos[0], pos[1], pos[2]);
}

bool ReadBe32(uint8_t* base, uint32_t addr, uint32_t* out) {
  uint32_t raw;
  if (addr < 0x10000 || addr >= 0xC0000000 ||
      !skate3::native_scene::GuestTryCopy(&raw, base + addr, 4)) {
    return false;
  }
  *out = __builtin_bswap32(raw);
  return true;
}

// One-shot test of the marker's own relocation branch. Reading sub_82DB88C8: when bit 0x00080000 of
// this+1296 is set it fetches a target matrix with sub_82585CB0 into a stack buffer (caller sp - 96),
// calls the teleport executor 82DB8998 with it, and clears the bit itself. Here we raise that bit once
// and swap the matrix for ours; a marker relocation that is already pending is never touched.
void WriteBeFloat(uint8_t* base, uint32_t addr, float v) {
  const uint32_t be = __builtin_bswap32(std::bit_cast<uint32_t>(v));
  std::memcpy(base + addr, &be, 4);
}

constexpr uint32_t kRelocateBit = 0x00080000;
thread_local uint32_t t_target_buf = 0;
thread_local bool t_target_set = false;
thread_local float t_target[16];

// The human skater's actor is the one whose own ground position (PlayerInputState.current_ground_position,
// this+1216) is within a few metres of the skater's presentation entity. Heap addresses change every run
// and the game relocates other actors through the same path, so neither can identify it.
// The career pit the game parks the skater in before the map loads: always about x = 0, z = -5 (seen at
// y = -62, -56 and +0.48), so identify it by that spot, not by height (interiors can have floors below 0).
// shortcut: a fixed spot; replace with a real "map loaded" signal.
bool InPit(float x, float z) { return std::hypot(x, z + 4.7f) < 3.0f; }

bool IsPlayer(uint8_t* base, uint32_t self, const float pos[3]) {
  uint32_t w[3];
  if (!ReadBe32(base, self + 1216, &w[0]) || !ReadBe32(base, self + 1220, &w[1]) ||
      !ReadBe32(base, self + 1224, &w[2])) {
    return false;
  }
  float far = 0;
  for (int k = 0; k < 3; ++k) {
    far = std::max(far, std::abs(std::bit_cast<float>(w[k]) - pos[k]));
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 12) {
    REXLOG_INFO("portal-probe: actor {:08X} ground pos ({:.2f}, {:.2f}, {:.2f}) vs skater ({:.2f}, {:.2f}, {:.2f}): {}",
                self, std::bit_cast<float>(w[0]), std::bit_cast<float>(w[1]), std::bit_cast<float>(w[2]), pos[0],
                pos[1], pos[2], far < 5.0f ? "PLAYER" : "no");
  }
  // shortcut: y < 0 is the career pit; that actor is thrown away when the map loads, so locking it means no hops.
  return far < 5.0f && !InPit(std::bit_cast<float>(w[0]), std::bit_cast<float>(w[2]));
}

// The human skater's actor, locked the first time one matches the presentation entity: at spawn the human is
// the only skater, but later the roaming park skaters appear and the entity helper can return one of them.
std::atomic<uint32_t> g_human{0};

// Arm the game's own relocation (the marker path) with a ready 4x4 target matrix; shared by the gate crossing.
bool ArmTeleport(uint8_t* base, uint32_t sp, uint32_t self, uint32_t flags, const float (&target)[16]) {
  uint32_t o1 = 0, o2 = 0, o3 = 0, o4 = 0;
  if ((flags & kRelocateBit) || !ReadBe32(base, self + 1880, &o1) || !ReadBe32(base, o1 + 12, &o2) ||
      !ReadBe32(base, o2 + 24, &o3) || !ReadBe32(base, o3 + 8, &o4)) {
    return false;
  }
  std::memcpy(t_target, target, sizeof(target));
  t_target_buf = sp - 96;  // sub_82DB88C8 takes 176 bytes of stack and keeps the matrix at +80
  t_target_set = true;
  const uint32_t be = __builtin_bswap32(flags | kRelocateBit);
  std::memcpy(base + self + 1296, &be, 4);
  return true;
}

// Portal gate pair, human skater only: track the skater's entity position frame to frame, ask the gate module
// whether it crossed a ring, and if so teleport it with the game's native path, which keeps airborne momentum.
// The game's world ray service. sub_82DB63C0 (StartSkeletonLineTests) queues the skeleton's foot tests through
// sub_82E0B4E0(manager, start*, delta*, zero*, filter) and gets a per-frame slot number back; sub_82DB6580
// (EndSkeletonLineTests) reads the results of those slots later in the same frame through service vtable +32.
// A slot number means nothing on a later frame (the log showed the same slot reused every frame), so the gun
// queues its shot inside the same Start/End pair: submit right after Start, read right after End.
thread_local bool t_in_start = false;
std::atomic<uint32_t> g_start_filter{0xFFFFFFFFu};  // the filter the game itself passes in Start

struct RayReq {
  std::atomic<int> state{0};  // 0 idle, 1 queued (waiting for Start), 2 submitted (waiting for End)
  int which = 0;
  float o[3] = {}, d[3] = {}, fallback[3] = {};
  bool free_aim = false;  // aimed with the mouse reticle: no wall under it means no portal, not a fallback
  uint32_t this_ptr = 0, handle = 0, mgr = 0;
  int age = 0;
};
RayReq g_ray;
thread_local bool t_gun_query = false;  // inside our own gun shot: never cut the opening out of it

struct SavedRegs {
  uint32_t r1, r3, r4, r5, r6, r7, lr, ctr;
  explicit SavedRegs(PPCContext& c)
      : r1(c.r1.u32), r3(c.r3.u32), r4(c.r4.u32), r5(c.r5.u32), r6(c.r6.u32), r7(c.r7.u32), lr(uint32_t(c.lr)),
        ctr(c.ctr.u32) {}
  void Restore(PPCContext& c) const {
    c.r1.u32 = r1;
    c.r3.u32 = r3;
    c.r4.u32 = r4;
    c.r5.u32 = r5;
    c.r6.u32 = r6;
    c.r7.u32 = r7;
    c.lr = lr;
    c.ctr.u32 = ctr;
  }
};

void WriteBeFloats(uint8_t* base, uint32_t addr, const float* v, int n) {
  for (int i = 0; i < n; ++i) WriteBeFloat(base, addr + 4 * i, v[i]);
}

// A shot with nothing usable under the reticle places nothing; the keyboard-only shot still falls back.
void GunFallback(const float skater[3]) {
  if (g_ray.free_aim) {
    REXLOG_INFO("portal-probe: GUN no wall under the reticle, no portal");
  } else {
    skate3::portal_gate::PlaceAhead(g_ray.which, skater);
  }
}

// Queue the shot: called every frame for the human; the actual query goes in at the next Start.
void GunStep(const float skater[3]) {
  int which = 0;
  if (g_ray.state == 0) {
    if (!skate3::portal_gate::TakeFire(&which)) {
      return;
    }
    float cam_o[3];
    const bool aimed = skate3::portal_gate::AimRay(cam_o, g_ray.d);
    g_ray.free_aim = aimed && skate3::portal_gate::AimFree();
    g_ray.which = which;
    if (g_ray.free_aim) {
      // From the camera through the reticle, starting just past the skater so the shot cannot hit the skater.
      const float t = std::max(0.0f, (skater[0] - cam_o[0]) * g_ray.d[0] + (skater[1] - cam_o[1]) * g_ray.d[1] +
                                         (skater[2] - cam_o[2]) * g_ray.d[2]) + 1.5f;
      for (int k = 0; k < 3; ++k) g_ray.o[k] = cam_o[k] + g_ray.d[k] * t;
    } else {
      const float hl = aimed ? std::hypot(g_ray.d[0], g_ray.d[2]) : 0.0f;
      if (hl < 0.05f) {
        REXLOG_INFO("portal-probe: GUN no camera ray; placing ahead");
        skate3::portal_gate::PlaceAhead(which, skater);
        return;
      }
      // The game camera looks down at the skater, so its centre ray hits the floor a few metres ahead. Portals go
      // on walls, so shoot level along the camera's heading, from the skater at hip height.
      g_ray.d[0] /= hl;
      g_ray.d[1] = 0.0f;
      g_ray.d[2] /= hl;
      std::copy(skater, skater + 3, g_ray.o);
    }
    std::copy(skater, skater + 3, g_ray.fallback);
    g_ray.age = 0;
    g_ray.state = 1;
    return;
  }
  if (++g_ray.age > 60) {  // never reached Start/End: do not leave the gun stuck
    g_ray.state = 0;
    REXLOG_INFO("portal-probe: GUN timed out in state {}", g_ray.state.load());
    GunFallback(g_ray.fallback);
  }
}

// Right after the game's own Start: add our query to the same batch.
void SubmitRay(PPCContext& ctx, uint8_t* base, uint32_t self) {
  uint32_t mgr = 0;
  const uint32_t filter = g_start_filter;
  if (!ReadBe32(base, self + 1908, &mgr) || !mgr || filter == 0xFFFFFFFFu) {
    g_ray.state = 0;
    REXLOG_INFO("portal-probe: GUN cannot submit (manager {:08X}, filter {:08X})", mgr, filter);
    GunFallback(g_ray.fallback);
    return;
  }
  const SavedRegs saved(ctx);
  const uint32_t scratch = (saved.r1 - 2048) & ~0xFu;
  const float vecs[12] = {g_ray.o[0], g_ray.o[1], g_ray.o[2], 1.0f, g_ray.d[0] * 60.0f, g_ray.d[1] * 60.0f,
                          g_ray.d[2] * 60.0f, 0.0f, 0, 0, 0, 0};
  WriteBeFloats(base, scratch, vecs, 12);
  ctx.r1.u32 = scratch - 256;
  ctx.r3.u32 = mgr;
  ctx.r4.u32 = scratch;
  ctx.r5.u32 = scratch + 16;
  ctx.r6.u32 = scratch + 32;
  ctx.r7.u32 = filter;
  sub_82E0B4E0(ctx, base);
  g_ray.handle = ctx.r3.u32;
  saved.Restore(ctx);
  g_ray.mgr = mgr;
  g_ray.this_ptr = self;
  g_ray.state = 2;
  REXLOG_INFO("portal-probe: GUN {} submitted in Start: from ({:.1f}, {:.1f}, {:.1f}) dir ({:.2f}, {:.2f}, {:.2f}) slot {} filter {}",
              g_ray.which ? "orange" : "blue", g_ray.o[0], g_ray.o[1], g_ray.o[2], g_ray.d[0], g_ray.d[1],
              g_ray.d[2], g_ray.handle, filter);
}

// Right after the game's own End: read our result from the same batch.
void CollectRay(PPCContext& ctx, uint8_t* base) {
  const SavedRegs saved(ctx);
  const uint32_t scratch = (saved.r1 - 2048) & ~0xFu;
  uint32_t service = 0, vtable = 0, fn = 0, res = 0;
  if (ReadBe32(base, g_ray.mgr + 4, &service) && ReadBe32(base, service, &vtable) &&
      ReadBe32(base, vtable + 32, &fn) && fn >= 0x82000000 && fn < 0x83000000) {
    static bool logged_service = false;
    if (!logged_service) {
      logged_service = true;
      uint32_t slot28 = 0, slot24 = 0, slot36 = 0, slot40 = 0;
      ReadBe32(base, vtable + 28, &slot28);
      ReadBe32(base, vtable + 24, &slot24);
      ReadBe32(base, vtable + 36, &slot36);
      ReadBe32(base, vtable + 40, &slot40);
      REXLOG_INFO("portal-probe: RAY SERVICE object {:08X} vtable {:08X} slots +24 {:08X} +28 (queue) {:08X} +32 (results) {:08X} +36 {:08X} +40 {:08X}",
                  service, vtable, slot24, slot28, fn, slot36, slot40);
    }
    ctx.r1.u32 = scratch - 256;
    ctx.r3.u32 = service;
    ctx.r4.u32 = g_ray.handle;
    ctx.ctr.u32 = fn;
    ctx.lr = 0;
    t_gun_query = true;
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    t_gun_query = false;
    res = ctx.r3.u32;
    saved.Restore(ctx);
  }
  float r[16] = {};
  uint32_t raw[16];
  const bool have = res >= 0x10000 && res < 0xC0000000 &&
                    skate3::native_scene::GuestTryCopy(raw, base + res, sizeof(raw));
  if (have) {
    for (int i = 0; i < 16; ++i) r[i] = BeFloat(raw[i]);
  }
  REXLOG_INFO("portal-probe: GUN result: ptr {:08X} {} pos ({:.2f}, {:.2f}, {:.2f}) normal ({:.2f}, {:.2f}, {:.2f}) time {:.3f}",
              res, have ? "readable" : "none", r[0], r[1], r[2], r[8], r[9], r[10], r[12]);
  g_ray.state = 0;
  const bool hit = have && r[12] >= 0.0f && std::isfinite(r[0]) && std::isfinite(r[8]);
  // The portal's front must face the shooter. The service's normal is not guaranteed to point back along the
  // shot (portals came out back to front), so flip it when it points the same way as the ray.
  if (hit && r[8] * g_ray.d[0] + r[9] * g_ray.d[1] + r[10] * g_ray.d[2] > 0.0f) {
    r[8] = -r[8];
    r[9] = -r[9];
    r[10] = -r[10];
    REXLOG_INFO("portal-probe: GUN normal pointed along the shot; flipped to face the shooter");
  }
  if (!hit || !skate3::portal_gate::PlaceOn(g_ray.which, r, r + 8, g_ray.d)) {
    GunFallback(g_ray.fallback);
  }
}

// The companion cube's own ray (one per frame) rides in the same Start/End batch as the gun's shot.
struct CubeRay {
  std::atomic<int> state{0};  // 0 idle, 1 wanted (waiting for Start), 2 submitted (waiting for End)
  skate3::portal_cube::RayWant want{};
  uint32_t handle = 0, mgr = 0, this_ptr = 0;
  int age = 0;
};
CubeRay g_cube_ray;

void SubmitCubeRay(PPCContext& ctx, uint8_t* base, uint32_t self) {
  uint32_t mgr = 0;
  const uint32_t filter = g_start_filter;
  if (!ReadBe32(base, self + 1908, &mgr) || !mgr || filter == 0xFFFFFFFFu) {
    g_cube_ray.state = 0;
    return;
  }
  const SavedRegs saved(ctx);
  const uint32_t scratch = ((saved.r1 - 2048) & ~0xFu) - 128;  // clear of the gun's scratch
  const auto& w = g_cube_ray.want;
  const float vecs[12] = {w.o[0], w.o[1], w.o[2], 1.0f, w.d[0] * w.len, w.d[1] * w.len, w.d[2] * w.len, 0.0f, 0, 0, 0, 0};
  WriteBeFloats(base, scratch, vecs, 12);
  ctx.r1.u32 = scratch - 256;
  ctx.r3.u32 = mgr;
  ctx.r4.u32 = scratch;
  ctx.r5.u32 = scratch + 16;
  ctx.r6.u32 = scratch + 32;
  ctx.r7.u32 = filter;
  sub_82E0B4E0(ctx, base);
  g_cube_ray.handle = ctx.r3.u32;
  saved.Restore(ctx);
  g_cube_ray.mgr = mgr;
  g_cube_ray.this_ptr = self;
  g_cube_ray.state = 2;
}

void CollectCubeRay(PPCContext& ctx, uint8_t* base) {
  const SavedRegs saved(ctx);
  const uint32_t scratch = ((saved.r1 - 2048) & ~0xFu) - 128;
  uint32_t service = 0, vtable = 0, fn = 0, res = 0;
  if (ReadBe32(base, g_cube_ray.mgr + 4, &service) && ReadBe32(base, service, &vtable) &&
      ReadBe32(base, vtable + 32, &fn) && fn >= 0x82000000 && fn < 0x83000000) {
    ctx.r1.u32 = scratch - 256;
    ctx.r3.u32 = service;
    ctx.r4.u32 = g_cube_ray.handle;
    ctx.ctr.u32 = fn;
    ctx.lr = 0;
    t_gun_query = true;
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    t_gun_query = false;
    res = ctx.r3.u32;
    saved.Restore(ctx);
  }
  g_cube_ray.state = 0;
  float r[16] = {};
  uint32_t raw[16];
  const bool have = res >= 0x10000 && res < 0xC0000000 && skate3::native_scene::GuestTryCopy(raw, base + res, sizeof(raw));
  if (have) {
    for (int i = 0; i < 16; ++i) r[i] = BeFloat(raw[i]);
  }
  const bool hit = have && r[12] >= 0.0f && std::isfinite(r[0]) && std::isfinite(r[8]);
  if (hit) {
    const auto& d = g_cube_ray.want.d;
    if (r[8] * d[0] + r[9] * d[1] + r[10] * d[2] > 0.0f) {  // normal faces back along the ray
      r[8] = -r[8];
      r[9] = -r[9];
      r[10] = -r[10];
    }
  }
  skate3::portal_cube::OnRay(g_cube_ray.want.kind, hit, r, r + 8);
}

constexpr uint32_t kLinRates = 0x140;

bool ReadVec(uint8_t* base, uint32_t addr, float out[3]) {
  uint32_t w[3];
  if (!ReadBe32(base, addr, &w[0]) || !ReadBe32(base, addr + 4, &w[1]) || !ReadBe32(base, addr + 8, &w[2])) {
    return false;
  }
  for (int k = 0; k < 3; ++k) out[k] = std::bit_cast<float>(w[k]);
  return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// Four big-endian floats (a vec4); false when the address is not readable.
bool ReadFloats4(uint8_t* base, uint32_t addr, float out[4]) {
  uint32_t w[4];
  if (addr < 0x10000 || addr >= 0xC0000000 || (addr & 3) || !skate3::native_scene::GuestTryCopy(w, base + addr, sizeof(w))) return false;
  for (int k = 0; k < 4; ++k) out[k] = BeFloat(w[k]);
  return true;
}

void Turn(const float m[9], const float v[3], float out[3]) {
  for (int k = 0; k < 3; ++k) out[k] = m[k * 3] * v[0] + m[k * 3 + 1] * v[1] + m[k * 3 + 2] * v[2];
}

// The pair's matrix, applied to every velocity vector the restore puts back (linear and angular alike).
float g_restore_m[9];
bool g_restore_turn = false;
bool g_turn_in_place = false;  // turn the live vectors after the teleport (when restore is not doing it)

// The Teleporting state (700/702) ends in state 100 (riding) when bit 0 of this byte is set and state 500 (off the
// board, speed gone) when it is clear. The byte is the skeleton's field at +10768 (skate-core's
// byte_10768 -> byte_1600), and the checkpoint request path writes it per marker; our own target never does, so
// a crossing could land on either. Keep it set for the frames around a crossing.
constexpr uint32_t kOnBoardByte = 10768;
int g_onboard_frames = 0;
int g_recover_window = 0;  // frames after a crossing in which the game's own recovery warps keep the board's rates

void ForceOnBoard(uint8_t* base, uint32_t self, const char* when) {
  uint32_t skeleton = 0, w = 0;
  if (!ReadBe32(base, self + 1884, &skeleton) || !ReadBe32(base, skeleton + kOnBoardByte, &w)) return;
  if (!(w & 0x01000000u)) {
    REXLOG_INFO("portal-probe: GATE on-board byte was clear ({:08X}) {}; setting it", w, when);
    const uint32_t be = __builtin_bswap32(w | 0x01000000u);
    std::memcpy(base + skeleton + kOnBoardByte, &be, 4);
  }
}

// Turn, in place, every velocity vector the teleport leaves behind: the skeleton's body rates (+0x2770..+0x2900,
// 16-byte stride, found by the scan), and the board block's 7 linear and 7 angular rates.
void TurnLive(uint8_t* base, uint32_t self) {
  uint32_t skeleton = 0, board = 0, block = 0;
  ReadBe32(base, self + 1884, &skeleton);
  const bool have_block = ReadBe32(base, self + 1880, &board) && ReadBe32(base, board + 12, &block);
  int turned = 0;
  auto one = [&](uint32_t addr) {
    if (addr < 0x10000 || addr >= 0xC0000000 || (addr & 3)) return;
    float v[3], o[3];
    if (!ReadVec(base, addr, v)) return;
    const float m = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(m > 0.05f && m < 200.0f)) return;
    Turn(g_restore_m, v, o);
    for (int k = 0; k < 3; ++k) WriteBeFloat(base, addr + 4 * k, o[k]);
    ++turned;
  };
  if (skeleton) {
    for (uint32_t off = 0x2770; off <= 0x2900; off += 16) one(skeleton + off);
  }
  if (have_block) {
    for (int i = 0; i < 7; ++i) {
      one(block + 0x140 + 16 * i);
      one(block + 0x1B0 + 16 * i);
    }
  }
  REXLOG_INFO("portal-probe: GATE turned {} live velocity vectors in place", turned);
}

void TurnRaw(uint32_t w[3]) {
  if (!g_restore_turn) return;
  const float v[3] = {BeFloat(w[0]), BeFloat(w[1]), BeFloat(w[2])};
  float o[3];
  Turn(g_restore_m, v, o);
  for (int k = 0; k < 3; ++k) w[k] = __builtin_bswap32(std::bit_cast<uint32_t>(o[k]));
}

// Surgery path: SetTransform keeps the board's rates but does not turn them, so the board kept falling straight down
// out of a wall (run 103: floor<->wall loop, boardV (0,-30,0) at every exit, then tunnelling through the floor).
// Turn the block's 7 linear and 7 angular rates by the pair matrix, after capping the exit speed. A cap is folded
// into the matrix so the restored skeleton vectors (TurnRaw) scale the same way.
// shortcut: one fixed cap for every surface, make it a cvar if a fast level needs more.
constexpr float kMaxExitSpeed = 80.0f;  // m/s: portal energy is allowed to build (was 22); only the 600+ m/s solver blow-ups are cut
void TurnAndCapBlock(uint8_t* base, uint32_t block) {
  float fastest = 0.0f;
  for (int i = 0; i < 7; ++i) {
    float v[3];
    if (ReadVec(base, block + kLinRates + 16 * i, v) && std::isfinite(v[0] + v[1] + v[2])) {
      fastest = std::max(fastest, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
    }
  }
  if (fastest > kMaxExitSpeed) {
    const float k = kMaxExitSpeed / fastest;
    for (float& x : g_restore_m) x *= k;
    g_restore_turn = true;
    REXLOG_INFO("portal-probe: GATE exit speed capped {:.1f} -> {:.1f} m/s", fastest, kMaxExitSpeed);
  }
  if (!g_restore_turn) return;
  // Run 121 FRAME log: the turned rates are replaced one frame later by the actor's own world velocity (+1248), which the
  // game leaves at its pre-crossing value for ~3 frames (frame +1 rates = actorV direction, flipped back into the wall).
  if (const uint32_t actor = g_human) {
    float v[3], o[3];
    if (ReadVec(base, actor + 1248, v) && std::isfinite(v[0] + v[1] + v[2])) {
      Turn(g_restore_m, v, o);
      for (int k = 0; k < 3; ++k) WriteBeFloat(base, actor + 1248 + 4 * k, o[k]);
      REXLOG_INFO("portal-probe: GATE actor velocity turned ({:.2f}, {:.2f}, {:.2f}) -> ({:.2f}, {:.2f}, {:.2f})", v[0], v[1],
                  v[2], o[0], o[1], o[2]);
    }
  }
  for (int i = 0; i < 7; ++i) {
    for (const uint32_t off : {kLinRates, uint32_t(0x1B0)}) {
      float v[3], o[3];
      const uint32_t addr = block + off + 16 * i;
      if (!ReadVec(base, addr, v) || !std::isfinite(v[0] + v[1] + v[2])) continue;
      Turn(g_restore_m, v, o);
      for (int k = 0; k < 3; ++k) WriteBeFloat(base, addr + 4 * k, o[k]);
    }
  }
}

// Where does the skater's velocity live? At each crossing, search the human actor, its board, the board's
// physics block and its skeleton for float pairs/triples equal to the horizontal velocity we measured, then log
// the same words right after the teleport and 3 frames later. A word that still holds the old value after the
// teleport (the game keeps it) and is rewritten by the game after is the carrier to turn.
struct ScanHit {
  uint32_t addr;
  const char* region;
  uint32_t off;
};
std::vector<ScanHit> g_scan;
int g_scan_frames = 0;

void ScanFor(uint8_t* base, const char* region, uint32_t start, uint32_t bytes, const float w[2]) {
  if (start < 0x10000 || start >= 0xC0000000 || (start & 3)) return;
  std::vector<uint32_t> words(bytes / 4);
  if (!skate3::native_scene::GuestTryCopy(words.data(), base + start, bytes)) return;
  const float tol = 0.15f * std::hypot(w[0], w[1]) + 0.3f;
  int found = 0;
  for (uint32_t i = 0; i + 3 < words.size() && found < 40; ++i) {
    const float f0 = BeFloat(words[i]), f1 = BeFloat(words[i + 1]), f2 = BeFloat(words[i + 2]);
    const bool triple = std::abs(f0 - w[0]) < tol && std::abs(f2 - w[1]) < tol;  // (x, y, z)
    const bool pair = std::abs(f0 - w[0]) < tol && std::abs(f1 - w[1]) < tol;    // (x, z)
    if (!triple && !pair) continue;
    g_scan.push_back({start + 4 * i, region, 4 * i});
    REXLOG_INFO("portal-probe: SCAN {} +0x{:X} ({:.2f}, {:.2f}, {:.2f}) {} of v=({:.2f}, {:.2f})", region, 4 * i, f0, f1,
                f2, triple ? "triple" : "pair", w[0], w[1]);
    ++found;
  }
}

void ScanLog(uint8_t* base, const char* when) {
  for (const ScanHit& h : g_scan) {
    uint32_t a, b, c;
    if (!ReadBe32(base, h.addr, &a) || !ReadBe32(base, h.addr + 4, &b) || !ReadBe32(base, h.addr + 8, &c)) continue;
    REXLOG_INFO("portal-probe: SCAN {} {} +0x{:X} now ({:.2f}, {:.2f}, {:.2f})", when, h.region, h.off,
                std::bit_cast<float>(a), std::bit_cast<float>(b), std::bit_cast<float>(c));
  }
}

// Crossing setups. Run 064 kept 6 m/s through a rolling crossing and later builds lost it, and one session cannot
// say which change did it, so the gate tries each setup and scores it: after a crossing at >= 3 m/s, the mean
// speed 15-30 frames later divided by the speed before (0 when the game bails or warps back). It tries every
// setup twice, then mostly uses the best (15% of crossings still explore), and keeps the scores in
// portal_gate_profiles.txt (working directory) for the next session.
struct Profile {
  bool restore;        // put back the velocity vectors the teleport zeroes (skeleton +0x2770.., block rates), turned
  bool surgery;        // replace the executor's board reset (zeroes every rate) with the rate-preserving SetTransform
  float lift;          // metres the feet start above the exit point (a ground-level exit loses the speed)
  const char* name;
};
// Runs 074-079: no knob changed how much speed survives a grounded crossing, and writing the actor's position fields
// causes the flicker/respawn glitch. The executor (82DB8998) always calls the board reset 82C05F50, which zeroes all
// 14 board rates; skate-core's set_transform (82C0B2C8) moves the same assembly and keeps them. B swaps one for the
// other inside the executor; C also restores the skeleton's zeroed vectors.
constexpr int kProfileCount = 3;
// Run 082: C kept 96% (n=29), B 88% (n=6), plain 39% (n=3) - so C is the default when the learner is off.
constexpr int kDefaultProfile = 2;
constexpr Profile kProfiles[kProfileCount] = {{false, false, 1.0f, "A: plain teleport"},
                                              {false, true, 1.0f, "B: board move keeps rates"},
                                              {true, true, 1.0f, "C: board move + restored skeleton"}};
struct ProfStat {
  int n = 0;
  float sum = 0;
};
ProfStat g_stats[kProfileCount];
bool g_stats_loaded = false;
int g_next_prof = -1, g_cur_prof = -1, g_cross_count = 0;
bool g_prof_restore = false;  // the profile of the crossing in progress wants the zeroed vectors put back
float g_score_sum = 0;
int g_score_n = 0;
constexpr const char* kStatsFile = "portal_gate_profiles6.txt";

void SaveStats() {
  if (FILE* f = std::fopen(kStatsFile, "w")) {
    for (const ProfStat& st : g_stats) std::fprintf(f, "%d %f\n", st.n, st.sum);
    std::fclose(f);
  }
}

int SelectProfile() {
  if (!g_stats_loaded) {
    g_stats_loaded = true;
    if (FILE* f = std::fopen(kStatsFile, "r")) {
      for (ProfStat& st : g_stats) {
        if (std::fscanf(f, "%d %f", &st.n, &st.sum) != 2) st = ProfStat{};
      }
      std::fclose(f);
    }
  }
  int least = 0;
  for (int i = 1; i < kProfileCount; ++i) {
    if (g_stats[i].n < g_stats[least].n) least = i;
  }
  if (g_stats[least].n < 2) return least;
  static std::mt19937 rng{std::random_device{}()};
  if (std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) < 0.15f) {
    return int(rng() % kProfileCount);
  }
  int best = 0;
  for (int i = 1; i < kProfileCount; ++i) {
    if (g_stats[i].sum / g_stats[i].n > g_stats[best].sum / g_stats[best].n) best = i;
  }
  return best;
}

void FinishScore(float pre_speed) {
  if (g_cur_prof < 0) return;
  if (pre_speed >= 3.0f && g_score_n > 0) {
    const float score = std::clamp(g_score_sum / float(g_score_n) / pre_speed, 0.0f, 1.5f);
    g_stats[g_cur_prof].n++;
    g_stats[g_cur_prof].sum += score;
    SaveStats();
    char line[400];
    int n = 0;
    for (int i = 0; i < kProfileCount; ++i) {
      n += std::snprintf(line + n, sizeof(line) - n, " %c=%.2f(%d)", 'A' + i,
                         g_stats[i].n ? g_stats[i].sum / g_stats[i].n : 0.0f, g_stats[i].n);
    }
    REXLOG_INFO("portal-probe: GATE learn: {} kept {:.0f}% of {:.1f} m/s; mean kept per setup:{}",
                kProfiles[g_cur_prof].name, score * 100.0f, pre_speed, line);
  }
  g_cur_prof = -1;
  g_next_prof = SelectProfile();
}

// The actor's position bookkeeping around +1200..+1264 (ground positions, deltas). Captured before a crossing, logged
// after, and optionally put back (deltas turned by the pair's matrix) so the game keeps seeing the old motion.
constexpr uint32_t kActorFrom = 1184, kActorWords = 24;
uint32_t g_actor_pre[kActorWords];
bool g_actor_pre_valid = false, g_prof_surgery = false;
int g_delta_frames = 0, g_actor_logged = 0;

void CaptureActor(uint8_t* base, uint32_t self) {
  g_actor_pre_valid = true;
  for (uint32_t i = 0; i < kActorWords; ++i) g_actor_pre_valid &= ReadBe32(base, self + kActorFrom + 4 * i, &g_actor_pre[i]);
}

void LogActor(uint8_t* base, uint32_t self, const char* when) {
  if (g_actor_logged > 24) return;
  ++g_actor_logged;
  for (uint32_t i = 0; i < kActorWords; i += 4) {
    uint32_t w[4] = {};
    for (int k = 0; k < 4; ++k) ReadBe32(base, self + kActorFrom + 4 * (i + k), &w[k]);
    REXLOG_INFO("portal-probe: GATE actor +{} {}: pre ({:.3f}, {:.3f}, {:.3f}, {:.3f}) now ({:.3f}, {:.3f}, {:.3f}, {:.3f})",
                kActorFrom + 4 * i, when, std::bit_cast<float>(g_actor_pre[i]), std::bit_cast<float>(g_actor_pre[i + 1]),
                std::bit_cast<float>(g_actor_pre[i + 2]), std::bit_cast<float>(g_actor_pre[i + 3]),
                std::bit_cast<float>(w[0]), std::bit_cast<float>(w[1]), std::bit_cast<float>(w[2]),
                std::bit_cast<float>(w[3]));
  }
}

float g_move_target[3];  // the exit ground position (feet) of the crossing in progress
int g_move_k = 0;

// Everything the game uses to see motion, moved as one: ground positions to where the skater now is (advanced by
// the per-frame delta on later frames), delta and velocity kept (turned). Without this the game sees a 15 m jump.
void WriteActor(uint8_t* base, uint32_t self, int k) {
  if (!g_actor_pre_valid) return;
  float d[3], v[3];
  const uint32_t di = (1232 - kActorFrom) / 4, vi = (1248 - kActorFrom) / 4;
  const float d0[3] = {std::bit_cast<float>(g_actor_pre[di]), std::bit_cast<float>(g_actor_pre[di + 1]),
                       std::bit_cast<float>(g_actor_pre[di + 2])};
  const float v0[3] = {std::bit_cast<float>(g_actor_pre[vi]), std::bit_cast<float>(g_actor_pre[vi + 1]),
                       std::bit_cast<float>(g_actor_pre[vi + 2])};
  std::copy(d0, d0 + 3, d);
  std::copy(v0, v0 + 3, v);
  if (g_restore_turn) {
    Turn(g_restore_m, d0, d);
    Turn(g_restore_m, v0, v);
  }
  for (int c = 0; c < 3; ++c) {
    const float pos = g_move_target[c] + d[c] * float(k);
    WriteBeFloat(base, self + 1200 + 4 * c, pos);
    WriteBeFloat(base, self + 1216 + 4 * c, pos);
    WriteBeFloat(base, self + 1232 + 4 * c, d[c]);
    WriteBeFloat(base, self + 1248 + 4 * c, v[c]);
  }
}

bool GateStep(PPCContext& ctx, uint8_t* base, uint32_t self) {
  const uint32_t sp = ctx.r1.u32;
  static float tracked[3], last[3];
  static bool have = false;
  static float up_offset = 0.85f;  // skater height above the actor's ground position
  uint32_t flags = 0, w[3];
  if (!ReadBe32(base, self + 1296, &flags) || !ReadBe32(base, self + 1216, &w[0]) ||
      !ReadBe32(base, self + 1220, &w[1]) || !ReadBe32(base, self + 1224, &w[2])) {
    return false;
  }
  const float ground[3] = {std::bit_cast<float>(w[0]), std::bit_cast<float>(w[1]), std::bit_cast<float>(w[2])};
  const bool riding = (flags & 0xF0000000) == 0xC0000000;
  if (InPit(ground[0], ground[2])) {
    have = false;  // career pit
    return false;
  }
  const float place_pos[3] = {ground[0], ground[1] + up_offset, ground[2]};
  {
    // first person looks from the rider, not the board
    float head[3];
    if (have && skate3::native_entity::SkaterWorldPosNear(base, tracked, 4.0f, head)) skate3::portal_gate::SetHead(head);
  }
  GunStep(have ? tracked : place_pos);
  float p[3];
  if (riding) {
    // The actor's ground position is current while riding. The entity helper latched onto other skaters after
    // a teleport (it follows "nearest to the last position"), so it is only used in the air, from here.
    std::copy(place_pos, place_pos + 3, p);
  } else if (!have || !skate3::native_entity::SkaterWorldPosNear(base, tracked, 2.5f, p)) {
    return false;  // airborne without a recent riding position to follow
  }
  std::copy(p, p + 3, tracked);
  // Speed of the tracked position, logged for ~48 frames after a crossing.
  static std::chrono::steady_clock::time_point last_t{};
  static float pre_speed = 0, speed_now = 0;
  static int speed_log_left = 0;
  static int trace_left = 0;  // longer trace after a crossing: where do momentum and tricks die?
  {
    const auto t = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(t - last_t).count();
    if (have && dt > 0.001f && dt < 0.2f) {
      speed_now = std::hypot(p[0] - last[0], p[2] - last[2]) / dt;
    }
    last_t = t;
    if (speed_log_left > 0) {
      if (speed_log_left <= 33 && speed_log_left >= 18) {  // 15-30 frames after the crossing
        g_score_sum += speed_now;
        ++g_score_n;
      } else if (speed_log_left == 17) {
        FinishScore(pre_speed);
      }
    }
    if (speed_log_left > 0 && (speed_log_left-- % 4) == 0) {
      uint32_t st = 0;
      ReadBe32(base, self + 1336, &st);
      REXLOG_INFO("portal-probe: GATE speed after crossing: {:.1f} m/s (before {:.1f}) flags {:08X} state {}",
                  speed_now, pre_speed, flags, st);
    }
    {
      // Trick continuity (user: a trick grab ends when crossing): which processed-input flag bits change right after a crossing?
      uint32_t in = 0;
      ReadBe32(base, self + 1804, &in);
      uint32_t cur[6] = {};
      bool ok = in >= 0x10000;
      for (int i = 0; i < 6 && ok; ++i) ok = ReadBe32(base, in + 2468 + 4 * i, &cur[i]);
      static uint32_t pre_in[6] = {};
      static int logged = 0;
      if (ok) {
        for (int i = 0; i < 6; ++i) {
          if (cur[i] != pre_in[i] && logged++ < 600) {  // (always, not only after a crossing: a trick grab with no portal is the baseline)
            REXLOG_INFO("portal-probe: INPUT flags +{}: {:08X} -> {:08X} (cleared {:08X}, set {:08X}) post-crossing frames left {}", 2468 + 4 * i, pre_in[i], cur[i],
                        pre_in[i] & ~cur[i], cur[i] & ~pre_in[i], trace_left);
          }
          pre_in[i] = cur[i];
        }
      }
    }
    if (trace_left > 0 && (trace_left-- % 6) == 0) {
      uint32_t st = 0, bd = 0, bk = 0;
      ReadBe32(base, self + 1336, &st);
      float av[3] = {}, bv[3] = {};
      ReadVec(base, self + 1248, av);
      if (ReadBe32(base, self + 1880, &bd) && ReadBe32(base, bd + 12, &bk) && bk >= 0x10000) ReadVec(base, bk + kLinRates, bv);
      REXLOG_INFO("portal-probe: TRACE after crossing, {} frames left: state {} flags {:08X} y {:.2f} speed {:.1f} actorV ({:.1f},{:.1f},{:.1f}) boardV ({:.1f},{:.1f},{:.1f})",
                  trace_left, st, flags, p[1], speed_now, av[0], av[1], av[2], bv[0], bv[1], bv[2]);
    }
  }
  // Crossings need the game to report the skater as airborne (flags 0x90000000 / 0x8E000000: top bit set,
  // 0x40000000 clear). Riding crossings (0xC0000000) lose all speed and often trigger the game's recovery warp.
  const bool in_air = (flags & 0x80000000) && !(flags & 0x40000000) && !(flags & 0x00800000) &&
                      !(flags & kRelocateBit);
  // With the hole the skater is already half through the surface when the crossing fires, and the game flags that as a
  // recovery (40000000 / 48000000 / 4E000000): refusing then left the skater falling through the open hole for good.
  const bool allowed = REXCVAR_GET(skate3_portal_gate_hole)
                           ? !(flags & kRelocateBit)
                           : in_air || (REXCVAR_GET(skate3_portal_gate_ground) && riding && !(flags & 0x00800000));
  float hx = have ? p[0] - last[0] : 0.0f, hz = have ? p[2] - last[2] : 0.0f;
  skate3::portal_gate::SetTravel(hx, hz, speed_now);
  {
    // Companion cube (Ctrl+5 / D-pad down + X): spawn, queue this frame's ray, step its physics.
    static auto cube_t = std::chrono::steady_clock::now();
    const auto now_t = std::chrono::steady_clock::now();
    const float cdt = std::chrono::duration<float>(now_t - cube_t).count();
    cube_t = now_t;
    if (skate3::portal_cube::TakeSpawn()) {
      float cam[3], dir[3];
      float fx = 0.0f, fz = 1.0f;
      if (skate3::portal_gate::AimRay(cam, dir)) {
        const float l = std::hypot(p[0] - cam[0], p[2] - cam[2]);
        if (l > 0.05f) {
          fx = (p[0] - cam[0]) / l;
          fz = (p[2] - cam[2]) / l;
        }
      }
      const float at[3] = {p[0] + fx * 2.4f, p[1] + 0.8f, p[2] + fz * 2.4f};
      skate3::portal_cube::Spawn(at);
    }
    // Carrying, like the portal gun: the object goes to the point under the crosshair that is 3 m from the skater (the far side of
    // a sphere around the skater, along the camera's aim ray); without an aim ray, 1.9 m ahead along the camera's heading.
    float face[2] = {0.0f, 1.0f};
    float aim_o[3] = {0, 0, 0}, aim_d[3] = {0, 0, 1};
    float hand[3];
    const bool aim_ok = skate3::portal_gate::AimRay(aim_o, aim_d);
    {
      if (aim_ok) {
        const float l = std::hypot(p[0] - aim_o[0], p[2] - aim_o[2]);
        if (l > 0.05f) {
          face[0] = (p[0] - aim_o[0]) / l;
          face[1] = (p[2] - aim_o[2]) / l;
        }
        constexpr float kReach = 3.0f;
        const float oc[3] = {aim_o[0] - p[0], aim_o[1] - p[1], aim_o[2] - p[2]};
        const float b = aim_d[0] * oc[0] + aim_d[1] * oc[1] + aim_d[2] * oc[2];
        const float c = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - kReach * kReach;
        const float disc = b * b - c;
        if (disc > 0.0f) {
          const float t = -b + std::sqrt(disc);
          for (int k = 0; k < 3; ++k) hand[k] = aim_o[k] + aim_d[k] * t;
        } else {
          for (int k = 0; k < 3; ++k) hand[k] = p[k] + aim_d[k] * kReach;
        }
        hand[1] = std::max(hand[1], p[1] - 0.6f);  // never into the ground
      } else {
        hand[0] = p[0] + face[0] * 1.9f;
        hand[1] = p[1] - 0.05f;
        hand[2] = p[2] + face[1] * 1.9f;
      }
    }
    {
      const float sv0[2] = {have && cdt > 0.001f && cdt < 0.2f ? (p[0] - last[0]) / cdt : 0.0f,
                            have && cdt > 0.001f && cdt < 0.2f ? (p[2] - last[2]) / cdt : 0.0f};
      const float sp0 = std::hypot(sv0[0], sv0[1]);
      const float cap0[2] = {sp0 > 25.0f ? sv0[0] * 25.0f / sp0 : sv0[0], sp0 > 25.0f ? sv0[1] * 25.0f / sp0 : sv0[1]};
      if (skate3::portal_cube::TakeGrab()) {
        // One bind for everything: drop what is held, else pick up what is under the crosshair (the closest of the cube and the game's
        // loose objects to the aim ray).
        float cube_score = 1e9f;
        skate3::portal_cube::Draw cd;
        if (skate3::portal_cube::Held()) {
          skate3::portal_cube::ToggleHold(p, cap0);
        } else {
          if (skate3::portal_cube::Snapshot(&cd)) {
            const float away = std::hypot(cd.c[0] - p[0], std::hypot(cd.c[1] - p[1], cd.c[2] - p[2]));
            float perp = std::hypot(cd.c[0] - hand[0], std::hypot(cd.c[1] - hand[1], cd.c[2] - hand[2]));
            if (aim_ok) {
              const float r[3] = {cd.c[0] - aim_o[0], cd.c[1] - aim_o[1], cd.c[2] - aim_o[2]};
              const float along = r[0] * aim_d[0] + r[1] * aim_d[1] + r[2] * aim_d[2];
              perp = std::hypot(r[0] - aim_d[0] * along, std::hypot(r[1] - aim_d[1] * along, r[2] - aim_d[2] * along));
            }
            if (away <= 5.0f && perp <= 1.8f) cube_score = perp + 0.15f * away;
          }
          if (!BodiesGrabToggle(base, p, cap0, aim_o, aim_d, aim_ok, hand, cube_score)) skate3::portal_cube::ToggleHold(p, cap0);
        }
      }
      BodiesFrame(base, p, cap0, hand, cdt);
    }
    if (skate3::portal_cube::Active()) {
      if (g_cube_ray.state == 0) {
        if (skate3::portal_cube::NextRay(&g_cube_ray.want)) {
          g_cube_ray.age = 0;
          g_cube_ray.state = 1;
        }
      } else if (++g_cube_ray.age > 30) {
        g_cube_ray.state = 0;  // never reached Start/End: do not leave it stuck
      }
      const float sv[2] = {have && cdt > 0.001f && cdt < 0.2f ? (p[0] - last[0]) / cdt : 0.0f,
                           have && cdt > 0.001f && cdt < 0.2f ? (p[2] - last[2]) / cdt : 0.0f};
      const float sp2 = std::hypot(sv[0], sv[1]);
      const float cap[2] = {sp2 > 25.0f ? sv[0] * 25.0f / sp2 : sv[0], sp2 > 25.0f ? sv[1] * 25.0f / sp2 : sv[1]};
      skate3::portal_cube::Step(cdt, p, cap, hand);
    }
  }
  std::copy(p, p + 3, last);
  have = true;
  const float speed_before = speed_now;
  skate3::portal_gate::Cross cross;
  const bool ab = REXCVAR_GET(skate3_portal_gate_ab);
  if (g_next_prof < 0) g_next_prof = SelectProfile();
  const Profile& prof = kProfiles[ab ? g_next_prof : kDefaultProfile];
  skate3::portal_gate::SetWallClear(1.2f);
  // A crossing from player state 100 keeps its speed; from 200 (board bouncing on rough ground) it does not. When
  // the trigger fires in another state, hold the teleport a few frames for a riding frame (the skater keeps rolling).
  static struct {
    bool active = false;
    int waited = 0;
    skate3::portal_gate::Cross cross;
  } hold;
  uint32_t st_now = 0;
  ReadBe32(base, self + 1336, &st_now);
  if (hold.active) {
    if (false) {
      return false;
    }
    REXLOG_INFO("portal-probe: GATE held {} frames, crossing now in state {}", hold.waited, st_now);
    cross = hold.cross;
    hold.active = false;
  } else if (!skate3::portal_gate::Step(p, allowed, flags, &cross)) {
    return false;
  } else if (false) {
    hold.active = true;
    hold.waited = 0;
    hold.cross = cross;
    return false;
  }
  pre_speed = speed_before;
  ++g_cross_count;
  g_cur_prof = ab ? g_next_prof : -1;
  g_prof_restore = prof.restore;
  g_prof_surgery = prof.surgery;
  CaptureActor(base, self);
  g_score_sum = 0;
  g_score_n = 0;
  REXLOG_INFO("portal-probe: GATE profile {} (crossing #{})", prof.name, g_cross_count);
  speed_log_left = 48;
  trace_left = 96;
  uint32_t state_at_cross = 0;
  ReadBe32(base, self + 1336, &state_at_cross);
  REXLOG_INFO("portal-probe: GATE crossing with flags {:08X} ({}), player state {}, speed {:.1f} m/s", flags,
              riding ? "riding" : "airborne", state_at_cross, speed_before);
  // Follow the skater through the portal: the airborne tracker only looks within 2.5 m of its last position, so after a
  // longer jump it stayed behind (camera and gun origin left at the old portal until the next landing), and the
  // jump itself read as a 30+ m/s speed.
  std::copy(cross.exit_pos, cross.exit_pos + 3, tracked);
  std::copy(cross.exit_pos, cross.exit_pos + 3, last);
  // Velocity in: the board's own rate (block +0x140), else the tracked displacement. Out: the pair's matrix
  // applied to it. Facing after the crossing = the horizontal direction of the velocity that leaves.
  uint32_t board = 0, block = 0;
  const bool have_block = ReadBe32(base, self + 1880, &board) && ReadBe32(base, board + 12, &block) &&
                          block >= 0x10000 && block < 0xC0000000 && !(block & 3);
  float vin[3] = {0, 0, 0}, vout[3];
  // The board's fastest body (body 0 is not always the one carrying the speed).
  bool board_rate = false;
  if (have_block && false) {
    float best = 1.0f;
    for (int i = 0; i < 7; ++i) {
      float v[3];
      if (ReadVec(base, block + kLinRates + 16 * i, v)) {
        const float m = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (m > best) {
          best = m;
          std::copy(v, v + 3, vin);
          board_rate = true;
        }
      }
    }
  }
  const float dlen = std::hypot(hx, hz);
  const float meas[2] = {dlen > 0.01f ? hx / dlen * speed_before : 0.0f, dlen > 0.01f ? hz / dlen * speed_before : 0.0f};
  if (!board_rate) {
    vin[0] = meas[0];
    vin[1] = 0.0f;
    vin[2] = meas[1];
  }
  if (speed_before >= 3.0f && REXCVAR_GET(skate3_portal_probe)) {
    g_scan.clear();
    g_scan_frames = 0;
    uint32_t skeleton = 0;
    ReadBe32(base, self + 1884, &skeleton);
    REXLOG_INFO("portal-probe: SCAN for v=({:.2f}, {:.2f}) speed {:.1f}: actor {:08X} board {:08X} block {:08X} skeleton {:08X}",
                meas[0], meas[1], speed_before, self, board, block, skeleton);
    ScanFor(base, "actor", self, 0x800, meas);
    ScanFor(base, "board", board, 0x200, meas);
    ScanFor(base, "block", block, 0x2000, meas);
    ScanFor(base, "skeleton", skeleton, 0x4100, meas);
    ScanLog(base, "pre");
  }
  // With the turn off the velocity is not touched, so the facing follows the motion we actually have.
  if (REXCVAR_GET(skate3_portal_gate_turn)) {
    Turn(cross.vel, vin, vout);
  } else {
    std::copy(vin, vin + 3, vout);
  }
  float len = std::hypot(vout[0], vout[2]);
  if (len < 0.5f) {
    hx = cross.exit_dir[0];
    hz = cross.exit_dir[1];
    len = std::hypot(hx, hz);
    if (len < 0.01f) {
      hx = 0.0f;
      hz = 1.0f;
      len = 1.0f;
    }
  } else {
    hx = vout[0];
    hz = vout[2];
  }
  hx /= len;
  hz /= len;
  float change = 0.0f;  // how far the matrix is from "no change"
  for (int k = 0; k < 9; ++k) change = std::max(change, std::abs(cross.vel[k] - ((k % 4 == 0) ? 1.0f : 0.0f)));
  REXLOG_INFO("portal-probe: GATE velocity in ({:.1f}, {:.1f}, {:.1f}) out ({:.1f}, {:.1f}, {:.1f}), matrix change {:.2f}",
              vin[0], vin[1], vin[2], vout[0], vout[1], vout[2], change);
  g_restore_turn = change > 0.05f && REXCVAR_GET(skate3_portal_gate_turn);
  g_turn_in_place = g_restore_turn && !(prof.restore || REXCVAR_GET(skate3_portal_gate_restore));
  std::copy(cross.vel, cross.vel + 9, g_restore_m);
  // Exit height: a target at ground level makes the game put the skater off the board (state 500) and the
  // speed dies; a target ~1 m above the ground starts it airborne (state 200), which keeps the velocity (7 of 9
  // logged crossings, all ~1 m up, kept 7-9 m/s; both ground-level ones lost it).
  const float kExitLift = prof.lift;
  const float target[16] = {hz, 0, -hx, 0, 0, 1, 0, 0, hx, 0, hz, 0,
                            cross.exit_pos[0], cross.exit_pos[1] - up_offset + kExitLift * cross.lift, cross.exit_pos[2], 1};
  // The grounded teleport zeroes the skeleton's velocities and the airborne one keeps them; the only difference
  // the executor can see is the actor's flags, so show it the airborne ones (the game rewrites them next frame).
  uint32_t arm_flags = flags;
  if (false && (flags & 0x40000000)) {
    arm_flags &= ~0x40000000u;
    REXLOG_INFO("portal-probe: GATE airmode: flags {:08X} -> {:08X} for the teleport", flags, arm_flags);
  }
  std::copy(target + 12, target + 15, g_move_target);
  const bool ok = ArmTeleport(base, sp, self, arm_flags, target);
  REXLOG_INFO("portal-probe: GATE teleport {}: feet target ({:.2f}, {:.2f}, {:.2f}) heading ({:.2f}, {:.2f})",
              ok ? "armed" : "REFUSED", target[12], target[13], target[14], hx, hz);
  have = false;  // after the jump the old position is meaningless; wait for a riding frame
  return ok;
}

// Move mode (cvar value 2): instead of the game's teleport (which resets every subsystem), call the board's
// SetTransform 82C0B2C8, which "moves the whole board assembly while preserving rates, forces and contacts"
// (Rust notes), and rewrite the actor's ground-position history so the jump is not read as a velocity.
thread_local bool t_move_pending = false;
thread_local float t_move_matrix[16];

// Test hop: jump back to where the human was kSteps ago, facing the current direction of travel, and keep
// the board's velocity (see the restore in sub_82C05F50). Positions are the actor's ground position, which
// is only current while riding on the ground, so the trail only records, and hops only fire, then.
// Executor matrix convention, from the game's own placements: rows are axes, row 2 is forward (it pointed
// along the skater's first push), row 0 is right = (f.z, 0, -f.x), translation in row 3.
struct Crumb {
  std::chrono::steady_clock::time_point t;
  float p[3];
};

bool ArmRelocation(uint8_t* base, uint32_t sp, uint32_t self) {
  using Clock = std::chrono::steady_clock;
  constexpr auto kTestPeriod = std::chrono::seconds(6);
  constexpr auto kBack = std::chrono::milliseconds(3000);
  static Clock::time_point next{};  // set when the player first looks steady, so a slow load can't eat the delay
  static std::deque<Crumb> trail;
  static std::mutex trail_mutex;
  const double enabled = REXCVAR_GET(skate3_portal_probe_teleport);
  float pos[3];
  if (!g_human) {
    uint32_t expected = 0;
    if (!skate3::native_entity::SkaterWorldPos(base, pos) || !IsPlayer(base, self, pos) ||
        !g_human.compare_exchange_strong(expected, self)) {
      return false;
    }
    REXLOG_INFO("portal-probe: human skater actor locked: {:08X}", self);
  }
  if (enabled == 0.0 || self != g_human) {
    return false;
  }
  uint32_t flags = 0, o1 = 0, o2 = 0, o3 = 0, o4 = 0, w[3];
  // The actor's own ground position is also where the game aims spawns and markers (feet level).
  if (!ReadBe32(base, self + 1296, &flags) || !ReadBe32(base, self + 1216, &w[0]) ||
      !ReadBe32(base, self + 1220, &w[1]) || !ReadBe32(base, self + 1224, &w[2])) {
    return false;
  }
  for (int k = 0; k < 3; ++k) {
    pos[k] = std::bit_cast<float>(w[k]);
  }
  // 0xC0000000 is riding on the ground. 0x40000000 (looks like a respawn state) and 0x00800000 (spawn in
  // progress) each left the game stuck re-relocating when we hopped in them.
  // shortcut: y < 0 is the career pit the game parks the skater in before the map loads.
  const bool riding = (flags & 0xF0000000) == 0xC0000000 && !(flags & 0x00800000) && !InPit(pos[0], pos[2]);
  const auto now = Clock::now();
  Crumb back{};
  float speed = 0, heading[2] = {0, 1};
  bool have_back = false;
  {
    std::lock_guard lock(trail_mutex);
    const float jump = trail.empty() ? 0.0f : std::hypot(pos[0] - trail.back().p[0], pos[2] - trail.back().p[2]);
    if (!riding || jump > 3.0f) {
      trail.clear();
    }
    if (riding) {
      trail.push_back({now, {pos[0], pos[1], pos[2]}});
    }
    while (!trail.empty() && now - trail.front().t > kBack + std::chrono::seconds(2)) {
      trail.pop_front();
    }
    // newest crumb at least kBack old = the hop target; newest crumb at least 0.4 s old gives the velocity
    for (auto it = trail.rbegin(); it != trail.rend(); ++it) {
      const auto age = now - it->t;
      if (age >= std::chrono::milliseconds(400) && speed == 0) {
        const float dt = std::chrono::duration<float>(age).count();
        const float vx = (pos[0] - it->p[0]) / dt, vz = (pos[2] - it->p[2]) / dt;
        speed = std::hypot(vx, vz);
        if (speed > 0.01f) {
          heading[0] = vx / speed;
          heading[1] = vz / speed;
        }
      }
      if (age >= kBack) {
        back = *it;
        have_back = true;
        break;
      }
    }
  }
  static Clock::time_point last_log{};
  if (now - last_log > std::chrono::seconds(2)) {
    last_log = now;
    REXLOG_INFO("portal-probe: hop waiting: flags {:08X} ({}), {}, speed {:.1f}, trail {}", flags,
                riding ? "riding" : "not riding",
                next == Clock::time_point{} ? "timer not started" : now < next ? "timer running" : "timer done",
                speed, have_back ? "ready" : "short");
  }
  if (!riding) {
    return false;
  }
  if (next == Clock::time_point{}) {
    next = now + std::chrono::seconds(10);
  }
  if (now < next || (flags & kRelocateBit) || !have_back || speed < 3.0f) {
    return false;
  }
  if (!ReadBe32(base, self + 1880, &o1) || !ReadBe32(base, o1 + 12, &o2) ||
      !ReadBe32(base, o2 + 24, &o3) || !ReadBe32(base, o3 + 8, &o4)) {
    REXLOG_INFO("portal-probe: test relocation not armed (object chain {:08X} {:08X} {:08X} {:08X})",
                o1, o2, o3, o4);
    return false;
  }
  next = now + kTestPeriod;
  // +0.3 m so a slightly uneven spot can't put the board inside the ground, which would kill the speed.
  const float target[16] = {heading[1], 0, -heading[0], 0, 0, 1, 0, 0, heading[0], 0, heading[1], 0,
                            back.p[0], back.p[1] + 0.3f, back.p[2], 1};
  if (enabled == 2.0) {
    std::memcpy(t_move_matrix, target, sizeof(target));
    t_move_pending = true;
    REXLOG_INFO("portal-probe: MOVE armed: self={:08X} flags {:08X}: ({:.2f}, {:.2f}, {:.2f}) -> ({:.2f}, {:.2f}, {:.2f}), speed {:.1f}, heading ({:.2f}, {:.2f})",
                self, flags, pos[0], pos[1], pos[2], target[12], target[13], target[14], speed, heading[0], heading[1]);
    std::lock_guard lock(trail_mutex);
    trail.clear();
    return false;
  }
  std::memcpy(t_target, target, sizeof(target));
  t_target_buf = sp - 96;  // sub_82DB88C8 takes 176 bytes of stack and keeps the matrix at +80
  t_target_set = true;
  const uint32_t be = __builtin_bswap32(flags | kRelocateBit);
  std::memcpy(base + self + 1296, &be, 4);
  REXLOG_INFO(
      "portal-probe: test relocation armed: self={:08X} flags {:08X} -> {:08X} buffer={:08X}: "
      "({:.2f}, {:.2f}, {:.2f}) -> ({:.2f}, {:.2f}, {:.2f}), speed {:.1f}, heading ({:.2f}, {:.2f})",
      self, flags, flags | kRelocateBit, t_target_buf, pos[0], pos[1], pos[2], target[12], target[13],
      target[14], speed, heading[0], heading[1]);
  {
    std::lock_guard lock(trail_mutex);
    trail.clear();  // the jump would otherwise look like a 25 m step and the old trail no longer leads here
  }
  return true;
}

// The board reset (82C05F50) zeroes every board body's velocity, which is why a grounded teleport stops the
// skater. The bodies are not behind board+24 (that table was empty); the reset's other stores land in the
// block at board+12 (e.g. +960/+976 are zeroed lin/ang velocity vectors, +7680/+7684 floats), so the bodies
// are probably inline there. It also zeroes the floats board+0x100/+0x104 (~8.2 at 8 m/s: cached speed).
// shortcut: instead of knowing the layout we diff that block around the reset and, during our armed test
// teleport only, write back every 16-byte vector that held a plausible velocity and became exactly zero.
// Replace with the real body offsets once the log shows them.
constexpr uint32_t kRatesBytes = 0x4000;
std::atomic<int> g_board_resets{0};
thread_local bool t_restore_rates = false;
thread_local bool t_surgery = false;  // this sub_82DB88C8 call is a gate crossing whose board reset is replaced
thread_local bool t_human_call = false;  // inside the human actor's sub_82DB88C8: its board resets are the human's
// After a restored hop: log the board's speed for the next frames to see whether the game keeps it.
std::atomic<uint32_t> g_follow_self{0}, g_follow_block{0};
std::atomic<int> g_follow_left{0};

// Every vector the teleport zeroed that held a plausible velocity, saved during our armed hop and written
// back (a) at once and (b) again at the start/end of the next few human sub_82DB88C8 calls, because the
// game's teleporting state overwrote the board's speed within ~3 frames. Rust notes say a teleport that keeps
// momentum has to set it in the board bodies, the skeleton bodies, the animated skeleton and one processed-input
// vector; we find those by diffing memory behind the arguments of each reset stage instead of knowing offsets.
// shortcut: replay count is a guess, and a stale vector replayed for 8 frames ignores gravity and friction.
struct Saved {
  uint32_t addr, len;
  uint32_t raw[3];
};
std::mutex g_bundle_mutex;
std::vector<Saved> g_bundle;
std::atomic<int> g_replay_left{0}, g_replay_total{0};
std::atomic<bool> g_replay_gate{false};  // a portal crossing: replay only through the teleport states, briefly
constexpr int kReplayFrames = 8;

void ReplayBundle(uint8_t* base) {
  std::lock_guard lock(g_bundle_mutex);
  for (const Saved& s : g_bundle) {
    std::memcpy(base + s.addr, s.raw, s.len);
  }
}

struct Snap {
  uint32_t addr;
  std::vector<uint32_t> words;
};

std::vector<Snap> TakeSnaps(uint8_t* base, std::initializer_list<uint32_t> addrs) {
  std::vector<Snap> out;
  for (uint32_t a : addrs) {
    Snap snap{a, std::vector<uint32_t>(kRatesBytes / 4)};
    if (a >= 0x10000 && a < 0xC0000000 && !(a & 3) &&
        skate3::native_scene::GuestTryCopy(snap.words.data(), base + a, kRatesBytes)) {
      out.push_back(std::move(snap));
    }
  }
  return out;
}

// Log (and, when restoring, write back and remember) the velocity-like vectors that became zero.
void DiffSnaps(uint8_t* base, const char* fn, const std::vector<Snap>& snaps, bool restore) {
  static std::atomic<int> logged{0};
  for (const Snap& snap : snaps) {
    std::vector<uint32_t> after(kRatesBytes / 4);
    if (!skate3::native_scene::GuestTryCopy(after.data(), base + snap.addr, kRatesBytes)) {
      continue;
    }
    int found = 0;
    for (uint32_t i = 0; i + 4 <= kRatesBytes / 4; i += 4) {
      const float b[3] = {BeFloat(snap.words[i]), BeFloat(snap.words[i + 1]), BeFloat(snap.words[i + 2])};
      const float mag = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
      if (!(mag > 0.3f && mag < 80.0f) || after[i] || after[i + 1] || after[i + 2]) {
        continue;
      }
      ++found;
      if (logged.fetch_add(1) < 160) {
        REXLOG_INFO("portal-probe: {} zeroed {:08X}+0x{:X} ({:.2f}, {:.2f}, {:.2f}) |v|={:.2f}{}", fn, snap.addr,
                    i * 4, b[0], b[1], b[2], mag, restore ? " RESTORED" : "");
      }
      if (restore) {
        Saved saved{snap.addr + i * 4, 12, {snap.words[i], snap.words[i + 1], snap.words[i + 2]}};
        TurnRaw(saved.raw);
        std::memcpy(base + saved.addr, saved.raw, 12);
        std::lock_guard lock(g_bundle_mutex);
        g_bundle.push_back(saved);
      }
    }
    if (found) {
      REXLOG_INFO("portal-probe: {} region {:08X}: {} velocity-like vectors zeroed", fn, snap.addr, found);
    }
  }
}

}  // namespace

REXCVAR_DECLARE(bool, skate3_portal_nobail);
REXCVAR_DECLARE(bool, skate3_portal_keep_tricks);
REXCVAR_DECLARE(bool, skate3_portal_keep_skeleton_state);

// No-bail: the wipeout bit (0x40000) lives in the processed-input struct at *(actor+1804)+2468, not in the actor. The
// selector (82DB6EC0) reads it each frame, so clear it right after ProcessData fills it and again at selector entry.
void NoBail(uint8_t* base, uint32_t input, const char* when) {
  uint32_t w = 0;
  if (input < 0x10000 || (input & 3) || !ReadBe32(base, input + 2468, &w) || !(w & 0x00040000u)) return;
  static int logged = 0;
  if (logged++ < 20) REXLOG_INFO("portal-probe: NOBAIL cleared wipeout bit ({}) word {:08X}", when, w);
  const uint32_t be = __builtin_bswap32(w & ~0x00040000u);
  std::memcpy(base + input + 2468, &be, 4);
}

uint32_t HumanInput(uint8_t* base) {
  uint32_t in = 0;
  const uint32_t h = g_human;
  return h && ReadBe32(base, h + 1804, &in) ? in : 0;
}

extern "C" REX_FUNC(sub_82BD8918) {
  const uint32_t input = ctx.r5.u32;
  __imp__sub_82BD8918(ctx, base);
  if (REXCVAR_GET(skate3_portal_nobail) && input && input == HumanInput(base)) NoBail(base, input, "process data");
  uint32_t w = 0;
  if (REXCVAR_GET(skate3_portal_keep_tricks) && input && input == HumanInput(base) && ReadBe32(base, input + 2484, &w) && (w & 0x04000000u)) {
    static int logged = 0;
    if (logged++ < 30) REXLOG_INFO("portal-probe: KEEPTRICKS hid the just-teleported flag (+2484 {:08X})", w);
    const uint32_t be = __builtin_bswap32(w & ~0x04000000u);
    std::memcpy(base + input + 2484, &be, 4);
  }
}

// requests_wipeout (skate-core wipeout/requests.rs): "a bail reason was raised this frame". Answer no for the human.
extern "C" REX_FUNC(sub_82DB9100) {
  if (ctx.r3.u32 == g_human && REXCVAR_GET(skate3_portal_nobail)) {
    static int logged = 0;
    __imp__sub_82DB9100(ctx, base);
    if ((ctx.r3.u32 & 0xFF) && logged++ < 20) REXLOG_INFO("portal-probe: NOBAIL denied a wipeout request");
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82DB9100(ctx, base);
}

extern "C" REX_FUNC(sub_82DB6EC0) {
  if (ctx.r3.u32 == g_human && REXCVAR_GET(skate3_portal_nobail)) NoBail(base, HumanInput(base), "selector");
  __imp__sub_82DB6EC0(ctx, base);
}

extern "C" REX_FUNC(sub_82DB88C8) {
  const uint32_t self = ctx.r3.u32;
  const bool hop_armed = ArmRelocation(base, ctx.r1.u32, self);
  const bool gate_armed = !hop_armed && skate3::portal_gate::Enabled() && self == g_human &&
                          GateStep(ctx, base, self);
  // the speed-restore machinery runs for hop tests, and for gate crossings when asked
  const bool armed = hop_armed || (gate_armed && (g_prof_restore || REXCVAR_GET(skate3_portal_gate_restore)));
  t_restore_rates = armed;
  // Off the board (state 500) the board is detached from the skater: keeping its rates exploded next frame (run 122:
  // (-592, -203) m/s, then repeated recovery warps), so those crossings take the plain reset.
  uint32_t player_state = 0;
  ReadBe32(base, self + 1336, &player_state);
  t_surgery = gate_armed && g_prof_surgery && player_state != 500;
  if (gate_armed && g_prof_surgery && player_state == 500) {
    REXLOG_INFO("portal-probe: GATE crossing while off the board (state 500): plain board reset, rates not kept");
  }
  t_human_call = self == g_human;
  if (armed) {
    std::lock_guard lock(g_bundle_mutex);
    g_bundle.clear();
  } else if (t_human_call && g_replay_left > 0) {
    ReplayBundle(base);
  }
  if (self == g_human && g_recover_window > 0) --g_recover_window;
  if (self == g_human && g_onboard_frames > 0) ForceOnBoard(base, self, "before the frame");
  __imp__sub_82DB88C8(ctx, base);
  t_surgery = false;
  if (self == g_human && gate_armed) {
    g_onboard_frames = 12;
    g_recover_window = 300;
    LogActor(base, self, "after the teleport");
  }
  if (self == g_human && g_onboard_frames > 0) {
    ForceOnBoard(base, self, "after the frame");
    --g_onboard_frames;
  }
  t_restore_rates = false;
  t_human_call = false;
  if (armed) {
    g_replay_left = kReplayFrames;
    g_replay_total = 0;
    g_replay_gate = !hop_armed;
    ReplayBundle(base);
  } else if (self == g_human && g_replay_left > 0) {
    ReplayBundle(base);
    // Keep replaying while the game is still in its teleport states; stop a few frames after it is back in
    // normal riding (state 100). shortcut: hard cap of 120 frames in case the state never settles.
    uint32_t state_id = 0;
    const bool gate = g_replay_gate;
    const bool read = ReadBe32(base, self + 1336, &state_id);
    const bool teleporting = gate ? (state_id == 700 || state_id == 702) : state_id != 100;
    if (read && teleporting && g_replay_total < (gate ? 12 : 120)) {
      g_replay_left = 6;
    }
    ++g_replay_total;
    --g_replay_left;
  }
  if (t_move_pending) {
    t_move_pending = false;
    uint32_t board = 0, block = 0;
    if (ReadBe32(base, self + 1880, &board) && ReadBe32(base, board + 12, &block)) {
      // guest scratch space below our frame, 16-byte aligned like the callers' own stack buffers
      const uint32_t saved_r1 = ctx.r1.u32, saved_r3 = ctx.r3.u32, saved_r4 = ctx.r4.u32;
      const uint32_t buf = ((saved_r1 - 1024) & ~0xFu) + 80;
      for (int i = 0; i < 16; ++i) {
        const uint32_t be = __builtin_bswap32(std::bit_cast<uint32_t>(t_move_matrix[i]));
        std::memcpy(base + buf + 4 * i, &be, 4);
      }
      ctx.r1.u32 = (buf - 80) & ~0xFu;
      ctx.r3.u32 = block;
      ctx.r4.u32 = buf;
      sub_82C0B2C8(ctx, base);
      ctx.r1.u32 = saved_r1;
      ctx.r3.u32 = saved_r3;
      ctx.r4.u32 = saved_r4;
      // previous/current ground position, and the deltas derived from them (PlayerInputState 1200/1216/1232/1248)
      for (uint32_t off : {1200u, 1216u}) {
        WriteBeFloat(base, self + off, t_move_matrix[12]);
        WriteBeFloat(base, self + off + 4, t_move_matrix[13] - 0.3f);
        WriteBeFloat(base, self + off + 8, t_move_matrix[14]);
      }
      for (uint32_t off : {1232u, 1248u}) {
        for (uint32_t k = 0; k < 12; k += 4) WriteBeFloat(base, self + off + k, 0.0f);
      }
      g_follow_self = self;
      g_follow_block = block;
      g_follow_left = 60;
      REXLOG_INFO("portal-probe: MOVE done: board {:08X} block {:08X}", board, block);
    } else {
      REXLOG_INFO("portal-probe: MOVE failed: board chain unreadable");
    }
  }
  if (gate_armed && g_turn_in_place) {
    g_turn_in_place = false;
    TurnLive(base, self);
  }
  if (self == g_human && !g_scan.empty()) {
    if (g_scan_frames == 0) ScanLog(base, "post-teleport");
    if (g_scan_frames == 3) ScanLog(base, "3-frames-later");
    // Which frame flips the turned rates back (run 113: turned at the crossing, reverted within 3 frames)?
    if (g_scan_frames <= 8) {
      uint32_t bd = 0, bk = 0, st = 0, fl = 0;
      float r0[3] = {0, 0, 0}, r1[3] = {0, 0, 0}, av[3] = {0, 0, 0};
      if (ReadBe32(base, self + 1880, &bd) && ReadBe32(base, bd + 12, &bk)) {
        ReadVec(base, bk + kLinRates, r0);
        ReadVec(base, bk + kLinRates + 16, r1);
      }
      ReadBe32(base, self + 1336, &st);
      ReadBe32(base, self + 1296, &fl);
      ReadVec(base, self + 1248, av);
      REXLOG_INFO("portal-probe: FRAME +{}: state {} flags {:08X} rate0 ({:.2f}, {:.2f}, {:.2f}) rate1 ({:.2f}, {:.2f}, {:.2f}) actorV ({:.2f}, {:.2f}, {:.2f})",
                  g_scan_frames, st, fl, r0[0], r0[1], r0[2], r1[0], r1[1], r1[2], av[0], av[1], av[2]);
    }
    if (++g_scan_frames > 8) g_scan.clear();
  }
  int follow_frame = -1;
  if (!armed && self == g_follow_self && g_follow_left > 0) {
    const int left = g_follow_left.fetch_sub(1);
    if (left > 88 || left % 3 == 0) {
      follow_frame = left;
    }
  }
  if (armed) {
    g_follow_self = self;
    g_follow_left = 100;
  } else if (follow_frame >= 0) {
    uint32_t w[6] = {};
    const uint32_t blk = g_follow_block;
    float pos[3] = {};
    const bool ok = ReadBe32(base, blk + 0x140, &w[0]) && ReadBe32(base, blk + 0x144, &w[1]) &&
                    ReadBe32(base, blk + 0x148, &w[2]) && ReadBe32(base, blk + 0x1B0, &w[3]) &&
                    ReadBe32(base, blk + 0x1B4, &w[4]) && ReadBe32(base, blk + 0x1B8, &w[5]);
    uint32_t g[3];  // the human actor's own ground position (the entity helper can return a park skater)
    if (ReadBe32(base, self + 1216, &g[0]) && ReadBe32(base, self + 1220, &g[1]) && ReadBe32(base, self + 1224, &g[2])) {
      for (int k = 0; k < 3; ++k) pos[k] = std::bit_cast<float>(g[k]);
    }
    uint32_t state_id = 0, state_flags = 0;
    ReadBe32(base, self + 1336, &state_id);
    ReadBe32(base, self + 1296, &state_flags);
    if (ok) {
      float lin[3], ang[3];
      for (int k = 0; k < 3; ++k) {
        lin[k] = std::bit_cast<float>(w[k]);
        ang[k] = std::bit_cast<float>(w[3 + k]);
      }
      REXLOG_INFO("portal-probe: after hop, {} frames left: board lin ({:.2f}, {:.2f}, {:.2f}) |v|={:.2f} "
                  "ang |w|={:.2f}; skater ({:.2f}, {:.2f}, {:.2f}) state {} flags {:08X}",
                  follow_frame, lin[0], lin[1], lin[2],
                  std::sqrt(lin[0] * lin[0] + lin[1] * lin[1] + lin[2] * lin[2]),
                  std::sqrt(ang[0] * ang[0] + ang[1] * ang[1] + ang[2] * ang[2]), pos[0], pos[1], pos[2],
                  state_id, state_flags);
    }
  }
  if (armed || gate_armed) {
    uint32_t flags = 0;
    const bool still = ReadBe32(base, self + 1296, &flags) && (flags & kRelocateBit);
    if (still) {
      const uint32_t be = __builtin_bswap32(flags & ~kRelocateBit);
      std::memcpy(base + self + 1296, &be, 4);
    }
    REXLOG_INFO("portal-probe: test relocation done (matrix {} by the game, request bit {})",
                t_target_set ? "NOT fetched" : "fetched", still ? "was still set, cleared by us" : "cleared by the game");
    t_target_set = false;
    t_target_buf = 0;
  }
}

// The matrix fetch inside the relocation branch: during our armed call, hand it our target instead.
extern "C" REX_FUNC(sub_82585CB0) {
  if (t_target_set && ctx.r3.u32 == t_target_buf) {
    for (int i = 0; i < 16; ++i) {
      const uint32_t be = __builtin_bswap32(std::bit_cast<uint32_t>(t_target[i]));
      std::memcpy(base + t_target_buf + 4 * i, &be, 4);
    }
    t_target_set = false;
    return;
  }
  __imp__sub_82585CB0(ctx, base);
}

// True when the board's rates are absurd (an exploded solver); a recovery warp must not keep those.
bool RatesInsane(uint8_t* base, uint32_t board) {
  uint32_t block = 0;
  if (!ReadBe32(base, board + 12, &block) || block < 0x10000 || block >= 0xC0000000 || (block & 3)) return false;
  for (int i = 0; i < 7; ++i) {
    float v[3];
    if (ReadVec(base, block + kLinRates + 16 * i, v) &&
        !(std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) < 1.5f * kMaxExitSpeed)) {
      return true;  // also catches NaN
    }
  }
  return false;
}

extern "C" REX_FUNC(sub_82C05F50) {
  const uint32_t board = ctx.r3.u32;
  if (t_surgery || (t_human_call && g_recover_window > 0 && !RatesInsane(base, board))) {
    // The executor's board reset zeroes every rate (a recovery warp after a crossing did this: run 102). Move the same assembly with the rate-preserving SetTransform
    // (block = *(board + 12), matrix = the reset's own r4) and skip the reset.
    uint32_t block = 0;
    if (ReadBe32(base, board + 12, &block) && block >= 0x10000 && block < 0xC0000000 && !(block & 3)) {
      ctx.r3.u32 = block;
      sub_82C0B2C8(ctx, base);
      if (t_surgery) TurnAndCapBlock(base, block);
      REXLOG_INFO("portal-probe: GATE board reset replaced by SetTransform (block {:08X}, rates kept{})", block,
                  t_surgery ? "" : ", recovery warp");
      return;
    }
    REXLOG_INFO("portal-probe: GATE surgery: board block unreadable, running the normal reset");
  }
  const bool restore = t_restore_rates;
  const int call = REXCVAR_GET(skate3_portal_probe) ? g_board_resets.fetch_add(1) : 1 << 20;
  uint32_t block = 0, speed_raw[2] = {};
  std::vector<uint32_t> before(kRatesBytes / 4);
  const bool have = (call < 60 || restore) && ReadBe32(base, board + 12, &block) && block >= 0x10000 &&
                    block < 0xC0000000 && !(block & 3) &&
                    skate3::native_scene::GuestTryCopy(before.data(), base + block, kRatesBytes) &&
                    ReadBe32(base, board + 0x100, &speed_raw[0]) && ReadBe32(base, board + 0x104, &speed_raw[1]);
  __imp__sub_82C05F50(ctx, base);
  std::vector<uint32_t> after(kRatesBytes / 4);
  if (!have || !skate3::native_scene::GuestTryCopy(after.data(), base + block, kRatesBytes)) {
    return;
  }
  const float speed = std::bit_cast<float>(speed_raw[1]);
  if (!restore && !(speed > 1.0f)) {
    return;  // only log resets of something that was moving
  }
  static std::atomic<int> dumps{0};
  uint32_t human = g_human, hw[3] = {};
  if (!restore && human && t_human_call && dumps.fetch_add(1) < 4 && ReadBe32(base, human + 1216, &hw[0]) &&
      ReadBe32(base, human + 1220, &hw[1]) && ReadBe32(base, human + 1224, &hw[2])) {
    REXLOG_INFO("portal-probe: BOARD DUMP board={:08X} block={:08X} cached speed {:.2f}; human actor {:08X} ground pos ({:.2f}, {:.2f}, {:.2f})",
                board, block, speed, human, std::bit_cast<float>(hw[0]), std::bit_cast<float>(hw[1]),
                std::bit_cast<float>(hw[2]));
    for (uint32_t i = 0; i < 0x240 / 4; i += 4) {
      REXLOG_INFO("portal-probe: BOARD DUMP +0x{:03X} {:.4f} {:.4f} {:.4f} {:.4f}", i * 4, BeFloat(before[i]),
                  BeFloat(before[i + 1]), BeFloat(before[i + 2]), BeFloat(before[i + 3]));
    }
  }
  int found = 0;
  char text[1200];
  int n = std::snprintf(text, sizeof(text), "block=%08X", block);
  for (uint32_t i = 0; i + 4 <= kRatesBytes / 4; i += 4) {
    const float b[3] = {BeFloat(before[i]), BeFloat(before[i + 1]), BeFloat(before[i + 2])};
    const float mag = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (!(mag > 0.3f && mag < 80.0f) || after[i] || after[i + 1] || after[i + 2]) {
      continue;
    }
    // lin velocity of the 7 board bodies at +0x140, angular at +0x1B0 (16-byte stride), seen in the log;
    // other vectors the reset zeroes (e.g. +0x1F40) are listed but left alone.
    if (restore && i * 4 >= 0x140 && i * 4 < 0x220) {
      Saved saved{block + 4 * i, 12, {before[i], before[i + 1], before[i + 2]}};
      TurnRaw(saved.raw);
      std::memcpy(base + saved.addr, saved.raw, 12);
      std::lock_guard lock(g_bundle_mutex);
      g_bundle.push_back(saved);
    }
    if (found++ < 24 && n < int(sizeof(text)) - 80) {
      n += std::snprintf(text + n, sizeof(text) - n, "\n  +0x%X (%.2f, %.2f, %.2f) |v|=%.2f", i * 4, b[0],
                         b[1], b[2], mag);
    }
  }
  if (restore) {
    std::memcpy(base + board + 0x100, speed_raw, 8);
    g_follow_block = block;
    std::lock_guard lock(g_bundle_mutex);
    g_bundle.push_back({board + 0x100, 8, {speed_raw[0], speed_raw[1], 0}});
  }
  REXLOG_INFO("portal-probe: board-reset #{} board={:08X} cached speed {:.2f}, {} velocity-like vectors zeroed{}: {}",
              call, board, speed, found, restore ? " (RESTORED)" : "", text);
}

#define TELEPORT_HOOK(addr)                                                                   \
  extern "C" REX_FUNC(sub_##addr) {                                                           \
    const uint32_t a[8] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,                   \
                           ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};                  \
    const bool log = REXCVAR_GET(skate3_portal_probe) && g_teleports.fetch_add(1) < 60;       \
    if (log) TeleportBefore(base, 0x##addr, a);                                               \
    __imp__sub_##addr(ctx, base);                                                             \
    if (log) TeleportAfter(base, 0x##addr);                                                   \
  }

TELEPORT_HOOK(82DB8998)
// The same hole for line probes: 82ADF5D8 is the triangle-segment dispatcher behind the board's wheel, deck and wall probes
// (and camera sweeps). Run 120: after a crossing the board's turned rates were replaced one frame later, as if a probe still
// saw the surface at the exit. r3 = result, r4 = segment start, r5 = direction, r6..r8 = vertices A, B, C (vec4 each),
// returns r3 = hit. A miss only clears the hit byte, so returning 0 leaves the old record alone.
extern "C" REX_FUNC(sub_82ADF5D8) {
  if (skate3::portal_gate::HoleArmed()) {
    float v[4][3];
    bool ok = true;
    const uint32_t ptr[4] = {ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
    for (int i = 0; i < 4 && ok; ++i) {
      uint32_t w[3];
      ok = ptr[i] >= 0x10000 && skate3::native_scene::GuestTryCopy(w, base + ptr[i], sizeof(w));
      if (ok) for (int k = 0; k < 3; ++k) v[i][k] = BeFloat(w[k]);
    }
    if (ok) {
      const float ab[3] = {v[2][0] - v[1][0], v[2][1] - v[1][1], v[2][2] - v[1][2]};
      const float ac[3] = {v[3][0] - v[1][0], v[3][1] - v[1][1], v[3][2] - v[1][2]};
      float n[3] = {ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]};
      const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      if (len > 1e-6f) {
        for (float& x : n) x /= len;
        if (skate3::portal_gate::HoleSkips(v[0], n, v[1])) {
          static std::atomic<int> skipped{0};
          const int count = skipped.fetch_add(1) + 1;
          if (count <= 3 || count % 5000 == 0) {
            REXLOG_INFO("portal-probe: HOLE skipped a line probe (#{}): start ({:.2f},{:.2f},{:.2f}) n ({:.2f},{:.2f},{:.2f})",
                        count, v[0][0], v[0][1], v[0][2], n[0], n[1], n[2]);
          }
          ctx.r3.u64 = 0;
          return;
        }
      }
    }
  }
  __imp__sub_82ADF5D8(ctx, base);
}

// Loose bodies through portals. 82AE6590 is RigidBody::DynamicUpdate (skate-core rigid_body notes): r3 = the body, which holds
// orientation quaternion (x,y,z,w) at +0, position +16, linear velocity +32, angular velocity +48, basis columns +64/+80/+96.
// After the update, a body that has reached an open portal moving into it is moved to the other portal: position mapped,
// velocities and orientation rotated by the pair's matrix. The surface stays solid (no hole), so nothing can fall through
// without crossing. Bodies within 2 m of the skater are left to the skater's own crossing.
void QuatFromColumns(const float c[3][3], float q[4]) {
  // m[row][col] = c[col][row]
  const float m00 = c[0][0], m01 = c[1][0], m02 = c[2][0];
  const float m10 = c[0][1], m11 = c[1][1], m12 = c[2][1];
  const float m20 = c[0][2], m21 = c[1][2], m22 = c[2][2];
  const float tr = m00 + m11 + m22;
  if (tr > 0.0f) {
    const float s = std::sqrt(tr + 1.0f) * 2.0f;
    q[3] = 0.25f * s;
    q[0] = (m21 - m12) / s;
    q[1] = (m02 - m20) / s;
    q[2] = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
    q[3] = (m21 - m12) / s;
    q[0] = 0.25f * s;
    q[1] = (m01 + m10) / s;
    q[2] = (m02 + m20) / s;
  } else if (m11 > m22) {
    const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
    q[3] = (m02 - m20) / s;
    q[0] = (m01 + m10) / s;
    q[1] = 0.25f * s;
    q[2] = (m12 + m21) / s;
  } else {
    const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
    q[3] = (m10 - m01) / s;
    q[0] = (m02 + m20) / s;
    q[1] = (m12 + m21) / s;
    q[2] = 0.25f * s;
  }
}

// ---- Loose rigid bodies: crossing and carrying -----------------------------------------------------------------------------------
// The integrator hook (any physics worker thread; the game integrates bodies in parallel) only RECORDS each body's position and
// velocity. Everything that WRITES bodies happens in BodiesFrame, once per frame on the game thread (called from GateStep), where
// the workers are idle: run 131 moved bodies from inside the hook, racing the other threads' integration of the same bodies.
struct BodySeen {
  float p[3];
  float v[3];
  int64_t ns;
  int near_frames;  // updates spent within 1.5 m of the skater (the skater's own board and skeleton accumulate hundreds)
};
std::mutex g_body_mutex;
std::unordered_map<uint32_t, BodySeen> g_bodies_seen;
std::unordered_map<uint32_t, int64_t> g_body_cooldown;  // a moved body is not moved again for a moment
// Bodies that have ever been the skater's own (600+ updates beside them): the board and skeleton parts lose their near_frames when the entry
// expires after a few idle seconds, and a board lying still then looked like a prop (run 149: the self-test picked the skateboard).
std::unordered_set<uint32_t> g_own_ids;
constexpr int kOwnFrames = 150;                          // near_frames at which a body counts as the skater's own

// A moved group followed for ~2 s (one line per 6 frames): where it is, how it is spread, how fast, and whether it still hangs
// together. Answers "did it fall, where did it go, did it break up / respawn".
struct GroupTrack {
  uint32_t lead = 0;
  std::vector<uint32_t> ids;
  int frames = 0;
  int budget = 0;
};
GroupTrack g_group_track;

void LogGroupTrack(uint8_t* base) {
  GroupTrack& t = g_group_track;
  float sum[3] = {0, 0, 0}, lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f}, speed = 0.0f;
  int n = 0;
  for (const uint32_t m : t.ids) {
    float p[3], v[3];
    if (!ReadVec(base, m + 16, p) || !ReadVec(base, m + 32, v)) continue;
    for (int k = 0; k < 3; ++k) {
      sum[k] += p[k];
      lo[k] = std::min(lo[k], p[k]);
      hi[k] = std::max(hi[k], p[k]);
    }
    speed += std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    ++n;
  }
  if (n == 0) return;
  REXLOG_INFO("portal-props: group of {} (lead {:08X}) t+{}: centre ({:.2f},{:.2f},{:.2f}) y {:.2f}..{:.2f} extent ({:.1f},{:.1f},{:.1f}) mean speed {:.1f}",
              n, t.lead, t.frames, sum[0] / n, sum[1] / n, sum[2] / n, lo[1], hi[1], hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2],
              speed / n);
}

// Hook side (any thread): record only.
void RecordBody(uint8_t* base, uint32_t body) {
  if (body < 0x10000 || body >= 0xC0000000 || (body & 3)) return;
  float p[3], v[3];
  if (!ReadVec(base, body + 16, p) || !ReadVec(base, body + 32, v)) return;
  float sk[3];
  const bool have_sk = skate3::portal_gate::TrackedPos(sk);
  const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::lock_guard<std::mutex> lock(g_body_mutex);
  if (g_bodies_seen.size() > 4096) g_bodies_seen.clear();
  BodySeen& s = g_bodies_seen[body];
  const bool near = have_sk && std::hypot(p[0] - sk[0], std::hypot(p[1] - sk[1], p[2] - sk[2])) < 1.5f;
  s.near_frames = near ? std::min(s.near_frames + 1, 100000) : std::max(0, s.near_frames - 3);
  if (s.near_frames >= 600) g_own_ids.insert(body);
  std::copy(p, p + 3, s.p);
  std::copy(v, v + 3, s.v);
  s.ns = now;
}

// Everything connected to `seed` (bodies seen in the last 0.3 s within 0.8 m of another member): one object, a pile, a ragdoll.
// Caller holds g_body_mutex.
std::vector<uint32_t> BodyCluster(uint32_t seed, int64_t now) {
  constexpr float kLink = 0.8f;
  std::vector<uint32_t> out{seed};
  for (size_t i = 0; i < out.size() && out.size() < 64; ++i) {
    const auto it = g_bodies_seen.find(out[i]);
    if (it == g_bodies_seen.end()) continue;
    for (const auto& [other, seen] : g_bodies_seen) {
      if (now - seen.ns > 300'000'000ll || std::find(out.begin(), out.end(), other) != out.end()) continue;
      const float d[3] = {seen.p[0] - it->second.p[0], seen.p[1] - it->second.p[1], seen.p[2] - it->second.p[2]};
      if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < kLink * kLink) {
        out.push_back(other);
        if (out.size() >= 64) break;
      }
    }
  }
  return out;
}

// Rigidly move one body: position about the reference point, velocities and orientation rotated by R. Caller holds g_body_mutex.
void MoveBody(uint8_t* base, uint32_t body, const float ref[3], const float np[3], const float R[9]) {
  constexpr float kMaxSpeed = 60.0f;
  float p[3], v[3], w[3] = {0, 0, 0}, rel[3], mp[3], nv[3], nw[3];
  if (!ReadVec(base, body + 16, p) || !ReadVec(base, body + 32, v)) return;
  const bool have_w = ReadVec(base, body + 48, w);
  for (int k = 0; k < 3; ++k) rel[k] = p[k] - ref[k];
  Turn(R, rel, rel);
  for (int k = 0; k < 3; ++k) mp[k] = np[k] + rel[k];
  Turn(R, v, nv);
  Turn(R, w, nw);
  const float sp = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
  if (sp > kMaxSpeed) {
    for (float& c : nv) c *= kMaxSpeed / sp;
  }
  // Orientation: rotate the basis columns, rebuild the quaternion; only trusted if the quaternion convention checks out.
  float cols[3][3], ncols[3][3], q_have[4] = {0, 0, 0, 1}, q_new[4];
  bool orient = true;
  for (int c = 0; c < 3 && orient; ++c) orient = ReadVec(base, body + 64 + 16 * c, cols[c]);
  uint32_t qw[4];
  for (int k = 0; k < 4 && orient; ++k) {
    orient = ReadBe32(base, body + 4 * k, &qw[k]);
    if (orient) q_have[k] = std::bit_cast<float>(qw[k]);
  }
  if (orient) {
    float q_cur[4];
    QuatFromColumns(cols, q_cur);
    const float dot = q_cur[0] * q_have[0] + q_cur[1] * q_have[1] + q_cur[2] * q_have[2] + q_cur[3] * q_have[3];
    orient = std::abs(dot) > 0.98f;  // the stored basis and quaternion agree (up to sign)
    static bool logged = false;
    if (!orient && !logged) {
      logged = true;
      REXLOG_INFO("portal-props: body {:08X} basis and quaternion disagree (dot {:.3f}); orientation left alone", body, dot);
    }
  }
  if (orient) {
    for (int c = 0; c < 3; ++c) Turn(R, cols[c], ncols[c]);
    QuatFromColumns(ncols, q_new);
    for (int c = 0; c < 3; ++c) WriteBeFloats(base, body + 64 + 16 * c, ncols[c], 3);
    WriteBeFloats(base, body, q_new, 4);
  }
  WriteBeFloats(base, body + 16, mp, 3);
  WriteBeFloats(base, body + 32, nv, 3);
  if (have_w) WriteBeFloats(base, body + 48, nw, 3);
  BodySeen& seen = g_bodies_seen[body];
  std::copy(mp, mp + 3, seen.p);
  std::copy(nv, nv + 3, seen.v);
}

// After a crossing something in the game pulls a moved prop straight back to where it came from at ~60 m/s (the game's speed cap;
// run 137: the same 5 bodies, four crossings, each followed by a straight glide to the old spot). For a short while the group's own
// ballistic path is re-asserted whenever a body has been pulled back toward its old position.
// Scripted self-test (Ctrl+6): three scenarios on the nearest loose prop, each scored from what the game does with it.
struct SelfTest {
  int scenario = -1;  // -1 = idle
  int64_t t0 = 0;
  uint32_t id = 0;
  float prev[3] = {}, prev_start[3] = {};
  int crossings = 0, jumps = 0, rescued = 0, passed = 0;
  float max_speed = 0, min_y = 1e9f, low_y = 0;
  bool flat = true, crossed = false, moved = false;
  int valid = 0, traced = 0, dragged = 0;
  std::vector<uint32_t> queue;  // every awake prop nearby, tested one after another
  size_t qi = 0;
  int cluster_size = 1;
  int64_t pending_ns = 0;  // the queue is built once the woken props have been integrated
  float home[3] = {};
};
SelfTest g_test;
constexpr float kTestSeconds = 4.5f;

struct FlightBody {
  uint32_t id;
  float home[3];  // where it was before it crossed
  float p0[3], v0[3];
};
std::vector<FlightBody> g_flight;
int64_t g_flight_ns = 0;
constexpr float kFlightSeconds = 0.7f;
constexpr float kFlightGravity = 14.0f;

// A body that has reached an open portal: move it and everything connected to it as one rigid group (joints drag a lone-moved
// part back or fight it). Caller holds g_body_mutex.
void CrossGroup(uint8_t* base, uint32_t body, int64_t now) {
  float p[3], v[3];
  if (!ReadVec(base, body + 16, p) || !ReadVec(base, body + 32, v)) return;
  float np[3], nv[3], R[9], plane[4];
  if (!skate3::portal_gate::BodyCross(body, p, v, np, nv, R, plane)) return;
  const std::vector<uint32_t> cluster = BodyCluster(body, now);
  for (const uint32_t m : cluster) {
    const auto it = g_bodies_seen.find(m);
    if (g_own_ids.count(m) || (it != g_bodies_seen.end() && it->second.near_frames >= kOwnFrames) || skate3::portal_gate::BodyIsSkaters(m, it != g_bodies_seen.end() ? it->second.p : p)) {
      static int logged_n = 0;
      if (logged_n++ < 10) {
        REXLOG_INFO("portal-props: body {:08X} reached a portal but its cluster of {} includes the skater's bodies, left alone",
                    body, cluster.size());
      }
      return;
    }
  }
  // The group moves rigidly about this body, so its far members can land behind the exit plane (inside the wall or floor, where the
  // solver shoves them out hard): lift the whole group out along the normal until the lowest member clears it.
  std::vector<uint32_t> members;
  float lowest = 1e9f;
  for (const uint32_t m : cluster) {
    float mp[3], rel[3];
    if (!ReadVec(base, m + 16, mp) || std::hypot(mp[0] - p[0], std::hypot(mp[1] - p[1], mp[2] - p[2])) > 6.0f) continue;
    for (int k = 0; k < 3; ++k) rel[k] = mp[k] - p[k];
    Turn(R, rel, rel);
    lowest = std::min(lowest, plane[0] * (np[0] + rel[0]) + plane[1] * (np[1] + rel[1]) + plane[2] * (np[2] + rel[2]) - plane[3]);
    members.push_back(m);
  }
  constexpr float kClear = 0.35f;
  if (lowest < kClear) {
    const float lift = std::min(kClear - lowest, 3.0f);
    for (int k = 0; k < 3; ++k) np[k] += plane[k] * lift;
  }
  if (g_test.scenario >= 0 && std::find(members.begin(), members.end(), g_test.id) != members.end()) g_test.crossed = true;
  g_group_track = GroupTrack{body, members, 0, 12};
  LogGroupTrack(base);
  std::vector<FlightBody> flight;
  for (const uint32_t m : members) {
    FlightBody f{};
    f.id = m;
    ReadVec(base, m + 16, f.home);
    MoveBody(base, m, p, np, R);
    g_body_cooldown[m] = now;
    if (ReadVec(base, m + 16, f.p0) && ReadVec(base, m + 32, f.v0)) flight.push_back(f);
  }
  g_flight = flight;
  g_flight_ns = now;
  if (g_body_cooldown.size() > 4096) g_body_cooldown.clear();
  static int logged_n = 0;
  if (logged_n++ < 40) {
    REXLOG_INFO("portal-props: body {:08X} + {} connected crossed ({:.2f},{:.2f},{:.2f}) v ({:.1f},{:.1f},{:.1f}) -> ({:.2f},{:.2f},{:.2f}) v ({:.1f},{:.1f},{:.1f})",
                body, members.size() - 1, p[0], p[1], p[2], v[0], v[1], v[2], np[0], np[1], np[2], nv[0], nv[1], nv[2]);
  }
}

// ---- Carrying a game object (grab / drop with the same bind as the companion cube) ----
struct HeldBody {
  uint32_t id;
  float off[3];  // from the group's centre
};
std::vector<HeldBody> g_held_bodies;



bool BodiesHolding() {
  std::lock_guard<std::mutex> lock(g_body_mutex);
  return !g_held_bodies.empty();
}

// Put the test prop (and its connected group) at the scenario's start spot and let it go.
bool TestSetup(uint8_t* base, int64_t now) {
  float pos[3], vel[3], low_y;
  bool flat;
  float p[3];
  if (!skate3::portal_gate::SelfTestSpot(g_test.scenario, pos, vel, &low_y, &flat) || !ReadVec(base, g_test.id + 16, p)) return false;
  for (const uint32_t m : BodyCluster(g_test.id, now)) {
    float mp[3];
    if (!ReadVec(base, m + 16, mp) || std::hypot(mp[0] - p[0], std::hypot(mp[1] - p[1], mp[2] - p[2])) > 6.0f) continue;
    const float np[3] = {pos[0] + mp[0] - p[0], pos[1] + mp[1] - p[1], pos[2] + mp[2] - p[2]};
    const float zero[3] = {0, 0, 0};
    WriteBeFloats(base, m + 16, np, 3);
    WriteBeFloats(base, m + 32, vel, 3);
    WriteBeFloats(base, m + 48, zero, 3);
    g_body_cooldown.erase(m);
    std::copy(np, np + 3, g_bodies_seen[m].p);
  }
  g_flight.clear();
  if (g_test.scenario == 0) std::copy(p, p + 3, g_test.home);
  std::copy(pos, pos + 3, g_test.prev);
  std::copy(pos, pos + 3, g_test.prev_start);
  g_test.moved = false;
  g_test.traced = 0;
  g_test.t0 = now;
  g_test.crossings = g_test.jumps = g_test.rescued = 0;
  g_test.crossed = false;
  g_test.max_speed = 0;
  g_test.min_y = 1e9f;
  g_test.low_y = low_y;
  g_test.flat = flat;
  static const char* names[3] = {"centre drop", "off-centre drop", "toss from further back"};
  REXLOG_INFO("portal-test: scenario {} ({}) started at ({:.2f},{:.2f},{:.2f}) v ({:.1f},{:.1f},{:.1f}) on body {:08X}", g_test.scenario + 1,
              names[g_test.scenario], pos[0], pos[1], pos[2], vel[0], vel[1], vel[2], g_test.id);
  return true;
}

// ---- Waking sleeping props ----
// A sleeping body sits on the simulation's sleeping list and is never integrated (writes to it do nothing). 82ADF7B8 (the game's own wake, used
// for the skater's parts) moves a body to the active list and sets its state to 4: this does the same on the body directly, from the same thread.
// Body layout used: +44 prev, +60 next (a list node), +76 the simulation, +140 state (low 3 bits == 2: asleep), +172 cool_down.
// Simulation: +40 active list head, +68 / +72 sleeping / active counts, +168 cool_down.
bool Rd32(uint8_t* base, uint32_t a, uint32_t* out) {
  uint32_t w;
  if (a < 0x10000 || a >= 0xC0000000 || (a & 3) || !skate3::native_scene::GuestTryCopy(&w, base + a, 4)) return false;
  *out = __builtin_bswap32(w);
  return true;
}
void Wr32(uint8_t* base, uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t*>(base + a) = __builtin_bswap32(v); }

bool WakeBody(uint8_t* base, uint32_t body) {
  uint32_t st, sim, prev, next, head, head_next, sim_cool, c68, c72;
  if (!Rd32(base, body + 140, &st) || (st & 7) != 2 || !Rd32(base, body + 76, &sim) || !Rd32(base, body + 44, &prev) ||
      !Rd32(base, body + 60, &next) || !Rd32(base, sim + 40, &head) || !Rd32(base, head + 60, &head_next) || !Rd32(base, sim + 168, &sim_cool) ||
      !Rd32(base, sim + 68, &c68) || !Rd32(base, sim + 72, &c72)) {
    return false;
  }
  uint32_t chk;
  if (!Rd32(base, prev + 60, &chk) || chk != body || !Rd32(base, next + 44, &chk) || chk != body || !Rd32(base, head_next + 44, &chk)) return false;
  Wr32(base, prev + 60, next);       // unlink from the sleeping list
  Wr32(base, next + 44, prev);
  Wr32(base, body + 44, head);       // push on the front of the active list
  Wr32(base, body + 60, head_next);
  Wr32(base, head_next + 44, body);
  Wr32(base, head + 60, body);
  Wr32(base, body + 140, 4u | (st & 8u));
  Wr32(base, sim + 68, c68 - 1);
  Wr32(base, sim + 72, c72 + 1);
  Wr32(base, body + 172, 0);         // (the game stores sim cool_down - 1 first, then clears it)
  (void)sim_cool;
  return true;
}

// The nodes of the circular list whose sentinel is `head` (empty if it is not a consistent list). Capped.
std::vector<uint32_t> ListNodes(uint8_t* base, uint32_t head) {
  std::vector<uint32_t> out;
  uint32_t prev = head, n;
  if (!Rd32(base, head + 60, &n)) return {};
  for (int i = 0; i < 20000; ++i) {
    if (n == head) return out;
    uint32_t back, nn;
    if (!Rd32(base, n + 44, &back) || back != prev || !Rd32(base, n + 60, &nn)) return {};
    out.push_back(n);
    prev = n;
    n = nn;
  }
  return {};
}

// Wake every sleeping body within `radius` of the skater (not the skater's own). Finds the lists by scanning the simulation object for
// list sentinels; logs what it found, since the layout is read from the game's code and not yet seen live. Caller holds g_body_mutex.
int WakeNearby(uint8_t* base, const float skater[3], float radius) {
  uint32_t sim = 0;
  for (const auto& [id, s] : g_bodies_seen) {
    if (Rd32(base, id + 76, &sim) && sim >= 0x10000) break;
    sim = 0;
  }
  if (!sim) {
    REXLOG_INFO("portal-wake: no body seen yet, cannot find the simulation");
    return 0;
  }
  int woken = 0, sleeping_seen = 0;
  std::vector<uint32_t> heads;
  for (uint32_t off = 0; off < 0x200; off += 4) {
    uint32_t head;
    if (!Rd32(base, sim + off, &head) || std::find(heads.begin(), heads.end(), head) != heads.end()) continue;
    const std::vector<uint32_t> nodes = ListNodes(base, head);
    if (nodes.empty()) continue;
    heads.push_back(head);
    int asleep = 0, here = 0;
    for (const uint32_t n : nodes) {
      uint32_t st;
      float p[3];
      if (!Rd32(base, n + 140, &st) || (st & 7) != 2 || !ReadVec(base, n + 16, p)) continue;
      ++asleep;
      if (g_own_ids.count(n) || std::hypot(p[0] - skater[0], std::hypot(p[1] - skater[1], p[2] - skater[2])) > radius) continue;
      ++here;
      if (WakeBody(base, n)) ++woken;
    }
    sleeping_seen += asleep;
    REXLOG_INFO("portal-wake: list at simulation+{} (sentinel {:08X}): {} bodies, {} asleep, {} of them within {:.0f} m", off, head, nodes.size(), asleep, here,
                radius);
  }
  REXLOG_INFO("portal-wake: woke {} bodies ({} asleep seen in {} lists)", woken, sleeping_seen, heads.size());
  return woken;
}

// What this prop looks like (cluster size, inverse mass, the raw 176-byte body), logged before it is moved: the dragged ones should share something.
void TestProfile(uint8_t* base, int64_t now) {
  uint32_t w[44];
  std::string hex;
  if (skate3::native_scene::GuestTryCopy(w, base + g_test.id, sizeof(w))) {
    char b[16];
    for (const uint32_t x : w) {
      std::snprintf(b, sizeof(b), "%08X ", __builtin_bswap32(x));
      hex += b;
    }
  }
  float im[3] = {0, 0, 0};  // ReadVec reads three floats
  ReadVec(base, g_test.id + 124, im);
  const float inv_mass = im[0];
  g_test.cluster_size = int(BodyCluster(g_test.id, now).size());
  REXLOG_INFO("portal-test: prop {} of {}: body {:08X}, cluster of {}, inverse mass {:.3f}; raw: {}", g_test.qi + 1, g_test.queue.size(), g_test.id,
              g_test.cluster_size, inv_mass, hex);
}

// Start the next prop in the queue (skips any that cannot be set up). False when the queue is done.
bool TestNextProp(uint8_t* base, int64_t now) {
  while (g_test.qi < g_test.queue.size()) {
    g_test.id = g_test.queue[g_test.qi];
    g_test.scenario = 0;
    g_test.passed = g_test.valid = g_test.dragged = 0;
    float p[3];
    if (ReadVec(base, g_test.id + 16, p)) {
      TestProfile(base, now);
      if (TestSetup(base, now)) return true;
    }
    ++g_test.qi;
  }
  return false;
}

// A real prop wearing the cube skin: its transform goes to the renderer every frame (basis columns +64/+80/+96, position +16).
uint32_t g_adopted = 0;

void AdoptFrame(uint8_t* base, const float skater[3], int64_t now) {
  if (skate3::portal_gate::TakeAdopt()) {
    if (g_adopted) {
      REXLOG_INFO("portal-cube: prop {:08X} released", g_adopted);
      g_adopted = 0;
      skate3::portal_cube::ClearExternal();
    } else {
      WakeNearby(base, skater, 8.0f);
      uint32_t best = 0;
      float best_d = 8.0f;
      for (const auto& [id, s] : g_bodies_seen) {
        const float d = std::hypot(s.p[0] - skater[0], std::hypot(s.p[1] - skater[1], s.p[2] - skater[2]));
        if (d < best_d && now - s.ns < 1'000'000'000ll && s.near_frames < kOwnFrames && !g_own_ids.count(id) &&
            !skate3::portal_gate::BodyIsSkaters(id, s.p)) {
          best_d = d;
          best = id;
        }
      }
      if (best) {
        g_adopted = best;
        REXLOG_INFO("portal-cube: prop {:08X} ({:.1f} m away) is now the companion cube", best, best_d);
      } else {
        REXLOG_INFO("portal-cube: no awake prop within 8 m (nudge one first)");
      }
    }
  }
  if (!g_adopted) return;
  float p[3], cols[3][3];
  if (!ReadVec(base, g_adopted + 16, p) || !ReadVec(base, g_adopted + 64, cols[0]) || !ReadVec(base, g_adopted + 80, cols[1]) ||
      !ReadVec(base, g_adopted + 96, cols[2])) {
    g_adopted = 0;
    skate3::portal_cube::ClearExternal();
    return;
  }
  skate3::portal_cube::SetExternal(p, cols);
}

void TestFrame(uint8_t* base, const float skater[3], int64_t now) {
  if (g_test.scenario < 0) {
    if (g_test.pending_ns == 0) {
      if (!skate3::portal_gate::TakeSelfTest()) return;
      if (!skate3::portal_gate::BodiesArmed()) {
        REXLOG_INFO("portal-test: place both portals first (and enable skate3_portal_gate_props)");
        return;
      }
      WakeNearby(base, skater, 15.0f);
      g_test.pending_ns = now + 400'000'000ll;
      return;
    }
    if (now < g_test.pending_ns) return;
    g_test.pending_ns = 0;
    std::vector<std::pair<float, uint32_t>> found;
    for (const auto& [id, s] : g_bodies_seen) {
      const float d = std::hypot(s.p[0] - skater[0], std::hypot(s.p[1] - skater[1], s.p[2] - skater[2]));
      const bool held = std::any_of(g_held_bodies.begin(), g_held_bodies.end(), [&](const HeldBody& h) { return h.id == id; });
      if (d < 15.0f && now - s.ns < 150'000'000ll && s.near_frames < kOwnFrames && !held && !g_own_ids.count(id) &&
          !skate3::portal_gate::BodyIsSkaters(id, s.p)) {
        found.push_back({d, id});
      }
    }
    std::sort(found.begin(), found.end());
    g_test.queue.clear();
    std::vector<uint32_t> covered;  // members of the groups already queued: one entry per object
    for (const auto& [d, id] : found) {
      if (g_test.queue.size() >= 5) break;
      if (std::find(covered.begin(), covered.end(), id) != covered.end()) continue;
      const std::vector<uint32_t> group = BodyCluster(id, now);
      if (std::any_of(group.begin(), group.end(), [&](uint32_t m) { return g_own_ids.count(m) > 0; })) continue;
      g_test.queue.push_back(id);
      covered.insert(covered.end(), group.begin(), group.end());
    }
    if (g_test.queue.empty()) {
      REXLOG_INFO("portal-test: no awake loose prop within 15 m of the skater (nudge one first)");
      return;
    }
    g_test.qi = 0;
    if (!TestNextProp(base, now)) g_test.scenario = -1;
    return;
  }
  float p[3], v[3] = {0, 0, 0};
  if (!ReadVec(base, g_test.id + 16, p)) {
    REXLOG_INFO("portal-test: the test prop disappeared, stopping");
    g_test.scenario = -1;
    return;
  }
  ReadVec(base, g_test.id + 32, v);
  const float jump = std::hypot(p[0] - g_test.prev[0], std::hypot(p[1] - g_test.prev[1], p[2] - g_test.prev[2]));
  if (g_test.crossed) {
    g_test.crossed = false;
    ++g_test.crossings;
  } else if (jump > 4.0f) {
    ++g_test.jumps;  // moved a long way with no crossing of ours: the game put it back (respawn)
  }
  std::copy(p, p + 3, g_test.prev);
  g_test.max_speed = std::max(g_test.max_speed, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
  g_test.min_y = std::min(g_test.min_y, p[1]);
  const float t = float(now - g_test.t0) * 1e-9f;
  const float from_start = std::hypot(p[0] - g_test.prev_start[0], std::hypot(p[1] - g_test.prev_start[1], p[2] - g_test.prev_start[2]));
  if (from_start > 0.05f) g_test.moved = true;
  // Where the prop goes, twice a second: shows a pull back toward its own spot (the game drags some moved bodies home at ~60 m/s).
  if (t >= 0.5f * (g_test.traced + 1) && g_test.traced < 11) {
    ++g_test.traced;
    REXLOG_INFO("portal-test:   t+{:.1f}s ({:.1f},{:.1f},{:.1f}) speed {:.1f}, {:.1f} m from where it lay before the test", t, p[0], p[1], p[2],
                std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), std::hypot(p[0] - g_test.home[0], std::hypot(p[1] - g_test.home[1], p[2] - g_test.home[2])));
  }
  const bool asleep = !g_test.moved && t >= 0.7f;
  if (t < kTestSeconds && !asleep) return;
  const bool under = g_test.min_y < g_test.low_y - (g_test.flat ? 2.0f : 3.7f);
  const bool pass = g_test.crossings >= 1 && g_test.jumps == 0 && g_test.rescued == 0 && !under;
  if (asleep) {
    REXLOG_INFO("portal-test: scenario {} INVALID: the prop never moved (asleep: the integrator does not update it), not scored", g_test.scenario + 1);
  } else {
    ++g_test.valid;
    REXLOG_INFO("portal-test: scenario {} {}: crossings {}, respawn jumps {}, rescued {}, under the map {}, max speed {:.1f} m/s, ends at ({:.2f},{:.2f},{:.2f})",
                g_test.scenario + 1, pass ? "PASS" : "FAIL", g_test.crossings, g_test.jumps, g_test.rescued, under ? "YES" : "no", g_test.max_speed,
                p[0], p[1], p[2]);
  }
  g_test.passed += (pass && !asleep) ? 1 : 0;
  if (!asleep && g_test.max_speed >= 45.0f && g_test.crossings == 0) ++g_test.dragged;  // flung at the game's speed cap with no crossing of ours
  if (++g_test.scenario >= 3 || !TestSetup(base, now)) {
    REXLOG_INFO("portal-test: RESULT prop {} of {} (body {:08X}, cluster of {}): {} of {} scored scenarios passed, dragged off by the game in {}", g_test.qi + 1,
                g_test.queue.size(), g_test.id, g_test.cluster_size, g_test.passed, g_test.valid, g_test.dragged);
    ++g_test.qi;
    if (!TestNextProp(base, now)) {
      REXLOG_INFO("portal-test: all {} props done", g_test.queue.size());
      g_test.scenario = -1;
    }
  }
}

void BodiesFrame(uint8_t* base, const float skater[3], const float skater_v[2], const float hand_pt[3], float dt) {
  const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::lock_guard<std::mutex> lock(g_body_mutex);
  for (auto it = g_bodies_seen.begin(); it != g_bodies_seen.end();) {
    it = (now - it->second.ns > 6'000'000'000ll) ? g_bodies_seen.erase(it) : std::next(it);
  }
  if (g_group_track.budget > 0 && ++g_group_track.frames % 6 == 0) {
    LogGroupTrack(base);
    --g_group_track.budget;
  }
  // Carrying: each held body glides toward its place around the hand point (a spring), its velocity set to match.
  if (!g_held_bodies.empty()) {
    const float* hand = hand_pt;
    const float k = 1.0f - std::exp(-12.0f * std::clamp(dt, 0.001f, 0.05f));
    const float inv_dt = 1.0f / std::clamp(dt, 0.001f, 0.05f);
    for (auto it = g_held_bodies.begin(); it != g_held_bodies.end();) {
      float p[3];
      if (!ReadVec(base, it->id + 16, p)) {
        it = g_held_bodies.erase(it);
        continue;
      }
      float goal[3], np[3], nv[3];
      for (int a = 0; a < 3; ++a) goal[a] = hand[a] + it->off[a];
      const float dist = std::hypot(goal[0] - p[0], std::hypot(goal[1] - p[1], goal[2] - p[2]));
      const bool snap = dist > 5.0f;  // the skater crossed a portal: the hand moved far in one frame
      for (int a = 0; a < 3; ++a) np[a] = snap ? goal[a] : p[a] + (goal[a] - p[a]) * k;
      for (int a = 0; a < 3; ++a) nv[a] = snap ? 0.0f : (np[a] - p[a]) * inv_dt;
      const float sp = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
      if (sp > 30.0f) for (float& c : nv) c *= 30.0f / sp;
      const float zero[3] = {0, 0, 0};
      WriteBeFloats(base, it->id + 16, np, 3);
      WriteBeFloats(base, it->id + 32, nv, 3);
      WriteBeFloats(base, it->id + 48, zero, 3);
      BodySeen& s = g_bodies_seen[it->id];
      std::copy(np, np + 3, s.p);
      s.ns = now;
      ++it;
    }
  }
  if (skate3::portal_gate::TakeWake()) WakeNearby(base, skater, 15.0f);
  AdoptFrame(base, skater, now);
  TestFrame(base, skater, now);
  if (!g_flight.empty()) {
    const float t = float(now - g_flight_ns) * 1e-9f;
    if (t > kFlightSeconds) {
      g_flight.clear();
    } else {
      int fixed = 0;
      for (const FlightBody& f : g_flight) {
        float p[3];
        if (!ReadVec(base, f.id + 16, p)) continue;
        const float ex[3] = {f.p0[0] + f.v0[0] * t, f.p0[1] + f.v0[1] * t - 0.5f * kFlightGravity * t * t, f.p0[2] + f.v0[2] * t};
        const float to_home = std::hypot(p[0] - f.home[0], std::hypot(p[1] - f.home[1], p[2] - f.home[2]));
        const float ex_home = std::hypot(ex[0] - f.home[0], std::hypot(ex[1] - f.home[1], ex[2] - f.home[2]));
        const float dev = std::hypot(p[0] - ex[0], std::hypot(p[1] - ex[1], p[2] - ex[2]));
        // The drag is fast (~60 m/s toward home, run 137); a bounce off the floor also moves a body "back" but slowly: run 144 had a
        // bin's landing overwritten with its path through the floor.
        float v[3] = {0, 0, 0};
        ReadVec(base, f.id + 32, v);
        const float toward = ((f.home[0] - p[0]) * v[0] + (f.home[1] - p[1]) * v[1] + (f.home[2] - p[2]) * v[2]) / std::max(to_home, 0.01f);
        if (dev > 0.8f && to_home < ex_home - 0.3f && toward > 30.0f) {  // dragged back toward where it started
          const float ev[3] = {f.v0[0], f.v0[1] - kFlightGravity * t, f.v0[2]};
          WriteBeFloats(base, f.id + 16, ex, 3);
          WriteBeFloats(base, f.id + 32, ev, 3);
          ++fixed;
        }
      }
      static int logged = 0;
      if (fixed > 0 && logged++ < 30) {
        REXLOG_INFO("portal-props: t+{:.2f}s: {} bodies were being pulled back, put back on their path", t, fixed);
      }
    }
  }
  {
    float props[24][6];
    int n = 0;
    for (const auto& [id, s] : g_bodies_seen) {
      if (n < 24 && now - s.ns < 200'000'000ll && s.near_frames < kOwnFrames && !g_own_ids.count(id) &&
          skate3::portal_gate::NearPortal(s.p, 12.0f)) {  // run 153: props 40 m from the skater got no hole when it was "within 30 m of the skater"
        std::copy(s.p, s.p + 3, props[n]);
        std::copy(s.v, s.v + 3, props[n] + 3);
        ++n;
      }
    }
    skate3::portal_gate::SetPropPositions(props, n);
  }
  // Safety net: a prop that crossed and then ended up far below the portals fell out of the world (the game would respawn it at its
  // default spot). Set it down beside the lower portal, with its connected group, at rest.
  if (skate3::portal_gate::BodiesArmed()) {
    std::vector<uint32_t> lost;
    for (const auto& [id, s] : g_bodies_seen) {
      float to[3];
      // never the skater's own bodies (run 145: 820 "rescues" of the skeleton while standing 3 m under a wall portal)
      if (now - s.ns < 200'000'000ll && s.near_frames < kOwnFrames && !g_own_ids.count(id) && g_body_cooldown.count(id) &&
          skate3::portal_gate::PropRescue(s.p, to) && !skate3::portal_gate::BodyIsSkaters(id, s.p)) {
        lost.push_back(id);
      }
    }
    for (const uint32_t id : lost) {
      float p[3], to[3];
      if (!ReadVec(base, id + 16, p) || !skate3::portal_gate::PropRescue(p, to)) continue;
      // The game pulls a body that was moved far back to its own spot (run 149: one body rescued 190 times, 0.85 s apart): three tries each.
      static std::unordered_map<uint32_t, std::pair<int64_t, int>> tried;
      auto& tr = tried[id];
      if (tr.second >= 3 || now - tr.first < 3'000'000'000ll) continue;
      tr = {now, tr.second + 1};
      const std::vector<uint32_t> cluster = BodyCluster(id, now);
      if (std::any_of(cluster.begin(), cluster.end(), [&](uint32_t m) { const auto it = g_bodies_seen.find(m); return g_own_ids.count(m) || (it != g_bodies_seen.end() && it->second.near_frames >= kOwnFrames); })) continue;
      const float zero[3] = {0, 0, 0};
      for (const uint32_t m : cluster) {
        float mp[3];
        if (!ReadVec(base, m + 16, mp) || std::hypot(mp[0] - p[0], std::hypot(mp[1] - p[1], mp[2] - p[2])) > 6.0f) continue;
        const float np[3] = {to[0] + mp[0] - p[0], to[1] + mp[1] - p[1], to[2] + mp[2] - p[2]};
        WriteBeFloats(base, m + 16, np, 3);
        WriteBeFloats(base, m + 32, zero, 3);
        WriteBeFloats(base, m + 48, zero, 3);
        g_body_cooldown[m] = now;
        std::copy(np, np + 3, g_bodies_seen[m].p);
      }
      g_flight.clear();
      if (g_test.scenario >= 0 && std::find(cluster.begin(), cluster.end(), g_test.id) != cluster.end()) ++g_test.rescued;
      REXLOG_INFO("portal-props: body {:08X} + {} fell out of the world (y {:.1f}), set down at ({:.2f},{:.2f},{:.2f})", id,
                  cluster.size() - 1, p[1], to[0], to[1], to[2]);
    }
  }
  // Crossing: bodies that reached an open portal (judged on what the hook recorded, then re-read from memory).
  if (skate3::portal_gate::BodiesArmed()) {
    std::vector<uint32_t> candidates;
    for (const auto& [id, s] : g_bodies_seen) {
      if (now - s.ns > 200'000'000ll || s.near_frames >= kOwnFrames || g_own_ids.count(id)) continue;
      const auto cd = g_body_cooldown.find(id);
      // A fall between two portals (floor to ceiling, 4.8 m) takes ~0.55 s: a longer cooldown held the second crossing back until the body
      // was 0.8 m inside the floor (run 142).
      if (cd != g_body_cooldown.end() && now - cd->second < 250'000'000ll) continue;
      if (std::any_of(g_held_bodies.begin(), g_held_bodies.end(), [&](const HeldBody& h) { return h.id == id; })) continue;
      float np[3], nv[3], R[9], plane[4];
      if (skate3::portal_gate::BodyCross(id, s.p, s.v, np, nv, R, plane)) candidates.push_back(id);
    }
    for (const uint32_t id : candidates) {
      const auto cd = g_body_cooldown.find(id);
      if (cd != g_body_cooldown.end() && now - cd->second < 250'000'000ll) continue;  // moved with an earlier group this frame
      CrossGroup(base, id, now);
    }
  }
  (void)skater_v;
}

// Grab / drop for game objects. Returns true if it handled the press: it dropped what was held, or it picked up the nearest object
// that is closer than `cube_dist` (the companion cube's distance, so the closest of the two wins).
bool BodiesGrabToggle(uint8_t* base, const float skater[3], const float skater_v[2], const float aim_o[3], const float aim_d[3],
                      bool aim_ok, const float hand[3], float cube_score) {
  const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::lock_guard<std::mutex> lock(g_body_mutex);
  if (!g_held_bodies.empty()) {
    const float up = 1.0f;
    for (const HeldBody& h : g_held_bodies) {
      float p[3];
      if (!ReadVec(base, h.id + 16, p)) continue;
      const float v[3] = {skater_v[0], up, skater_v[1]};
      WriteBeFloats(base, h.id + 32, v, 3);
    }
    REXLOG_INFO("portal-props: dropped {} bodies, thrown at ({:.1f}, {:.1f})", g_held_bodies.size(), skater_v[0], skater_v[1]);
    g_held_bodies.clear();
    return true;
  }
  // The object under the crosshair: closest to the aim ray (to the hand point without one), within 5 m of the skater. Bodies the
  // integrator updated in the last 3 s count: a nudged prop goes back to sleep within a second.
  uint32_t seed = 0;
  float best = 1e9f;
  struct Near {
    float d, age;
    uint32_t id;
    int own;
  } nearest[3] = {{1e9f, 0, 0, 0}, {1e9f, 0, 0, 0}, {1e9f, 0, 0, 0}};
  for (const auto& [id, s] : g_bodies_seen) {
    const float age = float(now - s.ns) * 1e-9f;
    const float away = std::hypot(s.p[0] - skater[0], std::hypot(s.p[1] - skater[1], s.p[2] - skater[2]));
    for (int i = 0; i < 3; ++i) {
      if (away < nearest[i].d) {
        for (int j = 2; j > i; --j) nearest[j] = nearest[j - 1];
        nearest[i] = {away, age, id, s.near_frames};
        break;
      }
    }
    if (age > 3.0f || s.near_frames >= kOwnFrames || g_own_ids.count(id) || away > 5.0f) continue;
    float perp = std::hypot(s.p[0] - hand[0], std::hypot(s.p[1] - hand[1], s.p[2] - hand[2]));
    if (aim_ok) {
      const float r[3] = {s.p[0] - aim_o[0], s.p[1] - aim_o[1], s.p[2] - aim_o[2]};
      const float along = r[0] * aim_d[0] + r[1] * aim_d[1] + r[2] * aim_d[2];
      perp = std::hypot(r[0] - aim_d[0] * along, std::hypot(r[1] - aim_d[1] * along, r[2] - aim_d[2] * along));
    }
    if (perp > 1.8f) continue;
    const float score = perp + 0.15f * away;
    if (score < best) {
      best = score;
      seed = id;
    }
  }
  if (!seed || best >= cube_score) {
    if (!seed) {
      REXLOG_INFO("portal-props: nothing under the crosshair to grab ({} bodies recorded); nearest to the skater: {:08X} {:.1f} m age {:.1f}s own {}, {:08X} {:.1f} m age {:.1f}s own {}, {:08X} {:.1f} m age {:.1f}s own {}",
                  g_bodies_seen.size(), nearest[0].id, nearest[0].d, nearest[0].age, nearest[0].own, nearest[1].id, nearest[1].d,
                  nearest[1].age, nearest[1].own, nearest[2].id, nearest[2].d, nearest[2].age, nearest[2].own);
    }
    return false;
  }
  const std::vector<uint32_t> cluster = BodyCluster(seed, now);
  float centre[3] = {0, 0, 0};
  int n = 0;
  for (const uint32_t m : cluster) {
    const auto it = g_bodies_seen.find(m);
    if (it == g_bodies_seen.end()) continue;
    if (it->second.near_frames >= kOwnFrames) {
      REXLOG_INFO("portal-props: that object is connected to the skater's own bodies, not grabbed");
      return true;
    }
    for (int a = 0; a < 3; ++a) centre[a] += it->second.p[a];
    ++n;
  }
  if (n == 0) return false;
  for (float& c : centre) c /= float(n);
  for (const uint32_t m : cluster) {
    const auto it = g_bodies_seen.find(m);
    if (it == g_bodies_seen.end()) continue;
    HeldBody h{m, {it->second.p[0] - centre[0], it->second.p[1] - centre[1], it->second.p[2] - centre[2]}};
    g_held_bodies.push_back(h);
  }
  REXLOG_INFO("portal-props: picked up {} bodies (lead {:08X}) at ({:.2f},{:.2f},{:.2f}), score {:.2f}", n, seed,
              centre[0], centre[1], centre[2], best);
  return true;
}

extern "C" REX_FUNC(sub_82AE6590) {
  const uint32_t body = ctx.r3.u32;
  __imp__sub_82AE6590(ctx, base);
  if (skate3::portal_gate::Enabled()) RecordBody(base, body);
}

// The portal hole: 8277B720 is the world-triangle vs moving-primitive contact query. r3 = the moving primitive (world
// position at +0), r4 = the triangle record (vertex A at +0, normal at +16; layout from the run-099 dump), r6 = the
// output manifest (count at +16). The caller stores r3 as the contact count and takes its no-contact branch on 0.
extern "C" REX_FUNC(sub_8277B720) {
  if (skate3::portal_gate::HoleArmed()) {
    uint32_t tri[8], mover[3];
    if (ctx.r4.u32 >= 0x10000 && ctx.r3.u32 >= 0x10000 &&
        skate3::native_scene::GuestTryCopy(tri, base + ctx.r4.u32, sizeof(tri)) &&
        skate3::native_scene::GuestTryCopy(mover, base + ctx.r3.u32, sizeof(mover))) {
      // tri words 0..2 = vertex A, 4..6 = normal (record offsets +0 and +16)
      const float a[3] = {BeFloat(tri[0]), BeFloat(tri[1]), BeFloat(tri[2])};
      const float n[3] = {BeFloat(tri[4]), BeFloat(tri[5]), BeFloat(tri[6])};
      const float m[3] = {BeFloat(mover[0]), BeFloat(mover[1]), BeFloat(mover[2])};
      if (skate3::portal_gate::HoleSkips(m, n, a)) {
        static std::atomic<int> skipped{0};
        const int count = skipped.fetch_add(1) + 1;
        if (count <= 5 || count % 2000 == 0) {
          REXLOG_INFO("portal-probe: HOLE skipped a world contact (#{}): mover ({:.2f},{:.2f},{:.2f}) tri A ({:.2f},{:.2f},{:.2f}) n ({:.2f},{:.2f},{:.2f})",
                      count, m[0], m[1], m[2], a[0], a[1], a[2], n[0], n[1], n[2]);
        }
        if (ctx.r6.u32 >= 0x10000) std::memset(base + ctx.r6.u32 + 16, 0, 4);
        ctx.r3.u64 = 0;
        return;
      }
    }
  }
  __imp__sub_8277B720(ctx, base);
}

// The filter the game itself passes to the ray service inside Start (all of Start's queries share it).
extern "C" REX_FUNC(sub_82E0B4E0) {
  if (t_in_start) {
    g_start_filter = ctx.r7.u32;
  }
  __imp__sub_82E0B4E0(ctx, base);
}

// The composite ray service's results function (service vtable +32): it asks every child service for its hit and keeps
// the nearest (hit record: position +0, normal +32, time +48). Inside a portal opening the hit is dropped.
extern "C" REX_FUNC(sub_8276EE88) {
  __imp__sub_8276EE88(ctx, base);
  if (t_gun_query || !skate3::portal_gate::Enabled() || !REXCVAR_GET(skate3_portal_gate_cutout)) return;
  const uint32_t res = ctx.r3.u32;
  if (res < 0x10000 || res >= 0xC0000000) return;
  uint32_t raw[4];
  if (!skate3::native_scene::GuestTryCopy(raw, base + res, sizeof(raw))) return;
  const float pos[3] = {BeFloat(raw[0]), BeFloat(raw[1]), BeFloat(raw[2])};
  if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) return;
  if (skate3::portal_gate::InsideOpening(pos, 0.6f)) {
    static std::atomic<int> cut{0};
    const int n = cut.fetch_add(1);
    if (n < 12 || n % 500 == 0) {
      REXLOG_INFO("portal-probe: CUTOUT dropped a ray hit at ({:.2f}, {:.2f}, {:.2f}) inside a portal opening (#{})",
                  pos[0], pos[1], pos[2], n + 1);
    }
    ctx.r3.u32 = 0;
  }
}

// Grind capture (Ctrl+8): the game finds grindable surfaces with line-vs-triangle tests (Rust notes: 82C20728 / 82C205D8 batch the lines, 827719B8 tests
// one segment against one triangle). Logged for 8 s after the key: the segment, the triangle, the result, so a fake cube edge can answer the same way.
extern "C" REX_FUNC(sub_827719B8) {
  if (!skate3::portal_gate::GrindCaptureOn()) {
    __imp__sub_827719B8(ctx, base);
    return;
  }
  static std::atomic<int> logged{0};
  const uint32_t hitp = ctx.r3.u32, p4 = ctx.r4.u32, p5 = ctx.r5.u32, p6 = ctx.r6.u32, p7 = ctx.r7.u32, p8 = ctx.r8.u32;
  float start[4] = {}, delta[4] = {}, tri[12] = {};
  const bool ok = ReadFloats4(base, p4, start) && ReadFloats4(base, p5, delta);
  for (int i = 0; i < 3; ++i) ReadFloats4(base, p6 + 16 * i, tri + 4 * i);
  const double radius = ctx.f1.f64, fat = ctx.f2.f64;
  __imp__sub_827719B8(ctx, base);
  float sk[3];
  if (ok && skate3::portal_gate::TrackedPos(sk) && logged.load() < 400 &&
      std::hypot(start[0] - sk[0], std::hypot(start[1] - sk[1], start[2] - sk[2])) < 2.5f) {
    float out[16] = {};
    for (int i = 0; i < 4; ++i) ReadFloats4(base, hitp + 16 * i, out + 4 * i);
    const int n = logged.fetch_add(1);
    REXLOG_INFO("portal-grind: #{} args r3..r8 {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} | start ({:.3f},{:.3f},{:.3f}) delta ({:.3f},{:.3f},{:.3f}) | tri A ({:.2f},{:.2f},{:.2f}) B ({:.2f},{:.2f},{:.2f}) C ({:.2f},{:.2f},{:.2f}) | radius {:.3f} fat {:.3f} | result {:08X} out [{:.3f} {:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f} {:.3f}]",
                n, hitp, p4, p5, p6, p7, p8, start[0], start[1], start[2], delta[0], delta[1], delta[2], tri[0], tri[1], tri[2], tri[4], tri[5], tri[6],
                tri[8], tri[9], tri[10], radius, fat, ctx.r3.u32, out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7], out[8], out[9], out[10], out[11],
                out[12], out[13], out[14], out[15]);
  }
}

extern "C" REX_FUNC(sub_82DB63C0) {
  const uint32_t self = ctx.r3.u32;
  t_in_start = true;
  __imp__sub_82DB63C0(ctx, base);
  t_in_start = false;
  if (g_ray.state == 1) {
    SubmitRay(ctx, base, self);
  }
  if (g_cube_ray.state == 1) {
    SubmitCubeRay(ctx, base, self);
  }
}

extern "C" REX_FUNC(sub_82DB6580) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_82DB6580(ctx, base);
  if (g_ray.state == 2 && self == g_ray.this_ptr) {
    CollectRay(ctx, base);
  }
  if (g_cube_ray.state == 2 && self == g_cube_ray.this_ptr) {
    CollectCubeRay(ctx, base);
  }
}

// The skeleton's teleport stages: find (and during our hop restore) whatever velocity they zero.
#define SKELETON_HOOK(addr)                                                                         \
  extern "C" REX_FUNC(sub_##addr) {                                                                 \
    const bool active = t_human_call;                                                               \
    std::vector<Snap> snaps;                                                                        \
    if (active) snaps = TakeSnaps(base, {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32});                      \
    __imp__sub_##addr(ctx, base);                                                                   \
    if (active) DiffSnaps(base, #addr, snaps, t_restore_rates);                                     \
  }

// 82BE3508 also clears a block of state flags and timers just past the velocity vectors (skeleton +16389..16506: bytes at 16389/16396/16504-16506,
// words at 16392/16400/16412/16420/16500). The vector restore (kRatesBytes = 0x4000) stops before them, so tricks and foot state were lost
// at every crossing. Take that block before the reset and put it back after, logging which words the reset changed.
extern "C" REX_FUNC(sub_82BE3508) {
  const bool active = t_human_call;
  const uint32_t skel = ctx.r3.u32;
  constexpr uint32_t kBlock = 0x4000, kLen = 0x90;
  std::vector<Snap> snaps;
  uint32_t before[kLen / 4] = {};
  bool have_block = false;
  if (active) {
    snaps = TakeSnaps(base, {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32});
    have_block = skel >= 0x10000 && skel < 0xC0000000 && !(skel & 3) && skate3::native_scene::GuestTryCopy(before, base + skel + kBlock, kLen);
  }
  __imp__sub_82BE3508(ctx, base);
  if (!active) return;
  DiffSnaps(base, "82BE3508", snaps, t_restore_rates);
  uint32_t after[kLen / 4];
  if (have_block && skate3::native_scene::GuestTryCopy(after, base + skel + kBlock, kLen)) {
    static int logged = 0;
    for (uint32_t i = 0; i < kLen / 4 && logged < 40; ++i) {
      if (before[i] != after[i]) {
        ++logged;
        REXLOG_INFO("portal-probe: SKELRESET skeleton+{:#x}: {:08X} -> {:08X}", kBlock + i * 4, __builtin_bswap32(before[i]), __builtin_bswap32(after[i]));
      }
    }
    if (t_restore_rates && REXCVAR_GET(skate3_portal_keep_skeleton_state)) std::memcpy(base + skel + kBlock, before, kLen);
  }
}
SKELETON_HOOK(82BDF7C0)

// Callees of PlayerUI::UpdateSessionMarker (82898FC8): one of them hands the marker's target to the player.
#define RING_HOOK(addr)                                                                  \
  extern "C" REX_FUNC(sub_##addr) {                                                     \
    RecordCall(base, 0x##addr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);        \
    __imp__sub_##addr(ctx, base);                                                       \
  }

RING_HOOK(824533C0)
RING_HOOK(824AD240)
RING_HOOK(824FCAD8)
RING_HOOK(825582D0)
RING_HOOK(8255DF90)
RING_HOOK(82591DA8)
RING_HOOK(82592B68)
RING_HOOK(825DFAF0)
RING_HOOK(82897730)
RING_HOOK(828977A8)
RING_HOOK(828AAE90)
RING_HOOK(82B938D8)

#define PROBE_HOOK(addr, slot)                                         \
  extern "C" REX_FUNC(sub_##addr) {                                     \
    const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32;  \
    __imp__sub_##addr(ctx, base);                                       \
    Probe(base, slot, a3, a4, a5);                                      \
  }

PROBE_HOOK(82BFB1E0, 0)
PROBE_HOOK(82BFB3F8, 1)
PROBE_HOOK(82BFC038, 2)
PROBE_HOOK(82BFC378, 3)

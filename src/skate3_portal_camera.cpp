#include "skate3_portal_camera.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "skate3_portal_gate.h"
#include "skate3_portal_math.h"

REXCVAR_DEFINE_BOOL(skate3_portal_camcarry, false, "Skate 3",
                    "Experiment: when you go through a portal the chase camera comes through it with you (its pose behind the skater is "
                    "carried through the portal, then eased into the game's camera). First person turns its view through the portal. "
                    "Log prefix PCAM.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(skate3_portal_camcarry_time, 0.6, "Skate 3",
                      "Seconds the carried camera takes to ease into the game's own camera after a portal crossing.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::portal_camera {
namespace {

namespace pm = skate3::portal_math;

int64_t NowNs() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

float Dist(const float a[3], const float b[3]) {
  const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  return std::sqrt(pm::Dot(d, d));
}

// Recent game-camera poses relative to the skater (position minus the skater's), newest last.
struct Rel {
  int64_t ns = 0;
  pm::Pose pose;
};
constexpr int kRing = 32;
Rel g_ring[kRing];
int g_ring_n = 0, g_ring_head = 0;

struct Carry {
  bool active = false;
  unsigned seq = 0;
  pm::Pose rel;          // the pre-crossing pose relative to the skater
  float R[9] = {};
  float entry[3] = {}, exit[3] = {};
  int64_t cross_ns = 0, start_ns = 0, blend_ns = 0;  // blend_ns: when the game camera first looked usable (0 = not yet)
  float max_jump = 0, game_max_jump = 0;
  float last_pos[3] = {}, last_game[3] = {};
  bool have_last = false, told_wait = false;
  int frames = 0;
};
Carry g_carry;
unsigned g_seen_seq = 0, g_fpv_seq = 0;
bool g_seq_init = false;

void Push(int64_t now, const pm::Pose& rel) {
  g_ring[g_ring_head] = {now, rel};
  g_ring_head = (g_ring_head + 1) % kRing;
  g_ring_n = std::min(g_ring_n + 1, kRing);
}

// Newest entry recorded before `ns` (and no older than 0.5 s before it).
const Rel* Before(int64_t ns) {
  for (int i = 1; i <= g_ring_n; ++i) {
    const Rel& r = g_ring[(g_ring_head - i + kRing) % kRing];
    if (r.ns < ns) return ns - r.ns < 500'000'000 ? &r : nullptr;
  }
  return nullptr;
}

float TurnDegrees(const float R[9]) {
  const float tr = std::clamp((R[0] + R[4] + R[8] - 1.0f) * 0.5f, -1.0f, 1.0f);
  return std::acos(tr) * 57.29578f;
}

}  // namespace

bool Apply(const float cam_view[16], const float proj[16], bool camera_owned, float view_proj[16], float cam_pos[3]) {
  float skater[3];
  if (!skate3::portal_gate::Enabled() || !skate3::portal_gate::TrackedPos(skater)) return false;
  const int64_t now = NowNs();
  pm::Pose game;
  pm::PoseFromView(cam_view, &game);
  float R[9], entry[3], exit[3];
  long long cross_ns = 0;
  const unsigned seq = skate3::portal_gate::LastCrossing(R, &cross_ns, entry, exit);
  if (!g_seq_init) {
    g_seq_init = true;
    g_seen_seq = g_fpv_seq = seq;
  }
  const bool on = REXCVAR_GET(skate3_portal_camcarry) && !camera_owned && !skate3::portal_gate::FpvActive();
  if (seq != g_seen_seq) {
    g_seen_seq = seq;
    const Rel* pre = Before(cross_ns);
    if (on && pre) {
      g_carry = Carry{};
      g_carry.active = true;
      g_carry.seq = seq;
      g_carry.rel = pre->pose;
      std::copy(R, R + 9, g_carry.R);
      std::copy(entry, entry + 3, g_carry.entry);
      std::copy(exit, exit + 3, g_carry.exit);
      g_carry.cross_ns = cross_ns;
      g_carry.start_ns = now;
      REXLOG_INFO("portal-camera: PCAM carry started (crossing {}): camera was ({:.2f}, {:.2f}, {:.2f}) from the skater ({:.1f} m, pose "
                  "{:.0f} ms before the crossing), portal turns it {:.0f} deg",
                  seq, pre->pose.pos[0], pre->pose.pos[1], pre->pose.pos[2], std::sqrt(pm::Dot(pre->pose.pos, pre->pose.pos)),
                  (cross_ns - pre->ns) / 1e6, TurnDegrees(R));
    } else if (on) {
      REXLOG_INFO("portal-camera: PCAM crossing {} but no camera pose recorded just before it: no carry", seq);
    }
  }
  pm::Pose rel_now = game;
  for (int k = 0; k < 3; ++k) rel_now.pos[k] -= skater[k];
  if (!g_carry.active || !on) {
    if (g_carry.active) {
      g_carry.active = false;
      REXLOG_INFO("portal-camera: PCAM carry cancelled (camera taken by first person / freecam, or the cvar went off)");
    }
    Push(now, rel_now);
    return false;
  }
  Carry& c = g_carry;
  ++c.frames;
  if (c.have_last) c.game_max_jump = std::max(c.game_max_jump, Dist(game.pos, c.last_game));
  std::copy(game.pos, game.pos + 3, c.last_game);
  // The game thread may not have moved the skater yet (render frames run ahead): while the tracked position is still nearer the entry,
  // the game camera is still consistent with it, so show that.
  if (Dist(skater, c.entry) < Dist(skater, c.exit)) {
    if (now - c.start_ns > 500'000'000) {
      c.active = false;
      REXLOG_INFO("portal-camera: PCAM the skater never showed up at the exit within 0.5 s: carry dropped");
    }
    return false;
  }
  pm::Pose carried;
  pm::CarryPose(c.rel, c.R, skater, &carried);
  // The game camera is usable once it is about as far from the skater as before the crossing (it may still be flying in from the entry).
  const float rel_len = std::sqrt(pm::Dot(c.rel.pos, c.rel.pos));
  const float game_len = Dist(game.pos, skater);
  const bool game_ok = game_len < std::max(2.5f * rel_len, rel_len + 4.0f);
  if (!game_ok && c.blend_ns == 0) {
    if (!c.told_wait) {
      c.told_wait = true;
      REXLOG_INFO("portal-camera: PCAM game camera is {:.1f} m from the skater (carried camera {:.1f} m): holding the carried pose", game_len,
                  rel_len);
    }
    if (now - c.start_ns > 2'500'000'000ll) c.blend_ns = now;  // give up waiting: blend anyway
  } else if (c.blend_ns == 0) {
    c.blend_ns = now;
  }
  const double T = std::max(0.05, double(REXCVAR_GET(skate3_portal_camcarry_time)));
  const float t = c.blend_ns ? float(double(now - c.blend_ns) / 1e9 / T) : 0.0f;
  pm::Pose shown;
  pm::BlendPose(carried, game, pm::Smoothstep(t), &shown);
  if (c.have_last) c.max_jump = std::max(c.max_jump, Dist(shown.pos, c.last_pos));
  std::copy(shown.pos, shown.pos + 3, c.last_pos);
  c.have_last = true;
  float view[16];
  pm::ViewFromPose(shown, view);
  pm::Mul4(view, proj, view_proj);
  std::copy(shown.pos, shown.pos + 3, cam_pos);
  if (t >= 1.0f) {
    c.active = false;
    REXLOG_INFO("portal-camera: PCAM carry done (crossing {}) after {:.0f} ms, {} frames, waited {:.0f} ms for the game camera; largest "
                "frame-to-frame move: shown camera {:.2f} m, game camera {:.2f} m",
                c.seq, (now - c.start_ns) / 1e6, c.frames, (c.blend_ns - c.start_ns) / 1e6, c.max_jump, c.game_max_jump);
  }
  return true;
}

bool FpvTurn(double* yaw, double* pitch) {
  if (!REXCVAR_GET(skate3_portal_camcarry)) return false;
  float R[9], entry[3], exit[3];
  long long ns = 0;
  const unsigned seq = skate3::portal_gate::LastCrossing(R, &ns, entry, exit);
  if (seq == g_fpv_seq) return false;
  g_fpv_seq = seq;
  if (NowNs() - ns > 500'000'000) return false;  // an old crossing (first person was off then, or this is the first look)
  const double y0 = *yaw;
  pm::TurnYawPitch(R, yaw, pitch);
  REXLOG_INFO("portal-camera: PCAM first person turned through the portal (crossing {}): yaw {:.0f} -> {:.0f} deg", seq, y0 * 57.29578,
              *yaw * 57.29578);
  return true;
}

}  // namespace skate3::portal_camera

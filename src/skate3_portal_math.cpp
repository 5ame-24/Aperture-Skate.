#include "skate3_portal_math.h"

#include <algorithm>
#include <cmath>

namespace skate3::portal_math {

bool Frame::Flat() const { return std::abs(n[1]) > kFlatNy; }

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void SetFrame(Frame& f, const float c[3], const float n[3], const float u[3], float trigger) {
  std::copy(c, c + 3, f.c);
  std::copy(n, n + 3, f.n);
  std::copy(u, u + 3, f.u);
  f.r[0] = u[1] * n[2] - u[2] * n[1];
  f.r[1] = u[2] * n[0] - u[0] * n[2];
  f.r[2] = u[0] * n[1] - u[1] * n[0];
  f.trigger = trigger;
}

bool InOval(float lat, float vert, float grow) {
  const float a = kHalfWidth + grow, b = kHalfHeight + grow;
  return lat * lat / (a * a) + vert * vert / (b * b) < 1.0f;
}

void Local(const Frame& f, const float p[3], float* lat, float* vert, float* depth) {
  const float rel[3] = {p[0] - f.c[0], p[1] - f.c[1], p[2] - f.c[2]};
  *lat = Dot(rel, f.r);
  *vert = Dot(rel, f.u);
  *depth = Dot(rel, f.n);
}

void PairRotation(const Frame& e, const Frame& x, float R[9]) {
  for (int k = 0; k < 3; ++k) {
    for (int j = 0; j < 3; ++j) R[k * 3 + j] = -x.r[k] * e.r[j] + x.u[k] * e.u[j] - x.n[k] * e.n[j];
  }
}

void Apply(const float R[9], const float v[3], float out[3]) {
  for (int k = 0; k < 3; ++k) out[k] = R[k * 3] * v[0] + R[k * 3 + 1] * v[1] + R[k * 3 + 2] * v[2];
}

void MapPoint(const Frame& x, float lat, float vert, float out_dist, float out[3]) {
  for (int k = 0; k < 3; ++k) out[k] = x.c[k] - lat * x.r[k] + vert * x.u[k] + out_dist * x.n[k];
}

bool TestCrossing(const Frame g[2], const float prev[3], const float now[3], bool standing_ok, const Rules& rules, Crossing* out) {
  for (int i = 0; i < 2; ++i) {
    const Frame& e = g[i];
    const Frame& x = g[1 - i];
    float lat, vert, dn, pl, pv, dp;
    Local(e, now, &lat, &vert, &dn);
    Local(e, prev, &pl, &pv, &dp);
    const float d = dn - e.trigger, prev_d = dp - e.trigger;
    const bool inside = InOval(lat, vert, rules.margin);
    bool crossing = prev_d > 0.0f && d <= 0.0f && inside;
    if (!crossing && standing_ok && e.Flat() && inside && d <= 0.0f && dn > -rules.standing_back) {
      const float away[3] = {now[0] - prev[0], now[1] - prev[1], now[2] - prev[2]};
      crossing = Dot(away, e.n) <= 0.01f;
    }
    if (!crossing) continue;
    const float depth = std::max(0.0f, -d);
    const float out_dist = (x.Flat() ? rules.flat_exit : x.trigger) + depth +
                           (x.Flat() ? rules.flat_clear : rules.wall_clear);
    out->from = i;
    MapPoint(x, lat, vert, out_dist, out->exit_pos);
    // A floor's "up" is the aim heading, so a sideways offset there becomes a vertical one on a wall: keep the exit above the
    // ground a wall portal usually stands on.
    if (!x.Flat()) out->exit_pos[1] = std::max(out->exit_pos[1], x.c[1] - 1.0f);
    PairRotation(e, x, out->R);
    out->exit_dir[0] = x.n[0];
    out->exit_dir[1] = x.n[2];
    out->flat_pair = e.Flat() || x.Flat();
    out->ceiling_exit = x.n[1] < -0.5f;
    return true;
  }
  return false;
}

void ExitTarget(const float vout[3], const float fallback_dir[2], const float pos[3], float target[16]) {
  float hx = vout[0], hz = vout[2];
  float len = std::hypot(hx, hz);
  if (len < 0.5f) {
    hx = fallback_dir[0];
    hz = fallback_dir[1];
    len = std::hypot(hx, hz);
    if (len < 0.01f) {
      hx = 0.0f;
      hz = 1.0f;
      len = 1.0f;
    }
  }
  hx /= len;
  hz /= len;
  const float t[16] = {hz, 0, -hx, 0, 0, 1, 0, 0, hx, 0, hz, 0, pos[0], pos[1], pos[2], 1};
  std::copy(t, t + 16, target);
}

void PoseFromView(const float view[16], Pose* out) {
  for (int i = 0; i < 3; ++i) {
    out->right[i] = view[i * 4 + 0];
    out->up[i] = view[i * 4 + 1];
    out->fwd[i] = view[i * 4 + 2];
  }
  for (int j = 0; j < 3; ++j) out->pos[j] = -(view[12] * view[j * 4 + 0] + view[13] * view[j * 4 + 1] + view[14] * view[j * 4 + 2]);
}

void ViewFromPose(const Pose& p, float view[16]) {
  std::fill(view, view + 16, 0.0f);
  for (int i = 0; i < 3; ++i) {
    view[i * 4 + 0] = p.right[i];
    view[i * 4 + 1] = p.up[i];
    view[i * 4 + 2] = p.fwd[i];
  }
  for (int k = 0; k < 3; ++k) view[12 + k] = -(p.pos[0] * view[k] + p.pos[1] * view[4 + k] + p.pos[2] * view[8 + k]);
  view[15] = 1.0f;
}

void Mul4(const float a[16], const float b[16], float out[16]) {
  float t[16];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) sum += a[r * 4 + k] * b[k * 4 + c];
      t[r * 4 + c] = sum;
    }
  }
  std::copy(t, t + 16, out);
}

void CarryPose(const Pose& rel, const float R[9], const float skater[3], Pose* out) {
  float o[3];
  Apply(R, rel.pos, o);
  for (int k = 0; k < 3; ++k) out->pos[k] = skater[k] + o[k];
  Apply(R, rel.right, out->right);
  Apply(R, rel.up, out->up);
  Apply(R, rel.fwd, out->fwd);
}

static void Cross3(const float a[3], const float b[3], float out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

static bool Normalise(float v[3]) {
  const float l = std::sqrt(Dot(v, v));
  if (!(l > 1e-4f)) return false;
  for (int k = 0; k < 3; ++k) v[k] /= l;
  return true;
}

void BlendPose(const Pose& a, const Pose& b, float w, Pose* out) {
  w = std::clamp(w, 0.0f, 1.0f);
  float ua_x_fa[3];
  Cross3(a.up, a.fwd, ua_x_fa);
  const float hand = Dot(ua_x_fa, a.right) >= 0.0f ? 1.0f : -1.0f;
  Pose r;
  float f[3], u[3];
  for (int k = 0; k < 3; ++k) {
    r.pos[k] = a.pos[k] + (b.pos[k] - a.pos[k]) * w;
    f[k] = a.fwd[k] + (b.fwd[k] - a.fwd[k]) * w;
    u[k] = a.up[k] + (b.up[k] - a.up[k]) * w;
  }
  if (!Normalise(f)) std::copy(b.fwd, b.fwd + 3, f);  // opposite forwards: no halfway, take the target
  float d = Dot(u, f);
  for (int k = 0; k < 3; ++k) u[k] -= d * f[k];
  if (!Normalise(u)) {
    const float wy[3] = {0, 1, 0};
    std::copy(wy, wy + 3, u);
    d = Dot(u, f);
    for (int k = 0; k < 3; ++k) u[k] -= d * f[k];
    if (!Normalise(u)) {
      const float wx[3] = {1, 0, 0};
      std::copy(wx, wx + 3, u);
      d = Dot(u, f);
      for (int k = 0; k < 3; ++k) u[k] -= d * f[k];
      Normalise(u);
    }
  }
  float rt[3];
  Cross3(u, f, rt);
  for (int k = 0; k < 3; ++k) {
    r.right[k] = hand * rt[k];
    r.up[k] = u[k];
    r.fwd[k] = f[k];
  }
  *out = r;
}

float Smoothstep(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

void TurnYawPitch(const float R[9], double* yaw, double* pitch) {
  const float f[3] = {float(std::sin(*yaw) * std::cos(*pitch)), float(std::sin(*pitch)), float(std::cos(*yaw) * std::cos(*pitch))};
  float o[3];
  Apply(R, f, o);
  *yaw = std::atan2(double(o[0]), double(o[2]));
  *pitch = std::asin(std::clamp(double(o[1]), -1.0, 1.0));
}

}  // namespace skate3::portal_math

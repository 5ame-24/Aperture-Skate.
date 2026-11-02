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

static float R2(const float* y, const float* fit, int n) {
  double mean = 0;
  for (int i = 0; i < n; ++i) mean += y[i];
  mean /= n;
  double ss_tot = 0, ss_res = 0;
  for (int i = 0; i < n; ++i) {
    ss_tot += (y[i] - mean) * (y[i] - mean);
    ss_res += (y[i] - fit[i]) * (y[i] - fit[i]);
  }
  return ss_tot > 1e-12 ? float(1.0 - ss_res / ss_tot) : (ss_res < 1e-12 ? 1.0f : 0.0f);
}

bool FitLine(const float* t, const float* y, int n, float* a, float* b, float* r2) {
  if (n < 3) return false;
  double st = 0, sy = 0, stt = 0, sty = 0;
  for (int i = 0; i < n; ++i) {
    st += t[i];
    sy += y[i];
    stt += double(t[i]) * t[i];
    sty += double(t[i]) * y[i];
  }
  const double den = n * stt - st * st;
  if (std::abs(den) < 1e-12) return false;
  *b = float((n * sty - st * sy) / den);
  *a = float((sy - *b * st) / n);
  float fit[512];
  const int m = std::min(n, 512);
  for (int i = 0; i < m; ++i) fit[i] = *a + *b * t[i];
  *r2 = R2(y, fit, m);
  return true;
}

bool FitQuadratic(const float* t, const float* y, int n, float* a, float* b, float* c, float* r2) {
  if (n < 4) return false;
  // Normal equations about the mean time (better conditioned), solved with Cramer's rule.
  double tm = 0;
  for (int i = 0; i < n; ++i) tm += t[i];
  tm /= n;
  double s[5] = {}, r[3] = {};
  for (int i = 0; i < n; ++i) {
    const double x = t[i] - tm;
    double p = 1;
    for (int k = 0; k < 5; ++k) {
      s[k] += p;
      if (k < 3) r[k] += p * y[i];
      p *= x;
    }
  }
  const double M[3][3] = {{s[0], s[1], s[2]}, {s[1], s[2], s[3]}, {s[2], s[3], s[4]}};
  auto det3 = [](const double m[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  };
  const double D = det3(M);
  if (std::abs(D) < 1e-18) return false;
  double coef[3];
  for (int col = 0; col < 3; ++col) {
    double Mc[3][3];
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) Mc[i][j] = j == col ? r[i] : M[i][j];
    }
    coef[col] = det3(Mc) / D;
  }
  // y = c0 + c1 x + c2 x^2 with x = t - tm  ->  powers of t.
  *c = float(coef[2]);
  *b = float(coef[1] - 2 * coef[2] * tm);
  *a = float(coef[0] - coef[1] * tm + coef[2] * tm * tm);
  float fit[512];
  const int m = std::min(n, 512);
  for (int i = 0; i < m; ++i) fit[i] = *a + *b * t[i] + *c * t[i] * t[i];
  *r2 = R2(y, fit, m);
  return true;
}

float RmsDiff(const float (*a)[3], const float (*b)[3], int n) {
  if (n <= 0) return 0.0f;
  double sum = 0;
  for (int i = 0; i < n; ++i) {
    for (int k = 0; k < 3; ++k) sum += double(a[i][k] - b[i][k]) * (a[i][k] - b[i][k]);
  }
  return float(std::sqrt(sum / n));
}

bool FunnelCorrection(const Frame& f, const float p[3], const float v[3], float gravity, float dt, const FunnelRules& rules, float dv[3],
                      float* landing_offset) {
  dv[0] = dv[1] = dv[2] = 0.0f;
  *landing_offset = 0.0f;
  if (f.n[1] <= kFlatNy || !(dt > 0.0f) || !(gravity > 0.0f)) return false;
  float lat, vert, h;
  Local(f, p, &lat, &vert, &h);
  const float vn = Dot(v, f.n);
  if (h <= 0.0f || h > rules.well_above || vn > -rules.min_fall) return false;
  // gravity is world -y; its parts along the normal and in the plane
  const float gvec[3] = {0.0f, -gravity, 0.0f};
  const float gn = -Dot(gvec, f.n);  // > 0: pulls into the plane
  const float g_lat = Dot(gvec, f.r), g_vert = Dot(gvec, f.u);
  // time to reach the plane: h + vn t - gn t^2 / 2 = 0
  const float disc = vn * vn + 2.0f * gn * h;
  if (disc < 0.0f) return false;
  const float t = gn > 1e-4f ? (vn + std::sqrt(disc)) / gn : (vn < -1e-4f ? -h / vn : -1.0f);
  if (!(t > 0.0f)) return false;
  const float vlat = Dot(v, f.r), vvert = Dot(v, f.u);
  const float land_lat = lat + vlat * t + 0.5f * g_lat * t * t, land_vert = vert + vvert * t + 0.5f * g_vert * t * t;
  *landing_offset = std::hypot(land_lat, land_vert);
  if (!InOval(land_lat / rules.capture, land_vert / rules.capture, 0.0f)) return false;
  if (h > rules.height && !InOval(land_lat, land_vert, 0.0f)) return false;  // well above: only when already heading into the opening
  if (*landing_offset < rules.snap) return false;
  // moving toward the portal at all?
  const float to_c[3] = {f.c[0] - p[0], f.c[1] - p[1], f.c[2] - p[2]};
  const float lt = std::sqrt(Dot(to_c, to_c)), lv = std::sqrt(Dot(v, v));
  if (lt < 1e-3f || lv < 1e-3f || Dot(to_c, v) / (lt * lv) < rules.gimme_dot) return false;
  // in-plane velocity that lands on the centre, reached at most max_accel * dt per step
  const float want_lat = -(lat + 0.5f * g_lat * t * t) / t, want_vert = -(vert + 0.5f * g_vert * t * t) / t;
  float dl = want_lat - vlat, dvv = want_vert - vvert;
  const float mag = std::hypot(dl, dvv), cap = rules.max_accel * dt;
  if (mag > cap) {
    dl *= cap / mag;
    dvv *= cap / mag;
  }
  for (int k = 0; k < 3; ++k) dv[k] = dl * f.r[k] + dvv * f.u[k];
  return true;
}

float RightRoll(Pose* p, float* omega, float dt, const RightRules& rules) {
  // world up with its forward component removed: the "upright" up for this forward
  const float wy[3] = {0, 1, 0};
  float target[3];
  const float d = Dot(wy, p->fwd);
  for (int k = 0; k < 3; ++k) target[k] = wy[k] - d * p->fwd[k];
  const float tl = std::sqrt(Dot(target, target));
  if (tl < 0.1f) {  // looking (almost) straight up or down
    *omega = 0.0f;
    return 0.0f;
  }
  for (float& c : target) c /= tl;
  // signed roll from up to target about fwd
  float cr[3];
  Cross3(p->up, target, cr);
  const float sinr = Dot(cr, p->fwd), cosr = std::clamp(Dot(p->up, target), -1.0f, 1.0f);
  const float roll = std::atan2(sinr, cosr) * 57.29578f;  // degrees, sign = direction to turn
  const float aroll = std::abs(roll);
  if (aroll < 0.05f) {
    *omega = 0.0f;
    return 0.0f;
  }
  const float want = std::min(rules.max_speed, rules.base + rules.rate * aroll / 180.0f);
  *omega = std::min(want, *omega + rules.accel * dt);
  const float step = std::min(aroll, *omega * dt) * (roll > 0 ? 1.0f : -1.0f) / 57.29578f;
  // rotate up and right about fwd by `step` (Rodrigues; fwd is unit and perpendicular to both)
  const float c = std::cos(step), s = std::sin(step);
  auto rot = [&](float v[3]) {
    float fx[3];
    Cross3(p->fwd, v, fx);
    for (int k = 0; k < 3; ++k) v[k] = v[k] * c + fx[k] * s;
  };
  rot(p->up);
  rot(p->right);
  return aroll - std::abs(step) * 57.29578f;
}

}  // namespace skate3::portal_math

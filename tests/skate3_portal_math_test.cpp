// Standalone check for the pure portal maths (skate3_portal_math). Build and run:
//   clang++ -std=c++23 -Wall -Wextra -Werror -Isrc tests/skate3_portal_math_test.cpp \
//       src/skate3_portal_math.cpp -o /tmp/skate3_portal_math_test && /tmp/skate3_portal_math_test

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

#include "skate3_portal_math.h"

using namespace skate3::portal_math;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

static bool Near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) < eps; }
static float Len(const float v[3]) { return std::sqrt(Dot(v, v)); }

static Frame Wall(float cx, float cy, float cz, float nx, float nz, float trigger = 1.1f) {
  Frame f;
  const float c[3] = {cx, cy, cz}, n[3] = {nx, 0, nz}, u[3] = {0, 1, 0};
  SetFrame(f, c, n, u, trigger);
  return f;
}
static Frame Floor(float cx, float cy, float cz, float hx, float hz, float trigger = 2.2f) {
  Frame f;
  const float c[3] = {cx, cy, cz}, n[3] = {0, 1, 0}, u[3] = {hx, 0, hz};
  SetFrame(f, c, n, u, trigger);
  return f;
}
static Frame Ceiling(float cx, float cy, float cz) {
  Frame f;
  const float c[3] = {cx, cy, cz}, n[3] = {0, -1, 0}, u[3] = {0, 0, 1};
  SetFrame(f, c, n, u, 0.95f);
  return f;
}

// R must be a rotation: orthonormal rows, determinant +1.
static void CheckRotation(const float R[9]) {
  for (int a = 0; a < 3; ++a) {
    for (int b = 0; b < 3; ++b) {
      const float d = R[a * 3] * R[b * 3] + R[a * 3 + 1] * R[b * 3 + 1] + R[a * 3 + 2] * R[b * 3 + 2];
      CHECK(Near(d, a == b ? 1.0f : 0.0f));
    }
  }
  const float det = R[0] * (R[4] * R[8] - R[5] * R[7]) - R[1] * (R[3] * R[8] - R[5] * R[6]) + R[2] * (R[3] * R[7] - R[4] * R[6]);
  CHECK(Near(det, 1.0f));
}

int main() {
  // Frames are right-handed.
  {
    const Frame w = Wall(0, 2, 0, 0, 1);
    float rxu[3] = {w.r[1] * w.u[2] - w.r[2] * w.u[1], w.r[2] * w.u[0] - w.r[0] * w.u[2], w.r[0] * w.u[1] - w.r[1] * w.u[0]};
    CHECK(Near(rxu[0], w.n[0]) && Near(rxu[1], w.n[1]) && Near(rxu[2], w.n[2]));
  }
  // Oval.
  CHECK(InOval(0, 0, 0));
  CHECK(InOval(kHalfWidth - 0.01f, 0, 0));
  CHECK(!InOval(kHalfWidth + 0.01f, 0, 0));
  CHECK(!InOval(0, kHalfHeight + 0.01f, 0));
  CHECK(InOval(0, kHalfHeight + 0.4f, 0.5f));
  CHECK(!InOval(2.0f, 1.6f, 0));  // corner of the old rectangle is outside the oval

  // Facing walls: blue at z=0 facing +z, orange at z=20 facing -z. Going into blue (-z) comes out of orange moving -z: same
  // direction in the world (a "window").
  {
    const Frame g[2] = {Wall(0, 2, 0, 0, 1), Wall(0, 2, 20, 0, -1)};
    float R[9];
    PairRotation(g[0], g[1], R);
    CheckRotation(R);
    const float vin[3] = {1, 0.5f, -6}, *v = vin;
    float vout[3];
    Apply(R, v, vout);
    CHECK(Near(vout[0], 1) && Near(vout[1], 0.5f) && Near(vout[2], -6));
    CHECK(Near(Len(vout), Len(vin)));
  }
  // Same wall, both facing +z, side by side: entering one turns you round (180 degrees about up).
  {
    const Frame g[2] = {Wall(0, 2, 0, 0, 1), Wall(10, 2, 0, 0, 1)};
    float R[9];
    PairRotation(g[0], g[1], R);
    CheckRotation(R);
    const float vin[3] = {2, 0, -5};
    float vout[3];
    Apply(R, vin, vout);
    CHECK(Near(vout[0], -2) && Near(vout[1], 0) && Near(vout[2], 5));
  }
  // Floor -> wall: falling into a floor portal (velocity -y) comes out of a wall facing +x moving +x.
  {
    const Frame g[2] = {Floor(0, 0, 0, 0, 1), Wall(30, 2, 0, 1, 0)};
    float R[9];
    PairRotation(g[0], g[1], R);
    CheckRotation(R);
    const float vin[3] = {0, -9, 0};
    float vout[3];
    Apply(R, vin, vout);
    CHECK(Near(vout[0], 9) && Near(vout[1], 0) && Near(vout[2], 0));
  }
  // Floor <-> ceiling: the endless fall keeps falling.
  {
    const Frame g[2] = {Floor(0, 0, 0, 0, 1), Ceiling(0, 6, 0)};
    float R[9];
    PairRotation(g[0], g[1], R);
    CheckRotation(R);
    const float vin[3] = {0.3f, -12, 1};
    float vout[3];
    Apply(R, vin, vout);
    CHECK(Near(vout[1], -12));
    CHECK(Near(Len(vout), Len(vin)));
  }
  // Round trip: through e then back through x is the identity (any frames, including angled ones).
  {
    Frame a, b;
    const float ca[3] = {1, 2, 3}, na[3] = {0.6f, 0.0f, 0.8f}, ua[3] = {0, 1, 0};
    const float s = std::sqrt(0.5f);
    const float cb[3] = {-4, 5, 9}, nb[3] = {0, s, s}, ub[3] = {0, s, -s};
    SetFrame(a, ca, na, ua, 1.1f);
    SetFrame(b, cb, nb, ub, 2.2f);
    float R1[9], R2[9];
    PairRotation(a, b, R1);
    PairRotation(b, a, R2);
    CheckRotation(R1);
    CheckRotation(R2);
    const float v[3] = {3, -1, 2};
    float m[3], back[3];
    Apply(R1, v, m);
    Apply(R2, m, back);
    CHECK(Near(back[0], 3) && Near(back[1], -1) && Near(back[2], 2));
    // Entering a means moving against a.n; leaving b means moving along b.n.
    const float into[3] = {-na[0], -na[1], -na[2]};
    float outv[3];
    Apply(R1, into, outv);
    CHECK(Near(outv[0], nb[0]) && Near(outv[1], nb[1]) && Near(outv[2], nb[2]));
  }

  const Rules rules;
  // Rolling into a wall portal: crosses at the trigger plane, comes out of the other one in front of it, mirrored across.
  {
    const Frame g[2] = {Wall(0, 1.7f, 0, 0, 1), Wall(50, 1.7f, 50, -1, 0)};
    const float prev[3] = {0.5f, 0.85f, 1.3f}, now[3] = {0.5f, 0.85f, 1.0f};
    Crossing c;
    CHECK(TestCrossing(g, prev, now, true, rules, &c));
    CHECK(c.from == 0);
    CHECK(!c.flat_pair);
    float lat, vert, d;
    Local(g[1], c.exit_pos, &lat, &vert, &d);
    CHECK(Near(lat, -0.5f) && Near(vert, 0.85f - 1.7f));
    CHECK(Near(d, 1.1f + 0.1f + rules.wall_clear));
    CHECK(Near(c.exit_dir[0], -1) && Near(c.exit_dir[1], 0));
  }
  // Moving away from the portal, or outside the oval, or from behind: no crossing.
  {
    const Frame g[2] = {Wall(0, 1.7f, 0, 0, 1), Wall(50, 1.7f, 50, -1, 0)};
    Crossing c;
    const float a[3] = {0, 0.85f, 1.0f}, b[3] = {0, 0.85f, 1.3f};
    CHECK(!TestCrossing(g, a, b, true, rules, &c));  // moving away
    const float o1[3] = {4.0f, 0.85f, 1.3f}, o2[3] = {4.0f, 0.85f, 1.0f};
    CHECK(!TestCrossing(g, o1, o2, true, rules, &c));  // beside the oval
    const float k1[3] = {0, 0.85f, -0.5f}, k2[3] = {0, 0.85f, -0.8f};
    CHECK(!TestCrossing(g, k1, k2, true, rules, &c));  // behind the wall
  }
  // Rolling onto a floor portal (riding height 0.85 is below the 2.2 m trigger): the standing rule fires, the plane rule does not.
  {
    const Frame g[2] = {Floor(0, 0, 0, 0, 1), Wall(30, 2, 0, 1, 0)};
    const float prev[3] = {0, 0.85f, 3.5f}, now[3] = {0, 0.85f, 1.0f};
    Crossing c;
    CHECK(!TestCrossing(g, prev, now, false, rules, &c));  // prev already below the trigger plane, no standing rule
    CHECK(TestCrossing(g, prev, now, true, rules, &c));
    CHECK(c.from == 0 && c.flat_pair);
    // Exit is in front of the wall, at least 1 m below the wall centre at the lowest.
    float lat, vert, d;
    Local(g[1], c.exit_pos, &lat, &vert, &d);
    CHECK(d > 1.1f);
    CHECK(c.exit_pos[1] >= 2.0f - 1.0f - 1e-4f);
    // Jumping up off a floor portal (moving along +n) does not re-enter it.
    const float up0[3] = {0, 0.85f, 0}, up1[3] = {0, 1.2f, 0};
    CHECK(!TestCrossing(g, up0, up1, true, rules, &c));
  }
  // Ceiling exit: flagged, so the caller does not lift the feet above it.
  {
    const Frame g[2] = {Floor(0, 0, 0, 0, 1), Ceiling(0, 6, 0)};
    const float prev[3] = {0, 3.0f, 0}, now[3] = {0, 2.0f, 0};
    Crossing c;
    CHECK(TestCrossing(g, prev, now, false, rules, &c));
    CHECK(c.from == 0 && c.ceiling_exit);
    CHECK(c.exit_pos[1] < 6.0f);  // below the ceiling
  }
  // Exit target: rows are an orthonormal yaw frame facing the velocity's heading, translation in row 3.
  {
    const float v[3] = {3, -2, 4}, fb[2] = {1, 0}, pos[3] = {7, 8, 9};
    float t[16];
    ExitTarget(v, fb, pos, t);
    CHECK(Near(t[8], 0.6f) && Near(t[10], 0.8f) && Near(t[9], 0));
    CHECK(Near(t[0], 0.8f) && Near(t[2], -0.6f));
    CHECK(Near(t[5], 1));
    CHECK(Near(t[12], 7) && Near(t[13], 8) && Near(t[14], 9) && Near(t[15], 1));
    const float slow[3] = {0.1f, -5, 0.1f};
    ExitTarget(slow, fb, pos, t);
    CHECK(Near(t[8], 1) && Near(t[10], 0));  // falls back to the exit normal's heading
    const float none[2] = {0, 0};
    ExitTarget(slow, none, pos, t);
    CHECK(Near(t[10], 1));
  }
  // ---- camera poses ----
  // View <-> pose round trip, for a left-handed basis too (the game's camera may be either).
  {
    for (float hand : {1.0f, -1.0f}) {
      Pose p;
      const float s = std::sqrt(0.5f);
      const float pos[3] = {3, 4, -5}, f[3] = {s, 0, s}, u[3] = {0, 1, 0};
      std::copy(pos, pos + 3, p.pos);
      std::copy(f, f + 3, p.fwd);
      std::copy(u, u + 3, p.up);
      p.right[0] = hand * (u[1] * f[2] - u[2] * f[1]);
      p.right[1] = hand * (u[2] * f[0] - u[0] * f[2]);
      p.right[2] = hand * (u[0] * f[1] - u[1] * f[0]);
      float view[16];
      ViewFromPose(p, view);
      // the camera position maps to the view origin
      float o[3];
      for (int k = 0; k < 3; ++k) o[k] = p.pos[0] * view[k] + p.pos[1] * view[4 + k] + p.pos[2] * view[8 + k] + view[12 + k];
      CHECK(Near(o[0], 0) && Near(o[1], 0) && Near(o[2], 0));
      Pose q;
      PoseFromView(view, &q);
      for (int k = 0; k < 3; ++k) {
        CHECK(Near(q.pos[k], p.pos[k]) && Near(q.fwd[k], p.fwd[k]) && Near(q.up[k], p.up[k]) && Near(q.right[k], p.right[k]));
      }
      // blending keeps the handedness and the endpoints
      Pose b = p;
      b.pos[0] += 10;
      const float yaw = 1.2f;
      const float bf[3] = {std::sin(yaw), 0, std::cos(yaw)};
      std::copy(bf, bf + 3, b.fwd);
      b.right[0] = hand * (b.up[1] * bf[2] - b.up[2] * bf[1]);
      b.right[1] = hand * (b.up[2] * bf[0] - b.up[0] * bf[2]);
      b.right[2] = hand * (b.up[0] * bf[1] - b.up[1] * bf[0]);
      Pose m;
      BlendPose(p, b, 0.0f, &m);
      for (int k = 0; k < 3; ++k) CHECK(Near(m.pos[k], p.pos[k]) && Near(m.fwd[k], p.fwd[k]) && Near(m.right[k], p.right[k]));
      BlendPose(p, b, 1.0f, &m);
      for (int k = 0; k < 3; ++k) CHECK(Near(m.pos[k], b.pos[k]) && Near(m.fwd[k], b.fwd[k]) && Near(m.right[k], b.right[k]));
      BlendPose(p, b, 0.4f, &m);
      CHECK(Near(Len(m.fwd), 1) && Near(Len(m.up), 1) && Near(Len(m.right), 1));
      CHECK(Near(Dot(m.fwd, m.up), 0) && Near(Dot(m.fwd, m.right), 0) && Near(Dot(m.up, m.right), 0));
      float c[3] = {m.up[1] * m.fwd[2] - m.up[2] * m.fwd[1], m.up[2] * m.fwd[0] - m.up[0] * m.fwd[2], m.up[0] * m.fwd[1] - m.up[1] * m.fwd[0]};
      CHECK(Near(Dot(c, m.right), hand));
      CHECK(Near(m.pos[0], 3 + 4));
      // opposite forwards do not produce NaNs
      Pose back = p;
      for (int k = 0; k < 3; ++k) {
        back.fwd[k] = -p.fwd[k];
        back.right[k] = -p.right[k];
      }
      BlendPose(p, back, 0.5f, &m);
      CHECK(std::isfinite(m.fwd[0] + m.up[0] + m.right[0]) && Near(Len(m.fwd), 1));
    }
  }
  // Carrying a chase-camera pose through facing walls: the camera that was 4 m behind (+z) a skater going -z into blue comes out 4 m
  // behind (+z) the skater leaving orange (still going -z) and still looks -z.
  {
    const Frame g[2] = {Wall(0, 2, 0, 0, 1), Wall(0, 2, 20, 0, -1)};
    float R[9];
    PairRotation(g[0], g[1], R);
    Pose rel;
    const float rp[3] = {0, 1.5f, 4}, f[3] = {0, 0, -1}, u[3] = {0, 1, 0}, r[3] = {-1, 0, 0};
    std::copy(rp, rp + 3, rel.pos);
    std::copy(f, f + 3, rel.fwd);
    std::copy(u, u + 3, rel.up);
    std::copy(r, r + 3, rel.right);
    const float sk[3] = {0, 1, 18};
    Pose out;
    CarryPose(rel, R, sk, &out);
    CHECK(Near(out.pos[0], 0) && Near(out.pos[1], 2.5f) && Near(out.pos[2], 22));
    CHECK(Near(out.fwd[2], -1) && Near(out.up[1], 1));
    // Same-wall pair (180 degrees): camera ends up on the other side, looking the other way.
    const Frame h[2] = {Wall(0, 2, 0, 0, 1), Wall(10, 2, 0, 0, 1)};
    PairRotation(h[0], h[1], R);
    CarryPose(rel, R, sk, &out);
    CHECK(Near(out.pos[2], 18 - 4) && Near(out.fwd[2], 1) && Near(out.up[1], 1));
    // First person: yaw facing -z (pi) through the same-wall pair faces +z (0).
    double yaw = 3.14159265358979, pitch = -0.1;
    TurnYawPitch(R, &yaw, &pitch);
    CHECK(Near(float(yaw), 0, 1e-3f) && Near(float(pitch), -0.1f, 1e-4f));
  }
  CHECK(Near(Smoothstep(0), 0) && Near(Smoothstep(1), 1) && Near(Smoothstep(0.5f), 0.5f) && Near(Smoothstep(2), 1));
  {
    const float a[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1};
    const float b[16] = {2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1};
    float o[16];
    Mul4(a, b, o);
    CHECK(Near(o[0], 2) && Near(o[5], 3) && Near(o[10], 4) && Near(o[12], 10) && Near(o[13], 18) && Near(o[14], 28) && Near(o[15], 1));
  }
  // ---- fits ----
  {
    // A 0.6 s ballistic arc at 60 Hz, g = 14, with +-1 cm deterministic noise: the quadratic recovers g, the line fit of v_y recovers it.
    float t[40], y[40], vy[40];
    for (int i = 0; i < 36; ++i) {
      t[i] = 3.0f + i / 60.0f;  // not starting at 0, like real timestamps
      const float tt = t[i] - 3.0f;
      y[i] = 64.0f + 5.0f * tt - 7.0f * tt * tt + 0.01f * ((i * 7919) % 3 - 1);
      vy[i] = 5.0f - 14.0f * tt;
    }
    float a, b, c, r2;
    CHECK(FitQuadratic(t, y, 36, &a, &b, &c, &r2));
    CHECK(Near(-2 * c, 14.0f, 0.6f) && r2 > 0.99f);
    CHECK(FitLine(t, vy, 36, &a, &b, &r2));
    CHECK(Near(-b, 14.0f, 1e-3f) && Near(r2, 1.0f, 1e-4f));
    CHECK(!FitLine(t, vy, 2, &a, &b, &r2));
    float tc[5] = {1, 1, 1, 1, 1};
    CHECK(!FitQuadratic(tc, y, 5, &a, &b, &c, &r2));
    // a flat series has r2 = 1 when it is matched exactly
    float flat[6] = {2, 2, 2, 2, 2, 2};
    CHECK(FitLine(t, flat, 6, &a, &b, &r2) && Near(b, 0) && Near(r2, 1));
    const float va[2][3] = {{1, 2, 3}, {0, 0, 0}}, vb[2][3] = {{1, 2, 7}, {0, 3, 0}};
    CHECK(Near(RmsDiff(va, vb, 2), std::sqrt((16.0f + 9.0f) / 2)));
  }
  // ---- funnelling ----
  {
    const Frame fl = Floor(0, 0, 0, 0, 1, 2.2f);
    const FunnelRules fr;
    float dv[3], off;
    // Falling from 3 m, 1.8 m off-centre across, drifting slightly away: steered toward the centre (dv points back to x = 0).
    const float p[3] = {1.8f, 3.0f, 0.0f}, v[3] = {-0.2f, -4.0f, 0.0f};
    CHECK(FunnelCorrection(fl, p, v, 9.81f, 1.0f / 60, fr, dv, &off));
    CHECK(dv[0] < 0 && Near(dv[1], 0) && Near(dv[2], 0));
    CHECK(std::hypot(dv[0], dv[2]) <= fr.max_accel / 60 + 1e-4f);
    // Iterating the correction (a simple ballistic simulation) lands within the snap distance of the centre.
    float q[3] = {1.8f, 3.0f, 0.0f}, w[3] = {-0.2f, -4.0f, 0.0f};
    for (int i = 0; i < 600 && q[1] > 0; ++i) {
      const float dt = 1.0f / 60;
      if (FunnelCorrection(fl, q, w, 9.81f, dt, fr, dv, &off)) {
        for (int k = 0; k < 3; ++k) w[k] += dv[k];
      }
      w[1] -= 9.81f * dt;
      for (int k = 0; k < 3; ++k) q[k] += w[k] * dt;
    }
    CHECK(std::hypot(q[0], q[2]) < 0.6f);  // without funnelling it would land at ~1.6 m
    // Not when rising, too high, landing far outside, already centred, or on a wall portal.
    const float up[3] = {0, 3, 0};
    CHECK(!FunnelCorrection(fl, p, up, 9.81f, 1.0f / 60, fr, dv, &off));
    const float high[3] = {1.8f, 9.0f, 0.0f};
    CHECK(!FunnelCorrection(fl, high, v, 9.81f, 1.0f / 60, fr, dv, &off));
    const float far[3] = {9.0f, 2.0f, 0.0f};
    CHECK(!FunnelCorrection(fl, far, v, 9.81f, 1.0f / 60, fr, dv, &off));
    const float centred[3] = {0.05f, 2.0f, 0.0f}, straight[3] = {0, -5, 0};
    CHECK(!FunnelCorrection(fl, centred, straight, 9.81f, 1.0f / 60, fr, dv, &off));
    CHECK(!FunnelCorrection(Wall(0, 2, 0, 0, 1), p, v, 9.81f, 1.0f / 60, fr, dv, &off));
    // Moving away from the portal (gimme dot): no help.
    const float away[3] = {6.0f, -1.0f, 0.0f};
    CHECK(!FunnelCorrection(fl, p, away, 9.81f, 1.0f / 60, fr, dv, &off));
  }
  // ---- camera righting ----
  {
    const RightRules rr;
    // A camera rolled 90 degrees (up = world x) looking along +z rights itself to up = world y, forward unchanged.
    Pose p;
    const float f[3] = {0, 0, 1}, u[3] = {1, 0, 0}, r[3] = {0, -1, 0};  // right = up x fwd for this layout
    std::copy(f, f + 3, p.fwd);
    std::copy(u, u + 3, p.up);
    std::copy(r, r + 3, p.right);
    float omega = 0, left = 90;
    int steps = 0;
    while (left > 0.05f && steps < 600) {
      left = RightRoll(&p, &omega, 1.0f / 60, rr);
      ++steps;
    }
    CHECK(left <= 0.05f);
    CHECK(Near(p.up[1], 1.0f, 1e-3f) && Near(p.fwd[2], 1.0f, 1e-5f));
    CHECK(Near(Dot(p.up, p.right), 0, 1e-4f) && Near(Len(p.up), 1, 1e-4f));
    // 90 degrees takes a noticeable but short time (accelerating, capped): between 0.25 and 1 s at these rates.
    CHECK(steps > 15 && steps < 60);
    // the other roll direction converges too
    const float u2[3] = {-1, 0, 0}, r2[3] = {0, 1, 0};
    std::copy(u2, u2 + 3, p.up);
    std::copy(r2, r2 + 3, p.right);
    omega = 0;
    for (int i = 0; i < 120; ++i) RightRoll(&p, &omega, 1.0f / 60, rr);
    CHECK(Near(p.up[1], 1.0f, 1e-3f));
    // upright stays put; straight down is left alone
    Pose q;
    CHECK(Near(RightRoll(&q, &omega, 1.0f / 60, rr), 0));
    const float down[3] = {0, -1, 0}, qu[3] = {0, 0, 1}, qr[3] = {1, 0, 0};
    std::copy(down, down + 3, q.fwd);
    std::copy(qu, qu + 3, q.up);
    std::copy(qr, qr + 3, q.right);
    CHECK(Near(RightRoll(&q, &omega, 1.0f / 60, rr), 0) && Near(q.up[2], 1));
  }
  {
    // gels: touching a floor patch, bounce, boost, painting over
    GelRules gr;
    GelPatch floor;
    floor.c[1] = 2.0f;
    float h = 0;
    const float above[3] = {1.0f, 2.3f, 0.5f}, high[3] = {0, 3.0f, 0}, wide[3] = {2.0f, 2.1f, 0}, sunk[3] = {0, 1.75f, 0};
    CHECK(GelTouch(floor, above, gr, &h) && Near(h, 0.3f, 1e-5f));
    CHECK(!GelTouch(floor, high, gr, &h));
    CHECK(!GelTouch(floor, wide, gr, &h));
    CHECK(GelTouch(floor, sunk, gr, &h));
    // a 12 m/s fall comes back at 12 m/s (restitution 1), horizontal speed kept
    float out[3];
    const float fall[3] = {4, -12, 1};
    CHECK(GelBounce(floor, fall, false, gr, out) && Near(out[0], 4) && Near(out[1], 12.0f) && Near(out[2], 1));
    GelRules soft = gr;
    soft.restitution = 0.5f;
    CHECK(GelBounce(floor, fall, false, soft, out) && Near(out[1], gr.min_bounce));
    // a small drop gets the minimum bounce; rolling onto it (grounded) too; moving away does nothing
    const float drop[3] = {3, -2, 0}, roll[3] = {6, 0, 0}, rise[3] = {0, 6, 0};
    CHECK(GelBounce(floor, drop, false, gr, out) && Near(out[1], gr.min_bounce) && Near(out[0], 3));
    CHECK(GelBounce(floor, roll, true, gr, out) && Near(out[1], gr.min_bounce) && Near(out[0], 6));
    CHECK(!GelBounce(floor, roll, false, gr, out));
    CHECK(!GelBounce(floor, rise, true, gr, out));
    // a wall patch facing +x: running into it at 8 m/s comes back at 8, along-wall part kept, at least wall_up upward; rolling past it does nothing
    GelPatch wall;
    wall.n[0] = 1;
    wall.n[1] = 0;
    const float into[3] = {-8, 1, 3}, past[3] = {0, 0, 9};
    CHECK(GelBounce(wall, into, false, gr, out) && Near(out[0], 8.0f) && Near(out[1], gr.wall_up) && Near(out[2], 3));
    CHECK(GelBounce(wall, into, true, gr, out) && Near(out[0], 8.0f) && Near(out[1], 1) && Near(out[2], 3));  // grounded: no wall jump
    // the cap: a 20 m/s fall comes out at max_bounce (never below the floor minimum)
    GelRules capped = gr;
    capped.max_bounce = 10.0f;
    const float hard[3] = {0, -20, 0};
    CHECK(GelBounce(floor, hard, false, capped, out) && Near(out[1], 10.0f));
    capped.max_bounce = 1.0f;
    CHECK(GelBounce(floor, hard, false, capped, out) && Near(out[1], gr.min_bounce));
    CHECK(!GelBounce(wall, past, true, gr, out));
    const float slow[3] = {-1, 0, 0};
    CHECK(GelBounce(wall, slow, false, gr, out) && Near(out[0], gr.wall_min));
    // boost: speeds up, capped, nothing from standstill
    CHECK(Near(GelBoost(10, 0.1f, gr), 11.27f));
    CHECK(Near(GelBoost(20.0f, 0.1f, gr), gr.boost_cap));
    CHECK(Near(GelBoost(30, 0.1f, gr), 30));
    CHECK(Near(GelBoost(0.5f, 0.1f, gr), 0.5f));
    // painting: overlapping paint of either kind is covered, a wall patch next to it is not, the list is capped
    GelPatch list[4];
    int n = 0;
    GelPatch a = floor, b = floor, c = floor, w2 = wall;
    b.c[0] = 0.4f;  // nearly the same spot: covers a
    b.kind = 1;
    c.c[0] = 10.0f;
    w2.c[1] = 2.0f;
    CHECK(GelPaint(list, &n, 4, a) == 0 && n == 1);
    CHECK(GelPaint(list, &n, 4, b) == 1 && n == 1 && list[0].kind == 1);
    CHECK(GelPaint(list, &n, 4, c) == 0 && n == 2);
    CHECK(GelPaint(list, &n, 4, w2) == 0 && n == 3);
    CHECK(!GelOverlaps(floor, wall));
    GelPatch next = floor;
    next.c[0] = 1.5f;  // a splat next to it (discs overlap) stays
    CHECK(!GelOverlaps(floor, next));
    for (int i = 0; i < 5; ++i) {
      GelPatch far = floor;
      far.c[2] = 20.0f + 10.0f * float(i);
      GelPaint(list, &n, 4, far);
    }
    CHECK(n == 4 && Near(list[3].c[2], 60.0f) && Near(list[0].c[2], 30.0f));
    // the drawn outline: in the lifted plane, inside the radius, counter-clockwise from the front (floor and wall)
    for (const GelPatch* gp : {&floor, &wall}) {
      float ring[20][3];
      CHECK(GelRing(*gp, 0.03f, 20, ring) == 20);
      for (int k = 0; k < 20; ++k) {
        const float d[3] = {ring[k][0] - gp->c[0], ring[k][1] - gp->c[1], ring[k][2] - gp->c[2]};
        const float hh = Dot(d, gp->n);
        CHECK(Near(hh, 0.03f, 1e-4f));
        const float rr = std::sqrt(Dot(d, d) - hh * hh);
        CHECK(rr >= 0.75f * gp->radius && rr <= gp->radius + 1e-4f);
        const float* q = ring[(k + 1) % 20];
        const float e1[3] = {ring[k][0] - gp->c[0], ring[k][1] - gp->c[1], ring[k][2] - gp->c[2]};
        const float e2[3] = {q[0] - gp->c[0], q[1] - gp->c[1], q[2] - gp->c[2]};
        const float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        CHECK(Dot(cr, gp->n) > 0.0f);
      }
    }
  }
  {
    // a shot into the blue portal on a wall (facing +z at z = 0) comes out of the orange one on the floor (facing up at y = 10), going up
    Frame g[2];
    const float c0[3] = {0, 1, 0}, n0[3] = {0, 0, 1}, u0[3] = {0, 1, 0};
    const float c1[3] = {20, 10, 0}, n1[3] = {0, 1, 0}, u1[3] = {0, 0, 1};
    SetFrame(g[0], c0, n0, u0, 0.0f);
    SetFrame(g[1], c1, n1, u1, 0.0f);
    const float o[3] = {0.5f, 1.2f, 10.0f}, d[3] = {0, 0, -1};
    int from = -1;
    float oo[3], od[3];
    CHECK(RayThroughPortal(g, o, d, 10.0f, 0.3f, &from, oo, od));
    CHECK(from == 0 && Near(od[1], 1.0f, 1e-5f) && Near(oo[1], 10.05f, 1e-4f) && Near(oo[0], 20.5f, 1e-4f) && Near(oo[2], 0.2f, 1e-4f));
    // outside the oval, from behind, or hit something before the portal: no
    const float wide[3] = {4.0f, 1.0f, 10.0f}, behind[3] = {0, 1, -10};
    const float back[3] = {0, 0, 1};
    CHECK(!RayThroughPortal(g, wide, d, 10.0f, 0.3f, &from, oo, od));
    CHECK(!RayThroughPortal(g, behind, back, 10.0f, 0.3f, &from, oo, od));
    CHECK(!RayThroughPortal(g, o, d, 5.0f, 0.3f, &from, oo, od));
  }
  {
    // fitting: a wall portal facing +z at the origin
    Frame f;
    const float c0[3] = {0, 2, 0}, n0[3] = {0, 0, 1}, u0[3] = {0, 1, 0};
    SetFrame(f, c0, n0, u0, 0.0f);
    FitRules fr;
    float o[3], d[3], dir2[2];
    RimProbe(f, 0, fr, o, d, dir2);  // k = 0: along +r, at 0.92 of the half width, 0.5 m in front, looking into the wall
    CHECK(Near(o[0], kHalfWidth * fr.rim) && Near(o[1], 2.0f) && Near(o[2], 0.5f) && Near(d[2], -1.0f) && Near(dir2[0], 1.0f));
    const float on[3] = {1, 2, 0.02f}, off[3] = {1, 2, -0.6f}, nz[3] = {0, 0, 1}, nx[3] = {1, 0, 0};
    CHECK(RimProbeOk(f, true, on, nz, fr));
    CHECK(!RimProbeOk(f, true, off, nz, fr));  // a hit far behind the plane (an outside corner)
    CHECK(!RimProbeOk(f, true, on, nx, fr));   // another surface
    CHECK(!RimProbeOk(f, false, on, nz, fr));
    bool ok[kRimProbes];
    for (bool& b : ok) b = true;
    float sh[2];
    CHECK(FitShift(ok, fr, sh) == 0);
    ok[0] = false;  // the right edge is off the wall: move left
    CHECK(FitShift(ok, fr, sh) == 1 && Near(sh[0], -fr.step) && Near(sh[1], 0));
    ok[1] = false;  // right and upper right: move left and down
    CHECK(FitShift(ok, fr, sh) == 1 && sh[0] < 0 && sh[1] < 0 && Near(std::hypot(sh[0], sh[1]), fr.step));
    ok[4] = false;  // left too: the wall is narrower than the portal
    CHECK(FitShift(ok, fr, sh) == 2);
    // overlap: the second portal 1 m to the right of the first on the same wall is pushed to 2 x half width + 5 cm
    Frame b = f;
    b.c[0] = 1.0f;
    float push[3];
    CHECK(OverlapPush(f, b, kHalfWidth, kHalfHeight, push) && Near(push[0], 2.0f * kHalfWidth + 0.05f - 1.0f) && Near(push[1], 0));
    b.c[0] = 0.0f;
    b.c[1] = 3.0f;  // 1 m above: pushed up to 2 x half height + 5 cm
    CHECK(OverlapPush(f, b, kHalfWidth, kHalfHeight, push) && Near(push[1], 2.0f * kHalfHeight + 0.05f - 1.0f));
    b.c[1] = 2.0f + 2.0f * kHalfHeight + 0.1f;  // already clear
    CHECK(!OverlapPush(f, b, kHalfWidth, kHalfHeight, push));
    Frame w2;
    const float c2[3] = {1, 2, 0}, n2[3] = {1, 0, 0};
    SetFrame(w2, c2, n2, u0, 0.0f);  // another wall: no push
    CHECK(!OverlapPush(f, w2, kHalfWidth, kHalfHeight, push));
  }
  {
    // facing: a skater whose forward is the matrix's third ROW, riding along it (and fakie 20% of the time), turning around
    FacingLearner fl;
    for (int i = 0; i < 100; ++i) {
      const float a = 0.13f * float(i);
      const float c = std::cos(a), sn = std::sin(a);
      // rows: right, up, forward = (sin a, 0, cos a)
      const float R[9] = {c, 0, -sn, 0, 1, 0, sn, 0, c};
      const float dir = (i % 5 == 0) ? -1.0f : 1.0f;  // fakie every fifth sample
      const float travel[2] = {dir * sn, dir * c};
      fl.Add(R, travel);
    }
    int axis = -1;
    float sign = 0, agree = 0;
    CHECK(fl.Best(&axis, &sign, &agree) && axis == 5 && sign > 0 && agree > 0.99f);
    FacingLearner few;
    const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, t0[2] = {0, 1};
    few.Add(I, t0);
    CHECK(!few.Best(&axis, &sign, &agree));
  }
  std::puts("skate3_portal_math_test: ok");
  return 0;
}

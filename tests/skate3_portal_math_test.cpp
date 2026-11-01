// Standalone check for the pure portal maths (skate3_portal_math). Build and run:
//   clang++ -std=c++23 -Wall -Wextra -Werror -Isrc tests/skate3_portal_math_test.cpp \
//       src/skate3_portal_math.cpp -o /tmp/skate3_portal_math_test && /tmp/skate3_portal_math_test

#include <cmath>
#include <cstdio>
#include <cstdlib>

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
  std::puts("skate3_portal_math_test: ok");
  return 0;
}

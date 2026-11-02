// Unit test for the Portal character skeleton retarget (pure maths).
// clang++ -std=c++23 -Wall -Wextra -Werror -Isrc tests/skate3_portal_retarget_test.cpp src/skate3_portal_retarget.cpp -o /tmp/skate3_portal_retarget_test && /tmp/skate3_portal_retarget_test
#include "skate3_portal_retarget.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace skate3::portal_retarget;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

static bool Near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; }

int main() {
  {
    // 3x4 helpers
    const float m[12] = {0, -1, 0, 1, 1, 0, 0, 2, 0, 0, 1, 3};  // 90 deg about z, then (1, 2, 3)
    float inv[12], id[12], p[3];
    CHECK(Invert34(m, inv));
    Mul34(m, inv, id);
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c) CHECK(Near(id[r * 4 + c], (c == r) ? 1.0f : 0.0f, 1e-5f));
    const float x[3] = {1, 0, 0};
    Apply34(m, x, p);
    CHECK(Near(p[0], 1) && Near(p[1], 3) && Near(p[2], 3));
  }
  // The skater: a T-pose arm along +x from the shoulder at the origin: upper arm (key 0x100) 0..1 m, forearm (key 0x101) 1..2 m, plus a
  // torso block (key 0x102) below the shoulder. The character: the same arm in an A-pose, 45 degrees down, with its own two bones and a
  // torso bone (its root).
  std::vector<DonorVertex> donor;
  auto add_donor = [&](float x, float y, float z, uint32_t key) {
    DonorVertex d;
    d.p[0] = x, d.p[1] = y, d.p[2] = z;
    d.key[0] = key;
    d.weight[0] = 1.0f;
    donor.push_back(d);
  };
  for (int i = 0; i <= 100; ++i)
    for (int j = -1; j <= 1; ++j) add_donor(2.0f * float(i) / 100.0f, 0.03f * float(j), 0.0f, i < 50 ? 0x100u : 0x101u);
  for (int i = 0; i <= 60; ++i)
    for (int j = 0; j <= 4; ++j) add_donor(-0.25f + 0.1f * float(j), -0.1f - 0.9f * float(i) / 60.0f, 0.0f, 0x102u);
  const float s = std::sqrt(0.5f);
  std::vector<Bone> bones(3);
  bones[0].parent = -1;  // torso, at the shoulder
  bones[1].parent = 0;   // upper arm, at the shoulder
  bones[2].parent = 1;   // forearm, at the elbow (1 m down the 45 degree arm)
  bones[2].rest[3] = s, bones[2].rest[7] = -s;
  std::vector<float> verts;
  std::vector<Influence> infl;
  auto add = [&](float x, float y, float z, uint8_t b) {
    verts.push_back(x), verts.push_back(y), verts.push_back(z);
    Influence in;
    in.bone[0] = b;
    infl.push_back(in);
  };
  for (int i = 1; i < 100; ++i) {
    const float r = 2.0f * float(i) / 100.0f;
    add(r * s, -r * s, 0.0f, i < 50 ? 1 : 2);
  }
  for (int i = 0; i <= 80; ++i)
    for (int j = 0; j <= 4; ++j) add(-0.25f + 0.1f * float(j), -0.15f - 0.8f * float(i) / 80.0f, 0.0f, 0);  // a torso as wide as the skater's
  const Result r = Retarget(bones, verts, infl, donor, 2);
  CHECK(r.map.size() == 3 && r.map[0] == 0x102u && r.map[1] == 0x100u && r.map[2] == 0x101u);
  CHECK(r.mapped == 3 && r.distinct_keys == 3);
  CHECK(r.turned >= 1 && r.max_turn_deg > 30.0f && r.max_turn_deg < 60.0f);
  CHECK(r.gap_after < 0.5f * r.gap_before);
  // the corrected arm lies along +x (T-pose), the elbow joint at about (1, 0, 0)
  for (int i = 1; i < 99; ++i) CHECK(std::abs(r.rest[size_t(i - 1) * 3 + 1]) < 0.06f && r.rest[size_t(i - 1) * 3] > 0.0f);
  CHECK(Near(r.posed[2 * 12 + 3], 1.0f, 0.06f) && Near(r.posed[2 * 12 + 7], 0.0f, 0.06f));
  // skinning with the skater's palettes: the forearm bent 90 degrees up about the elbow (1, 0, 0), the rest identity
  static float pal_id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  static float pal_bent[12] = {0, -1, 0, 1, 1, 0, 0, -1, 0, 0, 1, 0};  // R_z(90) about (1, 0, 0)
  const PaletteFn fn = [](uint32_t key, void*) -> const float* { return key == 0x101u ? pal_bent : key == 0x100u || key == 0x102u ? pal_id : nullptr; };
  float out[3];
  const size_t hand = 97;  // r = 1.96 m along the arm
  CHECK(SkinVertex(r, infl[hand], hand, fn, nullptr, out));
  CHECK(Near(out[0], 1.0f, 0.08f) && Near(out[1], r.rest[hand * 3] - 1.0f, 0.08f));
  const PaletteFn none = [](uint32_t, void*) -> const float* { return nullptr; };
  CHECK(!SkinVertex(r, infl[hand], hand, none, nullptr, out));
  // no skater vertices: nothing changes
  const Result e = Retarget(bones, verts, infl, {}, 2);
  CHECK(e.rest == verts && e.mapped == 0);
  std::puts("skate3_portal_retarget_test: ok");
  return 0;
}

// Standalone check for the portal gun math; needs no rexglue or guest. Build and run:
//   clang++ -std=c++23 -Wall -Wextra -Werror -Isrc tests/skate3_portal_test.cpp \
//       src/skate3_portal.cpp -o /tmp/skate3_portal_test && /tmp/skate3_portal_test

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "skate3_portal.h"

using namespace skate3::portal;

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                              \
    }                                                            \
  } while (0)

static bool Near(float a, float b) { return std::abs(a - b) < 1e-4f; }
static float Speed(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

int main() {
  // Floor (y = 0) and a wall (x = 10) facing -x, one quad each.
  const Triangle world[] = {
      {{-50, 0, -50}, {50, 0, -50}, {50, 0, 50}},  {{-50, 0, -50}, {50, 0, 50}, {-50, 0, 50}},
      {{10, -50, -50}, {10, 50, -50}, {10, 50, 50}}, {{10, -50, -50}, {10, 50, 50}, {10, -50, 50}},
  };
  PortalPair pair;

  // Straight down onto the floor: portal 0 lands at the ray's hit, facing up.
  CHECK(FirePortalGun(pair, 0, {0, 5, 0}, {0, -1, 0}, world, 4));
  CHECK(pair.portal[0].active && Near(pair.portal[0].position.y, 0) &&
        Near(pair.portal[0].normal.y, 1));
  // Sideways into the wall from inside the room: portal 1 faces back at -x.
  CHECK(FirePortalGun(pair, 1, {0, 3, 0}, {1, 0, 0}, world, 4));
  CHECK(Near(pair.portal[1].position.x, 10) && Near(pair.portal[1].normal.x, -1));
  // A miss, a zero ray and a bad index all leave the pair alone.
  const PortalPair before = pair;
  CHECK(!FirePortalGun(pair, 0, {0, 5, 0}, {0, 1, 0}, world, 4));
  CHECK(!FirePortalGun(pair, 0, {0, 5, 0}, {0, 0, 0}, world, 4));
  CHECK(!FirePortalGun(pair, 2, {0, 5, 0}, {0, -1, 0}, world, 4));
  CHECK(Near(pair.portal[0].position.x, before.portal[0].position.x));

  // Falling into the floor portal at (3, -10, 0), just past its plane.
  Vec3 pos{0.2f, -0.05f, 0.0f};
  Vec3 vel{3, -10, 0};
  const float speed = Speed(vel);
  CHECK(HandlePortalTeleportation(pair, pos, vel, 1.0f / 60.0f));
  CHECK(Near(Speed(vel), speed));              // kinetic energy conserved
  CHECK(Near(vel.x, -10));                     // fall speed now drives out of the wall (-x)
  CHECK(Near(pos.x, 10 - 0.05f));              // stepped out in front of the exit plane
  CHECK(!HandlePortalTeleportation(pair, pos, vel, 1.0f / 60.0f));  // no ping-pong

  // Outside the portal's radius, or only one portal open: nothing happens.
  Vec3 far_pos{2.0f, -0.05f, 0.0f};
  Vec3 down{0, -10, 0};
  CHECK(!HandlePortalTeleportation(pair, far_pos, down, 1.0f / 60.0f));
  pair.portal[1].active = false;
  Vec3 near_pos{0.2f, -0.05f, 0.0f};
  CHECK(!HandlePortalTeleportation(pair, near_pos, down, 1.0f / 60.0f));

  std::puts("skate3_portal_test: ok");
  return 0;
}

#pragma once

// Portal gun core: the portal pair, the fire-time surface raycast and the
// through-portal teleport. Pure host math - nothing here reads guest memory,
// so the caller supplies the camera ray, the world triangles and the player's
// world position/velocity, and writes the results back.

#include <cstddef>

namespace skate3::portal {

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct Portal {
  Vec3 position;
  bool active = false;
  Vec3 normal{0.0f, 1.0f, 0.0f};  // unit, pointing out of the surface it sits on
};

// portal[0] = primary fire (left click / left trigger), portal[1] = secondary
// (right click / right trigger). Each one exits through the other.
struct PortalPair {
  Portal portal[2];
};

struct Triangle {
  Vec3 a;
  Vec3 b;
  Vec3 c;
};

// Instant ray from `origin` along `forward` against `tris`; the nearest hit
// becomes portal `which` (0 or 1), activated and aligned to that surface.
// Returns false (pair untouched) on a miss, a zero `forward` or a bad `which`.
bool FirePortalGun(PortalPair& pair, int which, Vec3 origin, Vec3 forward,
                   const Triangle* tris, size_t count);

// If the player has just crossed an active portal's plane moving into it,
// moves `position` through the paired portal and rotates `velocity` by the
// same entry-to-exit rotation (speed is conserved exactly). One hop per call.
// `dt` widens the crossing window so a fast skater cannot tunnel past it in a
// single frame. Returns true when a teleport happened.
bool HandlePortalTeleportation(const PortalPair& pair, Vec3& position, Vec3& velocity, float dt);

}  // namespace skate3::portal

#pragma once

// A companion cube of our own: not a game object. Simple rigid-body physics on the game thread (gravity, ground and walls
// from the game's ray service, the skater pushes it, portals carry it through), drawn by the native renderer. Because we own
// its state, a crossing is exact (position, velocity, orientation all rotated by the portal pair's transform).

#include <cstdint>

namespace skate3::portal_cube {

constexpr float kHalf = 0.45f;  // half the cube's edge, metres

// UI thread (Ctrl+5, or D-pad down + X): spawn a cube in front of the skater, or bring it back.
void RequestSpawn();
// UI thread (Ctrl+G, or D-pad down + R1): pick the cube up if it is close, drop it if it is held.
void RequestGrab();

// ---- Game thread, once per frame for the human skater ----
bool TakeSpawn();
bool TakeGrab();
// Grab / drop. Returns true if the cube is now held. `skater_v` is the skater's horizontal velocity (a drop keeps it: a throw).
bool ToggleHold(const float skater[3], const float skater_v[2]);
bool Held();
void Spawn(const float at[3]);
bool Active();
// The ray this frame wants answered (the game's ray service, same Start/End batch as the gun). kind 0 = down (ground),
// 1 = along the horizontal velocity (wall).
struct RayWant {
  float o[3], d[3];  // origin, unit direction
  float len;
  int kind;
};
bool NextRay(RayWant* out);
void OnRay(int kind, bool hit, const float pos[3], const float normal[3]);
// dt seconds; skater = the skater's entity position and horizontal velocity; hand = where a held cube is carried (a point under the crosshair).
void Step(float dt, const float skater[3], const float skater_v[2], const float hand[3]);

// ---- Render thread ----
struct Draw {
  float c[3];
  float axes[3][3];  // world direction of each local axis
  float ground[3];   // where a shadow goes, valid if has_ground
  bool has_ground;
};
bool Snapshot(Draw* out);

// A real game prop wearing the cube's skin (Ctrl+9 next to a prop): the probe publishes its transform each frame, the renderer draws the cube there
// and skips the prop's own item. The prop keeps its physics, so it grinds, gets pushed and crosses portals like any game object.
void SetExternal(const float c[3], const float axes[3][3]);
void ClearExternal();
bool ExternalActive();
bool ExternalPos(float out[3]);
bool HidesItemAt(const float world_pos[3]);  // a dyn_entity item at this position is the adopted prop's own look

}  // namespace skate3::portal_cube

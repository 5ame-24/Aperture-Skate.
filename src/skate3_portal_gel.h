#pragma once

// Portal 2 style gels (spec docs/specs/002-portal2-gels.md, log prefix GEL, cvar skate3_portal_gel).
// Gel mode (Ctrl+H, or D-pad down + B on a pad) makes the portal gun paint instead of placing portals: blue = repulsion (bounce), orange =
// propulsion (speed). The paint list lives here; the game-side writes (board rates, FLING hold, prop bodies) are in skate3_portal_probe.cpp.
// Thread use: Paint/Clear/ToggleMode from the input, gun and UI threads; HumanStep from the game thread; BodyStep from the physics update;
// Snapshot from the overlay. All under one mutex, with an atomic count for the cheap "any gel at all?" check.

#include <cstdint>

#include "skate3_portal_math.h"

namespace skate3::portal_gel {

bool Enabled();  // skate3_portal_gel (and the portal gate itself)
bool Mode();     // the gun paints gel
void ToggleMode();
void Paint(int kind, const float hit[3], const float normal[3], uint32_t frame);  // kind 0 blue, 1 orange
void Clear();
int Count();
int Snapshot(portal_math::GelPatch* out, int max);
const portal_math::GelRules& Rules();
// The renderer drew the splats this frame (depth-tested, in the world): the ImGui overlay stands down.
void NoteDrawn();
bool DrawnRecently();

// The human, once per update. feet = ground point under the skater, centre = about 0.85 m above it, v = world velocity (m/s), grounded =
// riding flags, gravity = m/s^2 (AIR's measured value or 9.81).
struct Effect {
  enum Kind { kNone, kBounce, kBoost } kind = kNone;
  float v_out[3] = {};  // kBounce: the new velocity (hold it with FLING)
  float dv[3] = {};     // kBounce: v_out - v; kBoost: the velocity to add this update
  int patch = -1;
  bool lift = false;     // kBounce while grounded: needs a lift off the ground to stick
};
Effect HumanStep(const float feet[3], const float centre[3], const float v[3], bool grounded, float dt, float gravity, uint32_t frame);

// ---- Gel droppers (Portal 2's paint dropper: a pipe that keeps dropping gel) ----
// Gel mode has two gun sub-modes, Ctrl+Shift+H switches: paint (a shot paints where it hits) and dropper (a shot places a dropper of that
// colour: under a ceiling it hits, otherwise 4 m above the hit point). Each dropper drops a blob every 0.2 s; blobs fall, pass through
// portals (portal_math::BlobAdvance), and paint where they land, so a stream can be redirected with the portals. Clear removes droppers too.
bool DropperMode();
void ToggleDropperMode();
void PlaceDropper(int kind, const float hit[3], const float normal[3], uint32_t frame);
// Game thread, every human update: spawn and move the blobs. portals = the pair's frames or null.
void StreamStep(float dt, float gravity, const portal_math::Frame* portals, uint32_t frame);
// The ray the streams want this frame (one blob, round robin): origin, unit direction, length. False = none.
bool NextBlobRay(uint32_t* blob, float o[3], float d[3], float* len);
void OnBlobRay(uint32_t blob, bool hit, const float pos[3], const float normal[3]);
// Overlay: blob positions / kinds and dropper positions / kinds.
int BlobSnapshot(float (*p)[3], int* kind, int max);
int DropperSnapshot(float (*p)[3], int* kind, int max);

// A loose prop body after its physics update: true with the new velocity when it hit blue gel (cubes bounce forever, as in Portal 2).
bool BodyStep(const float p[3], const float v[3], float out_v[3]);

}  // namespace skate3::portal_gel

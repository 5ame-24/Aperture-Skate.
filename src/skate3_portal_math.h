#pragma once

// Pure portal maths (no guest, no ImGui, no locks): portal frames, the oval opening, the pair transform, a stateless crossing
// test and the executor's target matrix. Used by the NPC crossings in skate3_portal_probe.cpp and unit-tested in
// tests/skate3_portal_math_test.cpp. Same conventions as skate3_portal_gate.cpp (which keeps its own copy for the human).

namespace skate3::portal_math {

constexpr float kFlatNy = 0.7f;      // |n.y| above this behaves as a floor/ceiling portal
constexpr float kHalfWidth = 2.6f;   // oval half extents, metres
constexpr float kHalfHeight = 2.0f;

// Right-handed frame: r x u = n. n = front normal (the open side), u = "up" in the plane (world up on a wall, the shooter's
// heading on a floor or ceiling), r = across. `trigger` = metres in front of the plane where a crossing fires.
struct Frame {
  float c[3] = {};
  float n[3] = {0, 0, 1}, u[3] = {0, 1, 0}, r[3] = {1, 0, 0};
  float trigger = 0.0f;
  bool Flat() const;
};

float Dot(const float a[3], const float b[3]);
// Builds r = u x n.
void SetFrame(Frame& f, const float c[3], const float n[3], const float u[3], float trigger);
bool InOval(float lat, float vert, float grow);
// p in the frame: across, up, signed distance in front of the plane.
void Local(const Frame& f, const float p[3], float* lat, float* vert, float* depth);
// Rotation (row-major 3x3, column-vector form: out = R v) taking a direction entering `e` to the direction leaving `x`:
// (a, b, c) along (e.r, e.u, e.n) -> -a x.r + b x.u - c x.n.
void PairRotation(const Frame& e, const Frame& x, float R[9]);
void Apply(const float R[9], const float v[3], float out[3]);
// The point (lat, vert) of `e` comes out at (-lat, vert) of `x`, `out_dist` in front of x's plane.
void MapPoint(const Frame& x, float lat, float vert, float out_dist, float out[3]);

struct Rules {
  float margin = 0.5f;        // the test oval is this much bigger than the drawn one (board width)
  float wall_clear = 1.2f;    // extra metres a wall exit stays clear of the exit wall
  float flat_clear = 0.3f;    // ... and a floor/ceiling exit
  float flat_exit = 0.95f;    // a floor/ceiling exit comes out this far from the plane (+ depth + clear), not at its trigger
  float standing_back = 0.5f; // standing in a flat opening counts down to this far behind the plane
};

struct Crossing {
  int from = -1;            // portal entered (0 or 1)
  float exit_pos[3] = {};   // same point out of the other portal
  float R[9] = {};          // velocity rotation (see PairRotation)
  float exit_dir[2] = {};   // horizontal part of the exit normal (facing when there is no speed)
  bool flat_pair = false;   // either portal is a floor/ceiling (short cooldown: endless fall)
  bool ceiling_exit = false;
};

// Stateless crossing test for a point that moved from `prev` to `now` (no hole: the surface is solid, so the trigger planes sit in
// front of it, as for the human without skate3_portal_gate_hole). `standing_ok` allows "standing in a flat opening and not moving
// away" to count. False when nothing was crossed.
bool TestCrossing(const Frame g[2], const float prev[3], const float now[3], bool standing_ok, const Rules& rules, Crossing* out);

// The executor's target matrix (row-major 4x4, rows are axes, row 2 forward, row 0 right = (f.z, 0, -f.x), translation row 3)
// facing the horizontal direction of `vout` (or `fallback_dir` when that is slower than 0.5 m/s), translation `pos`.
void ExitTarget(const float vout[3], const float fallback_dir[2], const float pos[3], float target[16]);

// ---- Camera poses (portal camera carry) ----
// A camera pose: world position and its axes. Row-vector view matrices (p_view = p_world * V, as the game and the native renderer
// use) keep right/up/forward in columns 0/1/2 of the upper 3x3 and -pos . axis in row 3.
struct Pose {
  float pos[3] = {};
  float right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, 1};
};
void PoseFromView(const float view[16], Pose* out);
void ViewFromPose(const Pose& p, float view[16]);
// row-vector 4x4 product a * b
void Mul4(const float a[16], const float b[16], float out[16]);
// Pose `rel` (position relative to the skater) carried through a portal: position S + R rel.pos, axes R axes.
void CarryPose(const Pose& rel, const float R[9], const float skater[3], Pose* out);
// From a to b, w in [0, 1]: position lerped, forward and up lerped and re-orthonormalised, right rebuilt with a's handedness.
void BlendPose(const Pose& a, const Pose& b, float w, Pose* out);
float Smoothstep(float t);
// Yaw/pitch (forward = (sin yaw cos pitch, sin pitch, cos yaw cos pitch)) turned by R.
void TurnYawPitch(const float R[9], double* yaw, double* pitch);

}  // namespace skate3::portal_math

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

// A ray from o along unit d that hit something at distance hit_t: did it pass into a portal's opening first (front side, within `slack`
// metres past the plane, since the shot hits the wall the portal sits on)? Then `from` = that portal, out_o = the same point just out of the
// other portal (0.05 m in front of it) and out_d = the turned direction. The nearest portal wins.
bool RayThroughPortal(const Frame g[2], const float o[3], const float d[3], float hit_t, float slack, int* from, float out_o[3], float out_d[3]);

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

// ---- Fits for the airborne probe (R5: gravity and the velocity carrier) ----
// Least squares y = a + b t (FitLine) or y = a + b t + c t^2 (FitQuadratic) over n samples; r2 = coefficient of determination (1 = exact).
// False when n is too small or the times are degenerate.
bool FitLine(const float* t, const float* y, int n, float* a, float* b, float* r2);
bool FitQuadratic(const float* t, const float* y, int n, float* a, float* b, float* c, float* r2);
// Root mean square of |a_i - b_i| over n 3D samples.
float RmsDiff(const float (*a)[3], const float (*b)[3], int n);

// ---- Portal-style behaviours from public descriptions (docs/specs/001-portal-public-behaviour.md) ----
// Floor-portal funnelling: a body falling toward a floor portal is steered so it lands on the portal's centre instead of its rim.
struct FunnelRules {
  float height = 3.25f;       // funnel when this far (or less) above the portal plane (Portal: 128 units)
  float well_above = 6.5f;    // ... or up to this high when the predicted landing is already close (Portal: 256 units)
  float capture = 1.5f;       // the predicted landing must be inside the oval scaled by this
  float snap = 0.25f;         // no correction when the predicted landing is this close to the centre (Portal: 10 units)
  float gimme_dot = 0.7f;     // velocity direction . direction to the portal centre must be at least this
  float max_accel = 12.0f;    // m/s^2 of in-plane velocity change allowed
  float min_fall = 0.5f;      // m/s: must be moving into the plane at least this fast
};
// Returns true with dv (world, m/s, to add to the velocity this step) when portal `f` (a floor: n.y > kFlatNy) funnels the body at p with
// velocity v under `gravity` (m/s^2, downwards); landing_offset = distance of the predicted landing from the centre before correction.
bool FunnelCorrection(const Frame& f, const float p[3], const float v[3], float gravity, float dt, const FunnelRules& rules, float dv[3],
                      float* landing_offset);

// Camera righting after a tilted exit: the roll about the view's forward axis eases back to upright (up toward world up) with a
// rate-limited, accelerating angular speed. Interpretation of Portal's cl_portal_camera_orientation_* defaults (rate 480, base 45,
// max_speed 375 deg/s, acceleration 1000 deg/s^2): target speed = min(max_speed, base + rate * roll / 180 deg).
struct RightRules {
  float rate = 480.0f, base = 45.0f, max_speed = 375.0f, accel = 1000.0f;  // degrees
};
// Turns `p` about its forward axis by this step's amount; `omega` (deg/s) is the controller state. Returns the remaining roll in degrees
// (0 when upright or when forward is too close to vertical for "upright" to mean anything).
float RightRoll(Pose* p, float* omega, float dt, const RightRules& rules);

// ---- Portal 2 style gels (docs/specs/002-portal2-gels.md) ----
// A gel patch: a disc of paint on a surface. kind 0 = repulsion (blue, bounce), 1 = propulsion (orange, speed).
struct GelPatch {
  float c[3] = {};
  float n[3] = {0, 1, 0};  // surface normal, out of the surface
  float radius = 1.75f;
  int kind = 0;
  unsigned id = 0;  // the caller's label (log numbers); not used by the maths
};
struct GelRules {  // defaults from Portal 2's published cvar defaults (spec 002), converted at 1 unit = 0.0254 m
  float touch_above = 0.6f;    // a point this far in front of the surface (or less) touches the patch ...
  float touch_below = 0.4f;    // ... down to this far behind it (the feet sink a little into the ground)
  float restitution = 1.0f;    // repulsion: the speed into the surface comes back out times this (bounce_reflect_restitution 1.0)
  float min_bounce = 9.25f;    // m/s out of a floor patch at least: set from a launch height (bounce_paint_min_speed 500 u/s under
                               // sv_gravity 600 = 5.3 m) and the skater's gravity; 9.25 = 5.3 m at 8.07 m/s^2
  float max_bounce = 0.0f;     // m/s out of any patch at most (0 = no cap): keeps a bounce near the target height (ours, run 197: a drop onto
                               // the paint went higher than wanted)
  float wall_min = 3.0f;       // m/s out of a wall or ceiling patch at least (ours)
  float wall_up = 7.0f;        // m/s upward at least after an AIRBORNE wall bounce (bounce_paint_wall_jump_upward_speed 275 u/s); a grounded
                               // one has none (trampoline_bounce_off_walls_while_on_ground 0: Portal does not wall-jump you off the ground)
  float min_into = 0.5f;       // m/s into the surface needed to count as hitting it (except rolling onto a floor patch; ours)
  float boost_accel = 12.7f;   // propulsion: m/s^2 along the travel direction (sv_speed_paint_acceleration 500 u/s^2) ...
  float boost_cap = 20.3f;     // ... up to this speed (sv_speed_paint_max 800 u/s)
  float boost_min_speed = 1.0f;  // no push from standstill (there is no direction to push along; ours)
};
// Inside the disc (in-plane distance <= radius) and within [-touch_below, touch_above] of its plane; `height` = signed distance in front.
bool GelTouch(const GelPatch& g, const float p[3], const GelRules& rules, float* height);
// Repulsion: the velocity after touching patch g with velocity v. Moving into it: the normal part flips, times restitution, and is at least
// min_bounce (floor) or wall_min (wall/ceiling), and at most max_bounce; the in-plane part is kept; off a wall while airborne the upward part
// is at least wall_up. On a floor patch while grounded and not already rising at half
// of min_bounce: the normal part becomes min_bounce. Otherwise (moving away, or along a wall) false.
bool GelBounce(const GelPatch& g, const float v[3], bool grounded, const GelRules& rules, float out[3]);
// Propulsion: the speed after dt on an orange patch (never slower than `speed`).
float GelBoost(float speed, float dt, const GelRules& rules);
// Same surface (normals within ~25 degrees, planes within 0.3 m) and nearly the same spot (centres closer than a quarter of the two radii):
// painting there again replaces the old splat; paint next to it does not (run 197: neighbouring splats vanished).
bool GelOverlaps(const GelPatch& a, const GelPatch& b);
// Paints `p` over the list (`*n` entries, capacity `max`): overlapping patches of either kind are removed (new paint covers old), then p is
// appended (the oldest is dropped when full). Returns how many were covered.
int GelPaint(GelPatch* list, int* n, int max, const GelPatch& p);
// The splat's outline for drawing: `segments` (3..32) world points around the centre, `lift` metres in front of the surface, counter-clockwise
// seen from the front, with a slightly uneven rim (radius 0.76..1.0 x) seeded by the patch id so it reads as paint, not a portal.
int GelRing(const GelPatch& g, float lift, int segments, float out[][3]);
// A flying gel blob (a dropper's stream, spec 002): one step of dt under `gravity` (m/s^2, down). With portal frames (`g` may be null), a step
// that passes into an opening comes out of the other portal: position mapped, velocity turned, and at least `min_exit` m/s out of it (Portal 2:
// paintblob_minimum_portal_exit_velocity 225 u/s = 5.7 m/s, so a slow stream still clears a floor exit). Returns the portal entered, or -1.
int BlobAdvance(const Frame* g, float p[3], float v[3], float gravity, float dt, float min_exit);

}  // namespace skate3::portal_math

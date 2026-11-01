#pragma once

// Test portal pair ("gates") with a portal-gun aim ray. Two tall rectangles are placed on vertical surfaces
// (the game's own ray query finds the wall; see skate3_portal_probe.cpp), drawn as an ImGui overlay, and
// crossed while airborne (the native teleport keeps airborne momentum). Each portal has its own front normal,
// so a crossing turns the skater's heading from one wall's facing to the other's. Temporary scaffolding.

#include <rex/ui/imgui_dialog.h>

namespace skate3::portal_math {
struct Frame;
}

namespace skate3::portal_gate {

// Both portals as pure frames (skate3_portal_math), for crossings of actors other than the human. False unless both are placed.
bool PortalPairFrames(portal_math::Frame out[2]);
// Another actor (an NPC skater) was teleported from `entry` to `exit`: its skeleton bodies get the same 4 s no-props zone the human's
// crossing gets, so the prop code does not move them a second time.
void NoteActorCrossing(const float entry[3], const float exit[3]);
// The human's latest crossing (for the portal camera carry): a sequence number that changes with every crossing (0 = none yet), the
// velocity rotation (row-major 3x3, out = R v), when (steady_clock ns), and the entry and exit points.
unsigned LastCrossing(float R[9], long long* ns, float entry[3], float exit[3]);

bool Enabled();

// Key-bind side (UI thread): 0 = fire blue, 1 = fire orange, 2 = clear both.
void Request(int what);

// Game thread. A pending fire request (0 or 1), consumed here.
bool TakeFire(int* which);
// The camera's aim ray, from the published scene's view-projection: origin and unit direction.
bool AimRay(float origin[3], float dir[3]);
// True while the mouse is over the game view: the aim ray then goes through the mouse reticle instead of the
// screen centre, and the shot is not turned into a level shot from the skater.
bool AimFree();
// First-person view (Ctrl+4): the renderer's camera follows the skater's head and the mouse looks around.
bool FpvActive();
void ToggleFpv();
// Look change since the last call (radians; yaw right, pitch up) and the head position. Render thread.
void FpvConsumeLook(double* dyaw, double* dpitch);
bool FpvHeadPos(float out[3]);
// Travel direction and speed of the skater (game thread, every frame), used to steer the first-person camera.
void SetTravel(float dx, float dz, float speed);
// First person without a look input for a moment while moving: the yaw to ease toward (the travel direction).
bool FpvAutoYaw(double* yaw);
// The rider's own position (not the board's), from the entity helper; game thread.
void SetHead(const float p[3]);

// Gamepad (guest-facing pad state, called every poll). Hold Back = portal mode: RT fires blue, LT fires orange, Y
// clears both, R3 toggles first person, the right stick looks (in first person). While Back is held those inputs
// are removed from what the game sees, so a shot never also grabs or flips.
void FilterPad(uint16_t* buttons, uint8_t* left_trigger, uint8_t* right_trigger, int16_t* rx, int16_t* ry);
// How far in front of a wall portal's trigger plane the skater comes out (test knob).
void SetWallClear(float metres);
// Put portal `which` on a surface: hit point, the surface normal facing the shooter, and the shot direction
// (sets "up" on a floor or ceiling portal). False for slopes. Fallback when the ray finds nothing: PlaceAhead,
// 7 m in front of the skater.
bool PlaceOn(int which, const float hit[3], const float normal[3], const float shot[3]);
void PlaceAhead(int which, const float skater[3]);

struct Cross {
  float exit_pos[3];  // where the same point comes out of the other portal
  float exit_dir[2];  // horizontal part of the other portal's front normal, the facing if there is no velocity
  float vel[9];       // row-major 3x3: world velocity entering -> world velocity leaving
  float lift;         // metres to raise the feet above the exit point (0 for a ceiling exit)
};
// Game thread, once per frame for the human skater. `pos` is the skater's position. Returns true when `pos`
// crossed a portal while `allowed` (else the crossing is logged and ignored; `flags` is only for that log).
bool Step(const float pos[3], bool allowed, unsigned flags, Cross* out);

// True when both portals are placed and `p` lies in the opening of either (within `slack` metres of its plane and
// inside its rectangle): the world's ray answers there are reported as misses so the wall/floor is not solid.
bool InsideOpening(const float p[3], float slack);

// Body-collision hole. True when a world-triangle contact (triangle through `tri_point` with normal `tri_normal`) for a
// moving shape centred at `mover` should be dropped: the triangle lies in a placed portal's plane and the shape is over
// its opening. The triangle itself is never edited (world triangles are metres across); the hole is virtual.
bool HoleSkips(const float mover[3], const float tri_normal[3], const float tri_point[3]);
// Loose props near the skater (the probe knows which bodies are the skater's own): they keep a solid floor, so one that drifts
// off a portal lands on the edge instead of falling out of the world.
// A crossed prop that fell far below both portals (out of the world): true with a spot beside the lower portal to put it back.
bool PropRescue(const float p[3], float out[3]);
// Scripted self-test (Ctrl+6): drops / tosses a loose prop into the placed portals and logs a pass/fail line per scenario.
// Portal sounds: 0 blue shot, 1 orange shot, 2 through a portal, 3 clear, 4 companion cube, 5 no surface.
void Sfx(int kind);
// The 3D Portal gun (skate3_portal_gunmodel): where the crosshair is (normalised device coordinates) and how hard the last shot kicked it (0..1).
bool GunVisible();  // portal gate on and skate3_portal_gun_model
void AimNdc(float* x, float* y);
float GunKick(int* which);  // `which` = 0 blue, 1 orange
// Ctrl+8: for 8 s the probe logs the game's world line tests near the skater (what a real grind looks like, to make the companion cube grindable).
void RequestGrindCapture();
bool GrindCaptureOn();
void RequestAdopt();  // Ctrl+9: the nearest awake prop becomes the companion cube (again: back to a normal prop)
bool TakeAdopt();
void RequestWake();  // Ctrl+7: wake the sleeping props around the skater
bool TakeWake();
void RequestSelfTest();
bool TakeSelfTest();
// Start position and velocity for scenario 0..2 (centre drop, off-centre drop, toss from further back); low_y = the lower portal's height.
bool SelfTestSpot(int scenario, float pos[3], float vel[3], float* low_y, bool* flat);
bool NearPortal(const float p[3], float radius);  // within `radius` of either placed portal
void SetPropPositions(const float (*pv)[6], int n);  // position and velocity of each loose prop
// Cheap pre-check for the hot path: both portals placed and the hole enabled.
bool HoleArmed();

// World-space portal surfaces for the renderer (render thread): per active portal, two convex polygons in world
// space (a bright rim and a smaller dark core, same plane) as counter-clockwise point rings.
struct SurfaceShape {
  int portal_index;  // 0 blue, 1 orange
  int count;         // points in each ring
  float centre[3];   // portal centre and front normal, for the renderer's "camera is behind it" test
  float normal[3];
  float rim[28][3];
  float core[28][3];
};
int SurfaceShapes(SurfaceShape out[2]);
// See-through view of portal `look_at` (0 blue, 1 orange) from a camera at `cam`: the matrices that put the camera on the
// far side of the OTHER portal, so the world around that portal can be drawn into the opening. Row-vector 4x4s (p * M):
// `inv` = other-portal space -> the space the real camera sees (apply before the view-projection), `fwd` = the reverse
// (for the camera position), `plane` = the other portal's plane, positive on its open side (clip everything behind).
// False when the camera is behind the portal, a portal is missing, or the cvar is off.
bool ViewThrough(int look_at, const float cam[3], float inv[16], float fwd[16], float plane[4]);
// Frame of portal `from` and the matrix (row-vector 4x4) taking a point near it to the same place near the OTHER portal:
// used to draw clones of things that are partly through. False when the pair is not placed or clones are off.
bool PortalFrame(int from, float fwd[16], float c[3], float n[3], float r[3], float u[3]);
// A loose rigid body at `p` moving at `v`: true when it has reached an open portal and should come out of the other one at
// `out_p` with velocity `out_v`; R (row-major 3x3) is the rotation to apply to its orientation and angular velocity.
// Bodies near the tracked skater are left to the skater's own crossing.
bool BodyIsSkaters(uint32_t id, const float p[3]);  // the skater's own body (never a prop)
// The companion cube's centre went from `prev` to `now` (velocity v): true when that crossed an open portal's plane inside its oval,
// with the exit position/velocity and the rotation R (row-major 3x3) to apply to the cube's orientation.
bool CubeCross(const float prev[3], const float now[3], const float v[3], float out_p[3], float out_v[3], float R[9]);
bool TrackedPos(float out[3]);  // the skater's last tracked position (game thread data; false before the first frame)
bool BodiesArmed();  // cheap pre-check for the integrator hook
bool BodyCross(uint32_t id, const float p[3], const float v[3], float out_p[3], float out_v[3], float R[9],
               float exit_plane[4]);  // exit_plane: unit normal and offset (n.x = d) of the portal it comes out of
void NoteSurfaceDrawn();      // the renderer drew the portals this frame: the ImGui rings stand down
bool SurfaceDrawnRecently();

class Overlay final : public rex::ui::ImGuiDialog {
 public:
  explicit Overlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace skate3::portal_gate

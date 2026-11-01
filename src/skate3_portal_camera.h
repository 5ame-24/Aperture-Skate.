#pragma once

// Portal camera carry (cvar skate3_portal_camcarry, default off, log prefix PCAM). When the human crosses a portal, the chase camera's
// pose relative to the skater just before the crossing is carried through the portal (rotated by the pair's rotation) and then eased
// into the game's own camera, instead of the game camera lagging or swinging across the world. First person turns its look through
// the portal instead. Render thread only.

namespace skate3::portal_camera {

// Every frame, with the game's (raw guest) view and the published projection. True when the frame's camera is replaced: view_proj and
// cam_pos then hold the carried pose. Does nothing (false) while the freecam or first person owns the camera.
bool Apply(const float cam_view[16], const float proj[16], bool camera_owned, float view_proj[16], float cam_pos[3]);

// First person: true once per crossing with the look (radians, forward = (sin yaw cos pitch, sin pitch, cos yaw cos pitch)) turned
// through the portal.
bool FpvTurn(double* yaw, double* pitch);

}  // namespace skate3::portal_camera

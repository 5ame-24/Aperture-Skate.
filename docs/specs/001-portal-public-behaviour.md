# 001: Portal behaviour from public documentation

Source: public descriptions only (no game files, no code). Written by the implementation side from:
- the Portal 2 console variable list published with saul/cvar-unhide (`cvarlist-portal2.md`, GitHub): names, defaults, descriptions;
- Valve Developer Community pages "Portal funneling" and "Prop portal" (seen through search summaries; the site itself is not reachable
  from the cloud session).
Units: Hammer units, 1 unit = 1 inch = 0.0254 m. Local numbers from the user's own Portal 2 install come from
`tools/re/portal2_facts.py` (docs/handoff-feedback-4.md; spec 003 when written up).

## Movement and velocity
- **Momentum through portals**: the in-game rule is that speed relative to the exit portal equals speed relative to the entry portal
  (v_out = R v_in, R the pair rotation). `sv_portal_unified_velocity 1` ("removing patchwork velocity transformation in portals, moving
  to a unified approach") and `sv_portal_new_velocity_check 1` suggest one transform applied to the whole velocity.
  -> ours: `portal_math::PairRotation`, FLING holds it for N frames (PROGRESS 55-57).
- **Gravity** `sv_gravity 600` units/s^2 = **15.24 m/s^2**; terminal cap `sv_maxvelocity 3500` units/s **per axis** = **88.9 m/s**.
  -> ours: exit speed cap 80 m/s (magnitude). Skate's own gravity is measured by the AIR probe (PROGRESS 57), not assumed to be Portal's.
- Ground movement for scale: `sv_maxspeed 320` (8.1 m/s), `sv_accelerate 10`, `sv_airaccelerate 10`, `sv_friction 4`, `sv_stopspeed 100`.
- **Paint blobs** get a minimum exit speed (`paintblob_minimum_portal_exit_velocity 225` = 5.7 m/s). No such minimum is listed for the
  player or props: a slow floor exit falls back in.

## Funnelling (floor portals)
- `sv_player_funnel_into_portals 1`: "auto correct toward the center of floor portals"; players and fast objects aimed at a floor portal
  are steered into its middle instead of hitting the rim. The player must be looking at / heading for the portal
  (`sv_player_funnel_gimme_dot 0.9`).
- `sv_player_funnel_height_adjust 128` (3.25 m): funnelling happens when the player is this far (or less) above a floor portal;
  `sv_player_funnel_well_above 256` (6.5 m); `sv_player_funnel_snap_threshold 10` (0.25 m); `sv_player_funnel_speed_bonus 2`;
  props: `sv_props_funnel_into_portals 1`, `sv_props_funnel_into_portals_deceleration 2.0` ("when a funneling prop is leaving a portal,
  decelerate any velocity that is in opposition to funneling by this amount per second").
  -> ours: `portal_math::FunnelCorrection` (tested) + FUNNEL in the probe, cvar `skate3_portal_funnel`. Our interpretation of the
  undocumented parameters: steer the in-plane velocity so the predicted ballistic landing is the centre, at most 12 m/s^2, only while
  falling (> 0.5 m/s into the plane), landing predicted inside 1.5x the oval, velocity . direction-to-centre >= 0.7 (our portals are much
  bigger than Portal's, so 0.9 was too strict), no correction within 0.25 m.

## Camera after a tilted exit
- `cl_portal_camera_orientation_rate 480`, `_rate_base 45`, `_max_speed 375`, `_acceleration_rate 1000` (degrees): after leaving a portal
  that is not upright the view rolls back to upright at a limited, accelerating rate (no descriptions are published).
  `cl_mouselook_roll_compensation 1`: "if your view is being rolled, compensate for that, so mouse movements are always relative to the
  screen". -> ours: `portal_math::RightRoll` (tested; target speed = min(375, 45 + 480 x roll/180 deg), accelerating at 1000 deg/s^2),
  used by the camera carry (PCAM "righting").
- `cl_portal_teleportation_interpolation_fixup_method 1`: "0 = transform history only, 1 = insert discontinuity transform": Portal does
  not interpolate the player across the teleport; it inserts a discontinuity. -> matches our carry (pose carried, no fly-across).

## Placement and size
- Default portal: half-width 32, half-height 54 units = **1.63 m x 2.74 m** (tall oval); pairs must be the same size;
  `portal2_portal_width 72`. Placement: the portal sits 0-8 units (0-0.2 m) from the surface, its centre needs ~57 units (1.45 m) clear
  above/below and 32 units (0.81 m) to the sides, otherwise it is bumped (`sv_portal_placement_never_bump 0`) or fails
  (`sv_portal_placement_never_fail 0`); surfaces can refuse portals (%noportal).
  -> ours: 5.2 m x 4.0 m wide ovals (skater scale), no bump/fit test yet.
- Objects partly through a portal are drawn twice ("ghosting", `portal_ghosts_*`) and collide against a carved copy of the world around
  the portal (`portal_carve_vphysics_clips 1`, `portal_environment_radius 75` = 1.9 m). -> ours: clones (PROGRESS 21) and the hole.
- Views: `r_portal_stencil_depth 2` (portals visible inside portal views, 2 levels). -> ours: no recursion yet.

## Fling devices (for scale)
- Aerial Faith Plates are `trigger_catapult` entities with a player speed and a physics speed per map; the typical values come from the
  user's maps via `tools/re/portal2_facts.py`.

## Unknowns
- Exact funnelling formula and how `speed_bonus` / `well_above` apply; exact camera-righting controller; whether props funnel by the same rule.

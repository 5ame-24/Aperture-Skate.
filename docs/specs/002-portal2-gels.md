# 002: Portal 2 gels (repulsion / propulsion) from public documentation

Source: public descriptions only (no game files, no code). The published Portal 2 console variable list (saul/cvar-unhide,
`cvarlist-portal2.md` on GitHub: names, defaults, one-line descriptions) and general public knowledge of how the gels play.
Units: 1 Hammer unit = 0.0254 m. Portal 2 gravity `sv_gravity 600` = 15.24 m/s^2; the skater's own gravity is measured by AIR
(run 196: median 8.07 m/s^2).

## Repulsion gel (blue, "bounce paint")
- Hitting it sends you back out of the surface. `bounce_reflect_restitution 1.0` ("the elasticity of a collision with a bounce paint
  surface, 0..1"): the speed into the surface comes back out unchanged; the speed along the surface is kept.
- A minimum launch: `bounce_paint_min_speed 500` u/s = 12.7 m/s ("how high bounce paint launches the player"). Under Portal's gravity
  that is 500^2 / (2 x 600) = 208 u = **5.3 m**. Rolling onto it or a small drop still launches you this high.
- `bounce_auto_trigger_min_speed 500`: running over it at this speed auto-bounces.
- Walls: a bounce off a wall is also a wall jump: `bounce_paint_wall_jump_upward_speed 275` u/s = **7.0 m/s** upward is added;
  `bounce_reflect_wall_jumps_enabled 1`, `bounce_reflect_wall_jump_min_up_speed 10` u/s.
- `trampoline_bounce_min_impact_speed 4.0`: a minimum speed into the surface before it counts as a bounce.
- `bounce_ledge_fall_height_boost 45`: falling off a ledge onto it without jumping adds a hop on the first bounce.
- Objects (cubes) bounce too and keep bouncing.
- Colour `bounce_paint_color 0 165 255`.

## Propulsion gel (orange, "speed paint")
- On it you accelerate to a much higher top speed: `sv_speed_paint_acceleration 500` u/s^2 = **12.7 m/s^2**, `sv_speed_paint_max 800`
  u/s = **20.3 m/s** (normal running `sv_speed_normal 175` = 4.4 m/s, so about 4.6x). On a ramp `sv_speed_paint_ramp_acceleration 1000`
  = 25.4 m/s^2. Sideways input counts half (`sv_speed_paint_side_move_factor 0.5`).
- Leaving it you keep the speed (into the air or onto normal ground, where normal friction slows you).
- Speed into bounce: `sv_speed_paint_on_bounce_deceleration_delay 0.2` s before slowing when going from speed paint onto bounce paint
  (the long jump combo). `speed_funnelling_enabled 1`: floor-portal funnelling also applies when running on speed paint.
- Colour `speed_paint_color 255 106 0`.

## Not used
Conversion (white, portal-able) gel: every Skate surface takes portals already. Cleansing (water) gel: our Clear in gel mode.

## Ours (implementation choices, PROGRESS 61)
- Gel mode (Ctrl+H / D-pad down + B): blue and orange shots paint a 1.75 m disc where the shot hits; new paint covers older paint of
  either colour on the same surface; 32 patches at most (the oldest goes). Clear (Ctrl+3 / middle click / Y) removes all paint.
- Bounce: restitution 1.0; floor minimum = the speed that reaches `skate3_portal_gel_bounce_height` (5.3 m) at the measured gravity
  (9.25 m/s at 8.07); wall minimum 3 m/s out plus at least 7 m/s up; 0.35 s between bounces; rolling onto blue paint always bounces
  (no 12.7 m/s auto-trigger speed: skating is slower than that and the bounce is the fun part). Held for skate3_portal_fling_frames
  updates by the FLING machinery.
- Speed: only while riding on floor-ish paint (|n.y| > 0.7), 12.7 m/s^2 along the travel direction up to `skate3_portal_gel_boost_cap`
  (20.3 m/s); no ramp bonus yet.
- On foot (state 500) gels do nothing (the board is not under the skater; see FLING).
- Props: blue paint reflects loose bodies with restitution 1 and no minimum (one lying on it stays put); never the skater's or a
  character's bodies.

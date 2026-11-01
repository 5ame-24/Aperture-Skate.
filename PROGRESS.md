# Progress and handoff (Skate 3 x Portal)

Read CLAUDE.md first (stance, build/run, architecture, findings, run table). This file is the resume point: what is
built, what is untested, what to do next, and how to analyse a run. Update it at the end of every working session.

Last updated: 2026-11-01 (section 51; sections 19-50 newer than the list below). Latest build compiles; the items under "Untested" have not been run.

## State of the product
Working (user-confirmed or measured):
- Portal gun: mouse reticle, wall/floor/ceiling placement through the game's ray service, pad control (hold D-pad down).
- Crossing keeps momentum: board reset replaced by SetTransform + skeleton restore (96% / 92% kept in runs 082/086).
- First person (mouse and pad), auto-centre on travel direction, board-or-rider follow option.
- Teleport between any pair, heading from the actual velocity, no bails in surgery runs.

Untested in the latest build (run it and report):
1. In-world portal polygons (spline pipeline, flat colour). Look for `portal shader` / PSO errors; the ImGui rings stand down
   when they draw. Screenshot of the portals from several angles and with the skater in front.
2. Endless fall: floor portal + ceiling portal, drop in. Fix made: floor/ceiling pairs use an 8 frame lockout (was 45), the
   ray cut-out is OFF by default. Expectation: fall, exit the ceiling portal moving down, re-enter, repeat.
3. Esc menu: Controls tab -> "Portal Gun" section: First-Person View, First-Person Camera Follows, No Bail (Boots).
4. No-bail (`skate3_portal_nobail`) WORKS ("way better", user-confirmed run 095). Wipeout bit 0x40000 is in the processed input
   `*(actor+1804)+2468`, set by the animation graph's "Wipeout" attribute; the real fix is hook 82DB9100 (requests_wipeout)
   returning 0 for the human, plus clearing the bit after ProcessData 82BD8918 and at selector 82DB6EC0 entry. Known leftover:
   it can still get into a "false falling, still riding" state (animation falls, game thinks riding, no control). User is
   happy; a full god mode may come later (find the animation request path: ground/air landing checks, CheckForGroundWipeout).
6. Endless tricks through floor/ceiling (run 095 analysis): crossings fired only once the skater touched the floor (floor trigger
   0.95 m vs riding height 0.8), so flags were C0000000 at every crossing, the fall was cushioned (~4.4 m/s instead of ~8) and
   landings ended tricks. Built, untested: floor portals trigger at 2.2 m (kFloorTrigger); exit offset stays 0.95 (kFlatTrigger).
   Check: crossing lines show flags 90000000 (airborne) and speeds rising; if the skater still lands, the next step is real
   body collision (item 3 below), or a bigger exit clear.
7. Opening collision (goal: cross at the real plane, half in/out, endless tricks): the board's world contacts come from
   the world-collision query: 8277B720 (world triangle vs moving primitive, called by 8277BC58 <- 8277BAC0/8277C5D8, calls fixup
   82AD3130). Probe hook built (untested): logs `WORLDQ r<N>=... triples in opening: +off=(x,y,z)` for the first 40 calls
   with float triples inside an open portal (cvar skate3_portal_probe). Next: use the offsets to read the triangle's vertices,
   then in the hook skip triangles inside the opening (write 0 to the output count at r6+16, return 0: UNVERIFIED return
   semantics), then move the trigger to the true plane (trigger 0, exit depth = entry depth).
8. Run 099 (WORLDQ dump) decoded the world query 8277B720 input: r4 = triangle record: vertex A at +0, normal at +16 (0,1,0 for a
   floor), vertex B at +32, vertex C at +48 (float x,y,z,pad, big-endian); +64/+80/+96 edge directions; +112 edge lengths; +192
   a far copy of a vertex; +208/+224 NaN-padded extras. r3 = moving primitive: position +0 (x,y,z,w), rotation rows +16/+32/+48.
   r9 was 1 in the dumps. UNVERIFIED: return value for "no contact" and what r6 (output manifest, count at +16) needs.
   Next: hook 8277B720, if the triangle's plane is within ~0.3 m of an open portal's plane and its polygon overlaps the opening
   rectangle (use the three vertices, not vertex-in-opening), skip it: write 0 to r6+16 and return 0; then move the trigger to
   the true plane and map entry depth to exit depth.
9. Missed crossings ("on a portal, not travelling"): a skater inside a floor opening below the 2.2 m trigger plane never fired
   (prev_d <= 0, only lateral entry counted). Fixed (untested): flat portals also cross when inside, on the front side and not
   moving away. One other miss: flag 0x00800000 blocks crossings ("not airborne (flags C0800000)"); unexplained.
10. Run 100 feedback: flip trick started one side and landed the other = the final-goal criterion, achieved. Issues: (a) session marker
   (L1 + D-pad down) clashed with portal mode (D-pad down): fixed, L1 held now disables portal mode (untested); (b) momentum dies on
   landing unless the user keeps pushing, 90 deg portals (wall<->floor) loop but give no fling, grabs do not persist through
   looping. Code facts: GateStep's `vin` and the speed log are HORIZONTAL only (vertical velocity invisible); actor +1248 (world
   velocity) is never turned and the air controller may re-derive velocity from it. Added `TRACE after crossing` log lines
   (every 6 frames for 96 frames: state, flags, y, speed, actorV, boardV). Next: read the trace, then test turning actor +1248
   alone (velocity only; the flicker came from positions/deltas) and check which state follows 700/702 in air.
11. Run 102 (user narrated, TRACE lines): roll-through with and without pushing KEEPS momentum (crossings 2-4: 3.4->5.6, 7.5->8.4,
   8.5->8.1 m/s over 1.6 s; crossing 5's stop was the user braking, pad B 0x2000). Crossing 1 lost it to a game recovery warp
   (state 700, flags 40000000, actorV garbage 30 m/s, board rates zeroed) ~0.3 s after the crossing. Built (untested): board
   reset replaced by SetTransform also for the human's recovery warps for 300 frames after a crossing (g_recover_window).
   Wall-to-floor glitch: crossing floor->wall exited at y 63.93 (below the floor at ~64.5) because the floor frame's u is the aim
   heading; fix (untested): wall exit entity y >= wall centre - 1.0. Jump + grab through a portal works ("not perfect").
   Not tested: session marker (L1 + D-pad down). Still open: fling from a wall into a floor portal was not exercised.
12. Run 103: session marker (L1 + D-pad down) WORKS. Floor<->high wall fling looped 60+ times then tunnelled under the map: after each
   exit boardV stayed (0,-30,0) (surgery SetTransform keeps rates but nothing turned the board block's rates; only the
   restored skeleton vectors were turned) so fall speed grew to 30 m/s (0.5 m/frame), last crossings were "ignored: not airborne
   (flags D0000000)", then a recovery warp with garbage actorV and the skater fell under the map. Built (untested):
   TurnAndCapBlock in the surgery path turns the block's 7 lin (+0x140) and 7 ang (+0x1B0) rates by the pair matrix and caps
   exit speed at 22 m/s (scale folded into g_restore_m so TurnRaw stays consistent). Log: "GATE exit speed capped". Check:
   boardV after a wall->floor crossing should point out of the floor portal; a floor<->ceiling fall should plateau near 22.
13. Run 104 feedback: no more under-map glitch; wall->floor fling felt like "spawned ~3 m above the floor portal each time"
   (repeated fall, board audibly touches the floor). BREAKTHROUGH ATTEMPT (built, untested): the portal HOLE. Hook on 8277B720
   (r3 mover, position at +0; r4 triangle: A at +0, normal at +16; r6 output, count at +16; caller takes the no-contact branch when
   r3 = 0): drop a contact when the triangle lies in a placed portal's plane (<0.4 m, |n.n| > 0.9) and the mover is over its opening
   (rect + 0.3 m, within 2.5 m of the plane). Virtual hole: triangles are up to 18 m wide so they are never skipped as a whole.
   cvar skate3_portal_gate_hole (default true). With the hole the crossing fires at the REAL plane (trigger 0), the exit is the other
   plane + the same depth, lift 0 (the 1 m lift and 2.2 m floor trigger are bypassed). Logs: "HOLE skipped a world contact (#N)".
   If no HOLE lines appear the wall is still solid through another path (wheel/deck probe lines? ray service?) and portals will not
   fire at the plane: run with --skate3_portal_gate_hole=false for the old behaviour, then look at the line-vs-triangle queries
   (skate-core board_probes.rs 82C07788/82C079E0, board_ground wheel lines).
14. Run 105 (hole ON): HOLE works (60 logged skips on wall and floor triangles; crossings fire at the real plane, e.g. y 63.9 on a
   floor at 63.99; speed cap 27 -> 22 logged). Falls through the map: the crossing fires with the skater already half through,
   the game flags a recovery (40000000 / 48000000 / 4E000000) and `allowed` refused ("ignored it: not airborne"), a 30-frame
   cooldown followed, the standing-in-opening rescue only covered 0.5 m below the plane -> fell through the open hole forever.
   Fixed (built, untested): with the hole `allowed` = not mid-relocation only; rescue covers 3 m below the plane; a refused
   crossing starts no cooldown. If it still falls: consider a catch (if y is far below both planes, warp to the last crossing exit).
15. Run 111 (hole on, long narrated run + 3 screenshots): wall<->wall same-facing pair, airborne crossing: user exits "the back of the
   blue portal", wall-rides, glitches under the map; camera/green dot left at the old portal after a crossing. SCAN proves why:
   the board block's rates (+0x160/+0x170/+0x190) ARE turned by the surgery (-5.7,2.0,-1.1) but 3 frames later they are back to
   (+6.5,1.5,+1.2): block+0x3FC holds a velocity copy ((5.69,-0.53,0.81), unchanged through the teleport) that the game re-applies.
   Built (untested): TurnAndCapBlock also turns block+0x3FC; GateStep sets its tracked/last position to the exit position after a
   crossing (the airborne tracker only follows within 2.5 m, so it stayed at the old portal and produced fake 34 m/s speeds).
   Also seen, not addressed: black screen after defocus/refocus and the language screen sometimes not showing at launch (maybe the
   mod's overlay/render hook; check if it happens with --skate3_portal_gate=false); floor-roll-in still low exit momentum;
   first crossing often yanks back (recovery warp).
5. Run 093 user report: floor+ceiling loop is better but collisions stop endless kickflips in the portal; in-world portal
   polygons look right; Esc menu Portal Gun section works.

## Findings that changed direction (do not relearn)
- Exit distance, lift, airmode, hold-for-riding-state, board-rate heading, actor-field writes: none keep speed (or they
  cause the flicker). Only replacing the board reset (82C05F50 -> SetTransform 82C0B2C8) plus restoring skeleton vectors works.
- Ray cut-out (hook on 8276EE88): 13,000 floor hits dropped and the skater still stood on the portal. Body collision is not
  ray based; the missing foot-test hits probably set the recovery flag (40000000) that made crossings "not airborne". So the
  wall/floor is solid to the physics bodies; making it non-solid needs the body collision (not found yet).
- Endless fall failed in run 092 because: the ceiling exit is only ~2.5 m above the floor, the 0.75 s lockout outlasted the
  fall, and the cut-out set the recovery flag. Fixed in code, not yet verified.
- New HLSL cannot be compiled on this Mac (precompiled SPIR-V header built with Windows DXC). Reuse existing pipelines.

## Next actions (in order)
1. Run the latest build: portal polygons, endless fall, Esc menu rows, no-bail log. Collect the log + screenshots.
2. If no-bail does nothing: find the real wipeout flag owner (candidates found by scanning generated code for readers of
   offset 2468: 82DB6EC0 (this = r3, 17 reads), 82BFA4B0, 82D8ADE8, 82BD83E0, 82BF0680) and clear the bit where it is set
   or read; also look for the landing-angle threshold constants.
3. Body collision for a real "walk into the portal" opening (physics world, not the ray service). Start from how the board
   block / skeleton bodies are registered; look at what 82C06ED8 / 82C090C0 / 82C091F8 (executor tail) do.
4. See-through portal view: second camera pass or render-to-texture; the spline pipeline proves injection works.
5. Velocity turning for angled portals and flings (turn the restored skeleton vectors and the board block rates).
6. Portals acting on props and NPCs (their actors need the same atomic move-with-rates treatment).
7. Cleanup: probe dead code DONE (section 51). Left: SDK imgui pin notes (SDK repo), Portal 2 staging folders (sigf/,
   portal2-skate-app/ in the Steam Portal 2 dir, not in this repo).
Ideas parked: Chell-style boots are the no-bail mode above; bringing a Portal character model in would be a model-format
reverse-engineering plus skeleton retarget project (the user's own Portal 2 install is the source); GTA-like drivable cars.

## How to analyse a run (no game needed)
- `python3 tools/re/crossings.py [log]`: per-crossing kept-speed table and totals (newest log by default).
- `python3 tools/re/callees.py generated <ADDR...>` and `callers.py generated <depth> <ADDR...>`: call graph of the generated
  guest code (functions are `sub_ADDR` in generated/skate3_recomp.N.cpp; indirect calls through tables are not shown).
- Useful log patterns: `GATE crossing`, `GATE speed after crossing`, `GATE board reset replaced`, `portal on a`,
  `CUTOUT`, `NOBAIL`, `RAY SERVICE`, `pad buttons`, `portal shader`.
- Reference reading (understanding only): ~/Documents/rust-mashup/skate-engine/crates/skate-core (board_runtime.rs,
  player/teleport_state.rs, player/selector/*, physics/skeleton_*).

## Key addresses
Executor 82DB8998, update 82DB88C8 (self = actor), board reset 82C05F50, SetTransform 82C0B2C8 / 82C055F0, skeleton reset
82BE3508, ProcessData 82BD8918, skeleton teleport stage 82BDF7C0, ray Start/End 82DB63C0 / 82DB6580, ray queue 82E0B4E0.
Ray service: object 402ECC40 (composite of child services), vtable 82310764, queue 8276EDD0 (+28), results 8276EE88 (+32).
Actor offsets: +1200/+1216 ground pos, +1232 delta, +1248 velocity, +1296 flags, +1336 state, +1880 board, +1884 skeleton,
+1908 ray manager, +2468 flag word (wipeout bit 0x40000, unverified). Skeleton +0x2770..0x2900 velocity vectors.

16. Run 113 (wall<->wall same-facing, 15:39-15:40): exit vel IS turned (in (4.7,0,2.1) -> out (-4.7,0,-2.1)) and block +0x140.. is turned post-teleport,
    but 3 frames later the rates are back at (+6.75,-1.61,+2.61) = into the exit wall; hole then lets the skater through the wall
    and off the roof (y 82 -> 67). The unturned carriers MOVE between runs: run 111 block+0x3FC, run 113 actor +0x6B0..0x770
    (5 samples, stride ~0x30) and block +0x3E4 (history/ring copies). Fix (untested): TurnVelocityCopies() sweeps actor+0x600..0x800
    and block+0x300..0x500 and turns any vector parallel (cos>0.8) and similar in size (0.5-1.6x) to the entry velocity; logs
    "GATE turned N velocity copies". Check next run: block +0x150 3 frames later should stay turned; if not, N and the SCAN list say where.
    Also: walkers standing in a floor portal now trigger (standing rule uses kFlatTrigger in hole mode); flat exits come out
    kFlatTrigger higher (feet at the floor plane, not 0.9 m below). Not addressed: dropped board, NPCs (skaters bounce out,
    walkers ignore floor portals), ollie-into-floor-portal momentum loss on landing (may be the same copies bug).
    User note (run 113): movable props drop through the floor at a floor portal instead of crossing. Cause: HoleSkips in the 8277B720 hook
    skips contacts for ANY mover over the opening. Plan: (1) limit the skip to the human skater + board (mover identity in r3), (2) later,
    crossing test + teleport for props/dropped board/NPCs (needs actor discovery, unexplored).
    Step 1 done (untested): HoleSkips only for movers within 3 m of the tracked skater (g_tracked). Props/NPCs/far dropped board keep a solid floor.
    Step 2 (props, dropped board, NPCs actually crossing) is the agreed next progression after the main mechanic is polished.
    CRASH run 118 ("call to invalid guest address 0" right after the first crossing, 5 copies turned): the copy sweep rewrote pointer-shaped
    words (guest heap pointers 0x4xxxxxxx read as floats 3..12000). Guard added: skip any triple with a word where (w&3)==0 and top nibble is 4.
    Each rewrite is logged ("GATE turned copy at ADDR"); if it still crashes, the last such lines name the culprit, or drop the sweep and
    turn only the offsets the SCAN names.
    Run 119: still crashed (floor>ceiling) with 16 copies rewritten, incl. non-velocity vectors like (0,0,2.97) at block 0x46943858. SWEEP REMOVED.
    Re-read the run-113 SCAN: actor +0x6B0.. and block +0x3E4 were IDENTICAL post-teleport and 3 frames later (the game never rewrote them),
    so they are not what reverts the rates; the carrier is still unknown. Added per-frame "FRAME +k" log (k=0..8: state, flags, block rate0/rate1,
    actorV +1248) to see which frame flips the turned rates. Walker floor trigger, flat-exit height and step-1 hole limit are still in.
    Run 120 FRAME log (wall<->wall same-facing, ~8 crossings): rates are turned correctly at +0, then at +1/+2 they are replaced: x (wall
    normal axis) is cancelled, z kept, or flipped to the entry direction; actorV (+1248) stays frozen at the pre-crossing velocity for ~3 frames
    then decays (real speed is lost, 7.7 -> 4.8 in 8 frames). Floor exits (wall->floor, up 3.9): vertical rate gone by +1 too.
    Hypothesis: a probe other than 8277B720 (wall-ride / ground snap) sees the surface at the exit point (exit at plane+0.02) and flattens velocity.
    Changes: wall exits now come out cvar skate3_portal_gate_wall_exit = 0.6 m in front of the plane (A/B: 0 = old); flat-exit raise reverted
    (floor roll-out worked in run 113 without it). Walker floor trigger kept. If +1 still flattens at 0.6 m, hook the wall-ride/ground probes.
    Direction change (user): identical portal behaviour, exit ON the exit plane (wall_exit cvar default back to 0). New hook sub_82ADF5D8
    (triangle-segment dispatcher: r3 result, r4 start, r5 dir, r6..r8 vertices, returns hit; used by board wheel/deck/wall probes + camera sweeps;
    skate-core triangle_query.rs / board_probes.rs): skipped via HoleSkips with the probe start as mover. Log "HOLE skipped a line probe".
    Check next run: FRAME +1/+2 rates should stay turned. If still flattened, the culprit is neither 8277B720 nor 82ADF5D8 (next: 82C01F10 wall-line producer, 82C07788/82C079E0 deck+wall probes).
17. Run 121 (line-probe hook in): facing-each-other walls (matrix change 0.14) keep speed 6.6->6.6 and rates stay turned. Same-wall pair (change 2.0):
    FRAME +0 turned, +1 flipped to entry direction = direction of actorV (+1248, frozen at pre-crossing value ~3 frames). Fix (untested): TurnAndCapBlock
    also turns actor+1248 ("GATE actor velocity turned"). Other glitch: crossing in state 500 (off board) -> rates (-406,145) next frame, speed 451 m/s, recovery
    warp: board assembly explodes when SetTransform'd while the skater is bailed; to handle (skip surgery or zero rates in state 500).
18. Run 122 (facing walls only, matrix change 0.14): speed kept in nearly every crossing (rates stay turned all frames; actor+1248 turn did no harm).
    3 of ~25 crossings EXPLODED at FRAME +1 (rates ~(-592,-203), (853,331), (845,304) m/s): two in state 500 (off board), one state 200 with actorV 10.2 vs
    measured 4.3 (transitional). Not edge-of-opening (lat 0.17-1.0 m, successes similar). The recovery warps then re-applied the huge rates in a loop
    (8 warps in 0.4 s). Fixes (untested): state 500 -> plain reset (no surgery); recovery warp with any rate > 33 m/s -> plain reset. Same-wall pair (180 deg) NOT retested yet.

## 19. See-through portals (built, untested) + speed cap raised
- User: "try a see thru render" and "dont worry about crazy mad speeds, thats the portal energy". `kMaxExitSpeed` 22 -> 80 m/s
  (RatesInsane = 1.5x = 120 m/s still catches the 600-850 m/s blow-ups).
- Render design (no stencil in the RHI, no shader compiler on this Mac, so no new HLSL): in `RenderScene`, just before the
  real opaque draw loop, for each placed portal that is on screen and has the camera in front: `portal_gate::ViewThrough`
  gives T^-1 / T / exit plane; `PortalViewProj` = T^-1 * view_proj with an oblique near plane (exit plane); `draw_item` now
  reads `view_vp` / `view_cam` / `virtual_pass` (SSR capture skipped); opaque items drawn into the MSAA target, resolved
  into `g_r.portal_view[i]` (hdr scene format), then the frame is cleared and drawn as normal. The portal's core polygon is
  then drawn textured with `pso_portal_view` (spline ps_darken non-HDR blob, blend off), vertices pre-projected on the CPU
  (identity matrix, w = 1, screen-space uv) so there is no perspective error; depth = the polygon's, so occluders hide it.
- cvar `skate3_portal_gate_seethru` (default true, hot reload). Log: "portal see-through pass N (items)".
- Known limits: only items in the game's draw list (it culls by the real camera, so content behind/around the exit may be
  missing), transparents/water not drawn in the view, portals not visible inside portals, shadows/fog from the real camera,
  needs MSAA + HDR (defaults). Cost: one extra opaque item pass per visible portal.

## 20. Angled portals (built, untested)
- User: see-through "pretty good, a decent leap"; next: angled portals. Crossing, velocity turn (3D matrix), exit position, hole
  (triangle normal vs gate normal > 0.9) and the see-through camera were already general in the (n, u, r) frame; only
  `PlaceOn` limited it (slopes rejected, normals snapped to vertical/horizontal). Now: portal lies on the true normal, snapped
  to axis only within 6 degrees (`kSnap`); `kFlatNy` = 0.7 splits "floor-ish" (u = shot heading projected on the plane) from
  "wall-ish" (u = world up projected); `Flat()` uses the same 0.7. Log line says "(angled)" when off-axis.
- Not checked: exit onto a slope keeps the skater upright (yaw-only target matrix); steep slopes between 45 and 60 degrees are
  treated as walls; curved surfaces give a portal per-triangle normal (may look slightly off).

## 21. Portal clones + frames inside views (built, untested)
- `portal_gate::PortalFrame(from, ...)` = frame of a portal + pair transform (shared `PairMatrices` with `ViewThrough`).
  cvar `skate3_portal_gate_clones` (default true).
- Renderer `draw_clones(from)`: every opaque `dyn_entity` item (skater, board, NPCs, props) whose centre is within the
  opening box of portal `from` (depth in (-4, 1.2] m, |lat| < 4, |vert| < 3.5; centre = bone 0 translation for skinned items,
  bbox centre * world otherwise) is drawn again with `world * fwd` (`world_override`), max 48 per call. Real pass: both
  directions, after the opaque loop, unclipped (wall + portal polygon hide what lies behind the exit plane). Virtual
  (see-through) pass: clones through the portal being looked through, so the far side shows the emerging body.
- The looked-through portal's own flat frame is drawn inside the virtual view when the virtual camera is in front of it
  (facing pairs see each other). No recursion (portal views do not contain portal views).
- Visual only: props/NPCs still have no physics crossing (step 2).
- Not verified: bone-0 translation as the skinned item centre (bones assumed 3 float4 rows [R|t]); clone cost with many NPCs;
  clone of the board/skater at the exit while the camera sits behind the exit wall.

## 22. Props/loose bodies cross, oval portals, transparents in views (built, untested)
- **Loose bodies** (`skate3_portal_gate_props`, default true): hook `sub_82AE6590` (RigidBody::DynamicUpdate; r3 = body; quaternion
  +0, position +16, linear vel +32, angular vel +48, basis columns +64/+80/+96, all big-endian floats). After the original runs,
  `portal_gate::BodyCross` tests the body against both portals: centre within 0.7 m in front of the plane (or <= 1.5 m behind),
  inside the oval (+0.2), moving into it (> 0.5 m/s), and > 2 m from the tracked skater. Then position is mapped
  (`out_dist = 0.7 + (0.7 - d)`), velocity/angular velocity rotated by the pair matrix, basis columns rotated and the quaternion
  rebuilt (only if the stored basis and quaternion agree, else orientation left alone: look for "basis and quaternion
  disagree"). NO hole for props: the surface stays solid, so nothing can fall into the void. Log: "portal-props: body ... crossed".
  Unknown: whether the dropped board / NPC skater parts / ragdolls go through this function (skater-near bodies are excluded).
- **Ovals**: portals are ellipses (28-point rim/core in `SurfaceShapes`, `InOval` in the crossing, hole and opening tests).
- **Views** now include blended items (water, glass, fences) back to front from the virtual camera.
- Still open: NPC skaters crossing (need per-actor crossing state), camera continuity through a portal (chase camera lags
  and swings), portal-in-portal recursion, skater body tilt on angled exits, the older untested items (same-wall 180 case,
  off-board crossing, ollie into floor portal momentum, grabs through loops).

## 23. Run 126 (floor<->floor endless fall, first test of items 19-22) and fixes
- Floor/floor pair, ~140 crossings, no crash. See-through and clones rendered (screenshot: sky seen in the blue portal, a clone
  of the skater inside it). Glitches: (1) `portal-props` crossed 5 bodies (474A8A40..9F90) at the skater's position in the same
  frame as the skater's own crossing: they are the skater's board/skeleton rigid bodies. `g_tracked` still held the pre-teleport
  position so the 2 m exclusion missed them; double-teleported bodies then blew up (body 474A93E0 v = 500 m/s at 17:21:24.9) and
  from 17:21:25.2 on, crossings hit "exit speed capped 120-350 -> 80". (2) A portal seen from behind / from inside the floor
  drew as a flat dark disc filling the screen.
- Fixes: `BodyCross` now takes the body id and keeps a registry of bodies seen within 3 m of the skater (label lasts 10 s), so the
  skater's parts are never treated as props. The renderer skips a portal whose front the camera is behind (SurfaceShape now
  carries centre + normal). Untested.
- User wants to test ONE thing per run from here and have a general play; analyse logs for what happened.

## 24. Run 127: the flicker/warp glitch was the prop hook again (fixed, untested)
- Run 127 (angled floor + walls): at each skater crossing a batch of 4-15 bodies at the skater's EXIT point crossed back through the
  other portal one frame later ("portal-props: body 474A96A0.. crossed", e.g. 17:34:30.668), and at 17:34:30.701 a second batch with
  v ~ 100 m/s (the visible warp/flicker). They are the skater's skeleton bodies: the game's teleport relocates them to the exit, so the
  "seen within 3 m of the skater before" label (item 23) never covered them.
- Fix: `Step` records the crossing's entry and exit points; `BodyCross` refuses any body within 5 m of either for 4 s (and labels it).
  Next test: props only. Expect `portal-props` lines only for bodies away from the skater, and no "exit speed capped" /
  100 m/s bodies.

## 25. Run 128 props test: stuck and lost props (isolation filter + follow-up logging, untested)
- Evidence: bodies 474ACF00/ACFB0/AD060 (176-byte bodies, 0xB0 apart) "crossed" the floor portal every 33 ms from the SAME entry
  position (107.79,63.82,-240.55) with nearly constant velocity: the write is undone each frame. A cluster of ~10 bodies
  (474AD110..AE450) crossed with velocities up to 230 m/s -> props that "fall through the map and respawn" (the game's own respawn).
  Working theory: these are jointed assemblies (ragdoll / multi-part objects); teleporting one part gets it dragged back by its joints
  (stuck) or fights them (explosion). Alternative theories still open: the owner entity re-syncs the body's transform each frame.
- Change: `BodyThroughPortal` keeps every body's last position (`g_bodies_seen`) and only crosses a body with no other body within
  2 m seen in the last 0.3 s; assemblies log "reached a portal but has neighbours". After a crossing the body is followed for 4 calls and
  logged ("after crossing ... m from where it was put"): if that distance is large the write is being reverted by something else
  (then look for the owner's transform), if small the body really moved.

## 26. Run 129: nothing crossed (clusters), cluster teleport + falling in (built, untested)
- Run 129 (orange floor + blue wall, one object pushed around): no `portal-props ... crossed`; ten "reached a portal but has neighbours
  (assembly), left alone" lines for bodies 474B3D00, B3DB0, B6010, B71F0, B76C0 at (107.4-108.9, 63.8-64.6, -240.1..-240.4) next to the
  orange floor portal (107.31, 63.91, -239.20): the pushed "single object" is a ~10-body cluster, so the isolation filter blocked it.
- Change: instead of refusing, the whole cluster (bodies seen in the last 0.3 s, linked within 1.5 m, max 64) moves rigidly: positions
  about the triggering body (np + R (p - p_ref)), velocities / angular velocities / basis + quaternion rotated by R, each body's speed
  capped at 60 m/s; refused if any member is a skater body; moved bodies get a 0.6 s cooldown. Log: "body X + N connected crossed",
  then "after crossing ... m from where it was put" for the trigger body for 4 updates (a large distance = something else reverts the write).
- Floor portals: a body in the oval that is resting on it (velocity into the floor > 0.05 m/s, i.e. any tiny downward component) now falls
  in; other portals still need > 0.5 m/s into the plane. Sleeping bodies are never integrated, so a prop asleep on a portal stays.

## 27. Run 131: groups cross and the writes stick; exit clearance fix (built, untested)
- Run 131 (orange floor (102.24,63.97,-237.88) + blue wall (122.04,67.83,-249.32)): 6 "body X + N connected crossed" events with N = 25-32
  (NPC skaters: skeleton + board bodies; user: "a few skater npcs fell through, reacted a lot better"). Follow-up lines: the trigger body ends
  0.14-0.52 m from where it was put after 4 updates and keeps moving, so the writes are NOT reverted (the "owner re-syncs" theory is out).
  Groups cross back and forth (wall exit 17:52:37.9, back through the wall 38.66, 39.28, 39.89) with exit velocities up to 8-13 m/s.
- Fix: `BodyCross` also returns the exit plane; before moving a group, the member that would land lowest behind the plane is found and the
  whole group is lifted along the exit normal until it clears by 0.35 m (max 3 m). Untested. User did not say what is still wrong: ask.

## 28. Bin report (run 131 / the bin never falls after exiting, hangs halfway, respawns) - diagnostics (built, untested)
- User: the bin, pushed repeatedly: first time caught halfway, then "fell" parallel to the ground, then respawned; other times it vanished and
  reappeared. (A floor->wall exit leaves the object horizontal, bottom first: that is right, as in Portal. Not falling and respawning are not.)
- Log 131 has only 25-33 body groups; the same groups reappear as new bodies (474BFB70/BFA10 -> 474C16F0 -> 474C3480) at the same spot
  (103.8,64.2,-240.2) with v ~(-8.8,-2.6,-0.9): the respawns. Follow-up lines at the wall exit (120.8,67.6,-252.1): y stays 67.85-67.89 over four
  updates while v_y swings 13.5, 2.5, -0.1, -1.1 and positions barely move (0.3-0.5 m) -> held in the air, not falling (unknown why: contact
  with something near the exit, joints, or sleep/kill logic).
- Change: link radius 1.5 -> 0.8 m (smaller groups, fewer unrelated bodies carried along); after a group moves, `GroupTrack` logs one line per
  6 updates for 12 lines: member count, centre, y range, extent, mean speed ("portal-props: group of N (lead ..) t+K"). Read it for: falling
  (centre y dropping), floating (y flat), break-up (extent growing), respawn (a new group at the default spot).

## 29. Companion cube (built, untested)
- User: wants a companion cube to push around as the test object. A new game body is out of reach, so the cube is ours: `src/skate3_portal_cube.{h,cpp}`
  (physics on the game thread, called from `GateStep` for the human each frame) + drawing in `RenderScene`.
- Spawn: Ctrl+5 or D-pad down + X (X removed from the game's view in portal mode); appears 2.4 m ahead of the skater (away from the camera) and falls.
  Spawn again to bring it back. Falls out of the world (60 m below the lowest ground seen / spawn) -> back to the spawn point.
- Physics (`kHalf` 0.45 m, gravity 16): ground and wall from the game's ray service, one ray per frame (down on even frames, along the horizontal velocity
  on odd ones when moving) submitted in the same Start/End batch as the gun's shot (`CubeRay`, scratch 128 bytes below the gun's). A hit inside an open
  portal's opening is ignored, so the cube sinks through the plane like the skater and crosses exactly: `portal_gate::CubeCross` (centre plane crossing,
  position out at -d + 0.02, velocity and axes rotated by the pair matrix). The skater pushes it (circle in xz, relative approach speed). On the ground it
  damps and settles flat (no tumbling/rolling). The skater is not affected by the cube.
- Look: three layers per visible face (grey border, dark panel, pink heart), 4 shade levels from a fixed sun, soft blob shadow, all through the spline
  darken pipeline with 1x1 colour textures (`g_r.cube_tex[13]`). Also drawn in the see-through views and as a clone when partly through a portal.
- Log: "portal-cube: spawned / crossed / fell out of the world". Unknown: the ray service answering two rays per frame (look for the cube floating or
  sinking: no ground), whether Ctrl+5 is free, whether the pad X bit (0x4000) matches.

## 30. Carry the cube (built, untested)
- User: the cube behaves "perfectly", but cannot be grabbed like the game's own props; wants grab as a portal-gun mechanic.
- Grab/drop toggle: Ctrl+G, or D-pad down + R1 (RB bit 0x0200, removed from the game's view in portal mode). Pick-up needs the cube within 3.6 m of the skater.
  Held: a spring (gain 14, max 30 m/s) toward a point 1.6 m ahead of the skater along the camera-to-skater heading, 0.05 m below the entity centre, no gravity,
  settles flat, no skater push. Skater teleport (> 3 m jump in a frame) snaps the cube to the new hand point (carried through portals with the skater);
  more than 4.5 m from the hand lets it go ("let go, it is N m from the hand"). Drop keeps the skater's horizontal velocity (a throw).
- Logs: "portal-cube: picked up / dropped / too far to grab / let go". Only the cube can be carried: game props are not touched (their grab, and
  the 25-33 body groups, are separate unknowns).

## 31. Carry the game's own objects; body writes moved to the game thread (built, untested)
- User: "but with all skate moveable objects too?" (grab for everything, not only the cube).
- FOUND: the game integrates rigid bodies on several worker threads at once (the hook's log lines carry 4-5 different thread ids within one
  millisecond). Item 26-28 moved other bodies from inside the 82AE6590 hook, racing those threads - a possible cause of the bin floating / respawning.
  Now the hook only RECORDS (`RecordBody`: position, velocity, time, `near_frames` under `g_body_mutex`); all writes happen in `BodiesFrame`, called every
  frame from `GateStep` (game thread, workers idle): crossings (`CrossGroup`, same rules as before, judged on the recorded values then re-read) and carrying.
- Carry: the same bind as the cube (Ctrl+G / D-pad down + R1): drops what is held (cube first, then game objects), else picks the nearest of {cube, an
  awake game object within 3.6 m (xz) whose group is not the skater's}. A group = bodies linked within 0.8 m; its centre is held 1.9 m ahead of the skater
  (camera heading), each body gliding with a spring (k = 12/s), its velocity set to match, angular velocity zeroed; > 5 m from its place (the skater went
  through a portal) it snaps. Drop = every body gets the skater's horizontal velocity + 1 m/s up (a throw).
- Skater's own bodies: `near_frames` (updates within 1.5 m of the skater, decays 3 per update away) >= 150 counts as the skater's (never picked or crossed).
- Limits: only bodies the integrator updates are known, so a prop asleep on the ground cannot be picked until something wakes it (a nudge); NPC groups
  can be picked too; orientation is not turned by a portal while carried.
- Logs: "portal-props: picked up N bodies (lead ..)", "dropped N bodies", "nothing awake to grab within 3.6 m (N bodies recorded)",
  "that object is connected to the skater's own bodies". Existing "connected crossed" and "group of N .. t+K" lines still apply.

## 32. Run 137 (carry test) and fixes (built, untested)
- User: (screenshot) the carried cube is overlapped by an orange portal oval drawn over it; carrying should follow the crosshair like Portal;
  grabbed an NPC skater but never a native object, even awake ones; did not try dropping anything through portals.
- Render: the cube wrote no depth, so portal polygons (drawn later) painted over it. New `pso_spline_zwrite` (spline darken, depth write ON, straight alpha);
  cube faces use it, the shadow disc does not.
- Carry: the held point is now under the crosshair: along the camera aim ray (`AimRay`, mouse reticle), at the far intersection with a 3 m sphere around the
  skater (never below skater y - 0.6); no aim ray -> 1.9 m ahead on the camera heading. Pick-up chooses what is closest to the aim ray (perp distance + 0.15 x
  distance to the skater, within 1.8 m of the ray and 5 m of the skater), cube and game objects scored the same way.
- Grabbing game objects: freshness window 0.3 s -> 3 s (a nudged prop sleeps within a second and the hook stops recording it), prune 6 s. When nothing
  qualifies the log lists the three nearest recorded bodies (distance, age, near_frames). Log 137: "nothing awake to grab (211-252 bodies recorded)", then
  "picked up 7 bodies (lead 474A8AF0)" = an NPC skater, later "picked up 5 bodies (lead 474AA510)". Still unknown whether native props are integrated by
  82AE6590 at all or only appear while awake; also writing a sleeping body may not wake or move its visible entity.
- Pull-back finding (log 137): a 5-body native group (extent 0.4 m, near the floor portal at (155.2,64.0,-187.4)) crossed four times to the wall portal
  (exit (167.3,86.8,-180.6), v (0.3,-1.8,-0.4)); each time the whole group glided in a straight line back toward where it started at "mean speed 60.0"
  (the body speed cap) for ~24 updates and arrived with its old velocity. So the game restores/pulls a moved prop toward its previous state (cause unknown:
  stale contact/friction anchors, a previous-transform array indexed by body id, or a home servo). Writes to the body itself are not reverted (item 27).
- Attempted workaround: after a crossing the group is "in flight" for 0.7 s; each frame a body that is farther than 0.8 m from its own ballistic path and
  closer to its old position than the path is, is put back on the path (gravity 14). Log: "bodies were being pulled back, put back on their path". If the pull
  resumes after 0.7 s the real source has to be found (candidates: scan guest memory for copies of the pre-crossing position).

## 33. Run 138 (carry test) and pad-only aiming + on-screen clock (built, untested)
- Run 138 (19:12-19:23): 9 pick-ups ("picked up 1 bodies" x6 = single native props, y 64.22 near (106,-234), (108,-230), (120,-251), (99,-241);
  33 and 26 bodies = NPC skaters), 19 drops, 21 skater crossings, 0 prop crossings ("connected crossed" 0, "pulled back" 0), 0 cube crossings, 22 "nothing under the
  crosshair" (the skater's own bodies listed with own = 1664-2086 near_frames, so the own-body test works). The bench that went through the portals smoothly
  was carried: the skater crossed and the held group snapped to the new hand point. The 0.7 s pull-back workaround (item 32) is still untested.
- User: carry works well; wants the mouse/keyboard portal pointer gone (it interferes) but the mouse reticle is what made carrying the bench possible.
  Pad-only aiming: `skate3_portal_gate_mouse` now defaults to false; while portal mode is held (D-pad down) and not in first person, the right stick moves the reticle
  (1.4 NDC/s, dead zone 0.2, stored in g_pad_x/g_pad_y, starts at (0, 0.1)) and is removed from the game's view (no flicks); the overlay draws the reticle/gun at that
  point and shots, carrying and pick-up use it exactly like the mouse reticle. Keyboard binds (Ctrl+1/2/3/4/5/G) are left in place.
- Timestamps: screenshots carry none (the three pasted today all have file time 19:25:29 = paste time). Log lines are "[YYYY-MM-DD HH:MM:SS.mmm]" local time.
  New `skate3_portal_gate_clock` (default true): the same local clock to the millisecond drawn in yellow at the top right of the game view.

## 34. Run 138: a dropped bin never crossed (fixed, untested)
- User: dropped a bin into a portal on the last run and nothing happened. Cause: `BodyCross` refused any body within 3 m of the tracked skater and kept a
  10 s "skater body" label on it, so a bin carried at the hand point (3 m) or dropped beside the skater could never cross (log: portals placed, 19 drops with
  v (0, 0), no "connected crossed" line, 0 "pulled back"). The own-body test is now the probe's `near_frames >= 150` (updates within 1.5 m of the skater; the log
  showed own = 1664-2086 for the skater's bodies), plus the post-crossing zone (5 m, 4 s). `SkaterBodyLocked` only does the zone now.
- Pasted screenshots are renamed by the chat (11.png ... 15.png) so the original "Screenshot ... at HH.MM.SS" names are not available; the on-screen clock replaces them.

35. Falls through the map (runs 142-144), all untested:
    - Props: re-cross cooldown 0.6 -> 0.25 s (a floor-to-ceiling fall is 0.55 s); the 3 m "near the skater" floor hole no longer
      applies to loose props (`SetPropPositions`); the 0.7 s pull-back re-assertion only fires for a fast (>30 m/s) drag toward
      home (it was overwriting real floor landings); `PropRescue`: a crossed prop >3 m below the lower portal is set down beside
      it at rest (log: "fell out of the world").
    - Skater: `kFlatCooldownFrames` 8 -> 2 (run 143: the endless fall passed 44-57 m/s, reached the floor inside the lockout and fell to y=-11).
    - Not done: skater rescue if it still falls out; trick grab dropping on crossing (not investigated).
    - Run 145: the rescue also hit the skater's own skeleton (820 times at the spillway, standing under a wall portal): now excludes own bodies/clusters and allows 1.7 m more for wall portals.
36. Step 1 of the "make it seamless" plan (built, untested): portal-aware collision for props. `HoleSkips` matches the mover to the nearest loose
    prop (probe publishes position+velocity via `SetPropPositions`, 30 m around the skater); a prop ENTERING an open portal (flat: v.n <= 0.3,
    wall: v.n < -0.5, inside the oval +0.5, within 2.5 m of the plane) gets the same hole as the skater, otherwise a solid surface. Prop trigger
    0.7 -> 0.25 m so the swap happens at the real plane. Movers within 0.8 m of the tracked skater keep the skater rules.
    Watch: props falling through at the rim, `fell out of the world` lines, pops at the swap. Old cooldown/pull-back/rescue patches still in.
37. Scripted self-test (built, untested): Ctrl+6 with both portals placed takes the nearest loose prop (<15 m, not the skater's) and runs 3 scenarios of
    6 s each: centre drop, off-centre drop (0.8 m), toss from 3.5 m at 8 m/s (floor portal first, else a wall portal with throws). Logs
    `portal-test: scenario N PASS/FAIL: crossings, respawn jumps, rescued, under the map, max speed, end position` and `N/3 scenarios passed`.
    PASS = >= 1 crossing, no >4 m jump without a crossing (game respawn), no rescue, never far below the lower portal.
    Run 149: the test ran (3/3 pass) but on the skater's own board cluster (+6): a board lying idle lost near_frames when its entry expired. Fixed (untested):
    `g_own_ids` remembers any body that reached 600 updates beside the skater; props, test, grab, crossing and rescue all skip them and their clusters.
    The rescue also looped 190 times on one body (the game pulls a moved body back to its own spot, y 61 vs portals 64.6): 3 tries per body, 3 s apart.
    Open: why the game returns some moved bodies to their own spot (pre-crossing "home") is still unexplained.
    Run 150 (test on real props): several scenarios "froze" at the start spot with speed 0 (sleeping bodies are never integrated, so writes do not move
    them), and others ran away at the 60 m/s cap (0.7+ s after being placed) toward x 108..144: the game drags moved bodies somewhere (home?).
    Test now: picks only props updated in the last 0.15 s, marks "INVALID ... asleep" (not scored) when the prop never moves, and traces the path every
    0.5 s with the distance from where it lay before. Waking a sleeping body needs the game's own active-list call (skate-core notes: state_flags & 7 == 2
    is sleeping, 82ADF7B8 wakes it) - not done.
38. Self-test now runs a queue (built, untested): Ctrl+6 tests up to 5 awake props within 15 m (one entry per object), 3 scenarios of 4.5 s each, and logs per prop a
    `prop N of M: body, cluster of K, inverse mass, raw: <44 big-endian words of the 176-byte body>` line before moving it, a `RESULT prop ...: a of b passed,
    dragged off by the game in c` line after (dragged = max speed >= 45 m/s with no crossing), and `all N props done`. Goal: find what the dragged
    (home-pulled) bodies share (inverse mass, raw words) by diffing the raw lines of dragged vs fine props.
39. Waking sleeping props (built, untested): `WakeBody` mirrors the game's wake 82ADF7B8 on the body (state at +140 low 3 bits == 2 is asleep; unlink from
    the sleeping list via +44/+60, push on the active list at *(sim+40), state = 4 | (state & 8), sim counts at +68/+72, cool_down +172 = 0; sim = *(body+76)).
    `WakeNearby` scans the simulation object (+0..0x200) for circular lists of nodes and wakes the asleep bodies within 15 m of the skater (not own ids); logs
    `portal-wake: list at simulation+N ...` per list found and `woke N bodies`. Ctrl+7 does it alone; Ctrl+6 does it first and builds the test queue 0.4 s later.
    Spawning NEW objects is not done (needs the game's object factory). Risk: runs on the game thread while physics workers may be iterating the lists.
    Run 153 (first live wake): the lists were found. simulation+36 = SLEEPING list (21-22 bodies, all asleep), +40 = active (216), +28 = all (482), +0/+32 = 21 each.
    Only 1 sleeping body was within 15 m, so it woke 1. Queue test: 12 of 15 scored scenarios passed, 0 dragged by the game (the home-pull did not show);
    props 4-5 each run were INVALID (never moved). The one real FAIL: a prop 40 m from the skater got no hole (props were only published within 30 m of the skater):
    now published when within 12 m of a portal (`NearPortal`).
40. Trick continuity, step 1 = measure (built, untested): after each crossing the probe logs every change of the six processed-input flag words
    (*(actor+1804)+2468..2488) as `INPUT flags +N: old -> new (cleared, set)` (60 lines max). Next: do a trick grab across a floor/wall portal and read which bits
    drop; if the grab bit is among them, re-assert it for a few frames after the crossing (the animation graph restart is the other suspect).
    Run 157: baseline grabs (no portal) changed none of the six words, so the grab is not in the processed-input flags. What IS crossing-specific: +2484 bit
    0x04000000 is set for ~3-10 frames at every crossing ("just teleported"), never otherwise. Experiment (built, untested, cvar skate3_portal_keep_tricks, default on):
    clear that bit after ProcessData (82BD8918 post-call), log `KEEPTRICKS hid the just-teleported flag`. If grabs survive crossings: good; if the crossing
    breaks (stuck state, recovery warps): turn the cvar off. If nothing changes, the reset is in the animation graph / skeleton reset, not this flag.
41. Run 160: KEEPTRICKS fired on 30+ of 34 crossings (flag value 0C000000: bits 04000000 and 08000000; only 04000000 is hidden), kept speed ~100% (mean ratio 1.0,
    n=30), the user's last jump "held the trick". Skeleton reset 82BE3508 (r31 = r3 = skeleton) also clears state bytes/words at skeleton+16389..16506, just past
    kRatesBytes (0x4000), so the vector restore never covered them. Built, untested: `sub_82BE3508` hook snapshots skeleton+0x4000..0x4090 before and restores it
    after (cvar skate3_portal_keep_skeleton_state, default on), logging `SKELRESET skeleton+0x...: old -> new` for the words the reset changed (40 max).
    Open for the next sweep: bit 08000000 of +2484; the callees 82BE7078 / 82BED780 of the reset; camera continuity; NPC crossings; portal recursion.
42. Run 161 (skeleton-state restore on): the reset changes three words only: +0x4024 (1 or 2 -> 0), +0x4074 (a float ~0.008 -> 0), +0x4078 (00010000 -> 01010000,
    the game SETS a byte there). With the restore on, the user got "falling but on board" after one-foot/tweak crossings and the log shows 38 `NOBAIL denied a
    wipeout request` right after crossings: tricks now survive into the landing, the landing asks for a bail, No Bail denies it, the skater is left in the
    false-falling state. `skate3_portal_keep_skeleton_state` is now OFF by default (logging stays). Proper fix to find: what a denied wipeout should do instead
    (return the pose to riding) - the "full god mode" item from the No Bail notes.
    Portal sounds (built, untested): synthesised effects through their own SDL stream (`Sfx(kind)` in gate.cpp): blue/orange shot chirps, whoosh on a crossing, clear,
    cube pop; cvars skate3_portal_sfx (on) and skate3_portal_sfx_volume (0.35). Not done: Chell as the skater (model/animation swap), grinding the cube (needs registering
    as a game grind object), a 3D portal gun model.
43. Stage 1 of "true mashup" (built, untested in game), all read from the user's OWN Portal 2 install, nothing bundled:
    - Real Portal sounds: `Sfx` loads `sound/weapons/portalgun/portalgun_shoot_blue1/red1.wav`, `portal_enter_02`, `portal_close1`, `portalgun_powerup1` (cube), `portal_invalid_surface3`
      by reading `pak01_dir.vpk` (VPK v2 reader in gate.cpp, `ReadVpkFile`), SDL_LoadWAV_IO + ConvertAudioSamples, one SDL stream per sound; synthesised fallback per sound.
      cvars skate3_portal_sfx_dir (default ~/Library/Application Support/Steam/steamapps/common/Portal 2/portal2), skate3_portal_sfx_volume. Log: `portal-gate: sounds: N of 6 ...`.
    - 3D Portal gun: `tools/re/portalgun_extract.py` (pure Python; vpk_list.py reader) reads models/weapons/v_portalgun .mdl/.vvd/.vtx + its VTF (DXT) texture into
      ~/Library/Application Support/Skate3PortalMod/portalgun.bin (7242 triangles, 1 texture 512^2; the PotatOS, hands and glass meshes dropped; verified offline by
      rendering preview PNGs: it is the real gun, barrel along +z in bind pose). `skate3_portal_gunmodel.{h,cpp}` loads the cache, `draw_gun` in skate3_native_scene_gpu.cpp
      draws it with the spline z-write pipeline: first person = lower right of the view, third person = beside the skater; barrel through the crosshair (`AimNdc`);
      kicks back on a shot (`GunKick`). The old flat 2D gun and green skater dot are off unless skate3_portal_gun_2d. cvar skate3_portal_gun_model.
      UNVERIFIED on screen: orientation (barrel direction / mirrored handedness), scale (0.0254 * 0.8-0.9), placement, depth clipping, the ring budget (0.6 of 1 MiB).
    - Not done (user wants them, staged): grinding the companion cube (needs the cube registered as a game grind/collision object), Chell as the skater (Source MDL skinned onto
      Skate 3's skeleton: bone mapping + weights + animation retarget), a 3D aim marker to replace the 2D reticle ring.
44. Run 163/164 feedback on item 43: gun see-through (the VTF alpha is a material mask: alpha forced to 255 in the loader), held at head height in 3rd person (the tracked point is
    ~hip height: +0.25 not +1.12), too big in first person (scale 0.0254*0.45 FPV / 0.6 TPV, anchor right .17 down .15 ahead .5). Sounds: log said `0 of 6 are the Portal 2
    effects` (the WAVs are plain 16-bit stereo 44.1 kHz PCM, so the failure is in the VPK reader/loader: now logs the stage that fails) and nothing was audible at all: the six
    separate SDL streams are replaced by ONE stream with a mixing callback (`SfxMix`). Untested.
45. Run 166: the sound loader failed for all six files because the macOS Steam Portal 2 `pak01_dir.vpk` is VPK VERSION 1 (12-byte header, tree size at +8), not v2 (28 bytes).
    `ReadVpkFile` now handles both (and portalgun_extract.py's tree_end). Gun mesh now solid and sized OK (user). Idea from the user: Skate 3 has tazers - could give the
    arm/firing pose and a hand-attachment point for the gun and later Chell. A string search of game/data finds "taser/tazer" only in audio .big files (speech); the animation
    data is probably packed, so no pose/animation lead yet.
46. Stage 2 planning (cube grind + Chell). Sounds confirmed in game (user). User saw an NPC taze them (ZAP on screen, NPC stun animation): the stun-gun animation/prop exists in
    the game (use later for the firing pose / hand attachment). Grind facts from the Rust notes (crates/skate-game/.../player_input/grind/world.rs, skate-core grind_surface): the game
    finds grindable surfaces with batches of line probes (82C205D8/82C20728, 82E0AAD0: scene mask 7, side flags 3, rejection mask 0) against the world triangles (827719B8 =
    one segment vs one triangle); a hit is {fraction, position, normal = triangle face normal, packed_surface u32 (query result +100; &0x7f = surface code, &0xf80 == 0x400 = stairs)}
    and `classify.rs` turns the pattern of up to 7 hits into ThinRail/FatRail/Ledge. So a grindable cube = answer those probes analytically for the cube's box (a ledge surface code)
    inside the function that aggregates the world line test. NOT built: the exact argument/output layout is unread. Built, untested: Ctrl+8 opens an 8 s capture; the `sub_827719B8`
    hook logs `portal-grind: #n args ... start delta tri A B C radius fat result out[...]` (400 max, only within 2.5 m of the skater). Next: grind a real ledge during the capture, read the
    probe pattern and the output struct, then add the cube box to the answer. Chell: Source MDL (v49) + VVD + VTX reader exists (portalgun_extract.py parses the same formats; player
    model models/player/chell/) but skinning needs a bone map onto Skate 3's skeleton and a draw path inside the skater's own skinned items: not started.
47. Run 169 (Ctrl+8 capture): useless as built. The 400-line cap was used up in 0.1 s by the first press (board ground probes: 8 cm / 20 cm / 51 cm downward lines and short
    horizontal ones, plus ~1.7 m tilted lines), so the later map and bench grinds logged nothing, and my argument decoding was wrong (r3/r6 hold segment data, not hit/triangle).
    KEY FINDING: the user grinds a moveable BENCH natively, so real game props are already grindable. New plan for the companion cube (built, untested): ADOPT a real prop.
    Ctrl+9 next to an awake prop (wakes props within 8 m first) makes it the cube: `AdoptFrame` publishes its position (+16) and basis columns (+64/+80/+96) to
    `portal_cube::SetExternal`; `Snapshot` returns that when fresh (<200 ms) so `draw_cube` draws the cube mesh at the prop, and the two opaque item loops skip the prop's own
    dyn_entity item (within 0.9 m: `HidesItemAt`). Ctrl+9 again releases it. The prop keeps its game physics and shape, so it grinds, is pushed and carried (R1 grab picks real props)
    and crosses portals through the existing prop code. Unverified: the axis convention of the basis (rows vs columns), which prop is box-like enough, whether the hide radius
    catches the right item, how the prop's shape (a bin, a bench) differs from the 0.9 m cube.
48. Run 170 / user screenshot (14:18:24): adoption works (prop 474AA720, asleep, woken, 2.3 m away) and the user was grinding it (FS Nosegrind / FS Crooked on screen): the prop keeps its
    native grind. The cube mesh is drawn at the prop's centre, but the prop's own look (a long dark box with wooden slats) was NOT hidden: matching was by the item's origin, and a long prop's
    origin is at one end. Now matched by the item's world-space bbox centre within 0.9 m (`adopted_look`), plus a diagnostic `portal-cube: item near the adopted prop ...` (24 times, 1 s apart)
    listing items within 3 m (bbox-centre distance, origin, dyn_entity, extent) so the right item can be picked if that is still not it. Untested.
49. Floor-to-floor falls (run 170): the log shows portals placed on the VOID floor (y 50.55) and 207 crossings at 20/s between two overlapping floor portals (centres 4.6 m apart,
    half width 2.6 m): the exit lands inside the other portal's opening and the "standing in a floor opening counts" clause crosses again at once (I had cut kFlatCooldownFrames to 2).
    Fix (built, untested): `g_standing_block` = 60 frames after any crossing in which that clause is off (the through-the-plane clause, needed for the endless fall, stays on).
    The under-the-map fall itself was before the portals were placed (the game's own recovery put the skater back at y 64.7).
50. CHELL (built, untested; Ctrl+0 toggles, cvar skate3_portal_chell, default off). Pipeline: portalgun_extract.py (MODEL=models/player/chell/player SKIP= MAXTEX=256) -> chell.bin
    (6 meshes, ~22k triangles, 4 textures; verified by preview: textured A-pose Chell, Y up, 76.6 in tall, arms out) in ~/Library/Application Support/Skate3PortalMod/. The loader
    dedupes vertices (indexed draws; the gun now draws indexed too). `portal_chell::Capture` (from the vertex decode of every skinned mesh, capped at 64 meshes) keeps the skater's
    rest-pose vertices with their weight/bone-index bytes (decoded layout word 7 = 4 weight bytes, word 8 = 4 bone-index bytes). Per frame the renderer finds the skater's skinned
    parts (palette bone-0 translation within 1.8 m of the tracked skater), `Build` (once) scales Chell to the skater's height, picks the best of 8 turns/mirrors by mean
    nearest-vertex distance, centres on the torso, and gives every Chell vertex the weights of the nearest skater vertex (logs `portal-chell: skater cloud ... scale ... turn ... mean gap`
    and `weights transferred ...`); `Skin` skins her on the CPU with the skater's own palettes (bone k = 12 floats, three rows [R|t]); `draw_chell` draws her with the spline z-write
    pipeline; the skater's own skinned items (same lw_entity or mesh) are hidden through `hide_item` inside timed_draw. NOT done: a Free Play menu entry (the toggle replaces the skater
    model for whoever is nearest the tracked point), normals/lighting (flat albedo), eyes (no texture), NPC skaters. Unverified: the donor palettes may be per-part (handled: each vertex
    uses its donor part's palette), the weight byte order, the Skate rest-pose orientation (the 8-way search), the bone-0 position test.
    Other Portal 2 characters available the same way: models/player/ballbot (Atlas), models/player/eggbot (P-body) with skins, models/npcs/glados, personality_sphere, turret.
51. Probe cleanup (built here with -fsyntax-only against stub headers + unit test; not built on the Mac, untested in game). Only
    skate3_portal_probe.cpp, default behaviour meant to be identical. Removed:
    - The A/B learner: `Profile`/`kProfiles`, `SelectProfile`, `FinishScore`, stats file portal_gate_profiles6.txt (no longer read or
      written), the "GATE learn:" line, cvar `skate3_portal_gate_ab`. Profile C (board move + restored skeleton, lift 1.0) is hard-wired:
      `armed = hop || gate`, `t_surgery = gate && state != 500`. The "GATE profile C: board move + restored skeleton (crossing #N)" line is
      kept word for word so crossings.py still splits on it.
    - `false` branches: the hold-for-riding-state struct ("GATE held" line), the fastest-board-rate velocity pick (vin is the tracked
      displacement, as before), the airmode flag rewrite.
    - Code only those paths reached: `TurnLive` ("GATE turned N live velocity vectors in place", only when restore was off),
      `WriteActor` + `g_move_target`/`g_move_k`/`g_delta_frames` (never called), `BodiesHolding` (no caller), `#include <random>`.
    - cvars with no effect left: `skate3_portal_gate_airmode` (never read), `skate3_portal_gate_restore` (profile C restores anyway).
      Passing any of the three removed cvars on the command line now does nothing or warns (SDK cvar parser; not checked).
    Test: one normal run with the usual flags; a few wall<->wall and floor<->ceiling crossings, a carry, a prop drop. Expect the same
    log as before: `GATE profile C ... (crossing #N)`, `GATE crossing with flags`, `GATE velocity in ... out`, `GATE teleport armed`,
    `GATE board reset replaced`, `GATE speed after crossing` with kept speed like runs 160+. No `GATE learn` / `GATE held` lines.
    If launch fails on an unknown flag, drop --skate3_portal_gate_ab/airmode/restore from the command line.
52. LEAP: NPC skaters ride through portals (built, untested; cvar `skate3_portal_npc`, default OFF, hot reload). Log prefix `NPCX`.
    - Design: every skater actor runs sub_82DB88C8 (IsPlayer's comment: the game relocates the park skaters through the same path), so a
      non-human actor gets its own crossing state (`NpcStep`: previous position, cooldown 45 / 2 frames for flat pairs, 60-frame standing
      block) and is sent with the human's relocation (`ArmTeleport`, thread-local target via the 82585CB0 hook). Its board reset is swapped
      for SetTransform (`t_surgery` + `t_npc_actor`) and `NpcTurnBlock` turns its 7 lin + 7 ang board rates and actor +1248 by the pair
      rotation (thread-local `t_npc_m`, speed cap 80). No skeleton restore and no hole for NPCs (profile B, 88% for the human).
    - Crossing test: new pure module `src/skate3_portal_math.{h,cpp}` (frames, oval, pair rotation, stateless `TestCrossing`, `ExitTarget`),
      unit-tested in `tests/skate3_portal_math_test.cpp` (passes). It reproduces the human's NON-hole rules (wall trigger 1.1 m, floor 2.2,
      standing-in-a-floor-opening rule, exit = trigger + depth + clear 1.2 wall / 0.95 + 0.3 flat, wall exit >= centre - 1 m). The human's
      `Step` is untouched. `portal_gate::PortalPairFrames` snapshots both portals for it.
    - Position = ground +1216 + 0.85 m. Refused while state 500/700/702, flags 40000000 (recovery), 00800000 (spawn), relocate bit set, or
      after a > 3 m jump in one update. Velocity = actor +1248 (else displacement x 60).
    - Props: each NPC crossing adds a 4 s / 5 m no-props zone at its entry and exit (`NoteActorCrossing`, 8 slots), like the human's, so the
      NPC's skeleton bodies are not moved again by the prop code.
    Test (run with `--skate3_portal_npc=true` or toggle it in the cvar menu): place a wall portal on a wall the park skaters ride along,
    or a floor portal on their path, and the other portal somewhere visible; wait near it.
    - `NPCX actor X seen: state S flags F at (...)`: up to 24 lines at the start. If NONE appear, sub_82DB88C8 is human-only and the
      design needs another hook (tell me).
    - `NPCX actor X crossed the blue/orange portal (#N) ... teleport armed`, then `NPCX actor X board reset replaced by SetTransform ...
      N rate vectors turned` (N should be 15), then `NPCX actor X after crossing t+K ... m from the exit, speed ...` every 10 frames.
      Success = small distance from the exit (< 3 m at t+10) and speed near "in".
    - Failure signs: `teleport REFUSED` (relocate bit already set: the game was moving it), `the teleport did not reset the board` (NPC
      executor skips the board reset: rates not turned), the distance growing back to the entry (the AI drags the NPC back to its path),
      `GATE exit speed capped` in a burst, or NPCs vanishing (the game respawns them). Screenshot plus clock if it looks wrong.
    - Not done: airborne NPC crossings (ground position is stale in the air), hole for NPCs (they cannot enter a wall portal at the
      plane, only at the trigger 1.1 m out), walkers (pedestrians are probably not skater actors), NPC AI path after the jump.


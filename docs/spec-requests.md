# Spec requests (implementation side -> analysis side)

Each request: what is needed, the observation that would answer it, and a probe or log line to get it. Answer with a spec in
docs/specs/ (`NNN-name.md`, "answers Rn"). Mark answered requests here.

## R1. Do park skaters (NPCs) run the PlayerInputState update 82DB88C8 and the relocation executor 82DB8998?  (partly answered)
Run 180: yes for the update (10 NPC actors seen). Still open: the executor and board reset for an NPC (needs a crossing, PROGRESS 55).
Needed by: PROGRESS 52 (`skate3_portal_npc`). The design assumes yes (IsPlayer's comment says the game relocates other actors
through the same path).
Answers it: a run with `--skate3_portal_npc=true`: `NPCX actor X seen` lines (no lines = no). Statically: are there callers of
82DB88C8 other than the human's input owner, and is the per-actor object the same class (same vtable) for NPCs?
Also: does the executor reset an NPC's board through 82C05F50 (the `NPCX ... board reset replaced` line), and is the NPC's board at
*(actor+1880) -> +12 laid out like the human's (lin rates +0x140, ang +0x1B0, 7 bodies)?

## R2. Live world position of a non-human skater actor while airborne  (worked around: PROGRESS 55 links the actor to its presentation entity; a spec of the real actor -> entity link would replace the scan)
Run 180: the actor's +1216 ground position is all zeros for NPC actors.
Needed by: NPC crossings in the air (today only riding NPCs cross: +1216 is "current while riding").
Answers it: for one airborne NPC, which actor field (or entity field reachable from the actor) tracks the board/rider position every
frame in the air. Probe: log actor +1184..+1280 (24 floats) and the presentation entity position each frame through one NPC jump.

## R3. The chase camera after a teleport  (open)
Needed by: PROGRESS 53 (`skate3_portal_camcarry`). Today the carry overrides the rendered view only; the game's camera logic still
runs from its own state, so the game's culling uses the old camera for a moment.
Answers it: (a) does the chase camera cut or interpolate after the executor relocates the skater (frame-by-frame camera position
for ~1 s after a crossing: the PCAM "game camera" max move gives a first number), (b) where the chase camera keeps its position/
target/velocity state and what call re-seats it (the session-marker reset probably re-seats it): address + offsets, so the camera
could be carried through the portal in the game's own state instead of only in the renderer.

## R4. Drivable cars: the traffic vehicle  (open, research scaffolding)
Needed by: the "drivable cars" horizon. Nothing built yet.
Answers it: (a) the traffic vehicle object: class/vtable, per-frame update function, where its world transform and velocity live
(offsets, units); (b) how traffic is steered (a path follower writing a target speed/steer? direct transform writes? a physics body
with forces?); (c) whether vehicles are rigid bodies integrated by 82AE6590 (the prop integrator; PROGRESS 31) - if yes, which body
ids belong to one car; (d) any existing "player in vehicle" state (none expected). A list of addresses + one paragraph per question
is enough to start a spec-driven prototype ("possess the nearest car: steer/throttle from the pad, camera follows").

## R5. Game gravity and the airborne velocity carrier  (open)
Needed by: the FLING assist (PROGRESS 55) assumes 9.81 m/s^2 and treats the board block's 7 linear rates as the skater's velocity in the air.
Answers it: the gravity the skater's air controller integrates (m/s^2), and which state holds the airborne velocity the game integrates
(board rates, skeleton +0x2770.. vectors, or an air-controller field), so the exit velocity can be set once in the right place instead of
being topped up for N frames.

## R6. "Backwards man" speed glitch as a macro  (open, research)
Needed by: a player-triggered speed macro that the portal crossings could mimic. Seen in run 187 (00:20:40, "speed glitch exit,
felt like max velocity"): one crossing logged 548.9 m/s before settling near 37-39 m/s.
Answers it: the real Skate 3 "backwards man" trick, reproduced by input or by state, in game-facing terms: which inputs and states
(riding, air, off-board, landing) trigger it, which velocity or board fields it writes and in what units, and whether it can be
triggered on purpose and repeated. Then: is the 548.9 m/s spike from the same mechanism, or from our teleport writing the board
rates (`boardV` / `actorV` in the TRACE lines)?

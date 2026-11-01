# Spec requests (implementation side -> analysis side)

Each request: what is needed, the observation that would answer it, and a probe or log line to get it. Answer with a spec in
docs/specs/ (`NNN-name.md`, "answers Rn"). Mark answered requests here.

## R1. Do park skaters (NPCs) run the PlayerInputState update 82DB88C8 and the relocation executor 82DB8998?  (open)
Needed by: PROGRESS 52 (`skate3_portal_npc`). The design assumes yes (IsPlayer's comment says the game relocates other actors
through the same path).
Answers it: a run with `--skate3_portal_npc=true`: `NPCX actor X seen` lines (no lines = no). Statically: are there callers of
82DB88C8 other than the human's input owner, and is the per-actor object the same class (same vtable) for NPCs?
Also: does the executor reset an NPC's board through 82C05F50 (the `NPCX ... board reset replaced` line), and is the NPC's board at
*(actor+1880) -> +12 laid out like the human's (lin rates +0x140, ang +0x1B0, 7 bodies)?

## R2. Live world position of a non-human skater actor while airborne  (open)
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

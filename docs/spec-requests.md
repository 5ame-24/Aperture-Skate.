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

## R5. Game gravity and the airborne velocity carrier  (measurement built, PROGRESS 57: run with the AIR probe, then `python3 tools/re/airlog.py`; a spec confirming it from the analysis side is still welcome)
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

## R7. Existing Skate 3 first-person mod  (open, research)
Needed by: the first-person view (PROGRESS 53/55). A first-person mod for Skate 3 has been released; its approach may show how the
game's camera and skater model are driven from outside.
Answers it: what the mod hooks (camera state, head/skeleton bones, input), which game functions or fields it writes, and what it
does about the camera during riding and in the air. Compare with our follower-mode camera in `skate3_native_scene*.cpp` and the PCAM
logs. Written as a cleanroom summary: take the behaviour, not code.

## R8. First-person camera sticks in walking mode  (likely cause found in our own tracking code and fixed, PROGRESS 57; confirm with a run)
Reported by the human: occasionally, in walking mode (on foot), the first-person camera gets stuck and behaves like a second-person
(third-person) camera. Not yet seen in logs; no PCAM or first-person lines were checked for it.
Answers it: the first-person follower state (`skate3_native_scene*.cpp`, `UpdateFreecam` follower mode) and what switches it off while
walking: which state or flag the game's on-foot camera takes over, and whether a crossing or the Esc menu leaves the follower stale.
Needed: a run with first person on foot, plus the `PCAM` "first person turned" lines and the first-person state at the moment it sticks.

## R9. On-foot velocity and jump state  (open)
Needed by: FLING and AIR for walking/jumping crossings (PROGRESS 59: FLING is off while off the board, state 500).
Answers it: which player state(s) and flags an on-foot jump uses (the `AIR off-board updates ... by state/flags` line gives a first answer
at runtime), and where the walking character's velocity lives (actor field, character controller), so an exit velocity can be applied on foot.


## R10. Skate 3's own character slots and skeleton  (open; the Portal characters work without it)
Needed by: Portal 2 characters as real game characters (PROGRESS 62). Today they are drawn by us in place of a skater's skinned parts and
driven by that skater's bone palettes; the game still thinks the skater is its own character.
Answers it:
- the skater skeleton: bone names and hierarchy (rig data in game/data, or the skeleton object the palettes come from), and which palette
  index of each skinned part is which bone. With that, our bone map is exact instead of matched by geometry.
- the character slots: where Create-a-Skater / the character select keeps its list of characters (a table of models, rigs and outfits), so a
  Portal character could be listed as a selectable character, and how NPC skaters pick theirs.
- the rest pose the skin palettes are relative to (bind pose), measured once from a palette vs the captured rest vertices.

## R11. The game's character select (Free Play skater list, e.g. Shingo)  (open; CHARSEL probe built to gather data)
Needed by: Chell / Atlas / P-body as entries in the game's own character select, next to the skate characters (HANDOFF 9 goal 3).
The implementation side does not read game code, so this needs a spec:
- hook point: the function that builds the selector's list (its entries, their order, the count) and the one called when an entry is picked;
  argument registers and what they point to;
- data: the entry layout (name / display string, model or outfit reference, rig, unlock flag), where the table lives, how the count is kept;
- behaviour: what picking an entry does to the rider (which actor / presentation entity fields change), so a Portal entry can pick
  "the default skater + portal_chell slot 0 = Chell" instead of a game model.
Data help at runtime: Ctrl+Shift+9 (CHARSEL) logs every place the names in skate3_portal_charsel_names sit in guest memory, the pointers to
them and the table stride around each pointer. Run it with the selector open.

# FEEDBACK for HANDOFF 4 (local Claude -> cloud Claude)

Build: ok (skate3 linked from merged ce4d943)
Unit tests: ok (skate3_portal_math_test: ok, skate3_portal_test: ok)

## TEST A (offline Portal 2 facts): ok
maps read 106, failed 0
entity counts: prop_portal 21, trigger_catapult 185, prop_tractor_beam 25, trigger_push 192
catapult playerSpeed median 650 units (16.51 m/s); physicsSpeed median 650 units
prop_portal halfwidth 32, halfheight 54 (default sizes, 0.81 m x 1.37 m)
config cvars: none set

## TEST B log: skate3_194.log (3104 lines, quit by human)
Counts: AIR arc: 0 (0 usable), FLING armed: 1, FLING +: 1, FPVTRACK re-acquired: 22, HOLE kept solid: 10, EXITFIX: 0
Exact lines:
- FLING armed: v_in (-2.8, -0.1, -1.4) -> v_out (-0.1, 3.1, 0.4) 3.1 m/s for 10 frames, gravity 9.81 (assumed)
- FLING +1 stopped (bailed), 0 top-ups
- FPVTRACK re-acquired the skater through its entity at (133.24, 65.58, -289.76) after 0 lost frames
- FPVTRACK re-acquired the skater through its entity at (129.33, 64.87, -129.99) after 0 lost frames
- FPVTRACK re-acquired the skater through its entity at (81.95, 65.01, -225.20) after 0 lost frames
- FPVTRACK re-acquired the skater through its entity at (111.69, 65.10, -264.67) after 0 lost frames
  (re-acquires fire on consecutive frames, about 0.05 s apart, and the positions jump 50-160 m between them)
- HOLE skipped a world contact (#1): mover (131.89,64.54,-290.31) tri A (134.09,64.55,-284.76)
- HOLE skipped a line probe (#1): start (131.42,64.61,-290.83) n (-0.00,1.00,0.00)
- NPCX: 0 actors seen, 0 crossings

Human notes (verbatim): "did some jumps on foot and some ollies but then fling test caused our old warp and flicker bug"

Read against expectation: not met. No AIR arcs were logged, so gravity was assumed (9.81), not measured. The fling
armed once and bailed on its first frame. The FPVTRACK re-acquire path fired every frame and switched between
entities far apart, which matches the warp and flicker the human saw. The fling may not be the cause; the
re-acquire is the most likely one.

## NEXT PROMPT (local Claude -> cloud Claude)

This FEEDBACK block, then: fix the FPVTRACK re-acquire so it only runs when tracking is actually lost, and make
sure the fling does not trigger it. Then get AIR arcs logged on the on-foot jumps and ollies, since no arc was
logged this run. Use the HANDOFF format from docs/handoff-loop.md.

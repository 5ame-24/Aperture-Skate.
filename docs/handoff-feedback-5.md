# FEEDBACK for HANDOFF 5 (local Claude -> cloud Claude)

Build: ok (skate3 linked from merged a4c8abf)
Unit tests: ok (skate3_portal_math_test: ok, skate3_portal_test: ok)

## TEST A log: skate3_196.log (quit by human, wall clock 02:10 to 02:20)
Human run: ollies on flat ground, one blue portal then cleared as a marker, drops (mostly the same ledge, one from a
smaller one), a portal shot and cleared before on-foot steps, walking and jumping on flat ground. Human did not
note game-clock times.

Counts (leaplog.py): FPVTRACK human entity locked: 11, FPVTRACK human entity unlocked: ~12, FPVTRACK re-acquired: 40,
FPVTRACK refused a re-acquire: 20, FPVTRACK ignored a jump: 20, FPVTRACK lost: 86, FPVTRACK accepted: 24,
AIR arc: 57 (24 usable, 3 too short), AIR gravity estimate: 24, AIR off-board updates: 20,
FLING armed: 66, FLING +: 968, topped up: 919, FLING not armed: 5, HOLE kept solid: 10, EXITFIX: 17,
sinking through the opening: 1

Exact lines (key ones):
- 02:10:37.665 FPVTRACK human entity locked: 491AE6C0 (rider class, 0.28 m from the riding position for 30 frames)
- 02:13:11.194 FPVTRACK ignored a 29.8 m jump of the tracked point (riding, flags C0000000)
- 02:13:11.276 FPVTRACK refused a re-acquire: entity 491AE6C0 at (81.45, 64.92, -132.79) is 29.8 m from the last good position (limit 4.7 m, 5 lost frames)
- 02:13:12.523 FPVTRACK re-acquired the skater through its entity at (81.14, 65.00, -132.97) after 80 lost frames, flags 40000000
- 02:17:31.550 FPVTRACK human entity 491AE6C0 unlocked: it jumped 7.2 m in one frame while the skater moved 0.00 m
- 02:17:34.279 FPVTRACK human entity locked: 491AE6C0 (rider class, 0.16 m from the riding position for 30 frames)
- 02:17:43.287 FPVTRACK human entity 491AE6C0 unlocked: it jumped 7.9 m in one frame while the skater moved 0.03 m
- 02:18:27.144 FPVTRACK human entity 491AE6C0 unlocked: it jumped 8.6 m in one frame while the skater moved 0.00 m
- 02:19:49.586 FPVTRACK human entity 491AE6C0 unlocked: it jumped 5.4 m in one frame while the skater moved 0.07 m
- 02:11:26.627 AIR off-board updates in the last 15 s by state/flags: state 100 flags 90..: 93, state 200 flags D0..: 60, state 200 flags 40..: 42, state 100 flags 8E..: 19, state 700 flags D0..: 11, state 200 flags 90..: 5 (other rows small)
- AIR gravity from the path: median 8.07 m/s^2, min 3.23, max 9.18, n 24
- AIR ranking: skeleton+0x28E0 first in 20 of 24 arcs (median RMS 0.29 m/s, median g 8.04); skeleton+0x2850 first in 4
- 02:18:04.716 FLING not armed: exit speed 0.1 m/s (too slow to hold)
- 02:19:15.372 FLING not armed: crossing off the board (state 500, on foot or bailed)

Human notes (verbatim, what they could give):
- Warp or flicker, and clock time: "couldnt tell you". No game-clock times were recorded, so the 02:13 and 02:17-02:20
  windows are unmatched to what was on screen.
- First person on foot: "mostly followed altho there were other bugs present which made it challenging".

Read against expectation: inconclusive for the lock and re-acquire. The entity lock was right at the start (0.28 m) but
then dropped and re-took the same entity 12 times, each time after a 5-9 m one-frame jump while the skater did not move,
and the jump guard refused 20 re-acquires at 29.8 m. The AIR arc goal is met (24 clean arcs, gravity measured, store
identified). The FLING goal is inconclusive: it was armed 66 times, with many top-ups, and the human did not report
whether the board launched without them.

## NEXT PROMPT (local Claude -> cloud Claude)

This FEEDBACK block, then: make the human entity lock stop flipping. A 5-9 m one-frame jump of the entity while the
skater is still should be treated as the entity moving, not as a lost lock, and the lock should only be dropped when
the skater's own position is really lost. Keep AIR arcs and the state/flags histogram as they are, since they worked.
Add the game clock (on-screen time, or the frame/time the game reports) to the FPVTRACK and FLING lines so the next
human run can be matched to what was seen. Use the HANDOFF format from docs/handoff-loop.md.

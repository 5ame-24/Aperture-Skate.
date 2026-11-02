# FEEDBACK for HANDOFF 19 (consistency + fling fixes) (local Claude -> cloud Claude)

Build: ok (bf1a338, merge of 1a43463 with the gel dropper rolled back).
Unit tests: ok (math, portal, retarget).

## TEST A (same drop, same exit, on the board): log skate3_211.log (16:09 to ~16:15)
Human: "no flings :("

Counts: FLING armed 82, not armed 8, converged 2, CONSISTENCY lines 90, crossings 86
(51 riding state 200, 26 riding state 100, 3 riding C8000000, 3 airborne 48000000, 1 airborne state 700).

Exact lines:
- 16:11:59.580 FLING decision: ARMED; entry v (-0.1, -13.2, -2.5) 13.4 m/s, falling 13.2 m/s, riding 100; exit v (9.4, 9.5, 1.4) 13.4 m/s
- 16:11:59.597 FLING +1 topped up: -5.7 -> 13.3 m/s along the exit (0.70, 0.71, 0.11), state 100
- 16:11:59.663 FLING +5 topped up: -7.5 -> 13.0 m/s along the exit (0.70, 0.71, 0.11), state 100
- 16:11:59.730 GATE speed after crossing: 5.5 m/s (before 2.7) flags C0000000 state 100
- 16:15:49.629 CONSISTENCY entry velocity from the board rates: (-0.0, -59.7, 0.0) 59.7 m/s; estimate (-0.0, -45.1, 0.0) 45.1 m/s, differ by 14.6

Observations:
- The top-up holds 13 m/s on each frame, but the logged speed after the crossing stays 2.7-6.1 m/s.
- The entry speed reads 13.2 m/s in the FLING line and 59.7 m/s in the CONSISTENCY line: units or source differ.

Human notes (verbatim): "no flings :("

Read against expectation: not met. The arm and top-up run, but the skater does not fly.

## TEST B (on foot): not checked (no clear on-foot crossing; one airborne state 700 crossing).

## TEST C (walk input): FPVWALK input lines present, but all "not on foot (riding: the stick steers the board)" (69 lines).
No walking was logged, so the walk is still unverified.

## NEXT PROMPT (local Claude -> cloud Claude)
This FEEDBACK block, then: find where the fling's top-up goes after it is written. Log the body speed (actor velocity) and the
board speed (board rate) at each top-up frame AND on the next frame, so we see which store the game reads and whether it is reset.
Make the fling work on the board first: a crossing with an armed fling must change the skater's height or speed over the next 10
frames (log the height change). Resolve the 59.7 vs 13.2 m/s entry mismatch (units, and which store each line reads). Then test on
foot. Use the HANDOFF format from docs/handoff-loop.md.

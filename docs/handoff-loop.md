# Handoff loop (cloud Claude <-> local Claude <-> the human)

The human is the only one who can play the game. Local Claude builds, runs the unit tests, reads logs and writes the next prompt.
Cloud Claude writes code and never runs the game.

## The loop

1. **Cloud Claude** makes the change on `portal-mashup` (or a branch the human names), commits it, **pushes it to origin**, and
   ends with a HANDOFF block (format below). Local Claude can only see work that has been pushed.
2. **Human** pastes the HANDOFF block to local Claude.
3. **Local Claude** pulls, runs the unit tests, runs the build, and reports `build ready` or the first error with file and line.
   Then it relays each TEST to the human, one at a time, with the exact command and in-game steps.
4. **Human** plays the test and reports back, or pastes the game log path.
5. **Local Claude** runs `python3 tools/re/leaplog.py`, greps the named log tags in the newest log under
   `out/build/macos-release/logs/`, and reports counts plus exact log lines.
6. **Local Claude** writes a FEEDBACK block (format below) and a NEXT PROMPT for cloud Claude when the human asks for one.
7. Go to step 1.

Local Claude does not change game code itself unless the human asks; it only builds, tests, reads logs and writes prompts.

## HANDOFF block (cloud Claude -> human)

```
HANDOFF <n>: <one-line title>
Branch: <branch>   Commit: <short sha>
Build: cmake --build out/build/macos-release --target skate3 -j 8  (re-run configure first if CMakeLists.txt changed)
Unit tests: clang++ ... (one line each, as in the header of each test file)
Changed: <file: what changed, one line each>
New cvars / flags: <exact flag names and defaults>
TEST <letter>: <name>
  Command: cd out/build/macos-release && ./skate3 --game_data_root=/Users/tr/Downloads/Skate3Recomp-macOS/game --fullscreen=false <flags>
  In-game: <numbered steps>
  Log tag(s) to grep: <TAG1, TAG2>
  Expect: <what good looks like, with numbers>
  Watch for: <known oddities, so the human can report them>
Open questions for the human: <only things the game run can answer>
```

Rules for cloud Claude:
- Every log line you add has a stable tag (e.g. `portal-probe: NPCX ...`, `portal-camera: PCAM ...`) so the grep works.
- Say what a zero count means ("zero NPCX lines = the design does not apply").
- Do not touch `third_party/`, `generated/`, `out/`, `game/`.

## FEEDBACK block (local Claude -> cloud Claude)

```
FEEDBACK for HANDOFF <n>
Build: ok | failed (first error: file:line, message)
Unit tests: ok | failed
TEST <letter> log: <file name, time>
Counts: <tag: n, tag: n>
Exact lines: <copied verbatim, the key ones only>
Human notes (verbatim): <clock times, visuals, what felt wrong>
Read against expectation: <met | not met | inconclusive, one line why>
```

Numbers are copied, never paraphrased. A failed or inconclusive test is reported as such.

## NEXT PROMPT (local Claude -> cloud Claude)

Starts with the FEEDBACK block, then the next goal in one or two sentences, then "Use the HANDOFF format from docs/handoff-loop.md."

## Starter prompt for cloud Claude

```
You are working on the Skate 3 recomp x Portal mashup (branch portal-mashup). Read docs/handoff-loop.md and follow its HANDOFF
format at the end of your work. You cannot run the game: the human runs it and sends you FEEDBACK.

Goal for this round: <one or two sentences>.
Known state: see PROGRESS.md sections 52 (NPC portals), 53 (camera carry) and the latest FEEDBACK block.
Human-observed problems to address in priority order:
1. <e.g. NPCs vanish after entering a portal and fall as if pushed>
2. <e.g. floor fall-through after the final passings>
3. <e.g. carried momentum: on exit the player should keep their speed and direction, not stop>
Do not add features beyond these. Keep unit tests passing (tests/run.sh once it exists).
```

## Why this shape

- One HANDOFF per round keeps each test attributable to one change.
- Log tags are the contract between the two Claudes: cloud Claude decides what to log, local Claude greps for it.
- The human's notes are kept verbatim because clock-time and visual oddities are often the first clue.

## One feature per round, no flags to remember

- Each HANDOFF polishes and tests ONE feature. That feature is on by default in its test command; cloud does not add a
  `--skate3_portal_*` flag for it. Other features stay off unless the round is about them.
- The test command for each round is the same `./skate3 --game_data_root=... --fullscreen=false --skate3_portal_gate=true` line,
  with no extra flags, unless the HANDOFF says otherwise.
- Reason: runs with the wrong flag give "not working" results that are really missing features (run 192: fling off, FLING armed 0).

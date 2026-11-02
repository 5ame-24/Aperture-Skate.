# Contributing to Aperture Skate

Thanks for helping. This is a reverse-engineering experiment: most progress comes from careful runs and good logs, not big rewrites.

## Ground rules

- **No game files, no extracted assets, no decompiled or third-party source in the repo.** Tools that read game or Portal 2 files run on
  your machine and write outside the repo (`out/`, `~/Library/Application Support/Skate3PortalMod/`).
- **Cleanroom**: write code from observed behaviour and specs (`docs/specs/NNN-name.md`). If something is unknown, add a request to
  `docs/spec-requests.md` instead of guessing.
- Do not edit `third_party/` (the rexglue SDK is a separate repository), `generated/`, `out/` or `game/`.
- New experimental behaviour goes behind a cvar (`skate3_portal_*`); every new log line gets a stable tag (`FLING`, `EXIT`, `AUTOTEST`, ...).

## The loop

Changes are tested by playing. The loop between whoever writes code, whoever builds, and whoever plays is in
[docs/handoff-loop.md](docs/handoff-loop.md): a HANDOFF block (what changed, how to test, which log tags to read, what a zero count means)
goes out with each change, a FEEDBACK block (counts and exact log lines) comes back. Recent ones: `docs/handoff-feedback-*.md`.

## Unit tests (no game needed)

```sh
clang++ -std=c++23 -Wall -Wextra -Werror -Isrc tests/skate3_portal_math_test.cpp src/skate3_portal_math.cpp -o /tmp/skate3_portal_math_test && /tmp/skate3_portal_math_test
clang++ -std=c++23 -Wall -Wextra -Werror -Isrc tests/skate3_portal_test.cpp src/skate3_portal.cpp -o /tmp/skate3_portal_test && /tmp/skate3_portal_test
clang++ -std=c++23 -Wall -Wextra -Werror -Isrc tests/skate3_portal_retarget_test.cpp src/skate3_portal_retarget.cpp -o /tmp/skate3_portal_retarget_test && /tmp/skate3_portal_retarget_test
clang++ -std=c++23 -Wall -Wextra -Werror -Isrc tests/skate3_portal_autotest_test.cpp src/skate3_portal_autotest.cpp -o /tmp/skate3_portal_autotest_test && /tmp/skate3_portal_autotest_test
```

Pure logic (portal maths, retargeting, the AUTOTEST sequencer) lives in testable files with no game dependency; keep it that way.

## Reading a run

Logs: `out/build/macos-release/logs/skate3_NNN.log`. `python3 tools/re/leaplog.py` (counts and key lines), `python3 tools/re/crossings.py`
(speed kept per crossing and per pair kind), `python3 tools/re/airlog.py` (airborne arcs). AUTOTEST (Ctrl+Shift+T in game) writes scored
trials to `skate3_portal_autotest.json`.

## Where things are

`src/skate3_portal_gate.*` portals, placement, overlay; `src/skate3_portal_probe.cpp` game hooks (crossings, carry, flings, gun rays);
`src/skate3_portal_math.*` pure maths; `src/skate3_portal_gel.*`, `src/skate3_portal_chell.*` / `skate3_portal_retarget.*` gels and
characters; `src/native/` the renderer. Project notes for AI assistants: `CLAUDE.md`.

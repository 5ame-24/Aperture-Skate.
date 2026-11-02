# Local tasks (small, self-contained, for a cheaper model)

Each entry is a complete prompt. Paste one at a time. Shared rules for every task:
- Repo: Skate 3 recomp x Portal mashup. Do not touch third_party/, generated/, out/, game/.
- Keep behaviour identical unless the task says otherwise. No new features.
- Build check when the task touches src/: `cmake --build out/build/macos-release --target skate3 -j 8` must succeed with no new
  warnings in the files you touched. Unit tests: see T2 (`tests/run.sh` once it exists).
- Commit with a one-line message naming the task number.

## Which model, and how to run them
Rule of thumb: **Haiku 5.5** when the prompt already contains every fact and the edit has a known shape (delete/rename/move/
script from a recipe), **Sonnet 5.5** when it must read several files or keep behaviour identical through a non-trivial refactor,
**leave to cloud** when it needs reverse engineering, new mechanics, or design choices.
Batching: put Haiku tasks that touch different files into ONE session (they share nothing, the prompts are self-contained, and one
session start costs less than several); run each Sonnet task in its OWN fresh session (it needs the context room for the files it
reads). Commit after every task so a bad one can be reverted alone.

| Task | Model | Why |
|---|---|---|
| T1 | Haiku | one-line deletion, location given |
| T2 | Haiku | small shell script, exact commands given |
| T3 | Haiku | comment rewrite, all facts in the prompt |
| T4 | Haiku | move one block of text, no wording changes |
| T5 | Haiku | delete one log line + its comment |
| T6 | Sonnet | behaviour-identical refactor across two files, needs judgment and a build |
| T7 | Sonnet | touches constants used in several files, needs a build |
Suggested runs: one Haiku session for T1-T5, then one Sonnet session for T6.

---

## T1. Remove the unused `g_fpv` in skate3_portal_gate.cpp
Files: `src/skate3_portal_gate.cpp`.
Goal: `std::atomic<bool> g_fpv{false};` (around line 184, anonymous namespace) is never read or written (first person is the cvar
`skate3_portal_fpv`). Delete that one line.
Check: `grep -n g_fpv src/*.cpp src/*.h` prints nothing; the Mac build has one warning fewer (`-Wunused-variable`).

## T2. One script that builds and runs the standalone tests
Files: new `tests/run.sh` (executable), `tests/skate3_portal_test.cpp` and `tests/skate3_portal_math_test.cpp` (read only, for the
build lines in their header comments).
Goal: `tests/run.sh` builds each test with `${CXX:-clang++} -std=c++23 -Wall -Wextra -Werror -Isrc` exactly as its header comment says
(output into `${TMPDIR:-/tmp}`), runs it, and exits non-zero if any build or run fails. `set -euo pipefail`. Run from the repo root.
Check: `tests/run.sh` prints `skate3_portal_test: ok` and `skate3_portal_math_test: ok` and exits 0; breaking a CHECK makes it exit 1.

## T3. Fix the stale header comment of skate3_portal_gate.h
Files: `src/skate3_portal_gate.h` (comment lines 1-6 only).
Goal: the comment says portals are "two tall rectangles placed on vertical surfaces", "drawn as an ImGui overlay", "crossed while
airborne", "Temporary scaffolding". Today: oval portals on walls, floors, ceilings and slopes; drawn in the world (with an ImGui
fallback); crossed riding or airborne; also drives props, the cube, see-through views. Rewrite those 6 lines to say that, in the
same plain style. Comments only.
Check: `git diff` touches only comment lines in that file.

## T4. Put PROGRESS.md's numbered items in order
Files: `PROGRESS.md`.
Goal: under "Untested in the latest build", item 5 ("Run 093 user report") sits after item 15. Move it between items 4 and 6 (the
numbers already exist; there is no item 4 text, leave that). Do not change any wording. Do not touch sections 16 onwards.
Check: `git diff --stat` shows only PROGRESS.md, same number of added and removed lines.

## T5. Drop the legacy "GATE profile C" log line
Files: `src/skate3_portal_probe.cpp` (in `GateStep`).
Goal: delete the line `REXLOG_INFO("portal-probe: GATE profile C: board move + restored skeleton (crossing #{})", g_cross_count);`
and the comment line above it ("Same text as when the A/B learner..."). Keep `++g_cross_count;`. If `g_cross_count` is then
unused anywhere, delete its declaration too (and the 3-line comment above it, which describes the old learner's result; move that
comment's content into nothing - it is already in PROGRESS.md section 51).
Check: build passes; `grep -n "GATE profile" src tools -r` prints nothing; `python3 tools/re/crossings.py <an old log>` still works.

## T6. Make gate.cpp's prop and cube crossings use skate3_portal_math
Files: `src/skate3_portal_gate.cpp` (`BodyCross`, `CubeCross`, `PairMatrices`), `src/skate3_portal_math.{h,cpp}` (read; add nothing
unless needed), `tests/skate3_portal_math_test.cpp`.
Goal: those three functions each rebuild the portal rotation `R[k][j] = -x.r[k] e.r[j] + x.u[k] e.u[j] - x.n[k] e.n[j]` and the
exit point `x.c - lat x.r + vert x.u + d x.n` by hand. Replace the hand-written loops with `portal_math::PairRotation`,
`portal_math::Apply` and `portal_math::MapPoint` (build a `portal_math::Frame` from each `Gate` with `SetFrame`, or add a small local
helper). Results must be bit-for-bit the same maths (same formulas, same order of operations is not required, same values within
float rounding). Do NOT touch `Step` (the human's crossing) or `HoleSkips`.
Check: build passes; the unit tests pass; `git diff` shows only those functions; a short in-game run with a prop dropped through a
floor portal still logs `portal-props: body ... crossed` and the cube still logs `portal-cube: crossed`.

## T7. Portal size as a cvar (Portal's own proportions on demand)
Files: `src/skate3_portal_gate.cpp` (`kHalfWidth`, `kHalfHeight`, every use), `src/skate3_portal_math.{h,cpp}` (`kHalfWidth`, `kHalfHeight`, `InOval`).
Goal: add `REXCVAR_DEFINE_DOUBLE(skate3_portal_gate_size, 1.0, ...)` (hot reload) in skate3_portal_gate.cpp that scales both half extents
(default 1.0 = today's 2.6 x 2.0 m). Replace the constants by functions `HalfWidth()` / `HalfHeight()` returning constant x scale, in both
files (portal_math gets a setter `SetSizeScale(float)` the gate calls each frame; its default must stay 1.0 so the unit tests still pass).
Behaviour at 1.0 must be identical. Note in the cvar help: Portal's own portal is 1.63 x 2.74 m (half 0.81 x 1.37), i.e. tall, not wide.
Check: build passes; unit tests pass; in game `--skate3_portal_gate_size=0.5` draws and crosses half-size portals.


## T8. Extract the Portal 2 characters (run on the Mac, no code change) — model: Haiku 5.5
Run: `python3 tools/re/portalgun_extract.py --character all --preview /tmp/char` (each character also prints `vertices: ...` and `-> up axis`)
Writes ~/Library/Application Support/Skate3PortalMod/{chell,atlas,pbody}.bin (cache version 2: mesh, textures, skeleton, skin weights) and
/tmp/char_<name>_{side,top,front}.png previews. Paste back: the `candidates`, `kept meshes`, `bones: N [...]`, `bones used by the skin`
and `wrote ...` lines for each character, and look at the three front previews (a standing figure, not empty or exploded).
If a character fails (model not found, "strip group layout not recognised"), paste the error: nothing in the game changes.

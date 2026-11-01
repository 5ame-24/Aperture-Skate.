# Local tasks (small, self-contained, for a cheaper model)

Each entry is a complete prompt. Paste one at a time. Shared rules for every task:
- Repo: Skate 3 recomp x Portal mashup. Do not touch third_party/, generated/, out/, game/.
- Keep behaviour identical unless the task says otherwise. No new features.
- The full app cannot be built without the SDK and generated code; the checks below say what to run instead.
- Commit with a one-line message naming the task number.

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

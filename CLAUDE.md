# Skate 3 recomp: Portal mashup (project notes for Claude)

**Resume point: read PROGRESS.md next** (current state, untested items, next actions, how to analyse runs).
Analysis scripts live in tools/re/.

## North star
Port ideas from one engine into the other, cleanroom style, by reverse engineering: **Portal mechanics inside Skate 3's
world** (Port Carverton), taken all the way: true Portal feel (momentum, walking/skating/falling through, seeing
through, objects and NPCs crossing), for creativity, chaos and real skater style. Later horizon: drivable cars with a
GTA-like feel. Right now: **Portal + Skate only**. How it *feels* matters far more than which assets are used.

## Ground rules (set by the user, 2026-10-30)
- Personal, educational experiment. The user has said licensing is not a concern for this work; do not raise it. (Writing
  our own code from understood behaviour, never pasting other people's source, is still how we work: that is what
  "cleanroom" means here.)
- Collaboration style: can-do, we are both doing this to prove it's possible and to learn how it works. Claude is onboard.
- The user tests by running the game (macOS, M1 Max, PS4 pad + keyboard/mouse) and reports; Claude cannot run the GUI.
  Prefer designs that make a single run informative (logs, built-in A/B scoring). Screenshots beat descriptions.
- The Rust/Bevy mashup in ~/Documents/rust-mashup must keep working and is not edited. Session markers (the game's own
  reset) must keep working untouched.
- Credit is limited: be efficient, state a plan on multi-file work, keep replies short, end with what was not verified.

## Build and run
```sh
cmake --build out/build/macos-release --target skate3 -j 8
cd out/build/macos-release && ./skate3 --game_data_root=<path-to>/game \
  --fullscreen=false --skate3_portal_gate=true
```
Logs: out/build/macos-release/logs/skate3_NNN.log. Native shaders are PRECOMPILED with Windows DXC into
src/native/shaders/spirv/skate3_native_shaders_spirv.h: **new HLSL cannot be added on this Mac** (run 089: "no SPIR-V
blob"). Reuse the existing precompiled pipelines (e.g. the spline ones) instead.

## How the portal gate works (files)
- `src/skate3_portal_gate.{h,cpp}`: portal frames (n, u, r), placement (`PlaceOn`), crossing test (`Step`), pair matrix,
  ImGui overlay (reticle, gun, banner, fallback rings), first-person state, gamepad filter, world surface polygons.
- `src/skate3_portal_probe.cpp`: all game hooks. `sub_82DB88C8` (PlayerInputState update) drives `GateStep`; the
  teleport itself uses the game's own relocation protocol (set bit 0x00080000 in actor+1296, hook `sub_82585CB0` to hand
  the executor our target matrix). Gun ray goes through the game's ray service (`82DB63C0` Start / `82DB6580` End).
- `src/native/skate3_native_scene*.cpp`: renderer. First-person camera = follower mode added to `UpdateFreecam`.
  Portal surfaces are drawn just before `RenderOutlineMask` with the spline pipeline (`pso_spline_darken`) and four 1x1
  colour textures (`g_r.portal_tex`, created in `EnsureFallbackTextures`).
- SDK edits (third_party/rexglue-sdk, uncommitted): `InputSystem::SetGuestPadFilter`, Esc-menu row "Portal First-Person
  View Follows", `CmdLists.Size` ImGui fix; imgui pinned locally to 1.92.9 docking.

## Controls
Mouse: aim reticle, left click blue, right click orange, middle clear. Ctrl+1/2/3 fire blue/orange/clear. Ctrl+4 first
person. Pad: hold **D-pad down** (cvar `skate3_portal_pad_modifier`) then RT blue, LT orange, Y clear, R3 first person,
right stick looks. First person auto-centres on the travel direction (`skate3_portal_fpv_autocentre`); Esc menu picks
board or rider's head.

## Key findings (what the game does)
- Teleport executor `82DB8998` always calls board reset `82C05F50` (zeroes all 14 board rates) and skeleton reset
  `82BE3508` (zeroes ~26 skeleton vectors at skeleton+0x2770..0x2900). Skeleton = *(actor+1884), board = *(actor+1880),
  board block = *(board+12) (lin rates +0x140, ang +0x1B0, stride 16).
- **The fix that works (run 082/086):** replace the board reset with `SetTransform 82C0B2C8` (keeps rates) and restore the
  skeleton's zeroed vectors (the old learner's profile C, now the only crossing path). Kept 96% of speed (n=29), 92% mean/112% median (run 086,
  n=24), no bails. Plain teleport kept 39%.
- Writing actor fields (+1200/+1216 positions, +1232 delta, +1248 velocity) causes the flicker/respawn glitch: don't.
  Exit distance, lift, airmode, waiting for a riding state, board-rate heading: no effect on kept speed.
- Player states: 100 riding, 200 air (also toggles on rough ground), 500 off-board bail, 700/702 teleporting. The
  Teleporting state ends in 100 or 500 by skeleton+10768 bit 0 (was never the problem). Flags C0000000 grounded,
  8E/90000000 air, 40000000 recovery (1-3 samples after most crossings = the small hitch).
- Ray service: object 402ECC40, vtable 82310764, queue `8276EDD0`, results `8276EE88` (run 089). Start of the collision
  investigation; the wall is solid, the portal is only a trigger plane 1.1 m in front of it.
- skate-core (GPL, in the mashup repo) documents the same native functions (`board_runtime.rs`, `teleport_state.rs`).

## Run log (newest last)
| Runs | What | Result |
|---|---|---|
| 062-064 | Gate pair, ImGui overlay, human lock, exit lift | teleport reliable; momentum mixed |
| 066-072 | Gun, 3D frames, floor/ceiling, restore/airmode | restore and airmode did nothing |
| 073-076 | Self-scoring learner over 6/4/3 setups | every knob irrelevant (~25% kept) |
| 077-079 | Actor delta/state writes | flicker glitch; stop writing actor fields |
| 082 | Board reset -> SetTransform + skeleton restore | **96% kept**; first real fix |
| 083-085 | First person + pad (D-pad down) | works; right stick conflicts with flicks -> auto-centre |
| 086 | Trigger/pad/opaque overlay | 92% mean kept, no bails, wall hits near portal edges |
| 089 | In-world portal shader | failed (no SPIR-V); reworked to spline polygons (untested) |

## Status and next steps
- Done: gun + aim, wall/floor/ceiling portals, momentum-keeping crossings, first person (mouse and pad), Esc option.
- Untested build: in-world depth-tested portal polygons (look for `portal shader` errors; overlay stands down when drawn).
- Next: (1) read the ray service / world collision offline (`8276EDD0`, `8276EE88`) for a wall cut-out; (2) see-through
  view (second camera pass; spline-style injection is the proven way into the renderer); (3) velocity turning for angled
  portals and flings (turn the restored skeleton vectors; floor portal needs a falling entry); (4) portals acting on
  props and NPCs; (5) polish: wall "magnetism" after exits, state-100 crossings keep only ~45%, Portal 2 staging folders
  (sigf/, portal2-skate-app/, outside the repo). Probe dead code removed (PROGRESS 51).
- Horizon: drivable cars with a GTA-like feel (take over a traffic car, driving model from skate-vehicles research).

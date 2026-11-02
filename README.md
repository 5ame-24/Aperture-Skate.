# Aperture Skate

**Portal's mechanics inside Skate 3.** Shoot blue and orange portals onto Port Carverton's walls, floors and ceilings, then ride, fall,
walk and fling through them with your speed intact. A personal, educational reverse-engineering experiment built on the
[Skate 3 native PC recompilation](docs/skate3-recomp-README.md) (Windows, Linux, macOS).

> Not affiliated with EA, Valve or the Skate 3 recomp authors. No game files are included: you need your own copy of Skate 3 (Xbox 360),
> and for the optional Portal 2 characters and portal gun model, your own Portal 2 install. All mod code is written from observed
> behaviour and public documentation (see [docs/specs](docs/specs)), cleanroom style.

## What works

- **Portal gun**: aim with the mouse or the right stick, blue / orange on walls, floors, ceilings and slopes; portals fit themselves onto
  their surface and never overlap; see-through portals in the native renderer.
- **Momentum ("speedy thing goes in, speedy thing comes out")**: falling into a floor portal flings you out of the other one at your fall
  speed (carry: the skater's own physics bodies are flown along the exit arc). Rolling crossings keep most of their speed.
- **First person** (Ctrl+4 / D-pad down + R3), facing kept through portals.
- **AUTOTEST** (Ctrl+Shift+T): automatic, scored drops through test portals that compare settings for you.
- Experimental: Portal 2 gels (blue bounce, orange speed), Chell / Atlas / P-body as your skater or as co-op NPCs (needs your Portal 2 files).

## What is open

Flings out of wall portals (they lose speed), on-foot momentum and Portal-style walking in first person, the characters' look, gels on the
board, listing the Portal characters in the game's own character select. Status and history: [PROGRESS.md](PROGRESS.md).

## Build

Same toolchain as the base recomp (CMake 3.25+, Ninja, Clang 18+; full details in [docs/skate3-recomp-README.md](docs/skate3-recomp-README.md)).
The build-time code generation needs your extracted game dump (`default.xex`, `data/webkit/EAWebkit.xex`): put it in `game/`, or pass
`-DSKATE3_GAME_DATA_ROOT=/path/to/game`.

```sh
git clone --recursive <repo-url> && cd skate3recomp
cmake --preset macos-release -DSKATE3_GAME_DATA_ROOT="$PWD/game"     # or relwithdebinfo / linux-release / release
cmake --build out/build/macos-release --target generate-all --parallel
cmake --preset macos-release -DSKATE3_GAME_DATA_ROOT="$PWD/game"     # again, so CMake sees the generated sources
cmake --build out/build/macos-release --target skate3 -j 8
```

## Run

```sh
cd out/build/macos-release
./skate3 --game_data_root=/path/to/game --fullscreen=false --skate3_portal_gate=true
```

| Action | Mouse / keyboard | Pad (hold D-pad down) |
|---|---|---|
| Blue / orange portal | left / right click (Ctrl+1 / Ctrl+2) | RT / LT |
| Clear portals | middle click (Ctrl+3) | Y |
| First person | Ctrl+4 | R3 |
| Fling mode (carry, ollie, ...) | Ctrl+F | |
| AUTOTEST | Ctrl+Shift+T | |
| Gel mode / Portal characters panel | Ctrl+H / Ctrl+Shift+C | B |

Optional Portal 2 assets (portal gun model, characters), from your own install: `python3 tools/re/portalgun_extract.py --character all`.

## Contribute

See [CONTRIBUTING.md](CONTRIBUTING.md): the test loop (docs/handoff-loop.md), the unit tests, and the log tools in `tools/re/`.
Roadmap: the "Next" items in [PROGRESS.md](PROGRESS.md); later, drivable cars with a GTA-like feel.

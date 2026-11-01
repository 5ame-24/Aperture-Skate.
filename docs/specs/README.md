# Specs (analysis side -> implementation side)

Cleanroom channel. The analysis side (with the game files and the generated code) writes specs here; the implementation side writes
code only from these specs, PROGRESS.md findings and its own tests. One file per topic: `NNN-short-name.md`.

Each spec states:
1. **Behaviour**: what the game does, in prose or pseudocode (no pasted decompiled/generated code, no Valve/EA source).
2. **Hook point**: guest address + signature in terms of registers/arguments and what they point to (e.g. `82DB88C8(r3 = actor)`).
3. **Data**: struct offsets, types, units, endianness (guest memory is big-endian), valid ranges.
4. **Numbers**: measured values with the run/log that measured them.
5. **Evidence**: run number, log lines or probe output that showed it.
6. **Unknowns**: what is still unverified.

Requests from the implementation side are in `docs/spec-requests.md`; a spec that answers one names it (`answers R3`).

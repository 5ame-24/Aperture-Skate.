"""Summarise the experimental leaps in a game log: python3 tools/re/leaplog.py [log] (default: newest log).

NPCX (skate3_portal_npc): actors seen, crossings armed/refused, whether the board reset was swapped, and where each crossed NPC was
10..60 frames later (distance from the exit, speed vs entry speed).
PCAM (skate3_portal_camcarry): carries started/done/cancelled, waits for the game camera, and the largest frame-to-frame camera move
(shown vs game): a smooth carry has a small "shown" number while the game camera's own jump is large.
"""
import collections, glob, os, re, sys

root = os.path.join(os.path.dirname(__file__), "..", "..", "out", "build", "macos-release", "logs")
path = sys.argv[1] if len(sys.argv) > 1 else max(glob.glob(os.path.join(root, "skate3_*.log")), key=os.path.getmtime)
print(path)
seen, crossings, swaps, noswap, follows = set(), [], 0, 0, collections.defaultdict(list)
pcam = collections.Counter()
carries = []
for line in open(path, errors="replace"):
    if "NPCX" in line:
        m = re.search(r"NPCX actor (\w+) seen", line)
        if m:
            seen.add(m.group(1))
        m = re.search(r"NPCX actor (\w+) crossed the (\w+) portal \(#(\d+)\).*state (\d+) flags (\w+).*teleport (\w+)", line)
        if m:
            crossings.append(m.groups())
        if "board reset replaced by SetTransform" in line:
            swaps += 1
        if "did not reset the board" in line:
            noswap += 1
        m = re.search(r"NPCX actor (\w+) after crossing t\+(\d+).*?([\d.]+) m from the exit, speed ([\d.]+) m/s \(in ([\d.]+)\)", line)
        if m:
            follows[m.group(1)].append((int(m.group(2)), float(m.group(3)), float(m.group(4)), float(m.group(5))))
    if "PCAM" in line:
        for key in ("carry started", "carry done", "carry cancelled", "holding the carried pose", "no camera pose recorded",
                    "never showed up", "first person turned"):
            if key in line:
                pcam[key] += 1
        m = re.search(r"carry done .*after (\d+) ms, (\d+) frames, waited (-?\d+) ms.*shown camera ([\d.]+) m, game camera ([\d.]+) m", line)
        if m:
            carries.append(tuple(float(x) for x in m.groups()))

print(f"NPCX: {len(seen)} actors seen, {len(crossings)} crossings "
      f"({sum(c[5] == 'armed' for c in crossings)} armed, {sum(c[5] != 'armed' for c in crossings)} refused), "
      f"board reset swapped {swaps}, not swapped {noswap}")
for actor, rows in follows.items():
    last = rows[-1]
    print(f"  {actor}: t+{last[0]} {last[1]:.1f} m from the exit, speed {last[2]:.1f} (in {last[3]:.1f})"
          f"{'  <- drifted back?' if last[1] > 6 else ''}")
print("PCAM:", dict(pcam) if pcam else "no lines")
for c in carries:
    print(f"  carry {c[0]:.0f} ms / {c[1]:.0f} frames, waited {c[2]:.0f} ms, max move shown {c[3]:.2f} m vs game {c[4]:.2f} m")

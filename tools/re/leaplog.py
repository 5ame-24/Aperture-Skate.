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
linked, notsent = set(), 0
other = collections.Counter()
pcam = collections.Counter()
carries = []
for line in open(path, errors="replace"):
    line = re.sub(r"\[f \d+\] ", "", line)  # frame stamps on FPVTRACK / FLING lines (HANDOFF 6)
    if "NPCX" in line:
        if "NPCX actor" in line and "linked to skater entity" in line:
            linked.add(re.search(r"NPCX actor (\w+)", line).group(1))
        if "not sent" in line:
            notsent += 1
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
    for tag in ("FLING armed", "FLING not armed", "topped up", "FLING +", "EXITFIX", "portal-props: CHAR", "HOLE kept solid", "sinking through the opening", "CHARS ",
                "AIR arc", "AIR arc too short", "AIR gravity estimate", "AIR skeleton carriers learned", "AIR off-board updates", "also topped up",
                "FPVTRACK re-acquired", "FPVTRACK lost", "FPVTRACK human entity locked", "unlocked", "FPVTRACK locked entity", "FPVTRACK refused",
                "FPVTRACK ignored", "FPVTRACK accepted", "FLING not armed: crossing off the board",
                "GEL mode ON", "GEL painted", "GEL cleared", "GEL shot hit nothing", "GEL bounce on blue", "bounce held by FLING", "result: rose",
                "never left the ground", "on orange", "left orange", "GEL prop", "GEL skipped: on foot", "shot went into the", "GEL drawn in the world", "AIR loaded", "AIR no saved", "CHAR you:", "CHAR co-op", "follows the NPC", "own skeleton", "no skeleton in the cache",
                "skeleton now draws", "CHAR slot", "reset (", "CHARSEL",
                "lift armed", "lift done", "lift skipped", "lift refused", "boost: cached speed", ": held", "overwritten", "riding: lift", "capped", "dropper placed", "gun sub-mode", "went through the", "blob paint #", "droppers cleared", "set to", "(panel)", "building (", "GEL key Ctrl+Shift+H", "blob ray", "GEL mode ON", "DROPPER", "pointer pass", " place ", "candidate text IDs", "hold one of its text IDs", "LOOKS LIKE A LIST", "started from the key", "PORTALFIT", "fits", "best effort", "does not fit", "pushed",
                "rolling crossing: wall exit lifted", "FACING", "exit facing from your facing", "exit heading from your last heading", "CROSSCHECK", "exit facing from the velocity", "FACING [f", "locked:", "CHARSEL dump"):
        if tag in line:
            other[tag] += 1
    if "PCAM" in line:
        for key in ("carry started", "carry done", "carry cancelled", "holding the carried pose", "no camera pose recorded",
                    "never showed up", "first person turned", "carry superseded"):
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
print(f"NPCX: {len(linked)} actors linked to an entity, {notsent} 'reached a portal but was not sent'")
print("PCAM:", {k: v for k, v in pcam.items() if v} or "no lines")
print("other:", dict(other) if other else "no lines")
rises = [(float(m.group(1)), float(m.group(2))) for m in (re.search(r"result: rose (-?[\d.]+) m \(expected ([\d.]+) m\)", l)
                                                          for l in open(path, errors="replace")) if m]
if rises:
    print("GEL bounce heights (rose / expected m):", ", ".join(f"{a:.2f}/{b:.2f}" for a, b in rises),
          f"-> mean {sum(a for a, _ in rises) / len(rises):.2f} of {sum(b for _, b in rises) / len(rises):.2f}")
boosts = [tuple(float(x) for x in m.groups()) for m in (re.search(r"left orange #\d+ after ([\d.]+) s: speed in ([\d.]+), peak ([\d.]+), out ([\d.]+)", l)
                                                         for l in open(path, errors="replace")) if m]
for l in open(path, errors="replace"):
    if "own skeleton" in l and "mapped by their own vertices" in l or "loaded the" in l and "model from" in l:
        print(" ", l.split("portal-", 1)[-1].strip()[:220])
for l in open(path, errors="replace"):
    if "CHARSEL" in l and ("places in guest memory" in l or "looks like a table" in l or "LOOKS LIKE A LIST" in l or "text IDs" in l):
        print(" ", l.split("portal-probe: ", 1)[-1].strip()[:240])
for b in boosts:
    print(f"  orange ride {b[0]:.2f} s: in {b[1]:.1f}, peak {b[2]:.1f}, out {b[3]:.1f} m/s")
for c in carries:
    print(f"  carry {c[0]:.0f} ms / {c[1]:.0f} frames, waited {c[2]:.0f} ms, max move shown {c[3]:.2f} m vs game {c[4]:.2f} m")

"""Answer spec request R5 from a game log: python3 tools/re/airlog.py [log] (default: newest log).

Reads the AIR lines (skate3_portal_airprobe): per clean airborne arc, the gravity fitted to the skater's path and the velocity stores ranked
by RMS error against the measured velocity. Prints the gravity (median, spread), how often each store ranked first, and a draft spec
paragraph to check and paste into docs/specs/.
"""
import collections, glob, os, re, statistics as st, sys

root = os.path.join(os.path.dirname(__file__), "..", "..", "out", "build", "macos-release", "logs")
path = sys.argv[1] if len(sys.argv) > 1 else max(glob.glob(os.path.join(root, "skate3_*.log")), key=os.path.getmtime)
print(path)
arcs, best, rms = {}, collections.Counter(), collections.defaultdict(list)
gstore = collections.defaultdict(list)
for line in open(path, errors="replace"):
    m = re.search(r"AIR arc #(\d+) \((.*?)\): ([\d.]+) s, (\d+) samples, start speed ([\d.]+) m/s; gravity from the path (-?[\d.]+) m/s\^2 \(r2 ([\d.]+)\)(.*)", line)
    if m:
        arcs[int(m.group(1))] = dict(why=m.group(2), dur=float(m.group(3)), n=int(m.group(4)), g=float(m.group(6)),
                                     r2=float(m.group(7)), used="not used" not in m.group(8))
        continue
    m = re.search(r"AIR arc #(\d+) carriers \(best first\): (.*)", line)
    if m:
        a = int(m.group(1))
        entries = re.findall(r"([\w+]+(?: rates)?|actor\+1248) rms ([\d.]+) m/s g (-?[\d.]+)", m.group(2))
        if a in arcs and arcs[a]["used"] and entries:
            best[entries[0][0]] += 1
            for name, r, g in entries:
                rms[name].append(float(r))
                gstore[name].append(float(g))
used = [a for a in arcs.values() if a["used"]]
print(f"{len(arcs)} arcs logged, {len(used)} usable")
if not used:
    print("No usable arcs: do 5+ clean jumps/drops (no portals) with skate3_portal_airprobe on.")
    sys.exit(0)
g = [a["g"] for a in used]
print(f"gravity from the path: median {st.median(g):.2f} m/s^2, min {min(g):.2f}, max {max(g):.2f}, n {len(g)}")
for name, c in best.most_common():
    print(f"  ranked first in {c} of {len(used)} arcs: {name}  (median rms {st.median(rms[name]):.2f} m/s, median g from its v_y {st.median(gstore[name]):.2f})")
for name in ("board rates", "actor+1248"):
    if name in rms and name not in best:
        print(f"  {name}: never first (median rms {st.median(rms[name]):.2f} m/s, median g {st.median(gstore[name]):.2f})")
top = best.most_common(1)[0][0]
print("\nDraft spec (check against the numbers before pasting into docs/specs/NNN-airborne-velocity.md, answers R5):")
print(f"- Gravity while airborne (state 200): {st.median(g):.2f} m/s^2 (median of {len(g)} clean arcs fitted to the skater's path, "
      f"range {min(g):.2f}..{max(g):.2f}), log {os.path.basename(path)}.")
print(f"- The store that tracks the airborne velocity best: {top} (first in {best[top]} of {len(used)} arcs, median RMS "
      f"{st.median(rms[top]):.2f} m/s against the velocity measured from the path).")
print("- Unknowns: whether the game integrates from that store or only mirrors it; the drag/air-control terms (look at its g vs the path's).")

"""Summarise portal crossings in a game log: python3 tools/re/crossings.py [log] (default: newest log).

Prints each crossing with the profile used, player state at the crossing, entry speed and the share of speed kept (mean
speed 15-30 frames later / entry speed, from the 'GATE speed after crossing' lines), plus bail/recovery counts.
"""
import glob, os, re, statistics as st, sys

root = os.path.join(os.path.dirname(__file__), "..", "..", "out", "build", "macos-release", "logs")
path = sys.argv[1] if len(sys.argv) > 1 else max(glob.glob(os.path.join(root, "skate3_*.log")), key=os.path.getmtime)
print(path)
rows, cur = [], None
for line in open(path, errors="replace"):
    m = re.search(r"GATE crossing with flags (\w+) \((\w+)\), player state (\d+), speed ([\d.]+)", line)
    if m:
        cur = dict(mode=m.group(2), state=m.group(3), speed=float(m.group(4)), after=[], bail=0, rec=0)
        rows.append(cur)
        continue
    m = re.search(r"GATE speed after crossing: ([\d.]+) m/s \(before [\d.]+\) flags (\w+) state (\d+)", line)
    if m and cur is not None:
        cur["after"].append(float(m.group(1)))
        cur["bail"] += m.group(3) == "500"
        cur["rec"] += m.group(2) == "40000000"
scored = [r for r in rows if 3 <= r["speed"] < 20 and len(r["after"]) >= 8]
for r in rows:
    kept = st.mean(r["after"][4:8]) / r["speed"] * 100 if r in scored else None
    print(f"{r['mode']:9} state {r['state']} speed {r['speed']:5.1f} kept {'-' if kept is None else f'{kept:4.0f}%'} "
          f"bail-samples {r['bail']} recovery-samples {r['rec']}")
if scored:
    k = [st.mean(r["after"][4:8]) / r["speed"] for r in scored]
    print(f"{len(rows)} crossings, {len(scored)} scored: mean kept {100*st.mean(k):.0f}% median {100*st.median(k):.0f}%")
    for mode in ("riding", "airborne"):
        km = [st.mean(r["after"][4:8]) / r["speed"] for r in scored if r["mode"] == mode]
        if km:
            print(f"  {mode}: {len(km)} scored, mean kept {100*st.mean(km):.0f}% median {100*st.median(km):.0f}%"
                  f"{'  (rolling crossings: the wall-exit lift, PROGRESS 69)' if mode == 'riding' else ''}")
# CROSSCHECK (PROGRESS 71): per kind of pair, speed kept and heading errors at +10 / +20 updates
import collections
kinds = {}
by = collections.defaultdict(list)
for line in open(path, errors="replace"):
    m = re.search(r"CROSSCHECK #(\d+) \[f \d+\] (\S+) (rolling|airborne):", line)
    if m:
        kinds[m.group(1)] = (m.group(2), m.group(3))
        continue
    m = re.search(r"CROSSCHECK #(\d+) (\S+) \+(\d+): speed [\d.]+ of [\d.]+ m/s \((\d+)%\), heading off the expected exit by (n/a|[\d.]+ deg), "
                  r"off the facing given by (n/a|[\d.]+ deg), vertical (-?[\d.]+) \(expected (-?[\d.]+)\)", line)
    if m:
        h = None if m.group(5) == "n/a" else float(m.group(5).split()[0])
        f = None if m.group(6) == "n/a" else float(m.group(6).split()[0])
        by[(m.group(2), int(m.group(3)) >= 20)].append((int(m.group(4)), h, f, float(m.group(7)), float(m.group(8))))
if by:
    print("CROSSCHECK by pair (at +10 and +20 updates): n, speed kept median, heading error median, facing error median, vertical - expected median")
    for (kind, late), rows in sorted(by.items()):
        hs = [r[1] for r in rows if r[1] is not None]
        fs = [r[2] for r in rows if r[2] is not None]
        print(f"  {kind:16} {'+20' if late else '+10'}: n {len(rows):3}, kept {st.median(r[0] for r in rows):4.0f}%, heading off "
              f"{st.median(hs) if hs else float('nan'):5.1f} deg, facing off {st.median(fs) if fs else float('nan'):5.1f} deg, "
              f"vertical error {st.median(r[3] - r[4] for r in rows):+5.1f} m/s")

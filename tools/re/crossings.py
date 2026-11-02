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

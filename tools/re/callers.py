"""Reverse call graph of the generated guest code: callers.py DIR DEPTH ADDR [ADDR...]"""
import re, sys, glob, collections

root, depth, targets = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
define = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)", re.M)
call = re.compile(r"\bsub_([0-9A-F]{8})\(ctx, base\)")
callers = collections.defaultdict(set)
for path in sorted(glob.glob(root + "/skate3_recomp.*.cpp")):
    text = open(path, errors="replace").read()
    heads = list(define.finditer(text))
    for i, m in enumerate(heads):
        end = heads[i + 1].start() if i + 1 < len(heads) else len(text)
        for callee in set(call.findall(text[m.end():end])):
            callers[callee].add(m.group(1))
seen = set()
def walk(addr, level):
    if level > depth:
        return
    ups = sorted(callers.get(addr, ()))
    print("  " * level + "sub_%s  <- %s" % (addr, " ".join(ups) if ups else "(no direct callers)"))
    for up in ups:
        if up not in seen:
            seen.add(up)
            walk(up, level + 1)
for t in targets:
    seen.add(t)
    walk(t, 0)

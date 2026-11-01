"""List the direct callees of generated guest functions: callees.py DIR ADDR [ADDR...]"""
import re, sys, glob, collections

root, addrs = sys.argv[1], sys.argv[2:]
defn = {a: re.compile(r"^DEFINE_REX_FUNC\(sub_%s\)" % a, re.M) for a in addrs}
nxt = re.compile(r"^DEFINE_REX_FUNC\(", re.M)
call = re.compile(r"\bsub_([0-9A-F]{8})\(ctx, base\)")
found = {}
for path in sorted(glob.glob(root + "/skate3_recomp.*.cpp")):
    text = open(path, errors="replace").read()
    for a, rx in defn.items():
        if a in found:
            continue
        m = rx.search(text)
        if m:
            n = nxt.search(text, m.end())
            body = text[m.start(): n.start() if n else len(text)]
            found[a] = (path.split("/")[-1], body)
for a in addrs:
    if a not in found:
        print("sub_%s: definition not found" % a)
        continue
    f, body = found[a]
    calls = collections.Counter(call.findall(body))
    print("sub_%s in %s: %d lines, %d direct calls" % (a, f, body.count("\n"), sum(calls.values())))
    print("   ", " ".join("%s%s" % (k, "x%d" % v if v > 1 else "") for k, v in sorted(calls.items())))

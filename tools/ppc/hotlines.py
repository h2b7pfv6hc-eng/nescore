# Aggregates qemu hotblocks output by function (inlined frames included).
import subprocess, sys, collections
binary, hot = sys.argv[1], sys.argv[2]
rows = []
for line in open(hot):
    p = line.strip().split(",")
    if len(p) == 4 and p[0].startswith("0x"):
        rows.append((int(p[0], 16), int(p[2]), int(p[3])))
addrs = "\n".join(hex(a) for a, _, _ in rows)
out = subprocess.run(["llvm-symbolizer", "--obj=" + binary, "--inlining=false", "--functions=linkage"],
                     input=addrs, capture_output=True, text=True).stdout.strip().split("\n\n")
byfn = collections.Counter(); byline = collections.Counter()
total = 0
for (a, ic, ec), sym in zip(rows, out):
    lines = sym.strip().split("\n")
    w = ic * ec
    total += w
    byfn[lines[0]] += w
    byline[lines[1].split("/")[-1] if len(lines) > 1 else "?"] += w
print("total insns", total)
for f, w in byfn.most_common(15): print(f"{100*w/total:5.1f}%  {f}")
print()
for f, w in byline.most_common(25): print(f"{100*w/total:5.1f}%  {f}")

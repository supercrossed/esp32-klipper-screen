"""Average several low-current discharge curves into one voltage -> % table.
Each cell's % is relative to its own capacity between full and 3.0 V (the
lowest cut-off the firmware offers), so cells of any size count equally."""
import csv, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from curve import load

CELLS = [
    ("LG HG2 3.0Ah (NMC)",            "cells/hg2_c20.csv",   8, 9, 11, 30),
    ("Samsung 30T 3.0Ah 21700 (NMC)", "cells/s30t_c20.csv",  8, 9, 11, 30),
    ("Panasonic NCR18650PF 2.9Ah (NCA)", "cells/pf_dis.csv", 0, 1, 2, 0),
    ("Samsung INR18650-20R 2.0Ah (NMC)", "cells/sp20r_dis.csv", 0, 1, 2, 0),
]
FLOOR = 3.0

def v_at(rows, used):
    """voltage after `used` Ah (rows sorted by Ah), linear interpolation"""
    for (a0, v0, _), (a1, v1, _) in zip(rows, rows[1:]):
        if a1 >= used:
            return v0 if a1 == a0 else v0 + (v1 - v0) * (used - a0) / (a1 - a0)
    return rows[-1][1]

per = {}
for name, path, vc, ic, ac, skip in CELLS:
    rows = sorted(load(path, vc, ic, ac, skip))
    end = next(a for a, v, _ in rows if v <= FLOOR)          # capacity down to 3.0 V
    per[name] = {pct: v_at(rows, end * (100 - pct) / 100) for pct in range(100, -1, -5)}
    print(f"{name:34s} {end:.2f} Ah to {FLOOR} V")

print("\n  %   " + "  ".join(n.split()[1][:6].rjust(6) for n in per) + "    avg")
avg = {}
for pct in range(100, -1, -5):
    vs = [per[n][pct] for n in per]
    avg[pct] = sum(vs) / len(vs)
    print(f"{pct:3d}%  " + "  ".join(f"{v:6.3f}" for v in vs) + f"  {avg[pct]:6.3f}  spread {1000*(max(vs)-min(vs)):3.0f} mV")

print("\nC table:")
pts = [(round(avg[p] * 1000), p) for p in range(100, -1, -5)]
pts[0] = (4180, 100)    # full at rest after charge (curves start ~4.17-4.18)
pts[-1] = (3000, 0)
print(",\n".join("    " + ", ".join(f"{{ {mv}, {p} }}" for mv, p in pts[i:i + 7]) for i in range(0, len(pts), 7)))

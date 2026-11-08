"""Voltage vs remaining charge from a constant-current discharge log.
usage: python curve.py file.csv [vcol] [acol] [ahcol] [skip_rows]
Prints the voltage at 100, 95, ... 0 % remaining (by capacity)."""
import sys, csv

def load(path, vcol, icol, ahcol, skip):
    rows = []
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        r = csv.reader(f)
        for i, row in enumerate(r):
            if i < skip:
                continue
            try:
                v, a, ah = float(row[vcol]), float(row[icol]), float(row[ahcol])
            except (ValueError, IndexError):
                continue
            if a < -0.01:            # discharging
                rows.append((abs(ah), v, abs(a)))
    return rows

def curve(rows, step=5):
    rows.sort()
    total = rows[-1][0]
    out = {}
    for pct in range(100, -1, -step):
        used = total * (100 - pct) / 100
        # nearest sample
        best = min(rows, key=lambda r: abs(r[0] - used))
        out[pct] = best[1]
    return total, out

if __name__ == '__main__':
    p = sys.argv[1]
    vcol, icol, ahcol, skip = (int(x) for x in sys.argv[2:6])
    rows = load(p, vcol, icol, ahcol, skip)
    total, c = curve(rows)
    cur = sum(r[2] for r in rows) / len(rows)
    print(f"{p}: {len(rows)} samples, {total:.3f} Ah at {cur*1000:.0f} mA")
    print(" ".join(f"{k}:{v:.3f}" for k, v in c.items()))

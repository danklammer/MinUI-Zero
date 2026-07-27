#!/usr/bin/env python3
"""Turn a tools/mmp-autotest.sh artifact dir into screenshots + a bench verdict.

  python3 tools/mmp-autotest-report.py .notes/mmp-build/autotest-<ts>/

Framebuffers: MMP fbdev is 640x480 ARGB8888 with the panel mounted inverted, so raw pages are
converted BGRA->RGB and rotated 180.

Bench CSVs (telemetry.c): per-window p50/p95/p99/max work-us, over-budget count, clock, temp.
The OC question is answered by p95_work vs the frame budget at the pinned ceiling:
  needed_clock = ceil_khz * p95 / budget.
If that lands within a plausible MPLL overclock (~1500-1600 MHz), OC could close the gap;
if it is far beyond, OC cannot save the title and the cap stands.
"""
import csv, os, sys

W, H, BPP = 640, 480, 4
BUDGET_DEFAULT = 16667

def convert_fb(path):
    try:
        from PIL import Image
    except ImportError:
        print(f"  (PIL missing, skipping {os.path.basename(path)})")
        return
    raw = open(path, 'rb').read()
    if len(raw) < W * H * BPP:
        print(f"  {os.path.basename(path)}: short read ({len(raw)} bytes), skipped")
        return
    img = Image.frombytes('RGBA', (W, H), raw[:W * H * BPP], 'raw', 'BGRA')
    img = img.convert('RGB').rotate(180)
    out = path.replace('.fb', '.png')
    img.save(out)
    # a screenshot that is one flat colour is evidence of a blank/hung screen, worth flagging
    colors = img.getcolors(2)
    flat = ' (FLAT — blank screen?)' if colors and len(colors) == 1 else ''
    print(f"  {os.path.basename(out)}{flat}")

def analyze_csv(path, budget=BUDGET_DEFAULT):
    rows = []
    with open(path) as f:
        for r in csv.reader(f):
            if not r or r[0].startswith('#') or r[0] == 'frame':
                continue
            try:
                rows.append({
                    'n': int(r[1]), 'p50': int(r[2]), 'p95': int(r[3]), 'p99': int(r[4]),
                    'over': int(r[6]),
                    'temp': int(r[7]) if r[7] else -1,
                    'khz': int(r[8]) if r[8] else -1,
                    'under': int(r[13]) if len(r) > 13 and r[13] else 0,
                })
            except (ValueError, IndexError):
                continue
    if not rows:
        print(f"  {os.path.basename(path)}: no data rows")
        return
    frames = sum(r['n'] for r in rows)
    over = sum(r['over'] for r in rows)
    unders = sum(r['under'] for r in rows)
    p95s = sorted(r['p95'] for r in rows)
    p95_typ = p95s[len(p95s) // 2]           # median window p95: sustained load
    p95_hot = p95s[int(len(p95s) * 0.9)]     # 90th pct window p95: the heavy scenes
    khz = [r['khz'] for r in rows if r['khz'] > 0]
    temps = [r['temp'] for r in rows if r['temp'] > 0]
    name = os.path.basename(path)
    print(f"  {name}: {frames} frames over {len(rows)} windows")
    print(f"    over-budget: {over} ({100.0 * over / frames:.1f}%)   audio underruns: {unders}")
    if khz:
        print(f"    clock: min {min(khz) // 1000} / max {max(khz) // 1000} MHz"
              + (f"   temp max {max(temps)}C" if temps else ""))
    print(f"    p95 work: typical {p95_typ}us, heavy {p95_hot}us (budget {budget}us)")
    if khz and max(khz) > 0:
        ceil = max(khz)
        need_typ = ceil * p95_typ / budget / 1000.0
        need_hot = ceil * p95_hot / budget / 1000.0
        print(f"    clock needed to hold rate: typical ~{need_typ:.0f} MHz, heavy scenes ~{need_hot:.0f} MHz")

def main():
    art = sys.argv[1] if len(sys.argv) > 1 else '.'
    print("== screenshots ==")
    for f in sorted(os.listdir(art)):
        if f.endswith('.fb'):
            convert_fb(os.path.join(art, f))
    print("== bench ==")
    for f in sorted(os.listdir(art)):
        if f.startswith('bench-') and f.endswith('.csv'):
            # 60fps budget for everything we bench here (Celeste is an _update60 cart)
            analyze_csv(os.path.join(art, f), BUDGET_DEFAULT)

if __name__ == '__main__':
    main()

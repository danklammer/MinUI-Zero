#!/usr/bin/env python3
"""Analyze benchmark telemetry CSVs (from telemetry.c) and A/B-compare two runs.

Headline metric: energy per correctly-presented frame (mJ/frame), always reported next to
frame-time percentiles, drop rate, and thermals so a "cooler but janky" regression can't hide.

Usage:
  bench-analyze.py run.csv                 # summarize one run
  bench-analyze.py before.csv after.csv    # A/B diff + release-gate verdict
"""
import sys, csv, statistics


def _num(x):
    x = (x or "").strip()
    if x == "":
        return None
    try:
        return float(x)
    except ValueError:
        return None


def load(path):
    rows, budget_us, total_frames, over = [], None, None, None
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                for tok in line[1:].split():
                    k, _, v = tok.partition("=")
                    if k == "budget_us":
                        budget_us = int(v)
                    elif k == "total_frames":
                        total_frames = int(v)
                    elif k == "over_budget":
                        over = int(v)
                continue
            if line.startswith("frame,"):  # header
                cols = line.strip().split(",")
                continue
            parts = line.rstrip("\n").split(",")
            if not parts or not parts[0].isdigit():
                continue
            rows.append(dict(zip(cols, parts)))
    return rows, budget_us, total_frames, over


def summarize(path):
    rows, budget_us, total_frames, over = load(path)
    if not rows:
        raise SystemExit(f"{path}: no data rows")
    fps = 1e6 / budget_us if budget_us else None

    def col(name):
        return [v for v in (_num(r.get(name)) for r in rows) if v is not None]

    p50 = col("p50_us"); p95 = col("p95_us"); p99 = col("p99_us")
    temp = col("temp_c"); power = col("power_mw")

    m = {
        "path": path,
        "windows": len(rows),
        "total_frames": total_frames,
        "fps": fps,
        # typical/worst frame work (us): median window p50, median window p95, worst window p99
        "work_p50_us": statistics.median(p50) if p50 else None,
        "work_p95_us": statistics.median(p95) if p95 else None,
        "work_p99_worst_us": max(p99) if p99 else None,
        "drop_rate": (over / total_frames) if (over is not None and total_frames) else None,
        "temp_start": temp[0] if temp else None,
        "temp_peak": max(temp) if temp else None,
        "temp_end": temp[-1] if temp else None,
        "temp_delta": (temp[-1] - temp[0]) if temp else None,
        "power_mean_mw": statistics.mean(power) if power else None,
    }
    m["mj_per_frame"] = (m["power_mean_mw"] / fps) if (m["power_mean_mw"] and fps) else None
    return m


def fmt(v, suf="", nd=1):
    return "n/a" if v is None else (f"{v:.{nd}f}{suf}" if isinstance(v, float) else f"{v}{suf}")


def show(m):
    print(f"== {m['path']} ==")
    print(f"  windows={m['windows']} frames={fmt(m['total_frames'])} fps={fmt(m['fps'])}")
    budget = (1e6 / m['fps']) if m['fps'] else None
    print(f"  frame work:  p50={fmt(m['work_p50_us'],'us',0)}  p95={fmt(m['work_p95_us'],'us',0)}"
          f"  p99(worst)={fmt(m['work_p99_worst_us'],'us',0)}  budget={fmt(budget,'us',0)}")
    dr = m['drop_rate']
    print(f"  drop rate:   {fmt(dr*100,'%',2) if dr is not None else 'n/a'}")
    print(f"  thermals:    start={fmt(m['temp_start'],'C',0)} peak={fmt(m['temp_peak'],'C',0)}"
          f" end={fmt(m['temp_end'],'C',0)} Δ={fmt(m['temp_delta'],'C',0)}")
    print(f"  power:       mean={fmt(m['power_mean_mw'],'mW')}   **mJ/frame={fmt(m['mj_per_frame'])}**")


def diff(a, b):
    show(a); show(b)
    print("== A/B (after vs before) ==")
    gates_ok = True

    def cmp(label, key, lower_is_better=True, tol=0.0):
        nonlocal gates_ok
        va, vb = a.get(key), b.get(key)
        if va is None or vb is None:
            print(f"  {label}: n/a"); return
        d = vb - va
        better = (d < -tol) if lower_is_better else (d > tol)
        worse = (d > tol) if lower_is_better else (d < -tol)
        tag = "better" if better else ("WORSE" if worse else "same")
        if worse:
            gates_ok = False
        print(f"  {label}: {va:.1f} -> {vb:.1f}  (Δ{d:+.1f}) {tag}")

    cmp("frame p95 (us)", "work_p95_us")
    cmp("frame p99 worst (us)", "work_p99_worst_us")
    cmp("drop rate", "drop_rate")
    cmp("peak temp (C)", "temp_peak")
    cmp("mJ/frame", "mj_per_frame")
    print(f"\n  RELEASE GATE: {'PASS — no regressions' if gates_ok else 'FAIL — a metric got worse'}")
    return 0 if gates_ok else 1


def main(argv):
    if len(argv) == 2:
        show(summarize(argv[1])); return 0
    if len(argv) == 3:
        return diff(summarize(argv[1]), summarize(argv[2]))
    print(__doc__); return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

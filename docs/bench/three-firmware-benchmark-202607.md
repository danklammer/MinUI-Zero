# Three-firmware benchmark — TrimUI Brick, July 2026

A measured comparison of MinUI Zero v1.3-rc (20260710), stock MinUI v20251127-1, and
NextUI v6.12.0 (20260709) on one TrimUI Brick (Allwinner A133P). This document
consolidates and supersedes the numbers in three-firmware-real-20260710.md and its
2026-07-11 addendum; where they disagree, this document is correct (two figures were
revised after re-verification against the raw logs — see "Corrections" at the end).

## Setup

- One 64GB SD card, MBR, one partition per firmware, each installed untouched from its
  official release zip and booted through its own unmodified boot chain (a chooser on a
  separate partition selects which partition becomes /mnt/SDCARD before handoff).
- Identical 13-game library on every partition. Games were launched through each
  firmware's own menu protocol (/tmp/next), so per-game clock policy is exactly what a
  user gets.
- Every cell: 8 minutes of attract mode, sampled every 20 s over SSH (temp, clock,
  governor, battery, load; day-2 cells also log the fuel-gauge coulomb counter).
  Statistics below use the final 5 minutes of each cell.
- Equal conditions per cell: WiFi on, screen on, identical forged L3 keep-alive inputs,
  battery discharging. Battery-saving cells were re-run at 85–100% charge after a first
  attempt on a nearly-empty battery read 3–4°C hot (archived as *-lowbatt.csv).

## Results — shipped defaults

| Game | Zero default | stock default | NextUI auto (default) |
|---|---|---|---|
| Zelda DX (GBC) | 408 flat · 35.6°C | 1200 flat · **33.6°C** | modal 1800 (1416–1800) · 36.4°C |
| Yoshi's Island (SuperFX) | modal 600 (600–1416) · 38.0°C | 1608 flat · 38.1°C | modal 1800 · 40.0°C (43.1 peak) |
| THPS2 (PS1) | modal 1008 (600–1200) · **34.9°C** | 1608 flat · 38.4°C | modal 1800 · 38.1°C |
| Bloody Roar II (PS1 FMV) | modal 1008 (bursts to 1800) · **37.7°C** (44.2 peak) | 1608 flat · 40.9°C (44.5 peak) | modal 1800 · 39.0°C (43.2 peak) |

Notes, both directions:
- Zero posts the lowest clock in every row and the lowest temperature on the two PS1
  rows. Its clock is also the only one that varies with content.
- Stock's fixed 1200 for GBC is the **coolest Zelda cell in the whole dataset** — at
  featherweight loads the display dominates power draw and a lower CPU clock does not
  imply a lower device temperature. Zero's advantage is negligible-to-absent on such
  content; it materializes on heavy games.
- NextUI's auto (schedutil 408–1800) sits at or near its ceiling for all four games,
  including GBC. Its stack also carries three background daemons and a GL pipeline
  (~+1.0 loadavg vs the MinUI-family stacks), so its utilization-driven scaler sees a
  busier system.

## Results — battery-saving modes

Both competitors ship user-selectable battery modes; Zero does not (its default governor
is its only mode). Modes were engaged through each firmware's own mechanism.

| Game | stock Powersave (fixed 1200) | NextUI Powersave (conservative ≤1200) |
|---|---|---|
| Zelda DX | 1200 · 36.7°C | modal 600 (408–600) · **33.1°C** |
| Yoshi's Island | 1200 · 39.2°C | 1200 · 37.3°C |
| THPS2 | 1200 · 39.3°C | modal 816 (816–1008) · 37.7°C |
| Bloody Roar II | 1200 · 41.0°C | modal 816 (600–816) · 38.7°C |

NextUI's Powersave posts the coolest cell of the entire dataset on Zelda (33.1°C). The
frame-rate section below is required context for the PS1 rows.

## Frame-rate measurements

Neither stock nor NextUI logs frame rate (stock's fps/underrun log lines are commented
out upstream; NextUI computes fps/drops for its on-screen debug HUD but does not log
them). To measure, we built instrumented builds from each project's own tree — stock:
the author's own two debug lines uncommented (tag v20251127-1); NextUI: a 5-second log
of the exact fields its HUD displays (main @ 1672c89a) — ran the cells below, then
restored the shipped binaries. Patches (~35 lines each): docs/bench/receipts/ (stock-instrumentation.patch, nextui-instrumentation.patch).
Instrumented builds were used only for these verdicts, with one disclosed exception in
the Energy section.

Captured logs cover the final ~3.3 minutes of each 8-minute cell (40 samples at 5 s).

| Configuration, Bloody Roar II | Result (from the firmware's own counters) |
|---|---|
| stock Powersave (1200) | video renders **10.7–17.7 fps** while the core ticks ~60/s (audio remains clean) |
| NextUI Powersave (816) | drop counter +2,886 in the logged window (**~15/s**) |
| NextUI auto (1800, default) | drop counter +2,340 in the logged window (**~12/s**); fps dipped to 53.4 |
| NextUI Performance (2000) | drop counter **+5** in the logged window — holds |
| Zero default (modal 1008) | holds 60/60 (on-screen HUD observation; [narrative receipt](receipts/br2-zero-hud-20260709.md), no raw fight log retained) |
| NextUI Powersave, THPS2 | drop counter **+1** — holds |

Caveats: the semantics of NextUI's `frame_drops` counter are theirs (it is the "D:"
figure their debug HUD shows); we report its movement, not an interpretation of which
pipeline stage dropped. Stock's fps counter is the author's own rendered-frames metric.
Community reports that THPS2 runs well at 1008–1200 on NextUI are consistent with the
THPS2 Powersave row; BR2 is where sub-1800 static configurations measurably fail.

## Energy (fuel-gauge coulomb counter)

The AXP2202 gauge steps in 30 mAh quanta; only multi-quantum differences are treated as
signal. Same game, same duration, shipped binaries:

| Cell (8 min, THPS2) | Charge consumed |
|---|---|
| Zero default | 30 mAh |
| stock default (1608) | 30 mAh |
| NextUI auto | 60 mAh |

By the stated rule, none of these single-cell deltas is conclusive on its own: 30 vs 60
is a one-quantum gap whose true-value ranges overlap. The THPS2 comparison (2×) and the
BR2 Performance comparison (3×, via an instrumented cell run late in a warm chain) are
therefore both **indicative**; confirming them needs longer or repeated windows. They
point the same direction as the load-average and clock data, which is why they are
reported at all.

Undervolt A/B (Zero, UV on vs off, back-to-back): the difference is **below the gauge's
30 mAh resolution** in 8-minute cells. The supportable statement is rail arithmetic, not
a gauge figure: the corrected production envelope uses 1075 mV at the 1187.5 mV top OPP,
about 18% less CPU-rail dynamic power at that clock.

## What explains the differences

The mechanisms are separable, and were measured separately:

- **Clock policy** (isolated in the emulated three-policy bench, same firmware, policy
  as the only variable): plain schedutil ran Zelda DX at 1416–1800; the frame-aware
  ceiling held it at 408 at the same frame rate. Utilization-driven scaling overshoots
  on emulator workloads because per-frame CPU bursts read as demand regardless of
  deadline slack. This is the whole of Zero's advantage on light games — and it is a
  clock/energy advantage, not necessarily a temperature one (see the Zelda rows).
- **Workload cost** (the PS1 rows): Zero's pcsx build carries a NEON rewrite of the FMV
  color decoder (115→50 ms per 2-second window) and disables the emulator's async GPU
  thread, which serializes on this SoC (that change alone lowered THPS2's required
  clock from 1584 to 1008). The other firmwares ship the default core configuration and
  compensate with clock. The decoder fix is prepared for upstream submission; if adopted, their
  PS1 numbers should improve.
- **Stack weight**: NextUI runs a GL render pipeline plus batmon/audiomon/gametimectl
  daemons — features with real costs that its users may value; the THPS2 energy row
  quantifies the cost side only.
- **Undervolt**: Zero applies a per-chip calibrated voltage table. This device's smallest
  raw cliff headroom was 75 mV; after the 50 mV guard, the smallest applied high-OPP
  reduction is 25 mV. Every OPP at or below 1008 MHz reached the calibration floor without
  failure, but runtime stands down at or below 816 MHz after light-load testing. Contribution
  to these short cells is below gauge resolution.
- **NextUI history note**: NextUI's Performance mode originally ran schedutil with a
  2000 MHz ceiling; commit afb3783d (#727) changed it to the pinned `performance`
  governor after users reported slowdowns. Its author reports extensive testing behind
  the current defaults, and the Performance-mode BR2 row confirms the pinned mode does
  what it promises.

## Caveats

- Attract mode, not gameplay input. Single device, single run per cell (except Zelda,
  which repeated across boots within 0.5°C, and THPS2, which the emulated bench
  independently reproduced within 0.5°C).
- Temperatures compare fairly within a pass, less fairly across passes (late-chain
  cells run warmer; pass order is disclosed in the raw data). Day-2 UV-after cells ran
  on a chassis heat-soaked by a 2-hour plugged-in calibration and are excluded from
  temperature claims.
- WiFi-on was required for measurement and is not Zero's shipped default (radios off).
- Verdict blanks in the tables mean "not measured," not "passes."

## Raw data

Logged cells trace to bench3-real-raw/ (CSVs, *.fpslog, charts) and bench3-raw/ (the
emulated three-policy pass). Instrumentation patches are in docs/bench/receipts/. The
Zero BR2 fight result is a labeled HUD observation with a narrative receipt, not a raw log.

## Corrections (2026-07-11 re-verification)

Three figures published in the addendum were wrong or overstated and are corrected
here: NextUI drop rates were computed against the full 8-minute cell, but the captured
logs cover only the final ~3.3 minutes — the correct rates are ~12/s (auto) and ~15/s
(Powersave), not ~5/s and ~6/s. Stock Powersave's BR2 video floor is 10.7 fps, not
14.6. And the THPS2 energy comparison was called "solid" despite being a one-quantum
gauge difference — it is indicative, per this document's own resolution rule. The
direction of every conclusion is unchanged.

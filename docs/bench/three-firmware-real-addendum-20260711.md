# Addendum (2026-07-11): powersave modes, frame-rate receipts, energy, undervolt

> **Superseded by three-firmware-benchmark-202607.md** (the consolidated report), which
> also corrects three figures below: the captured fps logs cover the final ~3.3 minutes
> of each cell, not the full 8 — NextUI drop rates are ~12/s (auto) and ~15/s
> (Powersave), and stock Powersave's video floor is 10.7 fps.

Extends three-firmware-real-20260710.md with four measurement passes run the following day
on the same TESTER multiboot card and Brick. Raw CSVs + fps logs in bench3-real-raw/.

## 1. Battery-saving modes (rerun at healthy charge; first attempt archived as *-lowbatt)

Both competitors' battery modes measured via their own mechanisms (NextUI `governor.sh
powersave`; stock's in-game CPU Speed = Powersave seeded through its own cfg files, unseeded
after). Last-5-min stats, healthy battery (85-100%):

| Game | stock Powersave (1200 fixed) | NextUI Powersave (conservative ≤1200) | Zero default (reference) |
|---|---|---|---|
| Zelda DX | 1200 · 36.7°C | modal 600 · 33.1°C | 408 · 35.6°C |
| Yoshi | 1200 · 39.2°C | 1200 · 37.3°C | modal 600 · 38.0°C |
| THPS2 | 1200 · 39.3°C | modal 816 · 37.7°C | modal 1008 · 34.9°C |
| BR2 | 1200 · 41.0°C | modal 816 · 38.7°C | modal 1008 · 37.7°C |

The clocks look competitive — the frame-rate receipts below are why they aren't. A static
cap is simultaneously too high for light games and too low for heavy ones.

## 2. Frame-rate receipts (instrumented builds)

Stock and NextUI never log frame rate (stock's fps/underrun lines are commented out;
NextUI renders fps/drops to its debug HUD only). We built instrumented twins from their
own trees — stock: the author's own two log lines uncommented (v20251127-1 tag); NextUI:
a 5s-cadence log of the exact fields their HUD shows (main @ 1672c89a ≈ v6.12.0) — ran the
contested cells, then restored the shipped binaries. Patches: docs/bench/receipts/ (stock-instrumentation.patch, nextui-instrumentation.patch).

**Bloody Roar II, 8-minute cells, by the firmwares' own counters:**

| Config | Verdict | Evidence |
|---|---|---|
| stock Powersave (1200) | **FAILS — ~11-18 fps video** | own fps counter: 10.7-17.7 fps (core ticks 60/s, so audio is clean — a fluid-audio slideshow) |
| NextUI Powersave (816) | **FAILS — ~15 drops/sec** | own drop counter: +2,886 over the logged final ~3.3 min |
| NextUI auto (1800, default) | **FAILS — ~12 drops/sec** | +2,340 over the logged window; fps dipped to 53.4 |
| NextUI Performance (2000 OC) | **holds** | +5 over the logged window |
| Zero default (modal 1008) | **holds** | D48 HUD receipts (60/60), 0 underruns campaign-wide |
| NextUI Powersave, THPS2 | holds | +1 in the logged window — the community "1008-1200 works" report is right *for THPS2* |

Only two configurations hold BR2 at full speed: **Zero at modal 1008 / 37.7°C** and
**NextUI Performance at 2000 locked / 46.5°C (50.6 peak)**. Double the clock and +8.8°C
for the same on-screen result; the energy comparison (~3×) rests on an instrumented cell
and is indicative (see §3). See br2-hero.png.

## 3. Energy (AXP2202 coulomb counter, per 8-min cell)

The fuel gauge steps in 30 mAh quanta, so only multi-quantum deltas are claims:
- THPS2: Zero 30 · stock 30 · NextUI **60** (their GL pipeline + daemons cost real current)
- BR2 at full speed: Zero **30** (shipped binary) vs NextUI Performance **90** (instrumented
  cell — indicative only, see next bullet) — ~3×.
- Instrumented NextUI cells read 90 across modes (logging + late-chain heat inflate them;
  compare only shipped-binary cells across firmwares).

## 4. Undervolt: calibrated, armed, and honestly bounded

First completed calibration on this chip (Optimize CPU campaign, 2026-07-11): min margin
75 mV; 408-1008 MHz never cracked down to the tool floor (run at 812.5 mV); cliffs at
1200/1416/1608/1800 = 800/875/1025/1012.5 mV. Full log: docs/bench/receipts/uv-calibration/.

- **Energy A/B (UV on vs off, back-to-back cells): delta below the gauge's 30 mAh
  resolution in 8-minute cells.** We say that plainly instead of claiming a number. The
  supported statement is the tool's rail arithmetic: ~10% CPU-rail power at the top clock
  (V² on 75 mV), physically real but small against screen + GL + radios in short cells.
- UV-after temperature cells are NOT comparable to day-1 cells (they ran on a chassis
  heat-soaked by the 2h plugged-in calibration; several show elevated temps + more governor
  provisioning for identical content). Flagged, not published as UV results.
- Calibration side-findings, fixed in-tree during this work: the v1.3-branch watchdog gate
  could never pass on this BSP (procd owns /dev/watchdog) — calibration silently impossible
  until 1189cec1; soft wedges under procd's feeder now self-reboot via a per-round deadman
  (2b84ae1c, live-fired); tool UX now shows live progress and requires an affirmative
  Charging state to arm (f32d0501). No shipped release was affected.

## Caveats
Attract mode; single device; late-chain cells run hotter (order disclosed per pass);
instrumented builds used only for frame-rate verdicts, never for clock/temp/energy rows;
gauge quantization as above.

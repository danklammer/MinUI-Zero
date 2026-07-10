# Real three-firmware benchmark — Zero vs stock MinUI vs NextUI (2026-07-10)

**What this measures:** the three firmwares as actually shipped, each running from its own
untouched install on the TESTER multiboot card (one Brick, one afternoon, one battery
discharge 84%→46%). Unlike the emulated three-policy bench (three-policy-20260710.md),
every cell here runs the firmware's real stack: stock's fixed per-system clocks, NextUI's
GL pipeline + batmon/audiomon daemons + their auto governor, Zero's closed loop + undervolt.

- **Zero** = MinUI Zero v1.3-rc (20260710-0), all defaults (governor + UV on)
- **Stock** = MinUI v20251127-1, untouched
- **NextUI** = v6.12.0 (20260709-0), untouched, default auto CPU mode
- Launches went through each firmware's own menu-loop protocol (/tmp/next), so per-game
  clock policy is exactly what a user gets.

## Results (8 min attract per cell; stats over the last 5 min)

| Game | Zero v1.3 | stock MinUI | NextUI (auto) |
|---|---|---|---|
| Zelda DX (GBC) | **408 flat** · 35.6°C | 1200 flat · 33.6°C | modal 1800 (1416–1800) · 36.4°C |
| Yoshi's Island (SuperFX) | **modal 600** (600–1416) · 38.0°C | 1608 flat · 38.1°C | modal 1800 · 40.0°C (43.1 peak) |
| THPS2 (PS1) | **modal 1008** (600–1200) · 34.9°C | 1608 flat · 38.4°C | modal 1800 · 38.1°C |
| Bloody Roar II (PS1 FMV) | **modal 1008** (bursts to 1800) · 37.7°C (44.2 peak) | 1608 flat · 40.9°C (44.5 peak) | modal 1800 · 39.0°C (43.2 peak) |

**Bonus cell — NextUI "Performance" mode on BR2** (their shipped fix for heavy games,
engaged via their own governor.sh): **2000 MHz locked · 46.5°C mean · 50.6°C peak.**
+7.5°C over their own auto mode, +8.8°C over Zero, for the same content.

## Reading

- **Zero is the only column with ranges.** Stock runs fixed per-system presets (1200 GBC /
  1608 SNES+PS — saner than the flat 1608 the emulated bench assumed); NextUI's auto pins
  1416–1800 for everything including a Game Boy Color game. Only a frame-aware loop scales
  down when the frame is already done.
- **THPS2 is the cleanest row**: Zero holds the same content at modal 1008, 3.5°C cooler
  than stock and 3.2°C cooler than NextUI.
- **The no-OC thesis in one row**: NextUI's answer to heavy games is the 2.0 GHz overclock
  (50.6°C peak). Zero's answer is fixing the workload (NEON MDEC, GPU-thread-off) and
  resting at 1008.
- **Replication**: THPS2 real vs emulated — Zero 34.9° vs 35.4° predicted, stock 38.4° vs
  38.5°, same modal clocks. The two independent methods agree.

## Verification of the NextUI numbers

A Reddit report of THPS2 at "1008–1200 MHz on current NextUI" prompted an audit cell:
fresh NextUI boot, THPS2, mid-game process snapshot + policy readout + resample.
Result: process list = their shipped stack only (no harness contamination), policy =
schedutil 408–1800 (their auto), clocks reproduce at 1416–1800. The 1008–1200 report
matches their **Powersave** mode (conservative governor capped at 1200) — a manual
per-game setting, not the default. A tuned NextUI user can approximate Zero's clocks by
hand per game; Zero does it automatically for every game with a frame-hold guarantee.

## Method & caveats

Single Brick, discharging throughout, WiFi on for all cells (external SSH sampler @ 20s),
identical L3 keep-awake pokes in every cell, screens on everywhere. Firmware-major order
(zero → stock → nextui), same game order per block so chain-heat positions match across
firmwares. Attract mode, not gameplay input. Battery column granularity (1–2%/cell) is
too coarse to headline. Featherweight loads (GBC) are screen-dominated — the ±2°C spread
on the Zelda row is noise territory; claims should rest on the heavy rows. NextUI cells
carry their daemons' load (~+1.0 loadavg) — that is their real shipped behavior, not an
artifact (verified). Raw CSVs: bench3-real-raw/.

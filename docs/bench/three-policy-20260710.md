# Three clock policies, one firmware — stock MinUI vs NextUI vs Zero (2026-07-10)

**What this measures:** CPU clock policy in isolation. All twelve cells ran the SAME
MinUI Zero v1.3-rc build on the SAME TrimUI Brick — only the clock policy changed:

- **stock**: `userspace` governor pinned at 1608 MHz (stock MinUI's published Normal clock)
- **nextui**: plain `schedutil`, 408–1800 MHz (NextUI's auto mode IS the kernel governor —
  verified from their source; their closed-loop is nonexistent)
- **zero**: our frame-aware closed-loop governor, as shipped

**What this deliberately does NOT measure:** the rest of each firmware's stack. Our NEON
MDEC core, audio fixes, and render path travel with every cell (that's why the underruns
column is zero even under emulated stock policy — stock MinUI's actual scalar core would
not match this). A true three-firmware, three-card comparison is separate follow-up work.

## Steady state per cell (~8 min attract each, last-5-min stats, 0 underruns everywhere)

| Game | stock 1608 | nextui (schedutil) | zero |
|---|---|---|---|
| Zelda DX (GBC) | 1608 · 32.4°C | 1416–1800 · 33.1°C | **408 locked** · (temp n/c*) |
| Yoshi's Island (SuperFX) | 1608 · 40.0°C | modal 1800 · 39.2°C | **1416 · 38.8°C** |
| THPS2 (PS1) | 1608 · 38.5°C | 1416–1800 · 36.1°C | **1008 · 35.4°C** |
| Bloody Roar II (PS1 FMV) | 1608 · 39.9°C | 1416–1800 · 39.5°C (45.6° peaks) | **modal 1416, rests at 1008 · 38.8°C** |

\* zero-zelda ran LAST in the chain after ~90 minutes of hot cells; its 39.5°C mean is
sequence-poisoned and not comparable to the cold-start stock/nextui zelda cells. The clock
figure (408 vs 1416+ vs 1608 for identical content) is sequence-immune and is the claim.
A cold-start rerun for a fair temperature figure is queued.

## Reading

- **The THPS2 row is the cleanest** (adjacent cells, chain-heating biased AGAINST zero):
  same game, 600 MHz below what schedutil picks, 3.1°C cooler than static — because only
  a frame-aware loop knows the frame finished early. Utilization governors read emulator
  frames as "busy" even at 4x the needed clock (see the zelda row: schedutil runs a Game
  Boy Color game at 1416–1800).
- **BR2**: schedutil spikes to 1800/45.6°C chasing FMV bursts; zero's burst-then-sink
  pattern covers the same content mostly at 1008 with brief provisioning bursts.
- **Yoshi**: the honest heavyweight — SuperFX is expensive under every policy; zero still
  posts the lowest clock and temperature.

## Method

Single Brick, discharging (43%→0 across the run), Stay Awake armed, wifi on (equal across
cells), same brightness, attract-mode scenes, 20s sampling to CSV (raw files in
bench3-raw/). Policies re-asserted every sample tick to defeat launcher clock writes.
Competitor cells ran with the Zero governor and undervolt env-disabled (GOV_DISABLE=1,
ZERO_NO_UV=1) so the emulation is faithful — neither stock MinUI nor NextUI has either.

Caveats: attract-mode (not gameplay input); single device; temps carry chain-order bias
(clocks do not); underruns not comparable to real competitor firmwares (shared core).

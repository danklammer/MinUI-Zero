# Cross-system governor floors — v1.3 stack regression sweep, 2026-07-09

Per-system single-arm runs (first rom per system, 4 min, keep-awake poker, HUD on,
ZERO_GOV_DEBUG=1). Governor build: fail-memory-below-max + burst-on-true-size-change +
BUSY-preserves-slack (commits f517d16a..68ed1f25). Historic floors from the 2026-07-03
v1.0 sweeps.

## Brick (discharging, ~33-40C ambient-warm after long session)
| system | rom (first in dir) | settled ceiling | historic | verdict |
|--------|--------------------|-----------------|----------|---------|
| GBC  | Dr. Mario            | ~1008 (gen collapses at 408: 20.7/60) | 408 | governor honest; scene cost question filed |
| GBA  | Advance Wars         | 600 (dynamic 600-1416 tracking scenes) | ~600 | PASS |
| FC   | (first FC rom)       | 576→snaps 600, stable | ~816 | PASS (cooler than historic) |
| SUPA | (first SUPA rom)     | 600, stable 2+ min | 600 | PASS exact |
| MD   | (first MD rom)       | 600, stable | 600 | PASS exact |
| PS   | Ace Combat 2         | 1008 cruise, correct burst-to-1800 on scene change | hunts | PASS |

## Smart Pro (complete)
| system | settled ceiling | notes |
|--------|-----------------|-------|
| GBC  | ~1008 | matches Brick exactly — GBC cost is platform-wide, not device-specific |
| GBA  | 600 | matches Brick |
| FC   | descent to 792 captured in log (gen 60.3) before a wifi nap cut telemetry | consistent |
| SUPA | 600, locked 2+ min | matches Brick exactly |
| MD   | 600, locked 2+ min | matches Brick exactly |

Cross-device conclusion: identical governor behavior on both panels/calibrations.
SP uptime through its sweep: no reboots, no sleeps — all SP outages were wifi naps
(keep-awake daemon effective; the radio drops under sustained idle-ish load).

## Open questions filed
1. GBC (Dr. Mario attract) needs ~2ms more per frame than the 07-03 era floor implies —
   audit "pure work" accounting (audio-block waits may pollute p95) + A/B against v1.0 build.
2. p95 spike clusters (19-28ms windows, ~15s long) present on GBC — same audit.

All arms: 0 audio underruns, no crashes, temps ≤40C.

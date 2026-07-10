# Variety round 3 — 11 fresh games, both devices, final v1.3 stack (2026-07-09 night)

Per-game floors on minarch 8f52ae4c + core 854ecfc3 (immortal-sampler arms, ~3 min attract
each unless noted, all 0 audio underruns, all temps <47C).

## Brick
| game | system | settled clock | notes |
|------|--------|---------------|-------|
| Bomberman GB | GBC | **408** | ~25 min accidental endurance; floor-guard build stable at the brownout clock |
| Castlevania: Circle of the Moon | GBA | 600 | |
| Adventures of Bayou Billy | FC | **~800** | first honest above-floor NES title — the governor refuses to starve it |
| Chrono Trigger | SUPA | 600 | |
| Beyond Oasis | MD | 600 | |
| Gran Turismo 2 | PS | **1008** | heaviest 3D on the platform, single-threaded, 40C |

## Smart Pro
| game | system | settled clock | notes |
|------|--------|---------------|-------|
| Battletoads in Battlemaniacs | SUPA | 600 | |
| Adventures of Batman & Robin | MD | 600 | the hardest-pushing Genesis game ever holds the floor |
| Crash Team Racing | PS | **1608** | full-3D racer held one step below max — no hunting, 39C |
| (Crystalis GBC / Aria GBA) | | ran clean in earlier partials | radio ate their telemetry, logs clean |

## Reading
Every game gets its own clock: floor-dwellers at floors, honest heavyweights held where
they need (Bayou ~800, GT2 1008, CTR 1608), zero underruns across all arms, nothing pinned,
nothing hunting. The closed-loop thesis, demonstrated across 11 fresh titles in one night —
on the exact bits proposed for v1.3.

Harness note for posterity: this round burned through five harness bugs before producing
data (sleep chains, system dimtime, launcher resurrection, sed &-expansion, and the
"grep || echo 0" arithmetic ghost that killed shells only when logs measured CLEAN).
The surviving pattern — detached append-only samplers + foreground orchestration — is the
one to reuse. See DECISIONS D49/D50.

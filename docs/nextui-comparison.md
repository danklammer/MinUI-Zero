# NextUI thermal/CPU-governor comparison

Researched 2026-06-30 from the `nextui` remote (LoveRetro/NextUI) — release notes, PR #695,
and their shipped `governor.sh`. Goal: find NextUI's documented thermal benchmarks and compare
their CPU-scaling design against ours.

## The benchmark number you remembered
NextUI does **not** publish a formal temperature table. Their numbers live in two places:
1. **A runtime debug HUD** — v2.5.1 (2025-03-24) "Added cpu temperature in celsius to debug HUD".
2. **PR #695** (merged v6.11.0, 2026-05-14, "Replace userspace CPU governor with kernel scaling
   governors", by pvaibhav). The money quote:

   > "the biggest positive impact of this PR is significant reduction in CPU usage by the
   > userspace governor thread and correspondingly **significant reduction in core temperature
   > of the order of 5–10°C**."

That 5–10°C is the headline figure — and it comes from the **same change we made**: dropping the
`userspace` governor + `scaling_setspeed` pin (whose polling thread itself burned CPU/heat) in
favor of kernel scaling governors.

## NextUI's shipped design (their `governor.sh`, v6.11.x)
Reads `scaling_available_frequencies` live; three modes:
| Mode | Kernel governor | Range (TG5040) |
|------|----------------|----------------|
| **auto** | `schedutil` | 408 → **1800** (second_max = one step below the 2000 OC) |
| **performance** | `performance` | 408 → **2000** (they *do* expose the 2.0GHz OC here) |
| **powersave** | `conservative` | 408 → 1200 (midpoint) |
- Author's philosophy: *"there should not be any need for any option except Auto and at best a
  Powersave mode → that is how our phones and laptops work."* No frame-aware closed loop at all.
- Their auto restores on minarch exit; `PLAT_setCPUSpeed` became a no-op.
- Follow-up v6.11.1 "fix: slowdowns on Auto cpu speed" (PR #727) — they hit tuning pain too.

## How we compare
| | NextUI auto | Ours (measured on-device, Tony Hawk PS1) |
|---|---|---|
| Governor | schedutil | schedutil (hybrid) |
| Floor | 408 | 408 (just corrected from assumed 480) |
| Cap | 1800 global (every system) | **per-system** (PS1 1800, 16-bit 1416, 8-bit 1008) |
| 2.0GHz OC | exposed in Performance mode | **never** (thesis: no overclock) |
| Frame-aware loop | none | built, but **not firing** on-device (ceiling never sinks below f_max) |
| Result | "5–10°C cooler" vs old userspace | 36–37°C sustained PS1; schedutil self-scaled 816–1608 |

## The two findings that matter
1. **Independent convergence = strong validation.** Two forks, arrived separately at the identical
   core: *schedutil + range-limit, floor 408, cap one step below the 2.0 OC.* We're on the right road.
2. **Our closed-loop ceiling controller currently adds nothing over plain schedutil.** On-device the
   ceiling pins at f_max the whole session; schedutil does all the real scaling underneath. NextUI
   deliberately shipped *without* such a loop and still got the 5–10°C win. So our decision point:
   - **(a)** Debug/tune the frame-aware sink so it demonstrably beats plain schedutil, **or**
   - **(b)** Lean into schedutil + **per-system ranges** (our real edge — NextUI caps *everything* at
     1800; we cap an 8-bit game's schedutil at 1008, which should run cooler than NextUI on light
     systems) and treat the closed loop as optional.

   (b) fits the "stay lean / runs cold" thesis and is already half-built (per-system f_max is applied
   statically at game load). Our differentiator is **per-system caps**, not the bespoke controller.

## On-device A/B result (measured 2026-06-30, same NES game 1942, only clock policy differs)
| Run | CPU behavior | Settled CPU temp |
|-----|--------------|------------------|
| **A — our schedutil governor** | self-scaled 408–816 MHz | **39°C** |
| **B — old userspace 2.0GHz pin** (`GOV_DISABLE=1` + userspace@2000000) | locked 2000 MHz | **44°C** |
| **Δ** | | **5°C cooler with our governor** |

This **reproduces NextUI's 5–10°C claim on our own fork** — landing at the 5°C (light-load) end;
NES only pushed the pinned CPU to 44°C because the cores aren't fully worked, so a heavy game
(where the pin wastes far more) should widen the gap toward their 10°C. Nearly all of that 5°C is
the schedutil-vs-userspace-pin win that we *and* NextUI share; our per-system caps + closed-loop
ceiling are refinements on top of it.

## Correction to an earlier read: the closed-loop ceiling DOES fire
On Tony Hawk (PS1) the ceiling held at f_max, which first looked like the sink branch was dead. The
NES run disproved that: the ceiling actively sank toward the 408 floor (408–624 kHz, below the 1008
cap) because a trivial load has zero frame pressure — while PS1 correctly held high for real demand.
So the frame-aware loop works as designed. Its *marginal* benefit over plain schedutil is still open
(schedutil already runs light loads low), but it is not inert. Per-system static caps remain the
clearest, cheapest edge over NextUI's single global 1800 cap.

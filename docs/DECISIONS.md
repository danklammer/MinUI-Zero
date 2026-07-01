# Decisions log — closed-loop thermal governor

Running log of choices made while building the governor autonomously, so the
reasoning survives even when the diff doesn't show it. Newest at the bottom.

## D1 — Module placement: `workspace/all/common/governor.{c,h}`
The governor is shared frontend logic that calls a platform primitive
(`PLAT_setCPUFreq`), exactly like `api.c`. It lives next to `api.c` in
`workspace/all/common/` and is added to `minarch`'s makefile `SOURCE` list. It is
*not* pulled into every platform automatically — only `minarch` compiles it.

## D2 — Split the controller into a pure step + an I/O tick
`docs/thermal-governor-design.md` writes `gov_tick()` with file-static state and an
internal temp read. To make it testable with scripted traces (task 5) and free of
sysfs on non-device builds, the algorithm is factored into:
- `gov_step(GovState*, profile, temp_c, frame_overrun)` — **pure**, no I/O, returns
  next kHz. This is the exact algorithm from the design doc.
- `gov_tick(GovState*, profile, frame_overrun)` — reads temp from the thermal zone,
  calls `gov_step`, writes the result via `PLAT_setCPUFreq`. The device entry point.
- `GovState` struct replaces the design doc's three file-static globals so the
  harness can run many independent controllers and the real path keeps one.
The numbers and branch structure are unchanged from the design doc.

## D3 — Frequency write goes through `PLAT_setCPUFreq(int khz)`
Per the task list. tg5040 writes kHz to `scaling_setspeed` (reusing the existing
`GOVERNOR_PATH`, keeping the `userspace` governor `boot.sh` set). macOS logs instead
of writing. A `FALLBACK_IMPLEMENTATION` (weak) no-op lives in `api.c` so every other
platform (rg35xx, my355, …) still links without change.

## D4 — Temp read stays a direct sysfs read in `governor.c` (graceful -1)
The design doc reads `T_SENSOR` directly. Kept that way (clearly marked ASSUMED).
On any non-device build the path is absent, `fopen` fails, and the reader returns
`-1`, which the controller treats as "temp unknown → don't let temp gate downward
probing." So no extra platform function is needed for temp, and the harness injects
temp directly via `gov_step`.

## D5 — `frame_overrun` reuses minarch's existing frame-budget measurement
`api.c` already computes `SDL_GetTicks() - frame_start` vs `FRAME_BUDGET` (17 ms) to
decide vsync inside `GFX_flip`. We capture that same comparison into a flag exposed
as `GFX_didOverrun()` and feed it to `gov_tick`. No new timing path is introduced.
Caveat: `FRAME_BUDGET` is a fixed 60 Hz budget; for sub-60 Hz cores (PAL, VB, etc.)
a frame that comfortably holds the core's own rate can still read as an "overrun,"
making the governor slightly less eager to downclock those cores. This is
conservative (never unsafe) and is flagged as an on-device tuning item.

## D6 — Governor is authoritative during gameplay; the static CPU menu stays
The existing in-game `minarch_cpu_speed` menu (`setOverclock`) is left in place to
keep scope tight (the task says replace the *pick*, not remove the menu). Because
`gov_tick` rewrites `scaling_setspeed` every tick (~0.5 s), it re-asserts its clock
over whatever the static menu last set, so the governor wins during gameplay without
ripping out UI. Menu/sleep clocks (`CPU_SPEED_MENU`) are untouched.

## D7 — Per-pak `launch.sh` exports `MINARCH_FMIN` / `MINARCH_FMAX` (kHz)
Most flexible wiring for "per-system f_min/f_max": each emu pak exports its two clock
bounds as env vars; minarch reads them on game load and builds the profile. If the
vars are absent (un-updated pak / other platform), minarch falls back to a safe
default profile (`600000`/`2000000` kHz) — guaranteed to hold frame rate, governor
sinks from there. Profile numbers come from the design doc's P_8BIT / P_16BIT / P_PS1
brackets, mapped per system.

## D8 — Local build flag `-U__ARM_ARCH` for macOS verification only
`api.c` selects 32-bit ARM inline-asm blend helpers under `#if __ARM_ARCH >= 5`,
which clang cannot assemble on arm64 Apple Silicon (`__ARM_ARCH==8`). The documented
build command predates Apple Silicon. `-U__ARM_ARCH` forces the identical C-fallback
blend locally. This is a **local verification flag only** (in `.notes/verify.sh`); no
committed source changes, and it does not touch the render path. On the real Linux
cross-toolchain (`make tg5040`) and Intel Macs the stock command is unaffected.

## D9 — Governor safety hatch: `GOV_DISABLE` env
Setting `GOV_DISABLE=1` makes `gov_tick` a no-op (governor off, static menu clock
stands). Default is on. Cheap, reversible escape hatch for on-device bring-up.

## D10 — Batch overrun threshold: ≥25% of frames in a tick window
`minarch` samples `GFX_didOverrun()` every gameplay frame and calls `gov_tick` once
per `GOV_TICK_FRAMES` (30). The batch is declared "overrun" when `slips*4 >= frames`
(≥25% of the window missed budget). This catches a sustained inability to hold rate
while ignoring rare one-frame hiccups (loads, audio underruns). The 25% threshold is
a minarch-side aggregation policy, not part of the pure controller — a tuning knob.

## D11 — Per-system bracket assignment (tg5040 paks)
`launch.sh` exports (kHz), by system, mapping the design doc's three brackets:
- **8-bit `480000..1008000`**: FC, GB, GBC, SMS, GG, PCE, NGP, NGPC, PKM
- **16-bit `600000..1320000`**: SFC, MD, GBA, MGBA, SGB, SUPA, VB
- **heavy `1008000..2000000`**: PS, P8 (PICO-8/fake08, Lua interpreter)
`PAK.pak` is a stub (no `minarch.elf`) and is skipped. Other platforms' paks are not
touched (this fork targets tg5040); there minarch falls back to `GOV_P_DEFAULT` and
their `PLAT_setCPUFreq` is the weak no-op, so behavior is unchanged.
Borderline cases flagged for on-device tuning (may want a higher `f_max`): MGBA and
SGB (accuracy mGBA core, heavier than gpsp) and SUPA (supafaust SNES). Safe as-is —
if they slip at `f_max` the governor simply pins `f_max`; raising it only recovers
lost frames, never overheats (the ceiling still bounds it).


## D12 — Reworked to the HYBRID model + removed the 2.0 GHz overclock (per docs/project-direction.md)
The pure userspace-pin design (write exact kHz to `scaling_setspeed`) is superseded. Now:
- **`schedutil` + ceiling.** `boot.sh` selects `schedutil`; the controller (and the static menu
  tiers) set `scaling_max_freq` as a *cap*, and the kernel picks the instantaneous freq beneath
  it. Benefit: keeps frame-awareness *and* lets the kernel drop the clock in light scenes
  (cooler) with cheaper transitions. No governor-mode switching — every CPU-control point just
  sets the cap. `PLAT_setCPUFreq`->**`PLAT_setCPUMaxFreq`** (scaling_max_freq); `GovState.cur_khz`
  ->`ceil_khz`. The control math (fast-up/slow-down, hysteresis, thermal backstop) is unchanged.
- **No overclock.** `f_max` for PS1/DEFAULT 2000000->**1800000**; the static PERFORMANCE tier
  2000000->1800000; `launch.sh` PS/P8 FMAX 2000000->1800000; `boot.sh` no longer pins 2000000.
  `GOV_STOCK_MAX_KHZ=1800000` is a clearly-marked ASSUMED stock cap — confirm on device, never
  cap at/above 2.0 GHz (the number-one constraint: never require overclocking).
- **Deferred (needs device / follow-up):** full Auto-restore on every core init/exit/crash/
  resume (today the menu cap + gov re-assert cover the common paths); treating audio underruns
  as a separate signal from presentation misses; gating the governor off during fast-forward;
  runtime OPP-table discovery to replace the assumed `STOCK_MAX_KHZ`/`STEP_KHZ`. Tracked in
  docs/project-direction.md (Stage 1) + ON-DEVICE-CHECKLIST.md.


## D13 — Scope: tg5040 only (TrimUI Brick + Smart Pro). Expansion evaluated + declined (2026-06-30)
Considered widening support and explicitly chose not to. Rationale:
- **Miyoo Mini / Mini Plus — rejected.** Different SoC/arch entirely: SigmaStar SSD202D, 32-bit
  ARMv7 Cortex-A7 (confirmed from `mymin` miyoomini flags `-march=armv7ve -mtune=cortex-a7`), vs our
  64-bit ARMv8 Cortex-A53 (A133P). Our aarch64 cores wouldn't run; no A133P governor/DE2/thermal work
  transfers; philosophically inverted (MyMinUI *overclocks* the weak Miyoo for perf via `overclock.elf`
  — opposite of our downclock-for-cold). It's a from-scratch rewrite of a device we can't test, and
  MyMinUI already covers it. Skip.
- **Other A133P handhelds exist** (Powkiddy V20 640x480, R36S Plus 720p, Ampown Mini Zero 28, rebrands).
  These ARE the viable expansion IF ever wanted: same aarch64 A133P -> our -O3 cores + schedutil-cap
  governor transfer; only display-res/input/thermal-paths need per-device wiring (~a weekend). Declined
  for now to keep single-chip depth (our differentiator vs MyMinUI's breadth) and honesty (only ship
  what we can physically test).
- **Hedge (do regardless):** keep the governor's dynamic-OPP path clean (read `scaling_available_
  frequencies`, à la NextUI) so a future *tested* A133P device is config, not a rewrite. Don't hardcode
  A133P assumptions where a dynamic read is nearly free.
- Decision: **stay tg5040 (Brick + Smart Pro).** Revisit only if a same-SoC device lands in hand to test.

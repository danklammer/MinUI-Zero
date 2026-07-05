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


## D14 - Governor CAPS spikes; it must NOT force schedutil below race-to-idle (2026-07-01, on-device)
Tried "fixing" the governor's slip signal to use PURE CPU work (subtracting the audio-pacing block that
SND_batchSamples adds during core.run). It made the governor sink the PS1 ceiling aggressively to 1008
(the floor). On-device result from the project owner in free skate: "runs fine but feels warmer."
Why: capping schedutil at 1008 forces the CPU to ~100% util the whole frame, instead of running at ~1416
and then IDLING (WFI, near-zero power) for the rest of each frame. Running slower-continuously is WARMER
than faster-then-asleep -- race-to-idle. So the audio-pacing "false slips" that pinned the ceiling high
were actually PROTECTIVE: they let the CPU race-to-idle. Reverted the governor to GFX_didOverrun()
(proven cool: ceiling holds 1800, schedutil ~1416-with-idle, 35-36C). Kept the pure-work subtraction for
the BENCH metric only. Lesson: the closed-loop ceiling should cap runaway spikes, not drive schedutil
below the clock at which it can finish-the-frame-and-sleep. A future ceiling floor must respect race-to-idle.


## D15 — GPU-dark MENU shipped: software present to /dev/fb0 (2026-07-01, on-device)
The whole present path (games *and* menu) went through GLES on the PowerVR GE8300 — `minarch` held
`/dev/dri/renderD128`, the GPU power domain sat `active`, `pll_gpu`/`gpu` at 702 MHz. The menu is a
static native-res (1024×768) surface that needs **no scaling**, so software-presenting it is nearly free
(one RGB565→XRGB8888 convert + copy). Shipped `PLAT_flipFB` (tg5040 `platform.c`, env `ZERO_FB_PRESENT`,
**on by default** in `MinUI.pak/launch.sh`): the launcher/menu presents straight to `/dev/fb0`, no GL
client, so the **GPU power domain runtime-suspends while browsing.** On-device: **26°C, owner-confirmed
"super fast."** This is a structural edge NextUI can't copy — their entire UI is GPU-based. Games keep
GLES (see D18); GPU-dark is the low-risk, native-res menu slice where the CPU-scale cost that makes the
games case borderline simply doesn't exist.

## D16 — Deep sleep ENABLED (opt-in), validated on-device (2026-07-01)
Deep sleep (hybrid faux-sleep → suspend-to-RAM, ported from `zhaofengli`, see `THIRD_PARTY_NOTICES.md`)
moved from "designed, awaiting hardware" to **validated + enabled.** On-device: **33 → 27°C, clean
resume**, no stuck-radio `EBUSY`. Kept opt-in behind the `enable-deep-sleep` flag in `.userdata/shared`
rather than defaulting on, so a normal install is unaffected until the owner turns it on. The `bin/suspend`
choreography quiesces radios + saves the ALSA mixer (`alsactl store`) so the kernel accepts `mem` and
audio returns clean on resume. The resume-debounce interlock (D-series in deep-sleep-design) prevents the
wake-press from immediately re-sleeping.

## D17 — Radios + ambient LEDs dark by default (2026-07-01)
MinUI has no networking, yet OFW left Wi-Fi/BT powered (`S96wpa_supplicant`) — a continuous idle rail for
nothing. `boot.sh` now kills Bluetooth and **gates Wi-Fi on the dev-only `enable-ssh` flag**, so a normal
install boots radio-dark; SSH stays available for on-device iteration when the flag is set. Ambient LEDs
are zeroed every boot. These are standing-power rails NextUI's feature set (WiFi/NTP, Pak Store,
RetroAchievements, ambient LEDs) keeps lit — a continuous efficiency win we hold structurally.

## D18 — GPU-dark GAMES: measured EXACT break-even → SHELVED, menu-only (2026-07-01, on-device)
Tested whether the GPU-dark win extends to gameplay. Built a software-scale-to-fb0 game present
(`ZERO_FB_GAME`, `PLAT_flipFB_game`, commit `33b5e6e`): it **renders correctly and the GPU domain does
suspend during play**; naive nearest-neighbor scale (256→1024) cost ~1 core (choppy), a **row-caching**
pass (scale each source row once, `memcpy` to identical dst rows) cut it to **72% CPU — smooth, no
tearing** (owner-confirmed). Then the honest test: a clean `charge_counter` drain A/B with the GPU
**verified suspended the whole window** = **exact break-even, ~6h, identical to GLES** (also only ~1–2°C
cooler). The software-scale CPU cost *precisely offsets* the GPU power it saves → net zero. **Decision:
games keep GLES; GPU-dark is MENU-ONLY; `ZERO_FB_GAME` off by default.** Not worth a per-launch flag + an
in-game-menu GLES seam for zero net gain. The only real-win path is the **DE hardware scaler** (`/dev/disp`
layer — no GPU *and* no CPU scale), which probed unavailable on this kernel (legacy fbdev scanout; nobody
holds `card0`) — a research project, not shipped work. **Measured, not assumed** — the negative result is
itself the finding, exactly as the CLAUDE.md non-negotiable demands.

## D19 — QoL Tier-1 mostly no-ops on our tree: verify-before-porting (2026-07-01)
Surveyed NextUI/MyMinUI/upstream/zhaofengli for QoL wins, then checked each against **our actual tree**
(stock MinUI + schedutil governor). Most survey "wins" fell away because they refine features or baselines
our tree doesn't share: instant-first-press is already ours (`api.c` sets `just_repeated` on initial
press); favorite-toggle requires a favorites feature we don't have (= bloat); MyMinUI's sleep-clock
drop / audio-close savings are likely redundant under schedutil (measure-first, not ported blind).
**Not porting them *is* the thesis working.** Only two Tier-1 items were genuine gaps and both shipped
(#4 failed-load bail, #6 AV-info handling — see `qol-backlog.md`). Lesson: verify a fork's "fix" against
our code before porting; a patch that fixes *their* refactor is a no-op or a regression on ours.

## D20 — First measured baseline: ~6h on Game Boy, 408 MHz OPP floor (2026-07-01, on-device)
Measurement is unblocked: `current_now` is dead on the AXP2202, but `charge_counter` works, so drain over
a fixed window gives a real rate. First energy baseline: **~6h on Game Boy** (4% / 15 min). Confirmed the
**CPU OPP floor = 408 MHz** with no lower step — which settles a recurring design question: a sub-408
"sleep clock" is **impossible**, so schedutil already idles at the lowest hardware clock and there's
nothing lower to chase (kills QoL #2's premise on this SoC). Stock cap held at 1800; 2.0 GHz OC never
used. These are the first real numbers to validate future changes against — no more optimizing blind.

## D21 — FMIN floors stay at 600 for 16-bit systems: 408 pegs the core for zero thermal win (2026-07-01, on-device)
Tested Genesis (picodrive, Aladdin) with `MINARCH_FMIN=408000`: the governor sank cleanly to a 408
ceiling with no overruns — but at **102% CPU, fully saturated**, no idle time at all, and no measurable
temperature win over the 600 floor (36°C vs 37°C, within session drift; 600 runs 76% util with real
race-to-idle headroom). Saturated-at-min-clock is the exact anti-pattern D14 documented on PS1: no
C-state residency, fragile on heavy scenes, nothing gained. The shipped per-system floors (408 for
8-bit, 600 for 16-bit, 1008 for PS1) are already at the race-to-idle sweet spot — don't lower them
without new evidence.

**Field addendum (same day):** user-reported "serious slowdowns" in Contra (NES/fceumm) turned out to
be the governor **limit-cycling** at the FC floor: `408 overrun → 624 clean → sink → 408 overrun → …`
(visible in the FC log as alternating ceiling moves). Every re-probe of 408 = a burst of slow frames.
A floor must leave real idle headroom, not merely "usually hold" — fceumm's floor raised 408→600
(gambatte/GBC stays 408; it holds 54% util there). If a future game stutters periodically, check the
gov log for this oscillation signature first.

## D23 — The governor was blind to real slowdowns; fixed with period-based slip + failed-floor memory (2026-07-01)
Contra stayed slow at the 600 floor with **zero overruns logged** ("way better" pinned at 1008 = clock-
bound, yet the detector saw nothing). Two compounding blind spots, fixed in `api.c`/`governor.c`:
1. **The slip signal measured only pre-present work.** `frame_work_us` stops at `GFX_flip` entry, so
   `PLAT_flip`'s CPU cost (GL texture upload + PowerVR driver submission — expensive at low clocks)
   was invisible: work "under budget," vblank missed anyway. And the PowerVR stack **late-swaps**
   rather than snapping to 33ms, so the game floats at ~50-55fps (18-22ms periods). Fix: slip = frame
   PERIOD > core budget + ~6% (`GFX_setFrameBudget` tracks the core's real fps, incl. AV re-sync).
2. **The controller had no memory** — 4 clean ticks and it sank right back into the clock that just
   failed (600↔816 limit cycle, one slowdown burst per cycle). Fix: `fail_khz`/`fail_hold` — a ceiling
   that slips isn't re-probed for ~60s (`GOV_FAIL_HOLD`), then one probe is allowed (scene may have
   lightened). Unit tests extended pass (`make test-governor`).
Result: Contra rides a steady 816 ceiling (stock max 1800 — no OC), schedutil still dips to 600 in
light moments, temp unchanged (36-37°C), gameplay confirmed right. The thesis is "lowest clock that
HOLDS FRAME RATE" — a detector that can't see dropped frames silently optimizes the wrong half.

## D24 — Period-based slip detection was over-sensitive; the true signal is the core generation rate (2026-07-02)
Two clean automated sweeps (100%/95% battery, discharging, no USB — and the identical "dirty" run
that falsified the MTP-contamination theory) showed the D23 period detector false-slipping EVERY
system to its max ceiling at 60-70% CPU: the pipeline is bursty by design (audio-block pacing + GL
buffering), so a flawless 60fps average still contains >17ms frame gaps. Zelda "needed" 1008 by
period while an hour of validated play proved 408.
Redesign (`minarch.c` gov tick + `governor.h` GOV_SIGNAL_*):
- **SLIP (climb)** = core generation rate short of target: `cpu_double < core.fps × 0.975`
  (core.run iterations/sec from trackFPS — jitter-immune ground truth of game speed; rate window
  reset on menu exit to avoid a false drop).
- **BUSY (hold, don't sink)** = ≥25% of frames' pure work over budget — D14's protection against
  probing into saturation (audio-block noise overcounts work; that bias is protective).
- **SLACK (may sink)** = neither; fail-memory (D23) still bounds boundary re-probes.
Decisive sweep: GBC 45×408 `gen=59.6/59.7` (proven baseline restored), MD 600@75%, GBA 600@71%,
**supafaust holds full rate at 600 (131% CPU = multi-threaded across the quad — the saturation
scare was detector artifact; it may be the coolest SNES core)**. Known refinement: the BUSY hold
keeps NES/PS1 one OPP step above their proven minimums (1008 vs 816, 1800 vs 1416) — safe-direction
conservatism; tune only with BENCH telemetry data, not guesses.
Meta-lesson: three detector designs in one day, each falsified by measurement, until the signal
matched the thesis itself — measure "is the game running at full speed," not proxies for it.

## D25 — Upstream's `fceumm_sndquality = High` default cost NES ~400MHz AND crackled; Low restored, D21 evidence retracted (2026-07-02)
"Why is NES demanding?!" (user, correctly). Three-way BENCH A/B, Contra attract, 130s each:
| run | p50 work | lived at | budget misses | audio underruns |
|-----|----------|----------|---------------|-----------------|
| fceumm **High** (as shipped) | 9.3ms | 1008 | 60 | **76** |
| fceumm **Low** | 6.5ms | 600→408 | 0 | 10 |
| **QuickNES** (`5ae7551`, -O3/A53) | **2.5ms** | **408 flat** | 0 | 10 |
`fceumm_sndquality = High` ships in **stock MinUI's FC.pak default.cfg** — inherited by every fork;
the expensive SexyFilter DSP path burned ~400MHz of clock headroom and STILL underran audio 7×
more than Low. Nobody caught it because no other fork measures what NES costs. Fixed to Low in
skeleton `default.cfg`/`default-brick.cfg`.
**Retraction:** D21's "fceumm saturates at 408" data was collected under the hidden High regime —
FC floor restored to 408 (at Low, the A/B recorded clean 408 windows; the gen-rate detector +
fail-memory handle any climb). **QuickNES** measured 2.6× lighter than fceumm-Low and lives at the
OPP floor like gambatte; kept as a validated option (fceumm-Low stays default for mapper coverage —
swap only if a compat-vs-cost case emerges). Worth upstreaming the sndquality finding to MinUI.

## D26 — Option-cost audit of the remaining suspects: dithering + h_filter measured FREE, keep both (2026-07-02)
Same BENCH A/B harness, 130s runs: **PS1 `pcsx_rearmed_dithering`** enabled-vs-disabled = p50 9.0 vs
8.9ms, identical clock profile — the NEON GPU plugin dithers for free; keep the authentic look.
**supafaust `h_filter`** blend-vs-plain-512 = 8.6 vs 8.4ms — `phr256blend_auto512` only blends in
512-wide hires mode (rare); keep the core default. gambatte color-correction not tested (GBC already
runs 65%@408 — nothing to win). Audit tally: one conviction (D25, 400MHz), two acquittals, each
closed in ~10 min. The per-option A/B loop (guarded sweep + BENCH CSVs) is now the standard way to
answer "does this option cost anything" — measure, don't debate.

## D27 — Pillar 4/5 hardening: crash-safe saves shipped + 50-cycle deep-sleep soak passed (2026-07-02)
**Crash-safe saves** (`minarch.c` Crash_install/Crash_handler): on SIGSEGV/BUS/ILL/FPE/ABRT,
emergency-write the cached SRAM/RTC pointers using only async-signal-safe syscalls, atomic via
.tmp+rename (a mid-write death can't corrupt a good save), then re-raise. Validated on-device:
kill -SEGV on a live game → handler message → 32KB .sav written → clean crash → launcher recovered.
A core crash no longer costs the session's progress.
**Deep-sleep soak** (`tools/sleep-soak.sh`: RTC wakealarm + real `bin/suspend` choreography):
50 unattended suspend/resume cycles with a live game — 49 flawless 42s round-trips, game survived
all 50, 0 wake hangs; 1 cycle aborted suspend instantly (failed SAFE, awake — the runtime path
retries 3× for this case). 3% battery over the 47-min gauntlet; 26-30°C. **Default-on deep sleep
is now evidence-certified.** Known dev-only casualty: ~50 suspends can wedge wifi until reboot
(users don't run wifi; soak logs go to SD, not tmpfs, for this reason).

## D28 — Predictive sink gate replaces the busy-hold; NES recovered 600MHz of ceiling (2026-07-02)
The D24 busy-hold (25% of frames over budget → never sink) counted audio-BLOCKED frames as work and
held NES at 1008 / PS1 at 1800. Replacement (`gov_sink_target`/`gov_sink_fits`, unit-tested):
per-batch p95 of PURE work (audio-pacing wait subtracted), scaled to the next-lower clock
(work ∝ 1/clock); sink allowed only if predicted p95 fits **85% of the frame budget** — the 15%
idle headroom is D14's race-to-idle rule made quantitative. Mistakes stay bounded: the fps SLIP
signal climbs back and D23 fail-memory prevents cycling.
On-device: **Contra sinks 1008→792→576→408, gen 60.2/60.1 — full speed at the OPP floor**
(post-D25 NES is 8-bit-cheap and now runs like it). PS1 Tony Hawk: ceiling stays 1800 (attract's
FMV/3D spikes keep p95 high) but schedutil lives at 1416 with dips to 1008 — the D14 sweet spot
with burst headroom, arguably better than a hard cap. Note: the controller ladder is arithmetic
(1800−216=1584); the kernel quantizes ceilings to real OPPs, so 1584 ≡ the 1416 OPP effectively.

## D29 — NextUI PR mining: two real adopts, several verified non-applies (2026-07-02)
Mined LoveRetro/NextUI's PR queue (open+merged+closed) for shared-ancestry fixes. Adopted, both
validated on-device (3-cycle soak through the new path, 3/3 clean):
1. **Suspend-script hardening** (idea: NextUI #632): our `bin/suspend` ran a bare `echo mem` under
   `set -euo pipefail` — a failed suspend write aborted the script BEFORE the service-restore ran
   (radios/audio left torn down) and fed `PWR_deepSleep` a failure → spurious retry/power-off. Also
   guards the A133P quirk where the write returns nonzero despite a successful suspend (asleep ≥5s
   → treat as success). Likely explains the D27 soak's single 1s-abort anomaly.
2. **`RETRO_ENVIRONMENT_SHUTDOWN` handler** (idea: NextUI #699): env cmd 7 had no case, so cores
   with in-game quit menus (PRBoom, PICO-8) hung on quit. Now sets `quit = 1`.
Verified non-applies: picodrive `ARCH` (#636 — already falsified byte-identical here), rewind/
cheat/SRM-compression/resampler fixes (NextUI-only subsystems), governor stuck-boost class (#733 —
impossible here: our ceiling re-asserts every tick), white-point gamma LUT (#760 — feature, but
NOTED: the `/dev/disp` gamma ioctl WORKS on this kernel — a breadcrumb for the DE-scaler research).
Open question for the user: NextUI's #461 2s-resume-mute (alsactl restore) — chase only if heard.

## D30 — Full fork-archaeology campaign closed: 380 NextUI PRs + 811 MyMinUI commits triaged (2026-07-02)
Every finding verified against our tree before verdict (three tempting "fixes" would have actively
broken the software render path — #286 disables load-bearing GFX_resize/clearAll "because OpenGL").
**Adopted (5 total):** suspend-script hardening (#632), RETRO_ENVIRONMENT_SHUTDOWN handler (#699),
`/tmp/stay_awake`+`/tmp/stay_alive` suspend-inhibit locks (#756), msettings accessor null-guards
(#273 class — guarded at the source, all 8 accessors), supafaust thread affinity emu=0x3/ppu=0xc
(#161, documented in the core's own README; A/B on Aladdin: work within noise, pinned trended lower
clocks with zero excursions above 600 — kept; the claimed big win is Mode7/SuperFX, untested).
**Watch-items (symptom-tied, don't fix speculatively):** #461 2s-resume-mute (only if heard);
MyMinUI `74934b9a` Config_readOptions in SET_CORE_OPTIONS (only if a core drops saved options).
**Facts banked:** NextUI #300 independently converged on the 408 floor; Brick exposes
`/sys/class/motor/voltage` (variable haptics, 0.5-3.3V); the `/dev/disp` gamma-LUT ioctl WORKS on
this kernel (NextUI #760 — breadcrumb for DE-scaler research); MyMinUI's reliability fixes mostly
fix its own refactor (its portable wins were already extracted in the optimization sweep).
Method note: PR queues beat commit logs for this — they carry the discussion, the rejected
alternatives, and fixes for code both forks inherited. Zero's edge is verify-then-adopt with
on-device validation, which turned a rival's review queue into five shipped fixes in an afternoon.

## D22 — CPU core hotplug: exact break-even, closed (2026-07-01, on-device drain A/B)
Offlining cpu2/3 for a light workload (Genesis attract loop, 12-min `charge_counter` windows,
back-to-back same scene): 4 cores = 60 units, 2 cores = 60 units. Dead heat. `cpuidle` already
power-gates idle A53 cores, so hotplug only formalizes what the hardware does on its own — and with
keymon at 0 idle wakeups/sec the spare cores genuinely stay parked. No per-system hotplug bracket;
don't re-chase. (Break-even #3, after GPU-dark games and picodrive ARCH.)

## D31 — CPU Speed menu item hidden; the no-overclock stance reaffirmed (2026-07-03)
**The knob was dead:** since the closed-loop governor shipped, it re-asserts its ceiling every tick
(~0.5s), so the static CPU Speed pick (Powersave/Normal/Performance/Overclock) was overridden almost
immediately. A knob that does nothing is worse than no knob. Hidden via the cfg lock (`-minarch_cpu_speed`
in `system.cfg`/`system-brick.cfg` — zero code), and the seven inert per-pak `minarch_cpu_speed` relics
removed. The dev escape hatch remains `GOV_DISABLE=1` (D9). Considered and rejected: adding a "Smart
Variable" choice — the governor IS smart-variable, always, for every game; the fix was making the UI
stop pretending otherwise.
**Overclock stance (user asked "would there ever be a reason?"):** NO — reaffirmed, with reasoning:
1. Every "needs more clock" case this project has ever hit turned out to be a BUG, not a clock
   shortage (Contra = upstream sndquality config, D25; SNES "saturation" = detector artifact, D24).
   "Needs overclock" is a debugging smell, not a requirement.
2. No documented title slips at the 1800 stock max. Building OC support for a hypothetical violates
   the lean rule.
3. If such a title ever appears, the escalation ladder is: core option audit → alternate core →
   accept it's a bad fit for the device. The 2.0GHz OPP stays untouched — "never overclocks" is in
   the README as a product promise, and D14's race-to-idle work shows raw clock is rarely the answer
   anyway.

## D32 — Smart Pro fully validated: the second device now has the same evidence file as the first (2026-07-03)
First-ever instrumented session on Smart Pro hardware (via our own shipped dropbear — the device
firmware has no SSH server; the static build segfaulted, the dynamic build shipped). Findings:
- **Governor sweep: PASS, numbers rhyme with the Brick** — GBC 408@65%, NES ~816@100%, SNES 600@140%
  (multi-threaded), MD/GBA 600, PS1 hunting 1008-1584 with the demo's varying load exactly as the
  closed loop should. Panel runs ~61Hz vs the Brick's 60.8; the generation-rate detector is
  indifferent, as designed.
- **GPU-dark menu: UNGATED — works on both devices.** The Smart Pro "black menu" that motivated the
  gate was the model-cache misdetection (D-note 2026-07-02), not the fb path: PLAT_flipFB is
  geometry-agnostic (runtime vinfo/stride), fb verified 1280x720/5120 pixel-perfect by screenshot
  WITH the GPU suspended. Gate removed; the earlier gate was a misdiagnosis bandage.
- **Menu layout: upstream shipped the Smart Pro an 80px inset** (PADDING 40 vs the Brick's 5) and
  8 rows tuned to it — the "too much padding" report. Now PADDING 10 (20px ring) + 10 rows,
  fb-screenshot verified, user-approved.
- **Deep-sleep certification: 50 cycles, 0 wake-hangs**, game survived all 50, 2 benign insta-abort
  entries (same safe class as the Brick's single one; runtime retries cover it). 30-33°C, ~2%/hour
  while cycling. Default-on deep sleep is now evidence-backed on BOTH devices.
Remaining (user thumb-tests): analog sticks in a PS1 title, FN mute switch behavior.

## D33 — Smart Pro audio saga: stock had it right; the bug was elsewhere (2026-07-03)
User report: "volume doesn't work at all" on the Smart Pro (+ FN does nothing + aspect wrong).
Diagnosis wandered through two WRONG fixes before the ear tests settled it:
- WRONG #1: "DAC restore should be a percent" — the SP DAC range runs 0-255, but 255 = +72dB of
  digital GAIN (blown-out audio, ear-verified). Raw 160 IS the 0dB reference on BOTH devices.
- WRONG #2: "digital volume direction is normal on the SP per its dB table" — ear test showed
  volume-up got quieter. The control is attenuation-coded (reversed) on BOTH devices.
- The REAL story: the card's saved volume was 0 (silent boot state) + keymon had been killed by a
  deploy and never restarted (launch.sh starts it once per boot — remember to restart it manually
  after killall). Stock/NextUI semantics were correct all along and are now restored verbatim,
  with comments warning future readers off both traps. Fixes that DID survive: proper sh redirects
  (">/dev/null 2>&1", not the ash-mangled "&>"), Aspect default in system.cfg (non-brick file was
  missed when the brick one was set), SP menu layout (D32), FN mute confirmed handled natively by
  trimui_inputd (nothing of ours in the path; gpio243 is NOT the FN switch on the SP).
Lesson enshrined: dB tables suggest; ears verify. Trust the fork lineage (stock == NextUI == ours
now) over a clever reading of sysfs.

## D34 — Smart Pro-specific rails: all probed, all closed (2026-07-04)
Recon of hardware the Brick lacks, looking for idle waste. Verdicts, all on-device:
- **Analog stick power rails (gpio110/114): EXACT drain tie** (200 charge units/25min with rails
  on AND off, menu idle). No win exists; rails stay stock-on. Nobody in the scene ever measured
  this — now it's measured. Don't re-chase.
- **HDMI: the RK628 bridge is devicetree-only** — never instantiated on I2C, no driver bound,
  nothing to power down (same class as the Brick's DRAM rail probe). Even NextUI's GetHDMI() is
  a hardcoded `return 0`.
- **trimui_inputd: event-driven at idle** (0 jiffies over both 25-min windows) — the "stick
  sampling cost" concern was unfounded.
Conclusion: the Smart Pro has no device-specific waste; its only real deltas vs the Brick are
the panel scanout (no GPU-dark, D-logged) and the DAC range (D33). Remaining SP data gap:
its own battery baseline (the 7.5h figure is Brick-measured).

## D35 — Dynamic rate control ships: sync stutter fixed by feedback, not measurement (2026-07-04)
The classic MinUI stutter (one duplicated frame every ~1.3s) is a clock mismatch: cores generate
60.0, the present path runs faster. Fix = retune the audio-paced core speed to the display, the
RetroArch-proven idea, adapted to MinUI's audio-blocking architecture as a FEEDBACK controller:
audio blocked during the window -> below the present rate, +150ppm; never blocked -> vsync is
binding, -150ppm. Equilibrium = the true present rate, self-tracking, no constants to trust.
Two attempts documented: (1) measuring flip intervals FAILED (the 60.0 pacer contaminates the
measurement — you measure the system's cadence, not the panel's); (2) the controller cannot be
fooled by definition. And it promptly proved the point: the Brick's LCD advertises 60.8Hz but the
GLES present path actually gates at ~60.2 — a wrong constant we WOULD have shipped. Cost at lock:
+0.33% speed, +6 cents pitch (imperceptible). Guards: ~60fps cores + strict vsync only, FF/menu
freeze it, heavy games self-disable (ratchet to 0 = stock), ZERO_NO_DRC kill switch. Foundation:
the D-resampler (linear interpolation, same commit series) — nearest-neighbor would have turned
the ppm adjustments into zipper noise. User-verified on Sonic 2: no stutter, no tears.
Pillar 3 (tear-free, stutter-free, drift-free pacing) is complete.

## D36 — Latency: swap chain shrunk to 2 buffers via driver hint, fence not needed (2026-07-04)
The PowerVR present queue held ~3 frames of input latency. Probed the driver before writing code:
libpvrNULL_WSEGL honors NULLWS_BUFFERS_COUNT (validated range starts at 2). setenv in minarch main
= one frame (~16ms) less input lag for zero CPU/heat — no glFinish/fence needed (that path risked
spin-waiting against race-to-idle and stays unbuilt). Verified on-device: governor, DRC lock, and
temps unchanged; user-approved feel on Sonic 2. This closes the NextUI-claims triage: resampling
(linear, D-resampler), sync stutter (D35 feedback DRC), latency (this) — all three shipped as
measured, lean adaptations instead of an engine rebuild.

## D37 — v1.1 complete: SP calibrated by the shipping tool; first production run caught two gen_table bugs (2026-07-05)
The Smart Pro ran the full Tune Voltage flow as its first real user: pitch -> disclaimer -> 90-min
campaign -> green check. Its map: 1800@1037.5, 1608@950, 1416@887.5, 1200@812.5 (all stress-fail
detections, no crash-reboots needed); gaming range never cracked at the 762.5 floor. Margin 125mV
(vs the Brick 137.5 — near-twin silicon, Brick slightly luckier). Dogfooding immediately paid:
(1) gen_table shipped the RAW tested floor for no-cliff OPPs (zero guardband on the gaming range,
the OPPs games actually use); now floor+GUARD, matching the cliff philosophy. (2) the margin
parser read awk field 4 ("stock", the word) instead of 5 (the number) — the tool would have
displayed "-138% less CPU power". Fixed + arithmetic hardened against non-numeric parses.
Validation methodology settled: pinned-clock A/B with PROOF-OF-HELD rail sampling (two earlier
attempts invalidated themselves — kernel re-stock + thermal throttle at 1800); clean result
@1416: stock +11C vs undervolt +8C for identical 60s stress = ~27% less heat, consistent with
the V^2 prediction (~19%; "up to 20%" at the top OPP is honest: 1062.5^2/1187.5^2 = 0.8006).
WoWLAN probed: impossible (wlan0 has no wakeup attribute); dev answer = the disable-deep-sleep
flag; "summonable sleep" (RTC-heartbeat wake windows) designed but unbuilt.

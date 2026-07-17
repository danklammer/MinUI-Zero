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
the V^2 prediction (~19% for that pre-envelope row). D51 supersedes this as a production
claim: the corrected ceiling envelope uses 1075mV at 1800MHz, about 18%.
WoWLAN probed: impossible (wlan0 has no wakeup attribute); dev answer = the disable-deep-sleep
flag; "summonable sleep" (RTC-heartbeat wake windows) designed but unbuilt.

## D38 — The boot-loop incident: a never-booted minui shipped to both devices at once (2026-07-05)
The undervolt engine (platform.c) compiles into BOTH minarch and minui. For days the build
pipeline shipped a STALE pre-uv minui (77624 bytes) in every zip while minarch carried the
new code — so all testing exercised the new engine only in games. The first build containing
the new minui (86696 bytes) reached both devices in the same hour (live push + staged zips)
and dies in GFX_init before drawing a pixel — both devices boot-looped. Recovery: proven
minui restored via card; rescue asset -6 = full v1.1 system + proven menu binary, then
hardware-certified on BOTH devices through the real updater path. Root cause of the GFX_init
death: UNKNOWN — autopsy owed before ANY rebuilt minui ships. Policy from tonight:
(1) a binary's first boot happens on ONE device, with eyes on the screen;
(2) every device file push gets a receiving-end md5;
(3) the menu never needed the uv code — consider a compile guard excluding it permanently.
Haptic power cue reverted pending the autopsy (likely an innocent passenger on the doomed
binary; re-add cleanly after root cause).

## D39 — Boot-loop root cause FOUND + fixed: regulator sysfs read at early cold boot panics the BSP kernel (2026-07-05)
Panic-proof breadcrumbs (O_SYNC per-line file writes surviving kernel panics) + a self-healing
two-boot diag harness (auto.sh restores the proven binary on the boot after a diag run) cornered
it in four automated rounds: minui init all succeeds; the panic fires inside uv_init's DECODE
GATE — specifically the /sys/class/regulator scan cross-checking the PMIC voltage — when run
seconds into a cold boot, while the kernel power subsystem is still settling. Explains every
symptom: menu dies (runs at boot), minarch never dies (launches minutes later), warm respawns
fine (kernel settled), both devices identical (same BSP). TWO fixes: (1) the uv engine compiles
ONLY into minarch (-DZERO_UV_ENGINE; the menu carried ~9KB of engine it never used); (2) minarch
defers uv arming until 30s uptime (auto-resume can launch a game straight from boot into the
same window; uv_fd stays -1 so the governor tick retries until armed). Verified: fixed minui
cold-boots to menu (77728 bytes = proven size + the haptic cue); uv still arms in games
(hold thread @ 850mV observed post-fix). ALSO fixed forever: the stale-build trap that shipped
a days-old minui in every zip (D38) — the workspace makefile now cleans minui/minarch before
every build. The two incidents compound one lesson: the menu binary is boot-path code and gets
boot-path rigor.

## D40 — GPU clock cap: probed, all safe knobs absent, SHELVED (2026-07-05)
Mid-PS1 probe showed the GPU pinned at its top OPP (702MHz @ 1100mV; ladder 233@950 /
350@950 / 702@1100, driver dvfs:0 = dynamic scaling off) to do one blit per frame. Every
runtime control probed and dead: /sys/class/devfreq EMPTY (framework not exposed);
scenectrl/command = a vendor BOOST hint (useless at max); pvrsrvkm module params = stock IMG
plumbing, no freq knob; debugfs clk writes REJECTED (kernel lacks CLOCK_ALLOW_WRITE_DEBUGFS).
Remaining paths = raw CCU MMIO pokes (live PLL glitching = GPU hangs) or DT surgery (boot
flash — same risk class as the declined DTB undervolt). Win is also bounded smaller than the
702MHz headline: the GPU-dark-games A/B (GLES vs software scale = exact break-even) plus
EnableRDPowerIsland=2 indicate per-frame power-islanding already recovers much of the waste.
Risk >> bounded reward -> shelved beside the DE scaler. Same probe validated THPS2 governor
behavior as CORRECT: it sank 1800->1368, the game dropped to 48/60, it recovered and held —
with schedutil duty-cycling 1416<->1800 beneath the ceiling at 33C.

## D41 — PS1 battery measured + the fuel gauge caught distorting by charge region (2026-07-06)
Three-window drain campaign (THPS2 @95-80%: 300 units/h; Zelda DX @59-36%: 460/h; THPS2 again
@35-21%: 560/h — all dim brightness, discharging, game verified alive per sample). Two findings:
(1) the axp2202 gauge's units inflate ~85% from full to near-empty for the SAME true load
(voltage sag explains only ~14%) — single-window extrapolations are unreliable on this hardware;
all future drain claims come from full drains or adjacent windows. (2) The honest PS1 number:
interpolating THPS2 to Zelda's region gives PS1 = GB + ~3-15% — after the governor + undervolt,
the PANEL dominates the power budget and PlayStation costs about a Game Boy. Publishable: GB
~7.5h -> PS1 ~6.5-7h (dim). The earlier single-window "~10h PS1" figure is retracted as
top-region gauge flattery. Method note: attract/demo modes make honest unattended loads;
drain scripts must quit the game at the end (an unattended THPS2 kept running 2.7h post-test).

## D42 — Fast-forward retargets the governor (2026-07-07)
A Reddit question ("how does auto clock work in FF?") exposed a real gap: the governor only
raises on gen<target, and FF generates ABOVE target by definition — so FF ran clock-starved at
the settled floor (measured: Zelda DX 2.2x of the 4x cap at 408). Fix is one idea: FF is not an
exception, it is a different target — gov_target_fps = core.fps x (max_ff_speed+1); the loop
then finds the lowest clock that holds THAT. No sinking while FF is held (work measurement is
unreliable during skipped presentation). Measured: full 4x at 1008MHz/32C, clean return to the
408 floor on release. Cool Pokemon grinding, no pinned max.

## D43 — Thread-aware governor + mailbox threaded video (2026-07-07)
Stock MinUI ships core/render threading hidden as "Prioritize Audio" (double-locked away on
tg5040: system.cfg AND PS.pak). Its handoff was measured broken: main held core_mx through the
vsync wait, signals were lossy, the condvar was re-initialized per frame. Rebuilt as a
triple-buffer mailbox (core swaps under a brief lock, latest wins, main presents outside the
lock) + drift-free core cadence (bursty core.run reads as "behind" to pcsx's auto-frameskip:
15 real + 46 skipped frames/sec, in BOTH modes — that ratio is the game's internal render rate,
not a bug) + the governor judges the CORE thread's window utilization (per-frame budgets
misread internally-low-fps PSX games; util_next <= 0.85 gates the sink). DRC ppm is honored by
the core pacer so the sync-stutter fix survives threading. Validated: ceiling breathes
1008<->1440 with demo scenes at 32C, full gauntlet green (persisted-ON clean quit, FF round
trip, menu park/resume, sleep/wake with zero gov moves during sleep).
Audit trail: three passes + two independent reviewers = 15 findings, all fixed — headline
catches: startup thread creation skipped the liveness flag (clean exit = dlclose SIGSEGV,
caught only because the gauntlet tested the PERSISTED path, not just runtime toggling), and
the debug HUD's new "THR" label indexed NULL font entries (T and R glyphs never existed —
found by core-dump autopsy, fixed with pixel art). blitBitmapText also gained hard clipping:
its unclipped border writes were absorbed for years by over-allocated core buffers and
segfaulted on the exact-size mailbox buffer.
Option renamed conceptually (still "Prioritize Audio" in UI pending rename decision), unhidden
for PS1, still hidden for SUPA (supafaust pins its own threads). Default Off until hands-on +
gameplay floor A/B decide promotion.

## D44 — Auto-threading ships in v1.3 (2026-07-08; superseded by D51)
Dan's call: no per-system threading list, no visible option ("I'd hate to give an option then
take it away") — the machine decides, same charter as the clock. Implemented as measure/decide/
remember: launch single-threaded (safe for any core incl. sideloaded), trial threading when the
settled ceiling shows headroom pressure (>=1008 at a 60s check; floor-dwellers re-arm and never
trial), commit only if the ceiling verifiably sinks a step (slips raise the ceiling, so
instability self-fails the trial), persist the verdict in a per-game sidecar (game cfgs get
rewritten by the options menu). Explicit user cfg wins and disables auto. Validated on-device,
all five cells: BR2 honest-revert (its bottleneck needs 1800 regardless — also disproves the
"v1.2 runs BR2 worse" report on stock volts: 60/60 at 1800/35C mid-fight), DKC commit
(1200 -> 768 in trial, boots threaded at 600 thereafter), Zelda floor-dweller never trials,
both verdict polarities honored on relaunch. Threshold 1008 provisional pending the full
benchmark matrix.

## D45 — The smoothness dig: DRC was dormant in production (2026-07-08)
Dan's feel report ("threaded THPS not quite as smooth") triggered an instrumentation campaign
that went through THREE wrong sensors (waits!=dups: main normally waits every frame; flip
intervals: blind to internally-low-fps content AND to the panel beat; flip-block time: queued
swapchains always block) before unearthing the real finding: **DRC — the v1.1 sync-stutter
fix — has been ineligible in production the whole time.** Its gate required VSYNC_STRICT;
the tg5040 system.cfg locks prevent_tearing=Lenient. Both threading modes had the ~1.3s
panel-beat dup; threading was never a smoothness regression. Fix shipped: eligibility accepts
any vsync!=OFF (Lenient vsyncs every frame while the game holds rate = DRC's regime).
STILL OPEN: (1) on-device convergence verification of the revived single-thread DRC (needs a
ppm telemetry line; the only current print fires once at +6000ppm); (2) threaded-mode panel
lock — the correct sensor is a long-window precise flip-rate measurement (60.0 vs 60.8
distinguishable over ~10s; push ppm up until the flip rate plateaus at the panel ceiling =
locked). Both queued for a fresh session. New permanent instrumentation shipped along the
way: HUD D/S/U smoothness counters + per-minute thr-stats log lines in threaded mode.
Also today: live gameplay validation via forged-input navigation (Tony Hawk driven into a
real Hangar run by synthetic button presses; true-60fps delivery measured 0 dups / 0 skips /
0 underruns per minute — DELIVERY is clean; the panel beat is the remaining cosmetic item,
now equally present-or-absent in both modes pending the revival verification).

## D46 — Codex review round + panel-lock convergence (2026-07-08, late)
Dan ran the branch through Codex: 7 findings, 6 real, all fixed same-night. Headline: config
load was arming toggle_thread while thread_video was still false — persisted-On booted
threaded then tore itself down on the first main-loop pass. THAT was the unsolved ON-path
phantom (BR2 Threading=On death). Regression-proven: persisted-On now boots 11 threads and
holds. Also fixed: park_core() mandatory before menu/hdmi core access (500ms, logged), FF
None retargets to max (was floor-starved), trials abort on menu/FF interruption, On-Off-Auto
cycling cancels stale toggles, legacy minarch_thread_video honored as alias. Codex's
architecture note (replace thread_video/was_threaded/toggle_thread booleans with an explicit
lifecycle state machine) = the v1.4 refactor.
Panel-lock v2 (long-window flip-rate hill-climb, 1000ppm stride): converged in ~1 min and
held (ppm=850, dups=0, skips=0, underruns=0 across 3 min of THPS). Caveat: locked below the
panel's nominal 60.8 figure — either the swapchain drain differs from scanout or the beat is
not fully closed; instruments cannot distinguish, human eyes next session.
Bench honesty note: unattended benchmark arms ran with the 30s idle DIM active (both arms
equally — comparisons stand; absolute cc figures are dim-screen values).
Review roster for this feature, final: 3 self-audits, fork adversary, blind reviewer, a
16-arm benchmark campaign, Dan's ears (3 real bugs), Codex (6 real bugs). Every reviewer
found something all the others missed.

## D47 — The BR2 audio-chop investigation: a hardware-class boundary (2026-07-08, late)
Dan's ears reopened the case the counters had closed. Systematic exoneration ladder, each arm
measured on the exact logo->demo sequence (underruns): governor (30 at PINNED 1800 — not
clocks), revived DRC (chop survived the revert — not DRC; but the revival DID chop PS1
separately and was reverted, see D46), ring capacity 5->12 frames (33 — not buffering),
DAC prefill gate at 40% (23 — kept: right on principle, adapted from NextUI), pcsx Threaded
SPU (25 — not SPU contention). Invariance across every lever = structural: telemetry shows
42-60ms MDEC decode frames; during each, audio production halts beyond what any ring rides.
CONCLUSION: BR2's non-interactive sections exceed the A133P's stock-clock realtime capability
in pcsx. Gameplay is measured-flawless (60/60 fights, zero underruns). Others cover this class
with the 2GHz overclock; we do not, by charter (reaffirmed by Dan mid-investigation). The
original user report is hereby explained: their NextUI comparison ran Performance=2000.
Fixes kept from the dig: audio prefill gate (real, principled), audio-ring capacity note,
per-minute drc/thr telemetry. The reporter reply: gameplay receipts + the honest boundary.

## D47 addendum — replication closes it (2026-07-08, later still)
Dan rejected the first closure ("still sounds like shit") — correctly: the tuning sweep had
not run. It ran: psxclock=50+lighting-off scored 11 (hope!), psxclock=45 scored 30, and the
50-replicate scored 30. Full series: 30/23/33/25/11/30/30 = ~27±7, one lucky outlier, seven
configurations. NOTHING moves it. The MDEC-section boundary is now REPLICATED, not asserted.
Methodology lesson recorded: single short-sequence runs carry ±7 noise — the 11 fooled us for
one arm; conclusions need replicates (the 8-min benchmark pairs are long enough to be stable;
3-min sequence probes are not). BR2 final state: gameplay flawless (multi-day receipts),
non-interactive MDEC sections chop ~10 gaps/min on ANY stock-clock configuration; the only
cure is the 2GHz overclock, declined by charter, twice, with eyes open.

## D48 — D47 overturned: the "hardware boundary" was four software layers (2026-07-09)
Codex's profile-first roadmap cracked what seven invariant configurations couldn't: the MDEC
profiler split the 42-60ms frames into rl2blk+idct (~80ms/2s) vs yuv2rgb (~115ms/2s) and the
split counters caught BR2 decoding EXCLUSIVELY on the 24bpp path (blocks15=0) — the first
NEON port (yuv2rgb15) was dead code and only the counters could have said so. Four fixes,
each measured on-device, all at stock 1800:
(1) NEON yuv2rgb24 — 115→50ms/2s window; attract underruns 27±7/run → ~0-1/11min (the D47
    "boundary" itself). Ships in patches/pcsx_rearmed.patch, fresh-clone-proven.
(2) Crisp scaler's fixed 4x NN prescale made 480i menus into 2048x1920 GPU render targets
    (~1GB/s fill on the GE8300) — present blocked, machine slowed, CPU idle. Now: smallest
    integer prescale that reaches the panel (2x for 480-line; 240p unchanged at 4x).
(3) Governor limit-cycled on heavy screens (sink into a known-bad ceiling every FAIL_HOLD
    expiry = audible collapse ~every 60s). Now: repeat-offender holds escalate 60s→2m→4m→8m
    and a slip that survives one climb step bursts straight to f_max (race-to-idle: brief
    over-provisioning is inaudible; a crawling climb is not). Harness green.
(4) THE decisive layer: pcsx's async GPU thread (upstream default-on for multicore) —
    gpu_async's scanout-sync handshake serialized emu<->gpu threads on 480i screens (both
    half-idle, gen 41/60 at pinned max; raw work = 86% of ONE core). Config-only fix:
    pcsx_rearmed_gpu_thread_rendering=disabled, now the PS.pak default on both devices.
    Fights: HUD-verified 60/60 @1800/39C.
The no-OC charter survives with arithmetic teeth: the 2GHz OC is +11% against a 46% deficit —
NextUI/CrossMix users running this game overclocked are still degraded on these screens and
NEITHER fork touches any of the four causes (both ship async-GPU-on + brute-force clock).
Codex's speculative gpu_async scanout patch was reviewed and unwound (dormant once the thread
is off; preserved in .notes/upstream/ as an upstream PR candidate). Threading verdicts learned
under the old baseline are void — PS sidecars cleared, PS1 bench rows re-run before v1.3.
Lesson for the record: D47's replication was sound but its conclusion over-reached — we
replicated the SYMPTOM's invariance across knobs we had, not the absence of causes we hadn't
found. "Hardware boundary" claims need a profile, not just an exoneration ladder.

## D48 addendum — the fifth layer, and Dan's sign-off (2026-07-09, morning)
Dan's fresh ears found the residue the fixed stack still carried: "improved, still crunchy" —
and the HUD caught it live at 1008MHz/52-per-60 on character select. The crunch was the
GOVERNOR'S PROBE COST: sink to a too-low ceiling, several audible ticks of stepwise recovery,
repeat at every fail-hold expiry. Two refinements shipped (cbdec065, harness green):
(1) a slip arriving within ~8 ticks of a sink is CAUSED by that sink — restore the pre-sink
ceiling in one tick (thermal sinks deliberately excluded: temp always wins);
(2) GOV_SIGNAL_BIGSLIP (gen >=10% under target) jumps straight to f_max on the first tick.
Validation, 75s parked on char select: probe ladder 1584/1368/1152 all held 60/60 (the
GPU-thread fix lowered this screen's true floor from ~1584 to ~1152), the 1008 probe slipped
to 52.4, signal=3 recovered in ONE transition, ZERO ALSA underruns across the window
including the collapse — the ring cushion rode the entire dip. Dan: "Sounds pretty good!"
BR2 CLOSED at stock clocks, 35-40C. The Phase-3 480i render-skip was never needed.
Watch what happened in that log: the governor learned the screen's floor by touching it
once, briefly, inaudibly — the closed-loop thesis in one trace. Also caught during deploy:
per-game cfgs snapshot ALL core options at save time, so any pre-v1.3 saved PS game cfg pins
gpu_thread_rendering=auto forever — release notes must tell users to re-save or reset
per-game settings on PS titles (or minarch grows a migration; lean answer TBD).

## D48 second addendum — scene-change burst + Crisp restored (2026-07-09)
The last residue, ear-caught: brief crunch at TRANSITIONS (title->demo, VS card). Cause:
scene changes land on whatever clock the governor had settled for the previous scene; the
climb happened after the slip. Fix (35d941c7): gov_burst() — a video-mode/geometry switch is
an early-warning bell that fires BEFORE the new scene's cost, so the ceiling jumps to f_max
at the announcement and the sink ladder re-finds the floor afterward. Event-driven, no
polling. Hooked at the selectScaler trigger in minarch (fires only on real source-size change).
Crisp A/B: PASSED both judges — with the minimal-prescale fix, Crisp == Soft on the worst
480i screens (0 underruns/90s, same probe ladder, floor 1152) and the look was approved.
Crisp stays the shipped default; the Soft diagnostic line removed from the device cfg.
Ear-verified transitions clean. The v1.3 candidate stack is complete.

## D48 third addendum — SP bring-up: two more layers, both devices certified (2026-07-09)
The SP sounded "very crunchy" on the same screens the Brick had just passed. Two real causes:
(1) THE CFG-SHADOW TRAP BIT AGAIN, second device in hours: the SP console minarch.cfg
(written when sharpness was changed in its menu) snapshotted gpu_thread_rendering=auto,
silently overriding the new pak default — the GPU thread measured 51% busy while "disabled".
The v1.3 updater MUST ship a one-time cfg migration sweep (auto->disabled across saved PS
cfgs); release-notes-only was proven insufficient by our own two devices.
(2) SP-only supersample blowup: selectScaler oversized dst = MAX(ceil-cover) picked scale 3
for 512x480 on the 1280x720 panel -> a 2048x1440 padded dst the CPU scales into and uploads
EVERY frame (~3x the Brick cost for the same screen). Fix (045d04af): cap the supersample
at 2x panel area — every pre-existing Brick/SP case unchanged, only pathological combos
shrink (SP 480i: scale 3->2 = 1024x960). Upstream had this cap commented out as a TODO.
Also fixed in the same commit: GOV_SIGNAL_BIGSLIP now outranks the probe-undo restore (a
deep deficit restored to a too-low pre-sink ceiling paid two ticks; SP log caught it).
Auto-threading sidecars written under the broken baseline were cleared again — trials only
count when the environment they judged is the environment that ships.
Result: GPU thread verified dead on SP (0 jiffies), ear verdict "sounds way better."
BOTH devices now run the identical ear-certified v1.3 candidate.

## D49 — The hardcore regression campaign: five governor bugs, one myth, all floors (2026-07-09)
Dan called for hardcore testing after the BR2 stack landed; the grinder paid for itself.
GOVERNOR (all fixed same-day, each with a synthetic-harness regression test):
(1) escalating fail-hold; (2) burst-to-max on persistent slip; (3) fail memory must only
arm BELOW f_max (boot slips at max banned all sinking — the GBC 1008-pin, found by gate
telemetry showing signal=SLACK/p95 7.7ms with a frozen ceiling); (4) bursts only on TRUE
size changes (same-size SET_GEOMETRY resyncs re-fired selectScaler); (5) BUSY no longer
erases slack progress (sporadic ~20ms stall windows starved the 4-consecutive-slack rule).
Verdict after fixes, cross-device: GBA/FC/SUPA/MD at-or-below historic floors, PS1 cruising
1008 with correct scene bursts, GBC honestly holding ~1008 (gen craters to 20.7 at 408 —
the +2ms-vs-07-03 scene-cost audit is filed, it is NOT a governor defect).
THE SLEEP "CRASHES" WERE A MYTH: every mid-campaign reboot traced to api.c's designed
fallback (idle 2min -> deep sleep; 3 failed suspends OR the disable-deep-sleep flag ->
clean PWR_powerOff). The flag literally guarantees poweroff-after-idle — the Deep Sleep
tool should say so (task filed). Unattended harnesses need input pokes (L3; NOT L2 — L2
toggles FF and poisoned one run's floors).
THPS2 IN-LEVEL A/B (automated level-runner, recorded input sequence): threading Off vs On
both settle at the 1008 bracket floor, 60/60, 0 underruns. The GPU-thread-off baseline
moved the single-thread gameplay floor from ~1584 (thread-campaign estimate) to 1008 —
PS1 frontend threading is now moot for this class; its value concentrates on SuperFX SNES.
15BPP MDEC HUNT: probe core over the full 44-game PS library — every FMV that decoded
(BR2, Blood Omen, SotN, Wipeout XL at 14k blocks/2s) is 24bpp; zero 15bpp users found.
Ship decision: yuv2rgb15 routed back to upstream scalar (d7332124) — unverified vector
code does not ship hot; NEON15 returns if a verified specimen appears.
Reusable harness artifacts from the campaign: single-arm floor runner, keep-awake poker
daemon, and the visually-verified level-runner (press -> fb-capture -> confirm -> record).

## D50 — The floor brownout: undervolt vs light load, caught by variety testing (2026-07-09)
Every "GBC crash" in the campaign was one bug: a calibrated-undervolt device REBOOTS within
minutes when a light game dwells at the 408 floor. Dr. Mario never triggered it (its attract
scene pins ~1008); Donkey Kong sinks to 408 and died twice, heartbeat-documented (final
words: game=1 cur=408000, then power loss). Controlled A/B/A on the same scene:
UV on -> dead in ~2.5 min; ZERO_NO_UV=1 -> 12 min clean; UV on + floor guard -> 12 min clean.
ROOT CAUSE (physics, consistent with all data): the P2 calibration proved 762.5mV under
STRESS; a game idling at the floor puts the TCS4838 buck into light-load/PFM mode where the
same undervolted rail is not stable. Stress-proof != idle-proof — a calibration-methodology
gap, now recorded.
FIX (8c02bd31): PLAT_setCPUVoltForCeil stands down at or below an 816MHz ceiling. Idle
current at the floor is tiny (the delta saved uW), the measured UV wins live at the high
OPPs, so the guard costs ~nothing. Future calibrations must add idle-dwell arms per OPP.
Exposure: opt-in feature (calibrated devices only) — but OUR two devices are exactly that,
and any Tune Voltage user would have hit it. Ship-blocker caught before ship.
Credit where due: Dan's "test different games" call found in one afternoon what the
first-rom sweep design would never have hit.
Also shipped same evening: Stay Awake dev tool (workspace/tg5040/dev-tools/, never
packaged) — blocks all autosleep + WiFi power-save via the existing /tmp/stay_awake flag,
reboot-persistent; ends the harness sleep-roulette documented in D49. Codex audit round 2
verified NEON MDEC scalar-equivalence independently and yielded 5 fixes (0b0c76a5): hard
1.8GHz choke clamp, unconditional uv restore, clean-slate auto-thread trials, watchdog-gated
calibration, full-chain make targets; C11-atomics telemetry hardening deferred to v1.4
deliberately (post-soak sync rewrites invalidate certification).

## D51 — v1.3 release audit: frontend threading deferred; UV publication hardened (2026-07-11)
The final whole-branch audit supersedes D44's ship decision. The frontend thread invokes
scaler/SDL renderer mutation and governor bursts from the core callback while the main thread
presents and ticks the same state. Those are real ownership/data races, and no tested library
title needs the path after the PS1 GPU-thread fix (THPS2 was 1008 MHz either way, with the
threaded arm slightly warmer). v1.3 compiles frontend threading unavailable on tg5040, forces
legacy On/Auto configs to Off, and hides the option. The dormant implementation remains for a
future ownership redesign; core-internal worker threads are unaffected.

The same audit made calibration data fail closed: campaigns bind chip+model before the first
test and verify them on every resume; OPP pinning is ordered and read back; crash breadcrumbs,
stock rows, and exactly one numeric verdict per OPP are required; both VSEL writes require
readback; and table.conf publishes last only after eight validated rows. The generator now caps
each row at measured stock and emits the non-decreasing ceiling envelope. From the preserved
2026-07-11 log this means a 25 mV minimum applied high-OPP reduction and 112.5 mV at 1800 MHz
(about 18% CPU-rail dynamic power), distinct from the 75 mV minimum raw cliff headroom.

The post-audit verification pass found two more fail-closed requirements. Calibration now has
an atomic single-instance lock, and `DONE floor` requires either stock already at the floor or
a successful stress round there; a refused uvtool write or floor stress failure cannot become
a publishable success. Release builds also re-check every non-core pin, verify each core's
tracked source delta exactly equals its declared patches, and clean-rebuild all core outputs.
This prevents stale marker files or stale `.so` files from disagreeing with `commits.txt`.

## D52 — v1.3 disables the feature it was named after (2026-07-11)
The branch was born `feat/thread-aware-governor`; frontend multithreading was the
original thesis. The trial machinery built to prove it instead proved the workload was
the problem: once the FMV decoder went NEON and the serializing async-GPU thread was
turned off, one core covers the tested library at stock clocks (THPS2 threaded-vs-single
A/B: tie at 1008 MHz). The release audit then found the mailbox implementation mutates
renderer state across threads under `-O3 -flto` without a sound memory model. Decision:
ship v1.3 with `ZERO_DISABLE_FRONTEND_THREADING` (option locked Off, machinery
unreachable) — a whole class of latent races removed at no measured cost *on the PS1
titles that motivated the branch* (THPS2 tie; a matched BR2 A/B on the release binary:
both arms ~1800, threaded ran 3°C warmer). The honest cost is on SNES/supafaust, where
the receipts show threading was a real efficiency win: DKC mean 747→600 MHz with coulomb
delta 90→30, Yoshi (SP) mean 1212→741 MHz (docs/bench/2026-07-08-snes-*). That measured
opportunity is deferred to — and is the primary motivation for — the v1.4 thread-ownership
redesign, not denied. Re-enable only after that redesign. The legacy `minarch_thread_video`
migration still parses safely; it now lands on the locked-Off option. Emulator cores'
own internal worker threads are unaffected. Honest data does not care what the branch
is called.

## D53 — calibration freezes must self-recover (2026-07-12)
Dan's verdict after repeated marginal-voltage freezes (one 2026-07-11, two today) each
needing a manual POWER-hold: "It has to reboot on its own" — release blocker. The first deadman
(`sleep && reboot -f`, normal priority) never woke in the frozen state even though the
kernel reserves 5% CPU for non-RT tasks, which means those freezes were deeper than
scheduler starvation (partial core wedge while procd's RT feeder kept the hardware
watchdog fed). Redesign is layered, defense-in-depth: (1) a compiled deadman at
SCHED_FIFO max + mlockall firing the raw reboot(2) syscall — no exec, no sync at
trigger time (a wedged storage stack must not hang the reboot); (2) the kernel's
panic_on_rcu_stall + panic_on_oops armed per stress round with kernel.panic=10
auto-reboot (this BSP has no softlockup detector; RCU stalls are the kernel-visible
signature of a partial wedge); (3) /dev/watchdog0 exists as a possible true hardware
deadline but /sys/class/watchdog is absent on this BSP, so probing it blind (nowayout
unknown) was deemed riskier than shipping layers 1+2 — deferred. Acceptance on
hardware: SCHED_FIFO 99 verified, SIGTERM cancel clean, budget-expiry force-reboot
confirmed by uptime reset. Residual honesty: a wedge deeper than both layers still
needs POWER; the next real marginal-voltage freeze is the live-fire validation.

## D57 — threading v2 F-A: SDL-affine bootstrap stages run MAIN-side (2026-07-14)
The frontend_core F31 bootstrap machine dispatches every stage to the CORE thread via
fr_service. But FC_OP_AUDIO / FC_OP_RENDERER are SDL init (SND_init, SDL renderer) which
MUST run on the MAIN/window thread. Resolution: fc_bootstrap runs on MAIN and, for those
two stages only, calls the dispatcher (dispatch_service) DIRECTLY on MAIN instead of
enqueuing to CORE. This is race-free by construction — between bootstrap service ops the
CORE thread is QUIESCENT (parked in fr_core_service_next, blocked on its condvar), so
there is no concurrent CORE access to the SDL/audio state these stages touch; and the
FC_OP_AV stage has already completed, so core.sample_rate is available to audio_init.
State/boot_failed/cleanup-oracle semantics are identical because the same dispatch_service
runs the stage — only the thread differs. The stages emit no ring products, so no drain is
owed. Proven: the adversarial fake core now asserts thread affinity (audio/renderer see
the MAIN tid, all other stages a CORE tid) with bootstrap-failure coverage still at every
stage including the two MAIN-side ones; TSan/ASan silent. This unblocks the minarch S3.2
device bring-up (which remains interactive Dan+Brick work).

## D58 — fc_boot_stage: MAIN drives bootstrap stage-by-stage (2026-07-14)
fc_bootstrap ran the eleven F31 stages as one back-to-back sequence. Real minarch's
bootstrap is result-consuming and frontend-interleaved: get_system_info's result drives
MAIN directory setup (core.name/paths/mkdir); Config_load/readOptions, Input_init,
SND_init, Menu_init, State_resume run on MAIN between the core stages; and the controller
device comes from Config_readControls state, not a standalone call (see
docs/threading-v2-s32-bootstrap-finding.md). The monolith had nowhere to run that
interleaved MAIN work and no path to hand a stage result back to MAIN. Resolution: expose
`fc_boot_stage(fc*, fc_op)` — drive ONE stage (core.* on CORE via fr_service; audio/renderer
MAIN-side per D57) — plus `fc_boot_finish(fc*)` for the explicit QUIESCENT->RUNNING flip and
`fc_boot_failed(fc*)` for the per-stage check. fc_bootstrap becomes a thin loop over them,
so the monolithic path (and the host harness's default) is behaviorally unchanged. minarch's
guard-ON bootstrap now interleaves its MAIN frontend work between fc_boot_stage calls; the
required stage order (GET_INFO→INIT→OPEN→LOAD→MEMORY→ARM_CRASH→AV→CONTROLLER→AUDIO→RENDERER
→RESUME) is the caller's responsibility, documented in the header. Safe because stages are
serialized: the CORE thread is QUIESCENT between stages and MAIN blocks during each
(fc_boot_stage waits for the service ack), so shared core/game-struct result-passing is
race-free; F23 (every core.* on the one CORE thread) and F-A/D57 (SDL/SND on MAIN) both
hold. Validated: the fake core now drives bootstrap BOTH ways (monolithic and stage-by-
stage with simulated MAIN work interleaved), asserting the same cleanup-oracle state at
every failure injection, the thread-affinity oracle, and that RUNNING never flips before
fc_boot_finish (INV9). TSan silent 792 sessions, ASan silent 1968, framering/governor/
telemetry/forbidden-globals all pass. Together with D57 this fully unblocks the interactive
minarch S3.2 depth-1 bring-up: guard-ON main() now has a clean checklist (create CORE thread;
MAIN dlopen/dlsym+register; fc_boot_stage GET_INFO; MAIN dir setup + Game_open +
Config_load/readOptions; fc_boot_stage INIT; MAIN overclock/vsync; fc_boot_stage LOAD, MEMORY,
ARM_CRASH, AV; MAIN Input_init + Config_readControls; fc_boot_stage CONTROLLER; MAIN SND_init +
renderer (F-A) via AUDIO/RENDERER stages + Menu_init; fc_boot_stage RESUME; fc_boot_finish;
run loop drives fc_pump). The bring-up itself (run-loop reroute + on-device iteration)
remains interactive, not forkable.

## D59 — depth-1 fc_pump is a serial rendezvous, not a busy-spin (2026-07-14)
The first on-device depth-1 run (ActRaiser: picture, sound, menu, clean exit) worked but
MAIN busy-spun ~11k pumps/sec: `fc_pump` granted the epoch, drained whatever was ready, and
returned IMMEDIATELY — without waiting for that epoch's RUN_DONE. So MAIN raced ahead of the
CORE thread, re-granting into an already-full pipeline (fr_grant -> FR_NOSPACE, tolerated) and
looping at spin speed. Two costs: wasted clock (the exact opposite of this fork's thesis), and
a corrupted governor signal — trackFPS counts pump calls, so `gen` read the spin rate, not the
frame rate. Resolution: at depth 1 (serial mode) `fc_pump` now BLOCKS after a successful grant
until that epoch's RUN_DONE drains, then presents and returns — one pump == one epoch. The
block reuses framering's existing cancellable primitive `fr_wait_events` (bounded 100 ms tick,
woken on any producer state edge); it is NOT a busy-wait — every spin of the loop sleeps in
fr_wait_events until CORE publishes. Completion is detected by wrapping the drain callback and
counting the single FR_EV_RUN_DONE (at depth 1 exactly one credit is ever outstanding, so that
boundary IS the granted epoch). Cancellation is free from the contract: a cross-thread stop —
or a park that drops CORE out of RUNNING — flips the ring to STOPPED/QUIESCENT, fr_wait_events
wakes on that edge, and the loop's state check returns without a RUN_DONE instead of hanging
(and `fr_core_run_done` clears in_run before the release-publish of RUN_DONE, so MAIN reading
in_run==0 after the drain is race-free). Depth >= 2 is unchanged and MUST stay grant + prefix-
drain + return: pipelined mode overlaps MAIN and CORE by design, the epoch completes on a later
pump, and MAIN never blocks on any single epoch. Engine-only change (`fc_pump` in
frontend_core.c); no framering edit. The fake-core harness now asserts the serial rendezvous
directly: after any depth-1 pump that landed a grant (guarded on credits_out==0 + RUNNING
before the pump, which makes the grant certain), CORE is not mid-run (in_run==0), no credit is
outstanding, exactly one RUN_DONE drained, and gen advanced by exactly one (INV10-13) — all
four FAIL against the old grant+drain+return pump. A deterministic gated sub-test fires an
async fr_stop while a depth-1 pump is blocked mid-epoch and asserts the pump always returns
(hang => abort, not a wedged CI) (INV14-15). test-frontend-core{,-tsan,-asan} all silent (TSan
120 sessions, ASan 1248, no races); framering/governor/telemetry/forbidden-globals all pass.
minarch's guard-ON run loop needs NO change — it already calls `fc_pump` once per iteration; it
simply now paces to real frames, so the governor's gen signal reads true frame rate again.

## D60 — WP2: the run() signature now delivers the per-epoch input snapshot (2026-07-14)
Depth-1 (D59) is a serial rendezvous: MAIN is blocked in `fc_pump` for the whole epoch, so the
CORE thread reads the live input globals (`buttons`, `pad.laxis/raxis`) safely — no overlap.
Depth-2 (pipelined, the actual energy win) breaks that: MAIN captures the NEXT frame's input
while CORE is still running THIS epoch, so the core must read a STABLE, per-epoch input, not the
live globals racing underneath it. The framering already carries a 4-word snapshot end-to-end
(`fr_grant(snapshot)` on MAIN → the ring → `fr_core_wait_grant(&gen, snap)` on CORE); the only
gap was that the lifecycle engine never handed that snapshot to the core's `run()`. Resolution:
the fc vtable's `run` is now `void (*run)(void* c, const uint64_t snap[4])` and `fc_core_thread`
passes the just-received `snap` in. The engine treats the four words as OPAQUE — it makes no
claim about their meaning; the snapshot's semantics belong to the integrator (the harness for the
test, minarch's input packing for the device). New harness invariant INV16 proves the pipe is
faithful: MAIN builds each snapshot as a pure function of the epoch id (`mk_snap`), a CORE-side
`snap_ok` assert in the fake run fail-fasts at the exact torn epoch if any word is dropped /
zeroed / spliced from another epoch, and a join-safe atomic coverage counter drives a MAIN-side
`INV(16)` so the check can never be silently dead. Engine + harness only; frontend_core{,test}
plain/TSan/ASan all PASS at 16 invariant sites. minarch conforms to the new signature but does
NOT yet consume `snap` — depth-1 still (correctly) reads input on the CORE thread. Consuming it
is the depth-2 gate and a genuine re-architecture, NOT a small wire: `input_poll_callback` runs
INSIDE `core.run()` (i.e. on the CORE thread today) and does far more than read buttons — it
pumps SDL/PAD, runs `PWR_update` (sleep/power), detects the menu/FF, and calls `core.reset()` /
`Menu_saveState()` on shortcuts. At depth-2 that entire tick must move to MAIN (SDL/PWR/menu are
MAIN-affine and `show_menu`/`fast_forward` are MAIN-read), and the core-affecting shortcuts must
route through the CORE service channel (F23: only CORE ever enters the core) instead of being
called inline. That work needs on-device iteration and, for its payoff proof (the DKC 3×-energy
A/B), Dan's demanding-scene save states — so it is deliberately staged behind this contract.

## D61 — MAIN owns the governor under threading v2 (amends the v2.4 contract) (2026-07-15)
The v2.4 threading contract (`docs/threading-v2-design.md`, "Performance contract") reads as though
the governor runs on the CORE thread. That never matched the shipped code and it collides with the
depth-2 "CORE is a pure producer" correction (`threading-v2-depth2-integration.md` rev-2/rev-3):
the CORE thread must do only run/copy/audio/command work, so it cannot also own cpufreq. Ground
truth: the governor already runs on **MAIN** today — `Gov_start` (minarch.c:1212) is init-only (no
thread spawn), and the governor tick is called inline in the MAIN run loop around the present. So
this decision is a **contract-alignment, not a behavior change**: MAIN owns `GovState` and applies
the cpufreq ceilings; under depth-2 the CORE thread publishes immutable per-epoch work samples into
the ring and MAIN derives the generation rate from **drained RUN_DONE** (not loop/pump iterations —
the D59 lesson, generalized to the non-blocking depth-2 pump). Depth-2 adds up to one epoch of
actuation delay (MAIN sets the clock one epoch behind the work it measures); that is accepted.
Governor hardening required for depth-2 (from Codex round-3): start a new rate window on the first
post-release/post-park RUN_DONE rather than at release wall-time (a window spanning menu/sleep would
read as a false slowdown); tag ceiling changes with an effective generation; exclude the mixed-
frequency work samples immediately after a ceiling change from the sink estimator; reset governor
history symmetrically at every depth transition; and flip the depth predicate only while QUIESCENT
inside the depth-change gate. Design-of-record; not yet implemented (depth-1 ships governor-on-MAIN
already, unchanged). The stale v2.4 "governor on CORE" wording is superseded by this entry.

## D54 — the rail war ends: transition-gated voltage hold (2026-07-12, v1.4 opener)
v1.3's first field day found it: on calibrated devices, transition-heavy PS1 games (BR2
fights) dropped to gen 49/60 with choppy audio — both devices, while flat-clock games
stayed clean (why the uvab-thps2/zelda receipts never caught it). Root cause was not the
undervolt VALUES but the HOLD: the loop verified the rail over i2c every 5ms tick (400 bus
reads/sec under uv_lock), and that bus contention inflated DVFS transition latency exactly
in scenes that transition hardest. Fix: the kernel only re-stocks the rail on a DVFS
transition, and a transition is visible as a scaling_cur_freq change — a sysfs pread, no
i2c. The hold thread now polls cur_freq per tick and touches i2c only right after a
transition plus a 1s heartbeat; the per-flip PLAT_uvReassert takes one atomic load, never
the mutex (audit P2 closed). Failure direction is safe by construction: a missed
correction leaves the rail at stock, which is always >= the guarded value. Probed and
rejected alternatives: sysfs regulator constraints read-only; debugfs OPP u_volt
read-only; tcs4838 VSEL0/VSEL1 both driven so no free setpoint register. Validated: bot
fight harness, provably-new binary (log fingerprint), UV armed — one 53.3 slip at fight
onset (scene discovery, same as stock-volt sessions) then zero slips over ~3min of flat
1800, 0 underruns; vs continuous 49-57 collapses on the old hold. Deployment lesson
re-learned: the first test arm silently ran a stale platform.o (built via the platform
makefile instead of the always-clean workspace path, the D38 trap) — caught by log
fingerprint, not hash; fingerprint-verify test arms. Ships in v1.4; Dan's devices run it
now; ear-verdict pending, bench re-validation (UV BR2 fight cell) required before release.
Side intel from the probes: the kernel exposes 12 OPPs incl. 1320/1464/1512 never used by
the 8-step table (staircase task #10 material).

## D55 — BR2 "crunchy audio" root-caused: chronic ring starvation, never mitigated (2026-07-13)
The post-release crunch Dan heard in BR2 fight scenes (both devices) had TWO real mechanisms
found during one hunt. The first was the UV rail-war (D54) — real, fixed, but not the whole
story: crunch persisted at stock volts. The second is the root cause: BR2 fights run the
audio ring chronically at 45-48% occupancy — measured 289 presentation-drop engages in a
single 4-minute bot-fight cell at stock volts — and v1.3 had ZERO mitigation because its
audio catch-up path was a verified no-op (the SND_ringLow should_vsync flag feeds PLAT_flip,
which ignores it under SDL_RENDERER_PRESENTVSYNC; found in the release audit, wrongly
backlogged as dead-code-no-harm). Fix (feat/audio-catchup, api.c only): REAL presentation
frameskip — below 50% ring occupancy the entire present is skipped (no upload, no RenderCopy,
no RenderPresent, no PLAT_flip), emulation and audio production continue, presenting resumes
at 66%; occupancy is a torn-free atomic word published by producer and consumer; plus two
guards from in-repo receipts: a 100ms consecutive-drop cap (the 2026-07-08 receipt shows a
below-realtime game locks catch-up on forever — as presentation-drop that would freeze the
screen) and a 250ms producer-activity gate (the pause menu stops production and drains the
ring; ungated logic would throttle menu rendering). Governor sees gfx_flip_wait_us=0 on
skipped frames (reads as headroom — protective direction). Validation: bot cell zero
measured cost (crunch detector 0/0, one normal fight-onset gov slip, receipt
docs/bench/bench3-real-raw/zero-v14-catchup-br2-fightbot.txt at 99591c8d), then Dan's ears
on a real fight, 2026-07-13: "sounds awesome, 60fps, so much better." Credit: the
two-problem framing (frontend catch-up defect vs core decode cost) came from the Codex
audio plan (task #13). Phase 2 — RAM-based PCM instrumentation + MDEC/480i profiling —
remains open but is no longer release-gating; the presentation-drop defends the ring while
decode-side headroom is pursued on its own schedule. Lesson recorded: a mechanism verified
as dead code is a defect in the feature that needed it, not a curiosity — the "no measured
claim depended on it" framing hid a live user-facing bug for five days.

## D62 — Threading v2 depth-2 SHIPS for PS1: pacing recipe, zero-overhead gating, parking, canary (2026-07-16)
Depth-2 (pipelined emulation: CORE runs frame N+1 while MAIN presents N) went from losing on-device
(57-59fps, 7-13 underruns/s, governor oscillating) to shipping, via four fixes with receipts:
1. **Vsync-present as the SOLE pacer** — GFX_sync double-paced to 57-59fps (starved audio, held the
   governor high); the present's vsync blocks MAIN, which IS the overlap. After the v1.3.1 merge the
   never-skip guarantee comes from the GFX_flip/GFX_flipGame split (depth-2 + menu present via
   GFX_flip; serial game via GFX_flipGame so presentation-drop catch-up still works there).
2. **Spin-killer** — 500us nanosleep when no frame was presented this tick (MAIN was busy-spinning
   4500 loops/s). 3. **Governor reads fps_double at depth-2** (real frames; cpu_double is the MAIN
   loop rate — meaningless under pipelining). 4. **Engine gated on depth>=2 at runtime** — the
   always-engine depth-1 path cost ~8pp CPU vs serial (fc_pump rendezvous; Dr. Mario 30% vs 22%);
   now depth<2 runs the plain serial path (init/load/run/quit all on MAIN) so the guard compiles
   into ONE binary for all systems at zero cost, and only PS.pak opts into depth-2.
**The measured win (BR2 st9 fight, same-scene fresh-reload method):** depth-2 holds 60fps/0-under
at ~1608MHz; serial (v1.3.1, WITH its catch-up) holds 60fps-CORE only by dropping presents, pegged
at 1800. Cooler by ~200MHz + every frame presented, no OC. Floor sweep: 1416 holds-with-dips
(fps_min 56.8), 1200 breaks — bracket shipped {1008,1608}. Light games (SNES/DKC): exact parity.
GBA: a free-running A/B first showed "45%" — RETRACTED, scene variance + low-battery contamination;
the lesson is the same-scene pin-OPP method or nothing.
**Safety:** review caught depth-2's MAIN-side core entries (sleep-autosave + save/load/reset
shortcuts calling core.serialize/unserialize/reset against a live core.run) — fixed by parking the
CORE thread around them (state-gated: park only when RUNNING, so a menu-context sleep never
unbalances the menu's own park). Sleep->autosave->resume validated on-device by Dan. Plus a crash
canary: a depth-2 session that ends without reaching a known-good point (clean exit, or the parked
sleep/poweroff autosave — Menu_beforeSleep is the choke point for both) leaves a marker; the next
launch of that game runs serial once, clears it, and retries depth-2. Fails safe by construction.
**Launch design:** no frontend option, no runtime Auto — the per-pak defaults ARE the auto, each
backed by measurement (per-game cfg override is YAGNI until a real misbehaving game appears). HUD
shows T2 + fps_double when the pipeline is live (absent tag = canary fell back to serial).

## D63 — Zero-copy pool render for depth-2 (built; opt-in pending device A/B) (2026-07-16)
At depth-2 the CORE thread memcpy'd every frame into the pool — PS1 640x480x2 = 614KB @60 =
~37MB/s on the emu-critical thread. minarch's GET_CURRENT_SOFTWARE_FRAMEBUFFER stub is now a real
implementation: at depth-2 the env callback acquires a pool buffer and hands it to the core;
video_refresh recognizes the pointer and emits the index with NO memcpy; an epoch-end guard in
zero_ftv2_run releases an unconsumed grant (duped frame), and the env callback itself reclaims a
stale grant (mid-frame mode-change re-request). pcsx_rearmed patched to re-request every frame
(set_vout_fb() at vout_flip start — the libretro contract says the buffer may change per frame;
stock pcsx only re-requested on mode change, which would corrupt a rotating pool). Patch
regenerated; verify-source green. Cores that actually call the env: pcsx, fceumm, pce_fast (the
8-bit two are at the 408 floor — no benefit). Gated ZERO_FTV2_ZEROCOPY=1 until the BR2 visual
check + floor sweep (unvalidated frame-path code must not ride the default); expectation: the
1416 floor becomes comfortable. Bonus fix: the old stub fell through to return-true with the
struct UNFILLED — cores survived only because they zero-init fb and format 0 fails their check.

## D64 — Audio threading for light systems: NO (architecture + physics); their lever is work deletion (2026-07-16)
Asked directly ("move audio to another thread for lower-end systems?"): SDL already drains the ring
on its own audio thread; what remains on the emu thread is core-internal synthesis (that IS the
emulation) plus SND's cheap linear/nearest resample. Offloading cannot help floor-dwellers: the
A133P is a single clock/voltage domain (moving work sideways lowers no clock unless the source
core pinned it — 8-bit systems idle 35-65% at the 408 OPP floor), and race-to-idle (D14/D21/D22)
plus the keymon wakeup lesson (65->0/s) say extra cross-thread wakeups are a straight battery loss.
The light-system lever is DELETING work so they idle deeper at the floor: the unpriced-option audit
(gambatte's locked-on sinc resampler is the first target — the D25 method already yielded 400MHz
from one cfg line) and LTO for the cores shipping without it (gpsp, mgba). Ranked plan:
.notes/optimization-master-hunt.md; threading census for VB/MGBA/supafaust in
.notes/threading-other-systems-survey.md — depth-2 extends to a system only if it has at-the-edge
games (pins f_max or slips), which is what made PS1 the win and makes 8-bit a non-candidate.
## D56 — FF with sound (2026-07-14, feat/ff-audio)
Fast-forward was silent since forever: `audio_sample*_callback` dropped audio when
`fast_forward` (the old `// sound must be disabled for fast forward to work` note). The
real reason is `SND_batchSamples` BLOCKS the emulation thread when the ring is full
(SDL_Delay retry) — that block is what audio-paces normal play to real time, so feeding
4x audio during FF throttled FF back to 1x. Fix (lean, no time-stretch): during FF the
ring feeds NON-BLOCKING — `SND_setFastForward(1)` makes `SND_batchSamples` drop-on-full
instead of block. The DAC plays the samples that land at 1x = the game audio temporally
compressed (the expected sped-up/chipmunk FF sound); FF speed stays governed by
`limitFF()`. On by default (`ff_audio=1`, no menu; set 0 for classic silent FF).
NextUI parity confirmed against their tree: `ma_audio.c` gates on `(!fast_forward ||
ff_audio)` and routes FF through `SND_batchSamples_fixed_rate` — an occupancy-adaptive
non-blocking resampler (ON_TIME/LATE/VERY_LATE mode machine). Ours is the minimal
equivalent: same non-blocking-drop principle, no adaptive resampler (chipmunk is the
desired FF cue, not hi-fi). Interaction with the v1.3.1 presentation-drop catch-up is
benign BY CONSTRUCTION: FF floods the ring, `SND_ringPct` rides high, catch-up (engages
< 50%) never fires during FF — FF is never audio-starved. Device ear-test + engage=0
confirmation pending (Brick asleep at implementation time).

## D65 — Futile-climb governor ships DEFAULT-OFF: the harness passed, the device did not (2026-07-17)
The futile-climb stand-down (D62-era addition: a slip that a full climb to f_max did not cure
returns to the pre-climb ceiling) passed 18 synthetic tests and a Battletoads no-regression run —
then the v1.4-RC3 smoke caught a LIVE false positive on the flagship case: a genuine BR2 heavy-scene
arrival slips at f_max for ~2s while the depth-2 pipeline refills; the 4-tick detector judged the
scene unfixable and stood the ceiling down to 1008 MID-FIGHT (54fps, 8-16 underruns/s, ear-confirmed
crunch), and the bursty SLIP/BIGSLIP mix never tripped the BIGSLIP-only in-hold breaker for the full
60s hold. The discriminator ("slipping at max = unfixable") cannot distinguish transition-slips from
authentic engine slowdown at a 2s horizon. Gated OFF by default (ZERO_GOV_FUTILE=1 to enable for
tuning); FFSLIP (the FF exemption) stays — it is correct independently. Revisit needs a burstiness/
trend-aware discriminator and same-scene device validation on BOTH Battletoads (true positive) and
BR2 scene transitions (the false positive). Lesson repeated from D24: synthetic traces model the
signals we THINK the device produces; only the device produces the real ones.

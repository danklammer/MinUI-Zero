# Threading v2 — ownership-model design (v1.4)

Status: DESIGN FOR ADVERSARIAL REVIEW — no code until this survives review.
Predecessors: v1.3 threading (compiled out, D52 — 17 audit findings, shared-state races);
evidence base: DKC 747→600 MHz at 1/3 energy, Yoshi 1212→741 (docs/bench/2026-07-08-snes-*).

## Why v1.3's model failed
The mailbox handoff let the core thread mutate renderer/governor state owned by the video
thread (resize, scaler re-init, HUD fields) with ad-hoc volatiles under `-O3 -flto`. Races
are not testable defects: a clean soak proves nothing about their absence. v2 makes them
*structurally impossible* instead of carefully avoided.

## The ownership contract (single-writer, everywhere)
Every mutable datum has exactly ONE writing thread, named here, enforced by layout:

| State | Owner (sole writer) | Readers | Mechanism |
|---|---|---|---|
| Emulation state, SRAM/RTC ptrs | core thread | crash handler (async) | unchanged from single-thread |
| Frame ring buffers + indices | producer: core; consumer: present | — | SPSC ring, C11 acquire/release |
| Renderer/scaler/texture state | present thread ONLY | — | core NEVER touches; changes via command queue |
| Geometry changes (AV_INFO/GEOMETRY) | core detects → enqueues command | present applies | SPSC command queue, applied between frames |
| Governor inputs (gen count, frame work µs) | core thread | governor tick (core thread) | governor stays ON the core thread — no cross-thread gov state |
| uv_target / ceiling | governor (core thread) | uv hold thread | existing uv_lock + atomics (unchanged, D54) |
| Audio ring | producer: core; consumer: SDL callback | — | existing SND ring (already SPSC) — audited for C11 fences |
| Thread lifecycle (run/park flags) | main/menu thread | both workers | C11 atomics, join-before-free everywhere |
| HUD/debug fields | core thread writes snapshot into ring slot | present renders from its dequeued slot | data travels WITH the frame, never shared |

Design rules that fall out:
1. **The frame is the only crossing.** Everything the present thread needs (pixels, pitch,
   geometry generation number, HUD snapshot) travels inside the ring slot it dequeues.
   No other core→present data path exists.
2. **Commands, not calls.** Anything that must change present-side state (resize, sharpness,
   effect toggles) is an enqueued command consumed at slot boundaries. The core thread never
   calls SDL/GLES/scaler functions while threading is active.
3. **The governor never crosses.** v1.3 judged core-thread window utilization from another
   thread's data. v2 keeps measurement, decision, and PLAT_setCPUMaxFreq/VoltForCeil calls
   on the core thread (as single-threaded builds do today) — zero new shared governor state.
4. **C11 `<stdatomic.h>` only** (closes the deferred audit item): acquire/release on ring
   indices and lifecycle flags; no volatile-as-sync, no fence-free flags. `-O3 -flto` legal.
5. **Teardown is a protocol**: park → drain ring → join → free, in that order, on the menu
   thread; the v1.3 dlclose-SIGSEGV class (liveness flag skipped on persisted-ON) cannot
   recur because launch always starts single-threaded (see policy) so there is no
   persisted-ON boot path at all.

## Ship policy (unchanged from the approved auto-threading design)
No menu. Every game launches single-threaded. After governor settle (~60s): floor-band
ceiling → verdict "no", remember. Ceiling ≥ ~1008 → 60s threaded trial → commit only if
the ceiling sinks ≥1 OPP step with generation rate intact; else revert. Verdict persists in
the existing `.thread` sidecar. Re-trial if an unverdicted game later climbs. PS1 expected
"no" per receipts; SNES-class expected "yes".

## Verification plan (the shipping gate)
1. **TSan harness**: extract ring + command queue + lifecycle into a host-compilable module
   (`workspace/all/common/framering.{c,h}`) with a synthetic producer/consumer stress test
   run under ThreadSanitizer + AddressSanitizer on macOS/CI. The module IS the code minarch
   links — not a model of it.
2. **Torture gauntlet on-device**: ≥26 thread create/destroy cycles flat-RSS, persisted-
   verdict launches, FF round-trips, menu park/resume, sleep/wake with zero gov moves,
   threaded savestates, clean-quit after every path.
3. **Adversarial review** of this doc AND the diff (Codex + blind reviewer), before merge.
4. **Fingerprint discipline** (D54 lesson): every test arm verified by log fingerprint,
   never hash alone; workspace-level clean builds only.

## Measurement plan (after task #11's pipeline profile — sequencing matters)
Run the low-clock pipeline profile FIRST: if present-path waste is found, the single-thread
baseline gets cheaper and threading's honest marginal win must be re-measured against it.
Then Dan's save-state protocol: he plays each SNES-class game to a demanding scene once and
saves state (DKC, Yoshi's Island, Mario RPG, + picks); arms load identical states — real
gameplay load, never intro loops. Metrics per arm: generation rate, underruns, OPP
residency, coulomb delta, temperature. Ship gate per game class: threaded commits only
where the trial criterion (≥1 OPP sink, rate intact) reproduces in the matched bench.

## Explicitly out of scope
PS1 threading revival (receipts: no benefit), threading in the menu binary, more than two
threads, per-game user-facing toggles.

# Threading v2.1 — ownership-model design (v1.4)

Status: REVISED FOR RE-REVIEW — closes all seven P0s from the 2026-07-13 adversarial
review (.notes/threading-v2-review-codex.md). No code until this version survives review.
Predecessors: v1.3 threading (compiled out, D52); evidence: DKC 747→600 MHz at 1/3 energy,
Yoshi 1212→741 (docs/bench/2026-07-08-snes-*).

## Threads, named once
- **MAIN** — one OS thread: process main = menu = present = SDL/GLES/audio-lifecycle owner.
- **CORE** — one OS thread per loaded session: the ONLY thread that ever calls into the
  libretro core, from `retro_load_game` to `retro_unload_game`/`dlclose` prep.
- **UV-HOLD** — existing voltage thread, unchanged (D54 contract).
In single mode CORE's event loop simply runs inline on MAIN's schedule? **No — rejected.**
Single vs threaded differ ONLY in where *presentation* work happens; the core session is
pinned to CORE from load in every mode (P0-1). "Single mode" = MAIN blocks on a completed
frame each iteration (synchronous rendezvous); "threaded" = MAIN presents from the ring
without blocking on production. The trial toggles the rendezvous, never thread ownership.

## P0-1 — Session-pinned core thread
ALL libretro entry points execute on CORE: run, serialize/unserialize, reset, disc control,
options, memory queries. Today's violators, each becoming a request executed by CORE at the
safe point (between `retro_run` returns): `Menu_loop` save/load slot (`State_write`
minarch.c:4583, `State_read`:4607), `Game_changeDisc`:4601, `Core_reset`:3414 (menu +
SHORTCUT_RESET_GAME), `State_autosave`:3543 and `Menu_beforeSleep`/`PWR_update` save paths,
boot `State_resume`:741 (runs on CORE after load, before first frame). MAIN blocks on the
request's ack (core is paused-at-safe-point during menu anyway), so menu UX is unchanged.
No TLS/thread-affinity hazard remains: a core sees exactly one thread its whole life.

## P0-2 — Control plane (request/ack)
Core-side callbacks (input poll, environment, video/audio refresh) NEVER execute lifecycle
operations. They may only set request flags. MAIN executes: quit, menu enter/exit, sleep
(`PWR_update`'s synchronous close-audio/save/resize/suspend chain, minarch.c:4167/3597),
poweroff, FF toggle, reset, disc change, save ops — each as: MAIN sets `park_request` →
CORE acks at safe point (parked) → MAIN performs the operation (calling into CORE via the
request queue where the operation is a libretro call, per P0-1) → MAIN releases park.
Every request/ack pair is C11 acquire/release; ack carries the generation it parked at.

## P0-3 — Total ordering: one generation counter
A single monotonic `gen` (u64, CORE-owned) stamps BOTH frame slots and commands. Rules:
commands are immutable deep-copied payloads; present applies all commands with
`cmd.gen <= frame.gen` before presenting that frame; command-queue-full blocks CORE at the
safe point (never drops, never reorders); menu-owned mutations of the same state (options
screen) happen only while CORE is parked with both queues drained — no interleave exists.

## P0-4 — Frame lifetime: the retained snapshot
Present maintains an immutable **last-presented snapshot** (own allocation, copied — not
aliased — from the slot before slot release; RGB565 copy of the ≤1MB frame is ~0.2ms).
All retained uses read the snapshot, never a slot and never `renderer.src`: menu
background, save-state screenshot/BMP, HDMI re-blit. Save-state + screenshot is one
transaction: MAIN requests park → CORE parks at gen G → serialize (on CORE) + snapshot
tagged G → both written together. Slots are recycled only after present's copy completes;
`vid.blit`/platform pointers never outlive the frame they were handed.

## P0-5 — Audio ownership partition
| Audio state | Owner | Sync |
|---|---|---|
| Ring payload + indices | producer CORE / consumer SDL callback | C11 SPSC acquire/release |
| Stats (occupancy, underruns) read by flip path (`SND_ringLow`, api.c) | written by callback | single atomic word snapshot, torn-free |
| Config (rate, resampler/DRC ratio) | MAIN, applied via command at gen boundary | park producer first |
| Lifecycle (open/close/pause/free) | MAIN | producer parked + acked BEFORE any close/resize/free |
Producer writes are **stop-aware**: blocked-on-full waits on (space OR park_request), so
sleep can never deadlock against a full ring (P1-8's failure case).

## P0-6 — Crash contract
CORE publishes an **emergency snapshot** after each frame batch that dirtied SRAM/RTC:
double-buffered copies guarded by a seqlock generation (odd = torn). The signal handler
writes ONLY the snapshot whose generation reads even-and-stable — never live core memory.
A fault on a non-CORE thread with a torn snapshot skips the emergency save (the last
menu/sleep flush stands; a stale-but-consistent save beats a torn one). SIGTERM sets the
shutdown request flag only; orderly teardown runs outside signal context. Fatal exits are
the one sanctioned exception to park→join teardown (re-raise after snapshot write).

## P0-7 + trial protocol — verdicts that stay honest
- Floor-band observation writes **no durable verdict** (the "no" is session-local).
- A durable negative records the ceiling band it was measured in and **re-arms** when
  demand exceeds that band (SuperFX heavy-scene case).
- Trial windows count **active gameplay samples** (gov ticks with content running), not
  wall time; the window invalidates on: FF, menu, sleep, state load, geometry/timing
  change, core-option change, HDMI, thermal intervention.
- Governor history (fail memory, presink) resets symmetrically before EACH arm.
- Commit requires **A/B/A**: single-baseline → threaded → single-confirm, all valid.
- Commit criterion: threaded arm sinks ≥1 OPP **and** delivery health intact (generation
  rate, zero new underruns, present backlog) — plus the class must have matched
  total-device energy evidence from the bench (P1-12); ceiling alone never ships.
- A positive persists only after a clean post-trial dwell AND clean teardown.
- Sidecar v2 key: content path + core build hash + threading schema version + device
  model. Legacy `minarch_thread_video=On` configs are ignored; all v1 `.thread` sidecars
  invalidated on first v1.4 launch; corrupt/unknown records = unverdicted. A persisted
  positive is honored only after load, auto-resume, audio, renderer, and crash-snapshot
  setup reach the named safe point — the first 60 frames always run the synchronous
  rendezvous regardless of verdict.

## Teardown protocol (every exit path)
`park(stop-aware) → ack(gen) → drain-or-discard → join → free → dlclose`, on MAIN.
Drain vs discard per path: quit/menu/sleep = present pending frames then stop (no visual
pop); trial reversal/FF flip = discard (stale frames wrong-paced); load-failure/thread-
create-failure = discard. Fail-closed: if park or join times out, LEAK the resources and
exit the process — never dlclose or free while a worker may live. Covered paths: quit,
menu, sleep, poweroff, FF on/off, trial start/stop, HDMI restart, core-requested shutdown
(`RETRO_ENVIRONMENT_SHUTDOWN` → request flag), load failure, thread-create failure.

## Core→present path classification (complete, per review P1-9)
| Path | Class |
|---|---|
| Frame pixels, pitch, dimensions, HUD snapshot | frame slot |
| SET_GEOMETRY (incl. same-size aspect change) | command (gen-stamped) |
| SET_SYSTEM_AV_INFO (fps/sample-rate) | **versioned transaction**: one command carrying new timing; present acks; pacing, audio config, governor target, DRC all switch at that gen — never piecemeal |
| Scaler/crop/effect/sharpness/DMG palette/vsync config | MAIN-owned (options UI mutates while CORE parked); applied present-side |
| DRC ppm / present-rate feedback → core pacing | reverse channel: single atomic word written by present, read by CORE pacer |
| Governor (gen counts, work µs, ceiling calls, size-change burst) | CORE-thread-local (measurement, decision, PLAT calls all on CORE; unchanged from single-thread) |
| fb_present/fb_game software present | present-side only; reads slot/snapshot, never `vid.blit` aliases |
| Menu background / savestate screenshot | retained snapshot (P0-4) |
| `video_refresh(NULL)` duplicate frame | ring message "repeat gen G" — present re-shows snapshot; no pixel copy |
| Quit/shutdown/menu/FF/sleep requests | control plane (P0-2) |

## Verification gate (ships or it doesn't)
1. `framering.{c,h}` (ring + command queue + gen logic + lifecycle flags) is the linked
   shipping code AND host-compiled under **TSan** and **ASan as two separate builds**,
   driven by an **adversarial fake core** (random geometry storms, NULL frames, mid-run
   AV_INFO, saves during load, shutdown at every state) — not just a synthetic pump.
2. **Forbidden-globals audit, mechanical**: CI script (nm/objdump on minarch.o set) fails
   the build if CORE-compiled units reference present-owned symbols (list checked into
   the repo beside the CI script).
3. On-device gauntlet, expanded per review: geometry before/after/without a following
   frame; NULL dup frames; multiple video callbacks per retro_run; ring/command
   wraparound + saturation; slow producer / slow consumer; alloc, thread-create, audio-
   device failures; sleep while producer blocked; SIGTERM + fatal signals to each thread;
   auto-resume with old/corrupt/conflicting sidecars; HDMI/effects/scaling/fb_game; save/
   load/reset/disc from shortcuts and menu; core-requested shutdown; ≥26 create/destroy
   cycles flat-RSS; release-flag `-O3 -flto` soaks; assertions: dlclose never with live
   worker, CORE never touches renderer state.
4. Fingerprint discipline (D54): every test arm verified by log fingerprint, never hash;
   workspace-level clean builds only.

## Measurement plan (anti-bias, per review P1-15)
Task #11 pipeline profile FIRST, run in **both** modes once v2 exists; then freeze the
pipeline before any verdict testing. Invalidate all prior sidecars; recalibrate the trial
eligibility threshold on the frozen pipeline. Dan's save-state scenes split into
discovery (tuning) and holdout (validation) sets — never reused across roles. Arms run
counterbalanced A/B/A/B with matched charge/thermal preconditions (cold-start cells).
Ship evidence per class = total-device energy + temperature + delivery health.

## Explicitly out of scope
PS1 threading revival (receipts: no benefit), threading in the menu binary, >2 frontend
threads, user-facing toggles (the machine decides; design axiom).

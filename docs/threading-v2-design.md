# Threading v2.2 — ownership-model design (v1.4)

Status: REVISED FOR RE-REVIEW — resolves the 2026-07-13 re-review's seven new P0s
(findings 16–22) and closes its eight PARTIALs (.notes/threading-v2-review-codex.md holds
round 1; round 2 verbatim in the review thread). No code until this version survives
review. Predecessors: v1.3 threading (compiled out, D52); evidence: DKC 747→600 MHz at
1/3 energy, Yoshi 1212→741 (docs/bench/2026-07-08-snes-*).

## Threads, named once
- **MAIN** — one OS thread: process main = menu = present = SDL/GLES/audio-lifecycle owner.
- **CORE** — one OS thread per loaded session: the ONLY thread that ever calls into the
  libretro core, from `retro_load_game` through `retro_unload_game`/`retro_deinit`.
  `dlopen`/`dlclose` happen on MAIN, strictly before CORE starts / after CORE joins.
- **UV-HOLD** — existing voltage thread, unchanged (D54 contract).

## The run epoch (F16 — the execution model)
All execution is organized in **run epochs**, numbered by the single monotonic generation
counter `gen` (u64). Epoch N is:

1. MAIN **grants** run N: publishes the epoch-N input snapshot (see channels) and the
   current options snapshot pointer, then signals CORE.
2. CORE executes **exactly one** `retro_run`.
3. CORE publishes every product of that run stamped N — frame slot(s), commands, audio —
   in publication order (order within an epoch = the order the callbacks fired).
4. CORE publishes **RUN_DONE(N)**, carrying the epoch's video outcome:
   `FRAME` (one or more slots; the last is canonical), `DUP` (`video_refresh(NULL)` —
   re-present the retained snapshot), or `NONE` (no video callback fired).
5. MAIN **drains epoch N**: applies all commands with `cmd.gen <= N`, then presents
   (or re-presents, or skips) per the video outcome.
6. MAIN **acks** RUN_DONE(N). Only then may CORE begin epoch N+1.

**Single mode = strictly serial epochs**: steps 1–6 complete before the next grant; there
is NO overlap of emulation and presentation — this is the valid A-arm baseline, equivalent
to v1.3's single-thread loop. **Threaded mode = pipelined epochs**: MAIN may still be
draining epoch N while CORE runs epoch N+1..N+depth (depth = ring capacity); the ack
gates the *pipeline depth*, not each step. The trial toggles pipelining only; thread
ownership never changes. `gen` advances exactly once per grant (F3: advance point defined;
retro_run completion = RUN_DONE; multi-callback ordering = publication order within epoch).

**Commands never wait for a frame** (F21): RUN_DONE(N) — not a frame — is the application
boundary. A geometry or AV_INFO command in an epoch with video outcome NONE is applied and
acked when MAIN drains that epoch. Command acks therefore can never depend on a future
video callback. Commands pending at a terminal park are drained the same way before
unload; all commands are presentation-side (no persistent state), so applying them to a
session that is about to end is harmless and keeps every ack contract satisfied.

## CORE states and the request plane (F17, F18 — closes round-1 F2)
CORE is always in exactly one state:

- **RUNNING** — executing granted epochs.
- **QUIESCENT** — parked at the epoch boundary. `retro_run` is prohibited. CORE sits in a
  **command-service loop**: it accepts and executes approved libretro requests from MAIN
  (`serialize`, `unserialize`, `reset`, disc-control ops, option application,
  `unload_game`, `deinit`), publishing a completion ack per request. This is the F18
  resolution: a parked CORE *services* requests; park-then-wait-for-service deadlock
  cannot occur because QUIESCENT's definition is "servicing, not running."
- **STOPPED** — terminal; the thread has exited its loop and is joinable.

Core-side callbacks (input poll, environment, video/audio refresh) never execute lifecycle
operations; they only set request flags that MAIN acts on (quit, menu, FF, sleep, poweroff,
`RETRO_ENVIRONMENT_SHUTDOWN`). Every request/ack pair is C11 acquire/release and carries
the epoch it was serviced at.

### The wait graph, shown acyclic (F17 — closes round-1 F8)
Every blocking wait in the system, its holder, and its cancellation:

| Wait | Waiter | Wakes on | Cancelled by |
|---|---|---|---|
| Epoch grant | CORE | grant signal | stop |
| Frame-ring space | CORE | slot freed | park, stop |
| Command-queue space | CORE | entry consumed | park, stop |
| Audio-ring space | CORE | callback consumed | park, stop |
| RUN_DONE ack (serial mode) | CORE | MAIN ack | park, stop |
| Park ack / service-completion ack | MAIN | CORE publishes | **none needed — MAIN keeps draining (below)** |
| Present vsync | MAIN | display | bounded by hardware |

Rules that make it acyclic:
1. Every CORE wait includes `park_request` and `stop_request` in its wake predicate
   (single futex/condvar word per wait; the predicate re-checks all three). A parked CORE
   abandons the wait, transitions to QUIESCENT, and acks — so CORE can ALWAYS reach
   QUIESCENT regardless of queue fullness.
2. While MAIN awaits a park ack or a service-completion ack, it **continues draining the
   frame ring and command queue** (the wait loop *is* the drain loop). So the only
   MAIN→CORE wait edges are ones in which MAIN is actively freeing every resource CORE
   could be blocked on.
3. Queue locks are leaf-only: each queue's mutex protects only its own indices; no code
   path acquires a second lock or blocks on a condition while holding one.

Edge set: CORE→MAIN edges are all park/stop-cancellable; MAIN→CORE edges all drain while
waiting. No cycle can close. (Round-1 F2's menu-save deadlock: MAIN parks → CORE
transitions QUIESCENT even if it was blocked on a full queue (rule 1), MAIN drains anyway
(rule 2), CORE services the serialize request in QUIESCENT (F18), acks, MAIN releases.)

## Reversible vs terminal lifecycle (F19)
Two distinct protocols; v2.1 conflated them.

**REVERSIBLE** — menu enter/exit, sleep/resume, FF toggle, trial start/stop, options edit:
`park_request → CORE acks QUIESCENT → MAIN drains-or-discards → operations (as QUIESCENT
service requests where they are libretro calls; MAIN-side otherwise) → release → CORE
resumes RUNNING.` The session, the loaded core, and the CORE thread all survive. Drain vs
discard per path: menu/sleep = drain (present pending frames, no visual pop); FF flip and
trial reversal = discard (stale pacing).

**TERMINAL** — quit, poweroff, load failure, core-requested shutdown, thread-create
failure: `park_request → ack QUIESCENT → drain → MAIN requests unload_game then deinit
(executed BY CORE in QUIESCENT) → stop_request → CORE exits loop (STOPPED) → MAIN joins →
free frontend resources → dlclose on MAIN.` Fail-closed: if park, service, or join times
out, LEAK everything and exit the process — never dlclose or free while a worker may live.
Fatal signals are the one sanctioned exception to this ordering (see crash contract).

## Synchronous callback channels (F20 — closes round-1 F9)
Every channel the core needs an *immediate* answer from, with owner and mechanism:

| Channel | Direction | Contract |
|---|---|---|
| Input state | MAIN→CORE | Immutable **input snapshot** published at each epoch grant; `input_state`/`input_poll` read only the granted epoch's snapshot. Serial mode: same-frame freshness (snapshot taken immediately before grant). Threaded: staleness = pipeline depth (1 epoch ≈ 16ms) — same class as double-buffered vsync input; stated and accepted. |
| `GET_VARIABLE` / options | MAIN→CORE | Immutable **options snapshot** (pointer swap at epoch grant). Options mutate only during QUIESCENT (options UI runs with CORE parked), so a RUNNING core always reads a stable snapshot. |
| Option definitions, `SET_VARIABLES`, input descriptors | CORE→MAIN | Deep-copied, gen-stamped commands (applied at epoch drain). |
| Disk-control callback table | CORE-owned | MAIN never calls the table; disc operations are QUIESCENT service requests executed by CORE. |
| Rumble | CORE→MAIN | Single atomic word (effect+strength); MAIN reads at each drain and dispatches to the device. Device I/O never on CORE. |
| Log/message/perf env calls | CORE→MAIN | Log: async-safe append (lock-free or flockfile). Messages: gen-stamped command. Perf counters: CORE-local. |
| Const-after-load queries (system/save dirs, capabilities) | either | Immutable after `retro_load_game` returns; readable from any thread; frozen before CORE starts. |

## Frame lifetime (round-1 F4, drain rule added)
Present maintains an immutable **last-presented snapshot** (own allocation, copied from
the slot before slot release; RGB565 ≤1MB ≈ 0.2ms). All retained uses read the snapshot:
menu background, save-state screenshot/BMP, HDMI re-blit, DUP re-present. **Save
transaction ordering (F4 fix):** MAIN parks CORE at epoch G, then **drains through G**
(all frames ≤ G presented, snapshot updated to G) *before* tagging: serialize (QUIESCENT
service at G) + snapshot-G are then written as one transaction — the screenshot provably
matches the serialized state. Slots recycle only after the snapshot copy; `vid.blit` and
platform pointers never outlive their frame's drain.

## Audio ownership (unchanged from v2.1 — round-1 F5 RESOLVED)
| Audio state | Owner | Sync |
|---|---|---|
| Ring payload + indices | producer CORE / consumer SDL callback | C11 SPSC acquire/release |
| Stats (occupancy, underruns) for the flip path | written by callback | single atomic word |
| Config (rate, resampler/DRC ratio) | MAIN, applied at epoch boundary | producer parked first |
| Lifecycle (open/close/pause/free) | MAIN | producer parked + acked BEFORE close/resize/free |
Producer full-ring waits are park/stop-cancellable (wait-graph row above).

## Crash contract (F22 — replaces v2.1's seqlock; closes round-1 F6)
The v2.1 per-frame-batch seqlock snapshot is **withdrawn**: it was neither C11-legal
against handler reads nor bounded in cost, and libretro gives no portable SRAM-dirty
notification. v2.2 policy, zero new machinery:

- **Fault on CORE**: the handler runs on the sole SRAM/RTC-mutating thread, which is now
  halted in its own handler — single-threaded semantics, identical to v1.3's shipped
  emergency save. Write live SRAM/RTC via the existing async-signal-safe tmp+rename path,
  then re-raise.
- **Fault on any other thread**: **decline** the emergency save. The last on-disk save
  from the most recent park point stands (park points — menu, sleep, save transactions —
  already flush SRAM/RTC to disk via `SRAM_write`'s atomic path; these are exactly v1.3's
  flush points). Staleness = time since last park; consistency = guaranteed. Strictly
  better than v1.3, which would have torn-written live memory from the wrong thread.
- Cost: zero per-frame work, zero snapshot buffers, no cadence/byte-budget/energy
  questions — publication *is* the existing park-point disk flush. SRAM size/pointer
  changes are re-read at each park flush as today.
- `SIGTERM` sets the shutdown request flag only; orderly TERMINAL teardown runs outside
  signal context. Fatal re-raise after the CORE-fault write is the sanctioned exception
  to park→join ordering.
- Handler discipline: the handler reads one atomic word (faulting-thread == CORE?) set at
  thread start, and the pre-registered SRAM/RTC pointers — nothing else.

## Verdicts and the trial (round-1 F7 RESOLVED; F10/F11 closed)
- Floor-band observation writes no durable verdict (session-local).
- A durable negative records its ceiling band and re-arms when demand exceeds it.
- Trial windows count active gameplay samples (gov ticks with content running), never
  wall time; invalidated by: FF, menu, sleep, state load, geometry/timing change,
  core-option change, HDMI, thermal intervention.
- **Scene-stability check (F10 fix)**: the two single arms of A/B/A must agree — ceiling-
  residency histograms over active samples with L1 distance ≤ 0.2 (20% mass); disagreement
  = workload drifted = trial invalid, retried later. The B arm is judged only between
  matching A arms.
- Governor history resets symmetrically before each arm.
- Commit criterion: threaded arm sinks ≥1 OPP AND delivery health intact (generation
  rate, zero new underruns, no present backlog) AND the device/core class has matched
  total-device energy + temperature evidence from the bench. Ceiling alone never ships.
- A positive persists only after a clean post-trial dwell AND clean teardown.
- **Sidecar v2 key (F11 fix)**: content identity + core build hash + effective-options
  fingerprint (hash of the options snapshot the verdict was measured under) + threading
  schema version + device model. Content identity = file size + basename hash — full-
  content hashing is prohibitive for CD-image-class content (multi-hundred-MB bins) and
  no hash machinery exists in minarch today; collisions on same-name-same-size content
  are keyed apart by the other four fields. Legacy `minarch_thread_video=On` configs are
  ignored; all v1 sidecars invalidated on first v1.4 launch; corrupt/unknown = unverdicted.
- **Requalification gate (F11 fix)**: the first 60 frames after load always run serial
  epochs, and the persisted positive is honored only if that window's delivery health
  passes (gen rate ≥ target−1, zero underruns, no backlog); failure demotes the record to
  unverdicted and schedules a re-trial.

## Verification gate (ships or it doesn't)
1. `framering.{c,h}` (ring + command queue + epoch/gen logic + state machine + lifecycle
   flags) is the linked shipping code AND host-compiled under **TSan and ASan as two
   separate builds**, driven by an **adversarial fake core**: random geometry storms,
   NULL-dup frames, zero-video epochs, multiple video callbacks per run, saves during
   load, shutdown requests in every CORE state, park storms against full queues.
2. **Forbidden-globals audit, mechanical**: CI (nm/objdump over CORE-compiled units)
   fails the build on references to MAIN-owned symbols; the list lives beside the script.
3. On-device gauntlet (round-1 list retained) plus new-mechanism cases: RUN_DONE with
   video outcome NONE carrying commands; park during QUIESCENT service; TERMINAL
   requested during REVERSIBLE; input-snapshot staleness at pipeline depth; epoch-counter
   wrap (u64 — assert unreachable); requalification-gate failure path; A/B/A scene-
   mismatch invalidation.
4. Fingerprint discipline (D54): every test arm verified by log fingerprint, never hash;
   workspace-level clean builds only.

## Measurement plan (round-1 F15 RESOLVED — unchanged)
Task #11 pipeline profile first, both modes, then freeze the pipeline before verdict
testing. Invalidate all prior sidecars; recalibrate the eligibility threshold on the
frozen pipeline. Discovery vs holdout save-state scene split; counterbalanced A/B/A/B;
matched charge/thermal preconditions; ship evidence per class = total-device energy +
temperature + delivery health.

## Explicitly out of scope
PS1 threading revival (receipts: no benefit), threading in the menu binary, >2 frontend
threads, user-facing toggles (the machine decides; design axiom).

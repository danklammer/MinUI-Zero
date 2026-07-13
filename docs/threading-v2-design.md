# Threading v2.3 — ownership-model design (v1.4)

Status: REVISED FOR ROUND-4 REVIEW — resolves round-3's six new P0s (findings 23–28), its
remaining PARTIALs, and adds the required performance contract (rounds 1–3 in
.notes/threading-v2-review-codex.md + review thread). No code until this survives review.
Predecessors: v1.3 threading (compiled out, D52); evidence: DKC 747→600 MHz at 1/3 energy,
Yoshi 1212→741 (docs/bench/2026-07-08-snes-*).

## Threads, named once
- **MAIN** — one OS thread: process main = menu = present = SDL/GLES/audio-lifecycle owner.
- **CORE** — one OS thread per loaded *core* (F23): created immediately after
  `dlopen`/`dlsym` succeed, before ANY libretro symbol is called. **Every exported
  libretro call executes on CORE** — `retro_get_system_info`, `retro_set_environment`,
  all `retro_set_*` callback registration, `retro_init`, `retro_load_game`, `retro_run`,
  state/disc/option calls, `retro_unload_game`, `retro_deinit`. Only
  `dlopen`/`dlsym`/`dlclose` are MAIN-owned (they are loader calls, not core calls).
  The core sees exactly one thread for its entire lifetime; the round-1 TLS/affinity
  hazard cannot re-enter through the pre-load window.
  Bootstrap = CORE starts directly in QUIESCENT; MAIN issues the pre-load sequence
  (set_environment → callback registration → init → load_game) as ordered service
  requests and blocks on each ack while draining (wait-graph rule 2).
- **UV-HOLD** — existing voltage thread, unchanged (D54 contract).

## The run epoch and the credit pipeline (F16, F24)
All execution is organized in **run epochs**, numbered by the single monotonic `gen`
(u64, advances exactly once per grant). Three pipeline events are distinct (F24):

- **RUN_DONE publication** — CORE appends to an ordered, bounded **RUN_DONE queue**
  (capacity = `depth`), carrying the epoch's video outcome:
  `FRAME` (last slot canonical) / `DUP` (`video_refresh(NULL)`) / `NONE` /
  `CANCELLED(outcome)` (see ABORTING).
- **Epoch drain ack** — MAIN has consumed epoch N's event stream and presented (or
  re-presented/skipped) per its outcome.
- **Credit return** — MAIN returns the epoch's credit after the drain ack; the epoch's
  input snapshot slot is released only here (snapshot lifetime = grant → credit return).

CORE may **begin an epoch only while holding a credit**. Total credits = `depth`.
**Single mode is `depth = 1`** — the "ack before N+1" rule of v2.2 is the depth-1 special
case, not a separate law: with one credit, grant→run→publish→drain→ack→credit-return is
strictly serial, no emulation/present overlap — the valid A-arm, equivalent to v1.3's
loop. **Threaded mode is `depth > 1`** (v1.4 ships depth 2): CORE runs epoch N+1 while
MAIN drains N; RUN_DONE queue and input-snapshot array are sized `depth`, so nothing is
ever overwritten while MAIN is behind — CORE simply runs out of credits and waits
(cancellable). The trial toggles `depth` only; thread ownership never changes.

Epoch N proper: 1) MAIN grants (publishes epoch-N input snapshot + options snapshot
pointer, hands a credit, advances `gen`); 2) CORE runs exactly one `retro_run`;
3) CORE publishes epoch products; 4) RUN_DONE(N); 5) MAIN drains; 6) ack + credit return.

### Intra-epoch ordering: the event stream (F25 — closes round-3 F3)
Epoch products are published as **one ordered event stream** per epoch — frame slots and
commands interleaved **in emission order** (the order the callbacks fired). MAIN drains
the stream in order: a command emitted *before* a frame applies before presenting it; a
command emitted *after* the frame (e.g. frame → SET_GEOMETRY → RUN_DONE) applies *after*
presenting that frame. "Apply all commands then present" is withdrawn.

**Audio/timing barrier**: `SET_SYSTEM_AV_INFO` (and any sample-rate change) is a barrier
event. Audio produced after it may NOT enter the ring until MAIN has drained the stream
up to the barrier and applied the whole timing transaction (audio config + DRC + pacer +
governor target — one versioned switch). CORE's post-barrier ring writes block on the
config ack (park/stop-cancellable, wait-graph row). The SDL callback therefore never
consumes mixed-format samples: everything in the ring ahead of the barrier is old-format,
everything after is written under the new config.

**Commands never wait for a frame** (F21, retained): RUN_DONE — including outcome NONE —
is the application boundary; command acks can never depend on a future video callback.
Commands pending at terminal park are drained before unload (presentation-side only,
harmless at end of session).

## CORE states and the request plane (F17, F18, F26, F27)
CORE is always in exactly one state:

- **RUNNING** — executing granted epochs.
- **ABORTING** (F26) — a park/stop request arrived while `retro_run` is on the stack.
  CORE cannot teleport to an epoch boundary; instead: the abort flag turns every
  in-callback blocked wait into an immediate return with this drop policy —
  *frames droppable* (the epoch's outcome degrades toward DUP/NONE semantics; pixels are
  re-presentable), *commands NEVER dropped* (on queue-full they go to a CORE-local
  overflow buffer, published before RUN_DONE — the no-drop guarantee survives abort),
  *audio partially accepted* (the batch callback returns the count actually written, per
  libretro semantics; the shortfall is reported in the RUN_DONE record). `retro_run`
  completes, CORE publishes **RUN_DONE(CANCELLED|outcome)** for the interrupted epoch,
  and only then transitions to QUIESCENT and acks the park. Every epoch — even an
  aborted one — terminates with a RUN_DONE boundary; the park ack is legal only after it.
- **QUIESCENT** — parked at an epoch boundary. `retro_run` prohibited; CORE sits in the
  command-service loop executing approved libretro requests (serialize, unserialize,
  reset, disc ops, option application, bootstrap sequence, unload, deinit), one at a
  time, acking each.
  **Service re-entrancy (F27 — closes round-3 F2/F18):**
  - Video/audio callbacks fired by a service op (unserialize/reset commonly emit a frame)
    are LEGAL: their products are stamped with a **service pseudo-epoch** (gen advances
    as usual) and MAIN drains them *before* accepting the service-completion ack — so
    nothing a service op emits is ever orphaned or unordered.
  - Environment *lifecycle* requests raised from inside a service op are **deferred**:
    appended to the request queue, serviced after the current op completes and acks. No
    nesting.
  - MAIN never blocks on a second synchronous ack while one is outstanding (one
    outstanding service request at a time; the request queue orders the rest).
  - Terminal requests (stop/quit) take precedence at the next op boundary: queued
    non-terminal requests after a terminal one are dropped with their acks failed-closed.
- **STOPPED** — terminal; thread exited its loop, joinable.

Core-side callbacks never execute lifecycle operations; they set request flags MAIN acts
on. Every request/ack pair is C11 acquire/release and carries its epoch.

### The wait graph, complete and acyclic (F17 — all edges)
| Wait | Waiter | Wakes on | Cancelled by |
|---|---|---|---|
| Credit available | CORE | credit return | park, stop |
| Epoch grant | CORE | grant signal | stop |
| Frame-ring space | CORE | slot freed | park→ABORTING, stop |
| Command-queue space | CORE | entry consumed | overflow buffer (never blocks under abort), park→ABORTING, stop |
| Audio-ring space | CORE | callback consumed | park→ABORTING, stop |
| AV_INFO config ack (audio barrier) | CORE | MAIN applies transaction | park→ABORTING, stop |
| RUN_DONE queue space | CORE | MAIN drains an entry | park→ABORTING, stop |
| Service-request queue space | MAIN | CORE consumes | none needed — bounded producer is MAIN itself; queue sized for max outstanding (1 + deferrals) |
| Park ack / service ack | MAIN | CORE publishes | none needed — MAIN keeps draining while waiting (rule 2) |
| CORE join | MAIN | thread exit | bounded: only after STOPPED observed; timeout ⇒ fail-closed leak-and-exit |
| Pacing delay (GFX_sync) | MAIN | absolute deadline | wakeable on lifecycle flags (checked each tick as today) |
| Present vsync | MAIN | display | bounded by hardware |

Rules: (1) every CORE wait's wake predicate includes park & stop (park during RUNNING ⇒
ABORTING path above, so CORE always reaches a RUN_DONE boundary then QUIESCENT regardless
of queue fullness); (2) while MAIN awaits any ack it continues draining the RUN_DONE
queue, event streams, and pseudo-epochs — the wait loop *is* the drain loop, so every
MAIN→CORE edge actively frees whatever CORE could block on; (3) queue locks are leaf-only.
CORE→MAIN edges all cancellable; MAIN→CORE edges all drain-while-waiting; no cycle closes.

## Reversible vs terminal lifecycle (F19, F28)
**REVERSIBLE** — menu enter/exit, sleep/resume, FF toggle, trial start/stop (depth
change), options edit, **HDMI restart** (present-side rebuild; core parks, MAIN rebuilds
renderer, resume): `park → ack (via ABORTING if mid-run) → drain-or-discard → operations
→ release → RUNNING.` Session, core, and CORE thread survive. Drain vs discard: menu/
sleep = drain; FF flip, trial depth change = discard (stale pacing).

**TERMINAL** — by lifecycle state (F28), each with its own legal sequence:

| Failure/exit point | Protocol |
|---|---|
| `dlopen`/`dlsym` fails | No CORE was created; no libretro calls; MAIN reports and returns. |
| CORE thread create fails | No CORE exists: no park, no libretro cleanup possible; MAIN `dlclose`s and bails (nothing was initialized — bootstrap hadn't run). |
| Bootstrap `retro_init` OK, `retro_load_game` fails | CORE services `retro_deinit` (NO `retro_unload_game` — no game loaded), stop → join → free → `dlclose`. |
| Loaded session (quit, poweroff, core-requested shutdown) | park → ack → drain → `retro_unload_game` then `retro_deinit` as QUIESCENT services → stop → STOPPED → join → free frontend resources → `dlclose` on MAIN. |
| After join | frontend free + `dlclose` only ever on MAIN. |

Fail-closed everywhere: park/service/join timeout ⇒ LEAK and exit the process — never
`dlclose` or free while a worker may live. Fatal signals are the one sanctioned exception
(crash contract).

## Synchronous callback channels (F20, round-3 F9 — inventory completed)
| Channel | Direction | Contract |
|---|---|---|
| Input state | MAIN→CORE | Immutable input snapshot per epoch, one array slot per credit (`depth` slots); `input_state`/`input_poll` read only the granted epoch's slot; slot lives until that epoch's credit returns. Serial: same-frame fresh. Threaded: staleness = depth epochs (~16ms at depth 2) — double-buffered-vsync class, stated and accepted. |
| `GET_VARIABLE` / options | MAIN→CORE | Immutable options snapshot, pointer swap at grant; options mutate only in QUIESCENT. |
| `SET_VARIABLE`, option definitions, `SET_VARIABLES`, input descriptors | CORE→MAIN | Deep-copied, gen-stamped events in the epoch stream. |
| `SET_SYSTEM_AV_INFO` / timing | CORE→MAIN | Barrier event; ONE versioned transaction = audio config + DRC state + pacer period + governor target fps, applied atomically at the barrier drain (no piecemeal). |
| HUD / governor feedback | present/CORE | Named atomic words as today: `gfx_flip_wait_us` (present→gov), DRC ppm word (present→CORE pacer), gen-rate counters (CORE→gov, gov runs on CORE). No new shared state. |
| Scaler/effect/sharpness/palette, `fb_present`/`fb_game` | MAIN-owned | Present-side state, mutated only in QUIESCENT (options UI) or by gen-stamped commands; CORE never reads. |
| Disk-control callback table | CORE-owned | Invoked only via QUIESCENT service requests. |
| Rumble | CORE→MAIN | Single atomic word; MAIN dispatches at each drain. |
| Log/message/perf | CORE→MAIN | Log: async-safe append. Messages: stream events. Perf: CORE-local. |
| Const-after-load queries | either | Frozen before first RUNNING epoch; readable anywhere. |
| Callbacks during QUIESCENT service | CORE→MAIN | Service pseudo-epoch, drained before the service ack (F27). |

## Frame lifetime (round-3 F4 — visual-source generation)
Present keeps an immutable **last-presented snapshot** tagged with its true **visual-
source generation V** — the epoch that actually produced the pixels. DUP/NONE outcomes
re-present without retagging (V unchanged). **Save transaction:** park at epoch G, drain
through G, then record the pair **(state G, snapshot V, V ≤ G)** — serialize + snapshot
written together, never retagged. When G's outcome was FRAME, V = G and the screenshot
matches the state exactly; for DUP/NONE, V < G is recorded honestly (the screenshot is
the last real frame — same semantics v1.3 has today, now stated). Slots recycle only
after the snapshot copy; `vid.blit`/platform pointers never outlive their frame's drain.

## Audio ownership (round-1 F5 RESOLVED; barrier added)
| Audio state | Owner | Sync |
|---|---|---|
| Ring payload + indices | producer CORE / consumer SDL callback | C11 SPSC acquire/release |
| Stats (occupancy, underruns) | written by callback | single atomic word |
| Config (rate, resampler/DRC) | MAIN, applied only at the AV_INFO barrier drain | producer blocked at barrier first |
| Lifecycle (open/close/pause/free) | MAIN | producer parked + acked BEFORE close/resize/free; close waits for the in-flight callback to return (SDL_CloseAudioDevice semantics — wait edge in the graph) |
Producer waits are park/stop-cancellable (ABORTING under park).

## Crash contract (F22 RESOLVED — unchanged from v2.2)
- Fault on CORE: handler runs on the sole SRAM/RTC mutator, halted in its own handler —
  single-threaded semantics; write live SRAM/RTC via the async-signal-safe tmp+rename
  path; re-raise.
- Fault on any other thread: DECLINE the emergency save; the last park-point disk flush
  stands (park points = menu/sleep/save transactions, exactly v1.3's flush points).
  Strictly better than a torn write.
- Implementation note (round-3 requirement): "faulting thread == CORE" is a thread-local
  flag set at CORE start (async-signal-safe read of the faulting thread's own TLS), or
  equivalently `pthread_equal(pthread_self(), core_tid_atomic)` — both handler-legal.
- Zero per-frame cost; no snapshot buffers; `SIGTERM` = request flag only.

## Verdicts and the trial (F10/F11 closed per round 3)
- Floor-band observation: session-local, no durable verdict.
- Durable negatives record their ceiling band; re-arm when demand exceeds it.
- Trial windows count active gameplay samples; invalidated by FF, menu, sleep, state
  load, geometry/timing change, core-option change, HDMI, thermal intervention.
- **Workload validity across ALL arms (F10 fix)**: A/B/A requires (a) the two A arms'
  ceiling-residency histograms within L1 ≤ 0.2 of each other, AND (b) the **B arm's
  histogram within L1 ≤ 0.3 of the A-arm mean** — B is judged only on a workload the A
  arms certify. Either check failing invalidates the trial (retry later).
- Governor history resets symmetrically before each arm.
- Commit: ≥1 OPP sink AND delivery health intact AND class-level total-device
  energy+temperature evidence. Ceiling alone never ships.
- Positive persists only after clean post-trial dwell AND clean teardown.
- **Sidecar v2 key (F11 fix)**: content identity + core build hash + effective-options
  fingerprint + threading schema version + device model. Content identity = file size +
  basename hash + **normalized launch path** (resolved absolute path minus the mount
  root) — full-content hashing stays prohibitive for CD images and is not claimed.
  **Ambiguous identity fails closed**: multi-file content (m3u playlists, cue+multi-bin,
  archives) is treated as unverdicted-always in v1.4 — no sidecar is written or honored
  for it (stated limitation; SNES-class targets are single-file).
  Legacy `minarch_thread_video=On` ignored; all v1 sidecars invalidated; corrupt =
  unverdicted.
- Requalification gate: first 60 frames after load run depth 1; the persisted positive is
  honored only if that window's delivery health passes; failure demotes to unverdicted.

## Performance contract (round-3 requirement)
The epoch machinery must not cost the energy it exists to save:
- **Credit fast path is one uncontended atomic** (acquire a credit / return a credit);
  while the pipeline is neither empty nor full and no lifecycle flag is set, an epoch
  advance performs NO kernel wake — no futex, no condvar signal.
- Kernel waits/wakes are legal only on: pipeline empty (CORE out of credits), pipeline
  full (MAIN behind), and lifecycle transitions (park/stop/grant-after-idle).
- Serial mode (depth 1) inherently rendezvouses per frame; its wake budget is the
  baseline v1.3 loop's (vsync block) plus at most one signal pair per frame — must be
  measured, not assumed.
- **Acceptance adds**: synchronization CPU time and wakeups/sec (both modes), and
  total-device energy + temperature for serial-v2 vs v1.3-single as a REGRESSION GATE —
  v2's serial mode may not measurably regress the shipping single-thread baseline — plus
  the existing threaded-vs-serial class evidence.

## Verification gate (ships or it doesn't)
1. `framering.{c,h}` (ring + event stream + credit pipeline + state machine + lifecycle)
   is the linked shipping code AND host-compiled under **TSan and ASan as two separate
   builds**, driven by an **adversarial fake core**: geometry storms, NULL-dup frames,
   zero-video epochs, multi-callback runs, saves during load, shutdown in every state,
   park storms against full queues, **aborts mid-callback with full rings, service ops
   that emit frames, lifecycle requests from inside service ops, bootstrap failures at
   every step of the pre-load sequence**.
2. Forbidden-globals audit, mechanical (nm/objdump over CORE-compiled units), CI-fatal.
3. On-device gauntlet: round-1 list + v2.2 additions + new-mechanism cases: ABORTING
   with command overflow, RUN_DONE(CANCELLED) drain, service pseudo-epoch during
   unserialize, AV_INFO barrier with full audio ring, credit exhaustion at depth 2,
   bootstrap-failure teardown per F28 row, ambiguous-identity (m3u) fail-closed.
4. Fingerprint discipline (D54): arms verified by log fingerprint; workspace-clean builds.

## Measurement plan (unchanged, F15)
Task #11 pipeline profile first, both modes; freeze pipeline before verdict testing;
invalidate all prior sidecars; recalibrate eligibility on the frozen pipeline; discovery
vs holdout scenes; counterbalanced A/B/A/B; matched charge/thermal; ship evidence per
class = total-device energy + temperature + delivery health (+ the new sync-cost gate).

## Explicitly out of scope
PS1 threading revival (receipts: no benefit), threading in the menu binary, >2 frontend
threads, depth >2 in v1.4, user-facing toggles (the machine decides; design axiom).

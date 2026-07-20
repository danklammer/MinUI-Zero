# Threading v2.4 — ownership-model design (v1.5 candidate)

Status: CONTRACT PHASE CLOSED; implementation is integrated as the PS-only v1.5 candidate
described by D62. Device release gates remain open. Resolves round-4's four semantic P0s
(findings 29–32) and its remaining PARTIALs (rounds 1–4 in .notes/threading-v2-review-codex.md
+ review thread). From here, adjudication moves to the implementation harness; this document
is amended only when implementation contradicts it, with a D-log entry.
Predecessors: v1.3 threading (compiled out, D52). The historical DKC/Yoshi traces in
`docs/bench/2026-07-08-snes-*` motivated this design, but they measured the retired mailbox
path and are not total-device v2 release evidence; D63 keeps both SNES launchers serial.

**Implementation disposition:** the ownership, lifecycle, event-order, and credit contracts
below are implemented. The adaptive observe/trial/verdict policy was not selected: v1.5's
candidate requests depth two only from `PS.pak`, starts every other system on the plain serial
frontend, and uses the D62 crash canary for automatic fallback. References below to a trial or
positive sidecar are retained design history, not shipping behavior; D62 takes precedence.

## Threads, named once
- **MAIN** — one OS thread: process main = menu = present = SDL/GLES/audio-lifecycle owner.
  MAIN also owns **grant pacing**: MAIN issues run grants on the pacer schedule (GFX_sync
  cadence today), consuming the DRC feedback word. CORE never self-schedules.
- **CORE** — one OS thread per loaded *core* (F23): created immediately after
  `dlopen`/`dlsym` succeed, before ANY libretro symbol is called. **Every exported
  libretro call executes on CORE**; only `dlopen`/`dlsym`/`dlclose` are MAIN-owned. The
  core sees exactly one thread for its entire lifetime. The full birth-to-RUNNING
  sequence is the bootstrap state machine below (F31).
- **UV-HOLD** — existing voltage thread, unchanged (D54 contract).

## Two sequence namespaces (F16/F30 — the gen contradiction, resolved)
- **`gen`** (u64): advances **exactly once per RUN grant**, never otherwise.
- **`svc_seq`** (u64): advances **exactly once per QUIESCENT service operation**.
Run-epoch products are tagged `(RUN, gen, index)`; service products are tagged
`(SERVICE, svc_seq, index)`, `index` = emission order within the epoch/op. The two
namespaces are disjoint; nothing service-side touches `gen`, credits, or the input-snapshot
array.

## The run epoch and the credit pipeline (F16, F24)
Three pipeline events are distinct:
- **RUN_DONE publication** — CORE appends to an ordered, bounded RUN_DONE queue
  (capacity = `depth`): outcome `FRAME` / `DUP` / `NONE` / `CANCELLED(outcome)`.
- **Epoch drain ack** — MAIN has consumed epoch N's event stream through RUN_DONE and
  presented (or re-presented/skipped) per its outcome.
- **Credit return** — after the drain ack; the epoch's input-snapshot slot is released
  only here (snapshot lifetime = grant → credit return).

CORE begins an epoch only while holding a credit. Total credits = `depth`.
**Single mode is `depth = 1`** — strictly serial grant→run→publish→drain→ack→return; no
emulation/present overlap; the valid A-arm. **Threaded mode is `depth > 1`** (the v1.5
candidate uses depth 2): CORE runs N+1 while MAIN drains N; RUN_DONE queue and snapshot array are sized
`depth`, so nothing is overwritten while MAIN is behind — CORE runs out of credits and
waits (cancellable). The trial toggles `depth` only; thread ownership never changes.

Epoch N proper: 1) MAIN grants (publishes epoch-N input snapshot + options snapshot
pointer, hands a credit, advances `gen`); 2) CORE runs exactly one `retro_run`;
3) CORE publishes products into the epoch event stream; 4) RUN_DONE(N); 5) MAIN drains;
6) ack + credit return.

### Depth-change / park transition gate (F24 — transition lifetime)
Any depth change (trial start/stop) and any park that must reclaim the pipeline runs this
exact sequence:
1. stop issuing grants;
2. cancel queued-but-unrun grants (their credits and snapshot slots return immediately —
   no epoch existed);
3. finish or abort the active epoch (ABORTING rules if mid-run);
4. drain — or discard the discardable payloads of — everything published (persistent
   events are always applied; see below);
5. return every credit;
6. verify no snapshot slot is owned;
7. resize/replace the snapshot array and RUN_DONE queue while CORE is QUIESCENT.
Only then may grants resume at the new depth.

### Intra-epoch ordering: the event stream (F25/F29)
Epoch products are published as **one ordered event stream** per epoch — frame slots and
commands interleaved in emission order. MAIN drains in order: a command emitted before a
frame applies before presenting it; a command emitted after the frame applies after.

**Prefix-drain of an open epoch (F29):** MAIN's drain loop may consume published events of
the *current, still-running* epoch — it never has to wait for RUN_DONE to start draining.
Ordinary events make this an optimization; **barrier events make it mandatory**: a barrier
publication wakes MAIN (one kernel wake; rare, event-driven — legal wake class), MAIN
drains the stream up to and including the barrier and applies it, which releases CORE's
barrier wait. This breaks the round-4 progress cycle by construction: MAIN's drain never
depends on RUN_DONE, so "CORE waits for config ack → MAIN waits for RUN_DONE" cannot
close.

**Audio/timing barrier**: `SET_SYSTEM_AV_INFO` (and any sample-rate change) is a barrier
event carrying ONE versioned transaction (audio config + DRC state + pacer period +
governor target), applied atomically at the barrier drain. Audio produced after the
barrier blocks entering the ring until the transaction is applied (drain-releases wait);
the SDL callback never consumes mixed-format samples.

**Persistent events are never discardable (F29/F26):** `SET_SYSTEM_AV_INFO`, geometry,
option/variable changes, disk-state changes — any event that reflects a *state change the
core has already made* — must be applied on every path. "Discard" transitions (FF flip,
trial depth change) discard **visual and audio payload only**. ABORTING keeps a per-epoch
barrier ledger (applied/pending per barrier encountered); every pending persistent event
is applied before the park ack is published and before RUNNING resumes.

**Commands never wait for a frame (F21):** RUN_DONE — including outcome NONE — is the
application boundary; with prefix-drain, a barrier doesn't even wait for that. Commands
pending at terminal park are drained before unload.

## CORE states and the request plane (F17, F18, F26, F27, F30)
- **RUNNING** — executing granted epochs.
- **ABORTING** — a park/stop request arrived while `retro_run` is on the stack. The abort
  flag turns every in-callback blocked wait into an immediate return with this drop
  policy: *frames droppable* (outcome degrades toward DUP/NONE; pixels re-presentable);
  *commands NEVER dropped* (queue-full ⇒ CORE-local overflow, published before RUN_DONE);
  *audio partially accepted* (batch callback returns the count written; shortfall recorded
  in the RUN_DONE record); *persistent events per the ledger above*. `retro_run`
  completes, CORE publishes RUN_DONE(CANCELLED|outcome), transitions to QUIESCENT; the
  park ack is legal only after that boundary AND after MAIN has applied all pending
  persistent events.
- **QUIESCENT (F30)** — parked at an epoch boundary, `retro_run` prohibited, servicing
  approved libretro requests (bootstrap sequence, serialize, unserialize, reset, disc
  ops, option application, unload, deinit) one at a time, acking each.
  **Entering QUIESCENT consumes the park request.** While QUIESCENT there is no park
  edge: a park request arriving is absorbed as a no-op (already parked). Consequently:
  - **Service waits are drain-driven, not park-cancelled.** A service op's callbacks may
    emit frames/audio/commands; these flow through a **dedicated service stream slot**
    (fixed small buffer — service ops consume no pipeline credit and no input-snapshot
    slot) tagged `(SERVICE, svc_seq, index)`. If publication blocks on slot space, the
    wake is MAIN's drain — and wait-graph rule 2 guarantees MAIN drains while awaiting
    any ack, so progress is structural. No cancellation edge exists or is needed.
  - **Stop requests are handled at service-operation boundaries** — an in-flight
    serialize/unserialize/reset runs to completion (its waits drain-driven), then the
    stop is honored before the next op. Fail-closed timeouts (leak-and-exit) bound the
    pathological case.
  - Service callbacks read the **last-published input snapshot** (staleness: whatever the
    most recent grant published — acceptable; service ops are not gameplay).
  - MAIN drains **all** service events of an op before accepting its completion ack.
  - Environment lifecycle requests raised inside a service op are deferred to the request
    queue, serviced after the current op acks; MAIN never blocks on a second synchronous
    ack while one is outstanding; terminal requests take precedence at the next op
    boundary.
- **STOPPED** — terminal; thread exited its loop, joinable.

Core-side callbacks never execute lifecycle operations; they set request flags MAIN acts
on. Every request/ack pair is C11 acquire/release and carries its sequence tag.

### The wait graph, complete and acyclic (F17)
| Wait | Waiter | Wakes on | Cancelled by |
|---|---|---|---|
| Credit available | CORE | credit return | park, stop |
| Epoch grant | CORE | grant signal (MAIN-paced) | stop |
| Frame-ring space (RUNNING) | CORE | slot freed by drain | park→ABORTING, stop |
| Command-queue space (RUNNING) | CORE | entry consumed | overflow buffer (never blocks under abort), park→ABORTING, stop |
| Audio-ring space (RUNNING) | CORE | callback consumed | park→ABORTING, stop |
| AV_INFO config ack (barrier) | CORE | MAIN prefix-drains + applies | stop; under park→ABORTING the audio wait returns partial but the barrier itself is still applied pre-ack (ledger) |
| RUN_DONE queue space | CORE | MAIN drains an entry | park→ABORTING, stop |
| Service stream-slot space (QUIESCENT) | CORE | MAIN drains | none — drain-driven (no park edge in QUIESCENT); stop honored at op boundary; fail-closed timeout |
| Service-request queue space | MAIN | CORE consumes | none needed — bounded producer is MAIN itself |
| Park ack / service ack | MAIN | CORE publishes | none needed — MAIN keeps draining while waiting (rule 2) |
| Audio close vs in-flight callback | MAIN | SDL callback returns | bounded by callback duration; producer parked/acked first (order rule) |
| CORE join | MAIN | thread exit | bounded: only after STOPPED observed; timeout ⇒ fail-closed leak-and-exit |
| Grant-pacing delay | MAIN | absolute pacer deadline | wakeable on lifecycle flags (checked per tick) |
| Present vsync | MAIN | display | bounded by hardware |

Rules: (1) every RUNNING-state CORE wait's wake predicate includes park & stop (park ⇒
ABORTING, so CORE always reaches a RUN_DONE boundary then QUIESCENT regardless of queue
fullness); QUIESCENT-state waits are drain-driven per F30 — park cancellation does not
apply where no park edge exists. (2) While MAIN awaits any ack it continues draining the
RUN_DONE queue, open-epoch event streams (prefix-drain), and service streams — the wait
loop *is* the drain loop, so every MAIN→CORE edge actively frees whatever CORE could
block on. (3) Queue locks are leaf-only. CORE→MAIN edges cancellable or drain-driven;
MAIN→CORE edges drain-while-waiting; no cycle closes.

## Bootstrap state machine (F31 — the cleanup oracle)
Ordered states, with the calls made in each transition (core calls on CORE as QUIESCENT
service ops; frontend work on MAIN):

| State reached | Work done to get here |
|---|---|
| CORE_CREATED | `dlopen`/`dlsym` (MAIN); CORE thread created, starts QUIESCENT. No libretro call yet. |
| INFO_READY | `retro_get_system_info`, `retro_set_environment`, all `retro_set_*` callback registration (CORE). MAIN-side: content open + full-path prep, core config-path setup. |
| INITIALIZED | `retro_init` (CORE). |
| CONTENT_LOADED | `retro_load_game` returned true (CORE). |
| MEMORY_READY | `retro_get_memory_size`/`data` for SRAM+RTC; SRAM/RTC loaded from disk (CORE service + MAIN file I/O); **crash handler armed only now** (pointers valid). |
| AV_READY | `retro_get_system_av_info`, `retro_set_controller_port_device` (CORE); audio initialized from AV info (MAIN). |
| FRONTEND_READY | Renderer/present path initialized (MAIN); const-after-load queries frozen. |
| RESUME_APPLIED | Auto-resume `retro_unserialize` as a QUIESCENT service op (its emitted frames = service events, drained normally). **Resume failure is non-fatal**: log, continue without resume. |
| RUNNING | First grant issued. |

Failure → cleanup table (every row is a harness test with a defined expected result;
core-requested shutdown during bootstrap = failure at the current state):

| Failing state | Legal cleanup |
|---|---|
| dlopen/dlsym fail | No CORE, no libretro calls; report and return. |
| CORE thread create fail | No CORE calls ever ran; MAIN `dlclose`s and bails. |
| CORE_CREATED / INFO_READY | stop → join → `dlclose`. No `retro_deinit` (init never ran); registration needs no teardown. |
| INITIALIZED (content open failed, `retro_load_game` false) | `retro_deinit` on CORE — **no `retro_unload_game`** — stop → join → `dlclose`. |
| CONTENT_LOADED / MEMORY_READY / AV_READY / FRONTEND_READY | disarm crash handler if armed → `retro_unload_game` → `retro_deinit` on CORE → stop → join → free frontend resources → `dlclose`. |
| RESUME_APPLIED | non-fatal by policy (see above). |
| RUNNING onward | reversible/terminal protocols below. |

## Reversible vs terminal lifecycle (F19, F28)
**REVERSIBLE** — menu enter/exit, sleep/resume, FF toggle, trial start/stop (depth change
via the transition gate), options edit, HDMI restart (present-side rebuild):
`park → ack (via ABORTING if mid-run) → transition gate steps 1–6 as applicable →
operations → release → RUNNING.` Session, core, and CORE thread survive. Drain vs
discard: menu/sleep = drain; FF flip, trial depth change = discard — **payload only;
persistent events always applied (F29)**.

**TERMINAL** — loaded session (quit, poweroff, core-requested shutdown): park → ack →
drain → `retro_unload_game` then `retro_deinit` as QUIESCENT services → stop → STOPPED →
join → free frontend resources → `dlclose` on MAIN. Pre-RUNNING failures use the
bootstrap table. Fail-closed everywhere: park/service/join timeout ⇒ LEAK and exit —
never `dlclose` or free while a worker may live. Fatal signals are the one sanctioned
exception (crash contract).

## Synchronous callback channels (F20)
| Channel | Direction | Contract |
|---|---|---|
| Input state | MAIN→CORE | Immutable snapshot per epoch, one slot per credit (`depth` slots); `input_state`/`input_poll` read only the granted epoch's slot; slot lives until credit return. Serial: same-frame fresh. Threaded: staleness = depth epochs (~16ms at depth 2), stated and accepted. Service callbacks: last-published snapshot (F30). |
| `GET_VARIABLE` / options | MAIN→CORE | Immutable options snapshot, pointer swap at grant; options mutate only in QUIESCENT. |
| `SET_VARIABLE`, option definitions, `SET_VARIABLES`, input descriptors | CORE→MAIN | Deep-copied, sequence-tagged events in the stream; persistent class (never discarded). |
| `SET_SYSTEM_AV_INFO` / timing | CORE→MAIN | Barrier event; one versioned transaction (audio config + DRC + pacer + governor target) applied atomically at prefix-drain. Persistent. |
| HUD / governor feedback | present/CORE | Named atomic words: `gfx_flip_wait_us` (present→gov), DRC ppm word (**present→MAIN grant pacer** — pacing is MAIN-owned), gen-rate counters (CORE→gov; gov runs on CORE). No new shared state. |
| Scaler/effect/sharpness/palette, `fb_present`/`fb_game` | MAIN-owned | Present-side state, mutated only in QUIESCENT (options UI) or by sequence-stamped commands; CORE never reads. |
| Disk-control callback table | CORE-owned | Invoked only via QUIESCENT service requests. |
| Rumble | CORE→MAIN | Single atomic word; MAIN dispatches at each drain. |
| Log/message/perf | CORE→MAIN | Log: async-safe append. Messages: stream events. Perf: CORE-local. |
| Const-after-load queries | either | Frozen at FRONTEND_READY; readable anywhere. |
| Callbacks during QUIESCENT service | CORE→MAIN | `(SERVICE, svc_seq, index)` via the service stream slot, drained before the op's ack (F30). |

## Frame lifetime (round-3 F4)
Present keeps an immutable **last-presented snapshot** tagged with its true visual-source
generation V — the epoch that actually produced the pixels. DUP/NONE re-present without
retagging. **Save transaction:** park at epoch G, drain through G, record the pair
**(state G, snapshot V, V ≤ G)** — serialize + snapshot written together, never retagged.
When G's outcome was FRAME, V = G and the screenshot is from the **same epoch** as the
state (libretro cannot prove stronger than that, and we don't claim it); for DUP/NONE,
V < G is recorded honestly. Slots recycle only after the snapshot copy; `vid.blit`/
platform pointers never outlive their frame's drain.

## Audio ownership (round-1 F5)
| Audio state | Owner | Sync |
|---|---|---|
| Ring payload + indices | producer CORE / consumer SDL callback | C11 SPSC acquire/release |
| Stats (occupancy, underruns) | written by callback | single atomic word |
| Config (rate, resampler/DRC) | MAIN, applied only at the AV_INFO barrier prefix-drain | producer blocked at barrier first |
| Lifecycle (open/close/pause/free) | MAIN | producer parked + acked BEFORE close/resize/free; close waits for the in-flight callback to return (wait-graph row) |
Producer waits: park/stop-cancellable in RUNNING (ABORTING); drain-driven in QUIESCENT.

## Crash contract (F22 + round-4 hardening)
- Fault on CORE: handler runs on the sole SRAM/RTC mutator, halted in its own handler —
  single-threaded semantics; write live SRAM/RTC via the async-signal-safe tmp+rename
  path; re-raise.
- Fault on any other thread: DECLINE the emergency save; the last park-point disk flush
  stands. Strictly better than a torn write.
- **"Faulting thread == CORE" predicate (hardened)**: a statically allocated thread-local
  marker — `static __thread int zero_is_core_thread;` set to 1 once at CORE start — read
  directly by the handler. No `pthread_self`/`pthread_equal` (removed per round 4). The
  `__thread` access is signal-safe here because it is initial-exec static TLS in the main
  executable, allocated at thread creation — no lazy TLS runtime machinery can be invoked
  by the read. Crash saves are armed only at MEMORY_READY and disarmed before unload.
- Zero per-frame cost; no snapshot buffers; `SIGTERM` = request flag only.

## Verdicts and the trial (F10 → F32, F11)
- Floor-band observation: session-local, no durable verdict.
- Durable negatives record their ceiling band; re-arm when demand exceeds it.
- Trial windows count active gameplay samples; invalidated by FF, menu, sleep, state
  load, geometry/timing change, core-option change, HDMI, thermal intervention.
- **Workload validity is treatment-independent (F32).** Cross-arm equivalence is
  established by: (a) **matched save-state scenes** — every arm replays the same content
  from the same state, the primary control; (b) **emulated-work counters** — emulated
  frames per wall-second and audio samples produced per second must match across arms
  (at full speed these are treatment-equal by design; a mismatch means an arm slipped or
  the scene diverged, and the trial invalidates); (c) the A/A **ceiling-residency drift
  check stays** (same treatment, so residency is comparable *within* A arms — it catches
  environmental drift). The A-vs-B residency comparison is **removed** as a validity
  gate: ceiling residency is the treatment's expected *output* and is used only as an
  outcome metric.
- Governor history resets symmetrically before each arm.
- Commit: ≥1 OPP sink AND delivery health intact AND class-level total-device
  energy+temperature evidence. Ceiling alone never ships.
- Positive persists only after clean post-trial dwell AND clean teardown.
- **Sidecar v2 key (F11)**: content identity + core build hash + effective-options
  fingerprint + threading schema version + device model. Content identity = file size +
  **mtime** + basename hash + normalized launch path (resolved absolute path minus the
  mount root). Full-content hashing stays prohibitive for CD images and is not claimed.
  **Accepted residual, stated**: an in-place content swap preserving size AND mtime at
  the same path defeats the key — accepted as adversarial-only (normal file operations
  update mtime). Ambiguous identity (m3u, cue+multi-bin, archives) fails closed to
  unverdicted-always in v1.4. Legacy `minarch_thread_video=On` ignored; all v1 sidecars
  invalidated; corrupt = unverdicted.
- Requalification gate: first 60 frames after load run depth 1; the persisted positive is
  honored only if that window's delivery health passes; failure demotes to unverdicted.

## Performance contract
- **Credit fast path is one uncontended atomic** (acquire/return); while the pipeline is
  neither empty nor full and no lifecycle flag is set, an epoch advance performs NO
  kernel wake.
- Kernel waits/wakes are legal only on: pipeline empty, pipeline full, lifecycle
  transitions, and **barrier publication** (event-driven, rare — AV_INFO-class changes
  happen at mode switches, not per frame).
- Serial mode (depth 1) rendezvouses per frame; its wake budget is the v1.3 loop's
  (vsync block) plus at most one signal pair per frame — measured, not assumed.
- **Acceptance adds**: synchronization CPU time and wakeups/sec (both modes), and
  total-device energy + temperature for serial-v2 vs v1.3-single as a REGRESSION GATE,
  plus the existing threaded-vs-serial class evidence.

## Verification gate (ships or it doesn't)
1. `framering.{c,h}` (ring + event stream + credit pipeline + state machine + lifecycle)
   is the linked shipping code AND host-compiled under **TSan and ASan as two separate
   builds**, driven by an **adversarial fake core**: geometry storms, NULL-dup frames,
   zero-video epochs, multi-callback runs, saves during load, shutdown in every bootstrap
   state (the F31 table is the oracle), park storms against full queues, aborts
   mid-callback with full rings, **AV_INFO mid-run with a full audio ring (prefix-drain
   proof)**, **park arriving during a frame-emitting service op (no-park-edge proof)**,
   **depth changes under load (transition-gate proof)**, lifecycle requests from inside
   service ops.
2. Forbidden-globals audit, mechanical (nm/objdump over CORE-compiled units), CI-fatal.
3. On-device gauntlet: prior lists + ABORTING with command overflow, RUN_DONE(CANCELLED)
   drain, service events during unserialize, barrier under full ring, credit exhaustion
   at depth 2, bootstrap-failure teardown per F31 row, ambiguous-identity fail-closed.
4. Fingerprint discipline (D54): arms verified by log fingerprint; workspace-clean builds.

## Measurement plan
Task #11 pipeline profile first, both modes; freeze pipeline before verdict testing;
invalidate all prior sidecars; recalibrate eligibility on the frozen pipeline; discovery
vs holdout scenes; counterbalanced A/B/A/B; matched charge/thermal; ship evidence per
class = total-device energy + temperature + delivery health + the sync-cost gate;
workload validity per F32 (treatment-independent).

## Explicitly out of scope
PS1 threading revival (receipts: no benefit), threading in the menu binary, >2 frontend
threads, depth >2 in v1.4, user-facing toggles (the machine decides; design axiom).

## Contract phase closed
Round 4's convergence criterion, quoted: *"After these semantics are written, further
ownership and race risk should be adjudicated by the shipping framering implementation,
adversarial fake core, TSan/ASan, and on-device gauntlet rather than additional prose."*
The four semantic decisions are now written as decisions: barrier prefix-drain with a
non-discardable persistent-event class (F29); a park-free, drain-driven service-event
protocol in its own sequence namespace (F30); a bootstrap state machine with a per-state
cleanup oracle (F31); treatment-independent workload validity (F32). Remaining risk is
implementation risk, and its referees are `framering.{c,h}`, the adversarial fake core,
TSan/ASan, the forbidden-globals audit, and the on-device gauntlet. This document is
amended hereafter only when implementation contradicts a contract — each amendment gets a
DECISIONS.md entry.

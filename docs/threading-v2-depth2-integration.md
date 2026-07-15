# Threading v2 — depth-2 integration design (the Codex review artifact)

**Status:** design, pre-implementation. This is the plan to hand Codex before any code or
device time. Depth-1 is COMPLETE and on-device-validated (D52–D59); WP2 (the `run()` snapshot-
delivery contract) is committed (D60). This doc covers the **remaining** work: turning the serial
depth-1 rendezvous into the depth-2 **pipeline** that is the actual energy win.

---

# REVISION 2 — post-Codex adversarial review (2026-07-15)

Codex reviewed rev-1 at commit b2858858 and returned 13 findings (8×P0, 5×P1). Spot-checked
against source and accepted: F1 (CORE calls `GFX_clearAll` + mutates global `renderer` on resize,
minarch.c:3196+), F2 (`PWR_update → Menu_beforeSleep → State_autosave` enters the core, 1959→3604),
F6 (NULL early-return, no `fc_signal_dup`, 3305) — all as cited. Rev-1 was frame+input scoped and
under-specified the command channel, the sleep/power core-entry, the depth-2 pacer, the governor
gen-signal, and frame-descriptor immutability. **This revision supersedes rev-1 where noted; the
rev-1 body is retained below for the review trail.**

## Corrected architecture principle: CORE is a PURE PRODUCER

The CORE thread does exactly three things per epoch: (1) run `core.run()`, (2) **copy** each frame
into a credit-owned buffer and publish an **immutable descriptor**, (3) emit audio + commands into
the ring. It applies **nothing**. Every application of state — renderer/scaler selection,
`GFX_resize`/`GFX_clearAll`, `renderer.dst`, HUD, geometry/AV/variables, present, save-pairing —
happens on **MAIN**, in ring order. Rev-1's shortcut (CORE did the HUD blit + renderer setup +
resize) was the root of F1 and F5. This is the v2.4 contract taken literally.

## Work packages, revised (each finding folded in)

- **WP-A — input tick to MAIN + snapshots + service routing.**
  - Split `input_poll_callback` → `input_tick_main()` on MAIN; registered libretro callback is a
    no-op when `on_core`. (rev-1)
  - **F2 sleep flow:** `PWR_update` is NOT a plain MAIN move. MAIN detects the sleep request, then
    **park → drain → run autosave/memory-flush as CORE services → close audio only after park ack
    → stay QUIESCENT through sleep → resume audio on MAIN → release CORE.**
  - **F11 control snapshots:** carry `fast_forward` (and other per-epoch control flags) in the grant
    snapshot; do NOT read them live on CORE. Pack analog axes through `uint16_t` casts —
    left-shifting a negative signed axis is UB.
  - **F3 pacer + input latching:** MAIN must not free-spin when the pipeline is full (fc_pump
    returns immediately at depth≥2 — the D59 busy-spin, pipelined). Add a MAIN-owned grant cadence
    with a lifecycle-wakeable wait; **latch edge-triggered input until a grant succeeds** so a short
    press during a failed grant is never lost.

- **WP-B — immutable per-credit frame descriptor** (replaces rev-1's bare pool).
  - CORE copies pixels into the credit's buffer and publishes an **immutable** descriptor
    `{pixels, gen, width, height, pitch, format}`. MAIN owns renderer/scaler/`GFX_resize`/
    `GFX_clearAll`/HUD/dst/present. (**F1**)
  - **F6 DUP/NONE + multi-callback:** route NULL → `fc_signal_dup`; MAIN re-presents the MAIN-owned
    **last-presented** immutable frame for DUP/NONE. A retro_run that calls video_refresh multiple
    times needs per-published-frame storage or a specified coalescing+ack — one mutable buffer per
    epoch is insufficient.
  - **F12 failure paths:** validate depth∈{1,2}; validate dims/alloc sizes (checked multiply);
    allocate+verify all slots before enabling depth-2; never publish a stale/null slot; on OOM fall
    back to depth-1 through the depth-change gate.

- **WP-C — command channel** (NEW; the biggest rev-1 omission, **F5**).
  - Route `SET_SYSTEM_AV_INFO` / `SET_GEOMETRY` / `SET_VARIABLE` / `SHUTDOWN` / rumble through
    `FR_EV_CMD` (persistent/barrier as the design specifies); `zero_ftv2_drain` applies them on MAIN
    in order (it currently ignores FR_EV_CMD, minarch.c:5294). The libretro data pointer is
    transient → **deep-copy** the payload into the event. Shutdown/quit via an atomic/event, not a
    bare CORE-write/MAIN-read. Rumble via the contracted atomic handoff.

- **WP-D — governor + telemetry at depth-2** (NEW, **F4**).
  - Derive generation rate from **drained RUN_DONE** epochs, not loop/pump iterations. Publish CORE
    work through defined atomics/events. Replace the `thread_video` predicate (always 0 under
    guard-ON, so single-thread gov/DRC branches are wrongly selected) with an explicit v2
    depth/mode predicate. Reconcile with the v2.4 governor-ownership clause.

- **WP-E — audio lifecycle** (NEW, **F8**).
  - Make `SND_batchSamples` producer waits **park/stop-cancellable** with partial-accept, and
    propagate the shortfall into RUN_DONE (the framering already models `shortfall`). Always
    park-and-ack before any pause/close/resize/free of the audio device — otherwise the F2 sleep
    ordering can wedge park (CORE waiting on a full ring MAIN has stopped draining). The bounded
    audio lead itself is fine for bring-up; the cancellation/lifecycle gaps are the bug.

- **WP-F — save-state visual identity** (**F9**).
  - Keep a MAIN-owned **immutable last-presented** snapshot tagged with visual gen V. While parked,
    save the contractual pair `(state G, snapshot V)`. Add a terminal **save-and-quit** service path
    that does NOT release CORE or grant another epoch (rev-1's `fc_menu_op`-then-quit released
    RUNNING and could grant again before quit was seen).

- **WP-G — run signature** (**F10** — rev-1's TLS-stash cannot work; `zero_ftv2_run` never receives
  gen). Extend explicitly: `run(ctx, gen, slot, snapshot)` — pass the actual credit slot too, so
  minarch is not coupled to the `gen % depth` selection and needs no hidden TLS.

- **WP-C(enable) — unchanged in intent**, but default depth stays **1**; depth-2 is a guarded
  bring-up only; requalification gate precedes any auto-trial.

- **F7 (menu/disc F23):** valid but NOT a novel depth-2 blocker — depth-1 already calls
  `core.serialize` from MAIN during menu saves (shipped + on-device-validated), and `fc_park` fully
  quiesces CORE, so there is no concurrent access; only thread-identity differs, which our shipped
  cores don't care about in serialize. Fold the full service inventory in (memory flush, disc
  replace, controller change, save/load/reset, split MAIN screenshot/file work from CORE
  serialization) as **contract-hardening**, prioritized below F1–F6.

- **F13 (harness tests the glue):** the engine harness does not exercise minarch's renderer/input/
  audio/save/pool glue. Extract the shipping glue into a testable module (or compile the real
  callbacks/pool into a harness) and add the enumerated adversarial cases (pipeline-full-no-event,
  input edge with no credit, NULL/zero/multi-callback epochs, geometry/AV during N+1 while
  presenting N, normal+discard park with pool poisoning after credit return, save/load/reset/
  save-quit, sleep with a full audio ring, pool alloc/resize failure).

## Revised work-package verdict (accepting Codex's)

WP-A not sound until sleep-service + pacer + input-latch + control snapshots land. WP-B partially
sound (the raw credit-lifetime invariant holds; incomplete for renderer metadata / DUP-NONE /
multi-callback / last-presented / OOM). WP-C(enable) sound only as a guarded bring-up; depth-1 stays
the default. New WP-C(commands)/WP-D/WP-E/WP-F/WP-G are the added surface.

**Next process step:** iterate this to zero findings with a second Codex pass (as the v2.4 contract
did over 5 rounds) BEFORE writing code; the measurement A/B remains gated on Dan's save states.

---

## What is already reviewed vs. what is new (where to aim the review)  *(rev-1, superseded above)*

- **Already at zero findings** (do not re-review): the v2.4 ownership contract (5 Codex rounds),
  the framering protocol, and the frontend_core lifecycle engine. Depth-1 rode entirely on that
  surface and came up clean. The sanitizer harness (`run-frontend-core-tests.sh {plain,tsan,asan}`)
  tests the shipping engine, not a model.
- **New, un-reviewed surface = this doc**: the concrete **minarch** integration. Three work
  packages (WP-A input, WP-B frame, WP-C enable). All hazards live here, in the glue.

Key framing fact that shrinks the risk: **most of the depth-2 machinery already exists in-tree.**
- The framering already carries a **depth-sized** per-credit input snapshot (`snap[FR_MAX_DEPTH]`,
  lifetime grant→credit-return, framering.h:113) and a slot per grant (`grantq[].slot`). The F20
  "one snapshot slot per credit" design is **already implemented** — WP2 just delivers it to
  `run()`. No storage change.
- The core-touching shortcuts (save/load/reset) map onto service ops that **already exist**:
  `FC_OP_SERIALIZE` / `FC_OP_UNSERIALIZE` / `FC_OP_RESET` via `fc_menu_op` (park→service→release).
- Frame double-buffering already exists as a proven pattern: the dormant `thread_video` mailbox
  (`backbuffer` copy + `readybuffer`/`presentbuffer` swap, minarch.c:3307–3331, 5501–5532). Depth-2
  reuses the **copy-out-of-the-transient-core-framebuffer** idea, sized to credits instead of a
  latest-wins mailbox.

Under a guard-ON build, `ZERO_DISABLE_FRONTEND_THREADING` forces `thread_video=0` and locks the
Threading option (minarch.c:5433–5440), so the old mailbox path is **dormant, not deleted** — a
clean reference, zero interference.

---

## The depth-1 → depth-2 gap, precisely

Depth-1 is a **serial rendezvous**: `fc_pump` blocks for the epoch's RUN_DONE (D59), so MAIN is
idle while the CORE thread runs `core.run()` — including `input_poll_callback`, which today runs
**on the CORE thread**. Because there is no overlap, two shortcuts are safe *only serially*:

1. **Input** — `input_poll_callback` (minarch.c:1955) reads live globals (`buttons`, `pad`) and
   the CORE reads them back in `input_state_callback` (2085) during the same `core.run()`. Safe
   because MAIN never touches them concurrently.
2. **Frame** — the CORE-side `video_refresh` sets `renderer.src = data` (the core's *transient*
   internal framebuffer) and emits; MAIN's drain reads `renderer.src` and scales. Safe because the
   core cannot start the next epoch (and overwrite `data`) until MAIN releases the credit.

Depth-2 overlaps MAIN and CORE by design (CORE runs N+1 while MAIN presents N). Both shortcuts
then race. The three work packages remove each race.

---

## WP-A — move the input tick to MAIN; snapshot per epoch; route core ops through the service channel

**Problem.** `input_poll_callback` does far more than read buttons, and it currently runs on CORE.
Enumerated (minarch.c:1955–2084), with the depth-2 disposition of each:

| Work in `input_poll_callback` today | Depth-2 home | Why |
|---|---|---|
| `PAD_poll()` | **MAIN** | reads SDL/evdev — input source is MAIN-affine |
| `PWR_update(...)` (sleep, menu-before/after-sleep) | **MAIN** | power/sleep + SDL |
| MENU/PLUS/MINUS → `ignore_menu` | **MAIN** | menu state MAIN reads |
| BTN_POWER → `toggle_thread`/`was_threaded` | **MAIN** (dead under guard-ON: `thread_video==0`) | keep for parity |
| FF toggle / HOLD_FF → `setFastForward` | **MAIN** | `fast_forward` + audio lifecycle are MAIN's |
| CYCLE_SCALE / CYCLE_EFFECT → `Config_syncFrontend` | **MAIN** | frontend/display config |
| `SAVE_STATE` / `LOAD_STATE` → `Menu_save/loadState` | **CORE via `fc_menu_op(SERIALIZE/UNSERIALIZE)`** | calls `core.serialize/unserialize` — F23: only CORE enters the core |
| `RESET_GAME` → `core.reset()` | **CORE via `fc_menu_op(RESET)`** | enters the core |
| `SAVE_QUIT` → `Menu_saveState` + `quit=1` | **CORE service** (save) then MAIN sets quit | save enters the core |
| MENU release → `show_menu = 1` | **MAIN** | `show_menu` MAIN reads |
| compute `buttons` + read `pad.laxis/raxis` | **MAIN → the per-epoch snapshot** | this IS the input handed to the epoch |

**Design.**
- Extract the body of `input_poll_callback` into `input_tick_main()` and call it on MAIN, once per
  loop iteration, **before** the grant. The registered libretro `input_poll_callback` becomes a
  **no-op when `zero_ftv2_running && zero_ftv2_on_core`** (MAIN already did the work), else
  unchanged (guard-OFF and menu-redraw paths keep today's behavior).
- After `input_tick_main()`, MAIN packs the epoch input into the 4-word snapshot and passes it to
  `fc_pump` (replacing today's `snap[4]={0}`), e.g. `snap[0]=buttons`, `snap[1]=(laxis.x<<16)|
  (uint16)laxis.y`, `snap[2]=raxis`, `snap[3]` reserved. `fr_grant` stores it in the credit's slot
  (already depth-sized).
- `input_state_callback` (minarch.c:2085), when `zero_ftv2_running && zero_ftv2_on_core`, reads the
  **delivered snapshot** (WP2: `run(void*, const uint64_t snap[4])`) instead of the live globals.
  Mechanism: stash the delivered snapshot in a CORE-TLS (`static __thread uint64_t
  zero_ftv2_isnap[4]`) inside `zero_ftv2_run`, unpack in `input_state_callback`.
- The three core-touching shortcuts, detected on MAIN, issue `fc_menu_op` (park → service on CORE →
  release). This reuses the exact path menu save/load already takes. `SAVE_QUIT` = service-save then
  `quit=1`.

**What this costs / accepts.** Input is now `depth` epochs stale (~16 ms at depth 2) — **stated and
accepted** by the v2.4 F20 contract. Save/load/reset **park the pipeline** (drain + refill hitch),
identical to a menu visit — acceptable and already how menu ops behave.

**Edit sites:** `input_poll_callback` (split), `input_state_callback` (snapshot read), the run loop
at minarch.c:5485+ (call `input_tick_main`, pack snapshot, route shortcuts), `zero_ftv2_run` (stash
`isnap`).

---

## WP-B — per-credit frame buffers (stop reading the core's transient framebuffer)

**Problem.** At depth-2 the CORE overwrites its internal framebuffer (`data`) on epoch N+1 while
MAIN is still scaling frame N from `renderer.src = data`. Tearing / use-after-overwrite.

**Design (credit-indexed pool, reusing the mailbox copy pattern):**
- The CORE owns a pool of **`depth`** RGB565 buffers (allocate/resize with the existing
  `backbuffer` logic, minarch.c:3309–3318). In the CORE-side `video_refresh` (after the HUD blit,
  which must stay on the producing thread since it writes into the frame), `memcpy` `data →
  pool[gen % depth]`, then `fc_emit_frame(&zero_ftv2, gen % depth)` — the previously-unused `payload`
  carries the slot.
- MAIN's drain, on `FR_EV_FRAME`, sets `renderer.src = pool[ev->payload]` (or `pool[ev->seq %
  depth]` — `ev->seq` already carries the epoch gen, framering.h:59), then `GFX_blitRenderer` +
  `GFX_flip`. The scaler's destination is `screen->pixels`, **MAIN-owned and single** — no
  double-buffer needed there; only the raw pool needs slots.
- Credit accounting bounds outstanding epochs to `depth`, so `pool[gen % depth]` is never reused
  while MAIN still needs it (same invariant that protects `snap[]`).
- The CORE needs its epoch gen to index the pool. `fr_core_wait_grant` returns `gen`; stash it in a
  CORE-TLS (`zero_ftv2_slot`) in `zero_ftv2_run` alongside `isnap`. **Open question for review:**
  prefer passing `gen` into the `run()` vtable signature vs. a CORE-TLS stash (see hazards).

**Edit sites:** `video_refresh_callback_main` (pool copy + emit slot, minarch.c:3196–3303),
`zero_ftv2_drain` (read `pool[slot]`, minarch.c:5286), pool alloc/resize, `zero_ftv2_run` (stash
slot).

---

## WP-C — enable depth 2 (fixed first, auto-trial later)

- **Stage 1 (this bring-up):** force depth via env `ZERO_FTV2_DEPTH` (default 1), read in
  `fc_init(&zero_ftv2, &zero_ftv2_vt, depth)`. `fc_pump` at depth ≥ 2 already keeps
  grant+prefix-drain+return (no block — D59). This validates **correctness** of the pipeline
  (picture/sound/input) before any decision logic.
- **Stage 2 (later, separate):** wire the observe→trial→verdict→sidecar machinery onto
  `fc_set_depth` (the transition gate reclaims credits safely, F24). The design's requalification
  gate (first 60 frames after load run depth 1) belongs here, not in Stage 1.

Non-goals for depth-2: user-facing toggle, depth > 2, auto-trial in Stage 1 (v2.4 §out-of-scope).

---

## Open hazards / questions for the reviewer

1. **`gen` to the CORE video_refresh** — CORE-TLS stash vs. extending the `run()` signature to
   `run(void* c, uint64_t gen, const uint64_t snap[4])`. TLS is less churn; explicit is cleaner.
   Which?
2. **`downsample` global `buffer`** (minarch.c:3281–3284) — the downsample path writes a *shared*
   global and points `renderer.src` at it. At depth-2 this races. Fix: downsample into `pool[slot]`
   (fold into the WP-B copy) or per-slot downsample buffers. Confirm no other reader of `buffer`.
3. **HUD blit reads governor globals** (`gov_state`, `cpu_double`, minarch.c:3272) on the CORE
   thread while MAIN's governor writes them — benign text-only races (garbled digit at worst).
   Accept, or snapshot the HUD inputs at grant?
4. **`fast_forward`** — written on MAIN (FF toggle), read on CORE in `audio_sample_callback`
   (minarch.c:3337) to gate audio emission. Torn read = one frame's audio wrongly emitted/dropped
   on an FF flip. Accept, or carry FF in the per-epoch snapshot?
5. **`last_flip_time`** (minarch.c:3302) and other callback-written globals — audit thread owner
   under depth-2 (which thread's clock drives pacing/governor).
6. **Audio ownership at depth-2** — `audio_sample_batch_callback` still emits from CORE (unchanged
   from depth-1); confirm the SND ring + F5 audio-ownership contract hold when CORE runs ahead of
   MAIN's present by up to `depth` epochs (audio-vs-video skew ≤ depth epochs).
7. **Menu/park during a pipelined epoch** — `fc_park` (loop at minarch.c:5548) must reclaim *all*
   outstanding credits and drain the pool; verify no in-flight `pool[slot]` is presented after
   park. (Gate F24 should cover this; confirm against the pool.)

---

## Validation plan

1. **Harness first (no device):** extend `frontend_core_test.c` to run adversarial sessions at
   **depth 2** with a frame-pool + snapshot oracle: assert MAIN presents `pool[N]`'s content while
   CORE produces N+1 (no overwrite), and that each epoch's `input_state` reads its own snapshot
   (extends INV16 to the pipelined case). Plain/TSan/ASan all green — TSan is the real gate for the
   new overlap.
2. **On-device bring-up (with Dan):** `ZERO_FTV2_DEPTH=2`, one clean single-process instance
   (reboot first — there are currently stacked `minui.elf` procs). Verify per game: picture, sound,
   **input responsiveness** (the ~16 ms lag is expected; confirm playable), menu in/out, save/load/
   reset, FF. SNES first (the beneficiary).
3. **The measurement (gated on Dan's save states):** depth-1 vs depth-2 energy A/B at demanding
   scenes — DKC (mine-cart/boss), Yoshi's Island (Super-FX2 rotate/scale boss), Mario RPG (SA-1
   battle). Save states do not yet exist and **block this step**; the code above is validatable to
   "runs correctly pipelined" without them.

## One-line summary for the reviewer

Depth-2 = (A) move the input tick to MAIN + per-epoch snapshot + route save/load/reset through the
existing service ops; (B) copy each frame into a credit-indexed pool so MAIN never reads the core's
transient buffer; (C) flip depth to 2 (fixed for bring-up, auto-trial later). Almost every
primitive already exists and is reviewed; the risk is entirely in the minarch glue enumerated here.

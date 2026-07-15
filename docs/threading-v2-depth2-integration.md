# Threading v2 — depth-2 integration design (the Codex review artifact)

**Status:** design, pre-implementation. This is the plan to hand Codex before any code or
device time. Depth-1 is COMPLETE and on-device-validated (D52–D59); WP2 (the `run()` snapshot-
delivery contract) is committed (D60). This doc covers the **remaining** work: turning the serial
depth-1 rendezvous into the depth-2 **pipeline** that is the actual energy win.

## What is already reviewed vs. what is new (where to aim the review)

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

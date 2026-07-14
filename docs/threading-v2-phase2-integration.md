# Threading v2 — phase 2 minarch integration map

Wiring plan from `framering.{c,h}` (sanitizer-clean, phase 1) into `minarch.c`, per the
v2.4 contract (`docs/threading-v2-design.md`). This is the call-site map so phase-2 coding
is mechanical. Nothing here changes minarch yet — it's the blueprint.

## Thread split
- **CORE thread** (new): owns the whole libretro session from `dlopen` per F23. Its loop:
  `fr_core_wait_grant` → `core.run()` (one epoch) → CORE-side callbacks call
  `fr_core_emit` → `fr_core_run_done`. Between grants, when parked, it services via
  `fr_core_service_next` / `fr_core_service_emit` / `fr_core_service_ack`.
- **MAIN thread**: the existing run loop becomes the grant/drain driver:
  `fr_grant` on the pacer cadence → `fr_drain` per tick (applies commands, presents frames).

## Callback → framering map (all run ON the CORE thread)
| minarch site | framering call | notes |
|---|---|---|
| `video_refresh_callback` (3245) | `fr_core_emit(KIND_FRAME, persistent?0, slot)` | frame pixels into the granted snapshot slot; NULL data → emit DUP; multiple/none per run handled by RUN_DONE outcome |
| `audio_sample_batch_callback` (3281) | producer side of the audio ring (unchanged SPSC) + config-barrier check | audio ring stays the existing SND SPSC; AV_INFO barrier gates format switch |
| `environment_callback` SET_GEOMETRY | `fr_core_emit(KIND_CMD_GEOMETRY, PERSISTENT)` | persistent = never discarded (F29) |
| `environment_callback` SET_SYSTEM_AV_INFO | `fr_core_emit(KIND_CMD_AVINFO, PERSISTENT)` then `fr_core_barrier_wait` | the barrier: MAIN prefix-drains + applies, wakes CORE (F29) |
| `environment_callback` SET_VARIABLE | `fr_core_emit(KIND_CMD_SETVAR, PERSISTENT)` | core→MAIN option-def change |
| `environment_callback` GET_VARIABLE / input_state | read published snapshot (no emit) | immediate-answer channels: per-epoch snapshot passed via `fr_grant`'s `snapshot[4]` (F20/F30) |

## Bootstrap → service ops (F31 nine-state machine, all on CORE)
Run each as an `fr_service(op)` from MAIN while CORE is QUIESCENT, MAIN draining products:
`CORE_CREATED → get_system_info → set_environment/register → retro_init → content-open →
load_game → get_system_av_info → set_controller → SRAM/RTC size+data+load → arm crash
handler (only here, MEMORY_READY) → audio init → renderer init → auto-resume unserialize →
first fr_grant (RUNNING)`. Per-state failure cleanup = the F31 table (the fake-core oracle).

## Lifecycle → framering map (MAIN)
| minarch event | framering call |
|---|---|
| pacer tick (GFX_sync cadence) | `fr_grant` (if credit) + `fr_drain(APPLY)` |
| menu enter (`Menu_loop` 1906-area) | `fr_park(drain=present-pending)` → menu draws via GFX_flip (unskippable, already so) → on exit `fr_release` |
| `Menu_beforeSleep` (3541) | `fr_park(drain)` then existing SRAM/RTC flush runs as a service op |
| FF toggle | `fr_park(discard)` → `fr_release` (payload discarded, persistent events kept — F29) |
| trial start/stop (#6) | `fr_set_depth(2)` / `fr_set_depth(1)` — the 7-step credit-reclaim gate is inside |
| `State_read`/`State_write`/`Core_reset` (636/681/3416) | `fr_service(SERIALIZE|UNSERIALIZE|RESET)` — executed on CORE per F18 |
| quit (below `finish:`) | `fr_park` → CORE runs unload_game+deinit as service → `fr_stop` → join → dlclose on MAIN |

## Reuses phase-1 correctness (no re-derivation)
- Single mode = `fr_init(depth=1)`; threaded trial = `fr_set_depth(2)`. Same code path.
- Drain indices are NOT contiguous (discard/abort skip indices) — the drain callback must
  not assume gap-free (phase-1 note).
- Lifecycle transitions are lock-serialized inside framering (phase-1 note) — MAIN just
  calls park/release/set_depth; no external locking needed.

## Verification gate before ship (from the v2.4 close + phase-1 report)
1. Adversarial libretro-shaped fake core exercising every F31 bootstrap-failure row.
2. Forbidden-globals audit (`nm`/`objdump`: no cross-thread mutable globals bypass the ring).
3. Release-binary TLS inspection: reject any `__tls_get_addr` on the crash-marker path.
4. On-device gauntlet (the expanded case list) at `-O3 -flto`.
5. Serial-v2 vs v1.3-single energy regression gate; threaded vs serial total-device evidence.

## Sequencing (unchanged)
Task #11's pipeline profile is DONE (floor-control insight); the single-thread baseline that
threading trials measure against will shift again if floor-control ships — recalibrate the
trial threshold after floor-control's ship decision, before threading's final measurements.
Dan's save states (DKC/Yoshi/Mario RPG demanding scenes) remain the measurement arms.

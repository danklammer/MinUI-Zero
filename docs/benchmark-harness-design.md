# Benchmark / telemetry harness — make "measurement-first" executable

`docs/project-direction.md` mandates measurement before changing CPU/render/audio policy, with
explicit acceptance gates. Today that's manual. This harness makes it a command: run a core
through a fixed scene, capture the numbers, and compare runs. The north-star metric is
**energy per correctly-presented frame (mJ/frame)** — see project-direction §"reframe".

## What it measures
Per gameplay frame (cheap, no extra thread — reuses the run loop):
- **frame work time** (µs): how long the CPU took before the vsync wait — the headroom signal.
- **frame interval** (µs): wall time between presents — judder/hitch signal.
Sampled slowly (~1 Hz, also in the loop, no thread):
- **temp** (thermal zone, °C), **cur freq** (`scaling_cur_freq`), **battery** (`voltage_now` ×
  `current_now` → mW). All via sysfs; absent off-device → columns blank.

## Pipeline
```
[minarch + telemetry.c]  --BENCH=1-->  run.csv   (one row per ~1s sample window)
        |                                   |
   on-device run (bench-run.sh)        tools/bench-analyze.py
                                            |
                                   metrics + A/B comparison table
                                   (p50/p95/p99 work & interval, dropped frames,
                                    thermal slope °C/min, mean mW, mJ/frame)
```

## Components
- `workspace/all/common/telemetry.{c,h}` — ring buffer of frame times + periodic sysfs sampler,
  CSV writer. **Off by default**; enabled only when `BENCH=1` (zero cost otherwise — the hot
  path is a single branch + array store). Pure stats helpers (`tlm_percentiles`,
  `tlm_mj_per_frame`) are I/O-free and unit-tested.
- `workspace/all/common/api.c` — expose the per-frame work time the governor signal already
  measures: `GFX_getFrameWorkUs()` (µs from `GFX_startFrame` to `GFX_flip`).
- `workspace/all/minarch/minarch.c` — `tlm_init()` at game load, `tlm_frame(GFX_getFrameWorkUs())`
  each gameplay frame, `tlm_quit()` on exit. All under `tlm_enabled()`.
- `tools/bench-analyze.py` — host-side: parse one or more `run.csv` → metrics; diff two runs
  for an A/B verdict against the project's release gates (no regressions in pacing/audio/speed,
  equal-or-lower mJ/frame).
- `tools/bench-run.sh` — on-device driver: launch `<core> <rom>` with `BENCH=1` for a fixed
  duration, copy out the CSV. (Deterministic-scene capture is on-device work; the script is the
  skeleton.)

## Constraints honored (project-direction §cautions)
- **No high-frequency monitoring thread** — frame data comes from the existing loop; sysfs is
  sampled once per ~1s, inline.
- **No per-frame production logging** — telemetry is gated by `BENCH`; release builds pay one
  branch.
- **Preallocated** ring (no allocation in the hot path).

## Buildable now vs on-device
- **Now:** telemetry.c compiles standalone; `telemetry_test.c` validates the percentile +
  energy math under ASan; `bench-analyze.py` validated against a synthetic CSV; minarch/api
  integration cross-compiles for tg5040.
- **On-device:** real temp/freq/battery values, deterministic scenes, the actual A/B runs and
  thermal-soak matrix.

## Decisions
- **D1 — µs frame timing** via `getMicroseconds()` (utils.c), not `SDL_GetTicks` ms — 1 ms is
  too coarse for p99 against a 16.7 ms budget.
- **D2 — CSV, not a binary format** — greppable, diffable, trivially parsed; rows are ~1 Hz so
  size is tiny.
- **D3 — energy/frame is the headline**, but always reported alongside frame-time percentiles
  and thermals so a "cooler but janky" regression can't hide behind a good average.
- **D4 — power = `voltage_now × current_now`** from `/sys/class/power_supply/*`; if `current_now`
  is absent (some PMICs), fall back to discharge-rate estimation and flag it as lower-confidence.

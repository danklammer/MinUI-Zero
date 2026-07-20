# Status — MinUI Zero (shipped + on-device validated)

Branch: `integration`. Build: **MinUI-20260701-2**. Date: 2026-07-01.

A large batch of work shipped this session and was **validated on a real TrimUI Brick** — the
device boots, runs the launcher, plays games, sleeps and resumes cleanly, and measures cooler
and leaner than stock. What follows is the current state, not a plan. Facts below are measured
on-device unless marked otherwise. See `docs/DECISIONS.md` for the reasoning behind each call.

## Shipped + validated this session

### GPU-dark menu (the headline efficiency win)
Software present straight to `/dev/fb0` (RGB565 → XRGB8888, native 1024×768) — `PLAT_flipFB` in
`workspace/tg5040/platform/platform.c`, env `ZERO_FB_PRESENT`, **on by default** in
`MinUI.pak/launch.sh`. With no GL client holding it, the **PowerVR GPU power domain suspends
while at the menu**. On-device: **26°C, owner-confirmed "super fast."** This is the one
efficiency edge NextUI (fully GPU-based UI) structurally can't copy.

### Deep sleep (suspend-to-RAM) — enabled
Hybrid faux-sleep → suspend-to-RAM, ported from `zhaofengli/MinUI` (see `THIRD_PARTY_NOTICES.md`).
**Validated on-device (33 → 27°C, clean resume) and enabled** via the opt-in `enable-deep-sleep`
flag in `.userdata/shared`. The `bin/suspend` choreography quiesces radios and saves the ALSA
mixer so the kernel accepts `mem` and audio comes back clean.

### Closed-loop governor (hybrid ceiling + schedutil)
The frame-aware controller sets a `scaling_max_freq` **ceiling**; the kernel **`schedutil`**
governor picks the instantaneous frequency beneath it. Measured OPP values applied. The hardware
still idles at **408 MHz**; low-end systems retain a **1008 MHz** ceiling so short GLES present
bursts are not clipped, while PS1 rides **~1416–1800 MHz**. The system runs
**~4–5°C cooler than stock**. Key on-device lesson (**D14, race-to-idle**): capping the ceiling
*too low* runs the CPU **warmer**, because it forces ~100% util instead of letting schedutil
finish-the-frame-and-idle (WFI). The ceiling caps runaway spikes; it must not drive schedutil
below the clock where it can race to idle.

### Radios + LEDs dark by default
`boot.sh` kills Bluetooth and gates Wi-Fi on the dev-only `enable-ssh` flag (MinUI has no
networking, so a normal install runs radio-dark). Ambient LEDs are zeroed every boot. These are
continuous idle-power rails NextUI's feature set keeps lit.

### Correctness / QoL (crash + pacing pillars)
- **#4 — bail cleanly on failed `core.load_game`** (`minarch.c`): a bad/unsupported ROM used to
  run an unloaded core → hang / black-screen needing a hard power-off. Now captured and bailed via
  the existing `finish:` path.
- **#6 — handle `SET_SYSTEM_AV_INFO` / `SET_GEOMETRY`** env calls (`minarch.c`): cores that change
  fps/res/aspect mid-run no longer desync pacing + the scaler.

### Build efficiency
- **-O3 pinned cores** — 6 stock cores build at their intended `-O3`/`-Ofast` (was silently `-O2`
  via last-`-O`-wins Makefile merge), pinned to reproducible core HEADs.
- **Drift-free absolute-schedule frame pacer.**

## Measured numbers (first real device data)
- **Battery: ~6h on Game Boy** — first energy measurement, via `charge_counter` (4% / 15 min).
  `current_now` is dead on the AXP2202; `charge_counter` works and is the meter.
- **CPU OPP floor = 408 MHz** (measured; no lower step exists). A sub-408 "sleep clock" is
  therefore impossible — schedutil already idles there, so there's nothing lower to drop to.
- **Governor thermals: ~4–5°C cooler than stock**; converged with NextUI (both `schedutil`
  408–1800).

## Tested → shelved: GPU-dark games
Built a software-scale-to-fb0 game present (`ZERO_FB_GAME`, `PLAT_flipFB_game`, commit `33b5e6e`).
It **renders correctly and the GPU domain does suspend during play**; a row-caching optimization
(scale each source row once, `memcpy` to identical destination rows) got it **smooth at 72% CPU**.
But a clean drain A/B with the GPU verified suspended the whole window came out **exact break-even
— ~6h, identical to GLES** — because the software-scale CPU cost precisely offsets the GPU power
saved (also only ~1–2°C cooler). **Decision: games keep GLES; GPU-dark is menu-only;
`ZERO_FB_GAME` is off by default.** The only path to a real games win is the **DE hardware scaler**
(`/dev/disp` layer — no GPU *and* no CPU scale), which probed as unavailable on this kernel (see
`no-gl-present-proposal.md`) and is a research project, not shipped work. *Measured, not assumed.*

## Where the comparison landed
- **vs NextUI:** a governor tie — their `auto` *is* `schedutil` 408–1800, the same mechanism, ~5–10°C
  cooler than old userspace (we reproduce ~4–5°C on our own fork). Our edges are per-system caps,
  never exposing the 2.0 GHz OC, radios/LEDs/GPU dark, and the GPU-dark menu they can't copy.
- **vs MyMinUI:** the lean-software-render peer (NEON + double-buffer, no GL) — our reference for
  the fb0 present path.

## What's next
- **DE hardware-scaler research** (`/dev/disp` layer) — the only route to a GPU-dark *games* win
  without paying the CPU software-scale cost. Probed unavailable on the stock kernel via the legacy
  fbdev scanout path; would require becoming DRM master + raw ioctls. Scoped follow-up, not cheap.
- **Backlight** is a limited lever (small, bounded).
- Longer thermal-soak / battery A/B under sustained load in a warmer environment (bench ambient
  keeps absolute temps in a narrow band; the efficiency/battery story is bigger than the °C delta).

See `docs/zero-efficiency-roadmap.md` for the full lever list and `docs/nextui-comparison.md` for
the head-to-head detail.

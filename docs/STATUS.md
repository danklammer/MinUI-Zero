# Status — closed-loop thermal/perf governor

Branch: `feat/thermal-governor` (off `main`, which is untouched).
As of this writing the whole no-hardware task list is implemented, verified where it
can be verified without the Brick, and committed. Date: 2026-06-30.

## What the governor does
Replaces MinUI's static 4-tier CPU pick with a feedback controller: during gameplay it
runs each system at the lowest CPU clock that still holds frame rate, capped by a
conservative thermal ceiling. Reads frame-slip + temperature every ~30 frames, climbs
fast on slip, sinks slowly on slack, and always backs off above the ceiling. Stays on
MinUI's lean software render path — no GL, no new features.

## Done + committed
| Task | State | Commits |
|------|-------|---------|
| 1. `PLAT_setCPUFreq(int khz)` (tg5040 writes `scaling_setspeed`; macOS logs; weak no-op fallback) + `GFX_didOverrun()` | done, builds clean | `088a525` |
| 2. `gov_step`/`gov_tick` + per-system profiles + synthetic harness | done, tests green | `c4d43cd` |
| 3. Wired `gov_tick` into the minarch run loop (~30 frames) + game-load profile pick | done, type-checked | `359983b` |
| 4. Per-system `f_min`/`f_max` exported from 18 tg5040 pak `launch.sh` | done, `sh -n` clean | `9e91c67` |
| 5. Standalone synthetic harness, in the build + green | done | `c4d43cd`, `45d65fb` |
| Decisions log | running | `24ce9aa` + updates |

Key files:
- `workspace/all/common/governor.{c,h}` — controller (pure `gov_step` + I/O `gov_tick`).
- `workspace/all/common/governor_test.c` — synthetic harness (5 scenarios).
- `workspace/all/common/run-governor-tests.sh` + `make test-governor` — committed runner.
- `workspace/all/common/api.{c,h}` — `PLAT_setCPUFreq` decl/weak fallback, `GFX_didOverrun`.
- `workspace/tg5040/platform/platform.c`, `workspace/macos/platform/platform.c` — `PLAT_setCPUFreq`.
- `workspace/all/minarch/minarch.c` — `Gov_start()` + run-loop tick.
- `skeleton/**/tg5040/**/*.pak/launch.sh` — `MINARCH_FMIN`/`FMAX` exports.
- `docs/DECISIONS.md` — every non-obvious choice (D1–D11).

## How it was verified (no hardware)
- **macOS dummy-platform launcher build** under AddressSanitizer — green
  (`api.c` / `macos` `platform.c` changes). Needs `-U__ARM_ARCH` locally on Apple
  Silicon to dodge a *pre-existing* 32-bit ARM-asm blend path in `api.c`; see D8. The
  stock build command is unchanged and works on the Linux toolchain / Intel Macs.
- **Synthetic harness** (`make test-governor`) under ASan — green. Asserts: clock never
  leaves `[f_min,f_max]`; a hot trace (≥ceiling) walks the clock down monotonically and
  never up; cold/idle sinks to `f_min`; a real workload converges to the lowest *stable*
  clock that holds frame rate (settles at/above the requirement but below `f_max`);
  alternating slip/slack does not hunt low; `gov_tick`'s I/O path writes in-bracket.
- **Integration type-check** (`.notes/integration_check.c`, host `cc` + ASan) — green.
  Compiles the exact `minarch.c` governor code against the real `governor.h`.
- **tg5040 cross-compile** — green. Built `minarch` (incl. `governor.c`) under the real
  `aarch64-linux-gnu-gcc` toolchain (`tg5040-toolchain` Docker image via Colima, `-flto -Os`,
  zero warnings) → `workspace/all/minarch/build/tg5040/minarch.elf`, a valid ARM aarch64 ELF.
- **launch.sh** — all 18 pass `sh -n`; exports land in scope just before `minarch.elf`.

## Blocked on the Brick — see docs/ON-DEVICE-CHECKLIST.md
- **Replace ASSUMED hardware values** with `tools/brick-recon.sh` output: thermal-zone path
  (`GOV_T_SENSOR`), OPP ladder / `GOV_STEP_KHZ`, cluster-wide policy confirmation.
- **Behavioral tuning** on real silicon: convergence/no-hunting, `GOV_T_TARGET_C`/dwell,
  the sub-60 Hz `FRAME_BUDGET` caveat (D5), and per-core `f_max` for MGBA/SGB/SUPA (D11).
- **Confirm** `GOV_DISABLE=1` disable hatch and the governor↔static-menu hand-off (D6).

None of the above changes the controller's structure — only better numbers in the
`#define`s and the `launch.sh` brackets, exactly as the design doc anticipated.

## Notes for the next session
- `.notes/` (gitignored) holds local-only scratch: `verify.sh` (full build+test),
  `integration_check.c`, `wire-launch.sh` (the launch.sh editor). Not part of the repo.
- Nothing here touched the render path or added features (CLAUDE.md non-negotiables).
- No on-device flashing or thermal/perf claims were made — that work is deferred to the
  checklist, not faked.

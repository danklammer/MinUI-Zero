# Architecture map: MinUI base ↔ MyMinUI ↔ NextUI (tg5040)

All three are MinUI-lineage. Branch `main` on each. Line numbers are from current source and
**approximate — grep the symbol, don't trust the number.** NextUI split MinUI's single
`minarch.c` into `ma_*` modules, so its pairings are MinUI(one file) → NextUI(a module).

## Licensing (decide before copying anything)
| Repo | License | Implication |
|------|---------|-------------|
| `shauninman/MinUI` (base) | **none** (all rights reserved) | Tolerated fork scene; keep attribution, don't relicense the repo. |
| `LoveRetro/NextUI` | **GPLv3** | Copying its code → our distributed fork is GPLv3 (full source, no closed dist). Reimplement the *technique* to stay non-GPL. |
| `Turro75/MyMinUI` | **none** (no LICENSE file) | Same gray zone as MinUI, **but** contains GPLv3 code lifted from NextUI (cheats) → those bits are GPLv3 regardless. Mixed provenance. |

Ideas/techniques aren't copyrightable; specific code is. Default stance: **reference, then
reimplement.**

## 1. CPU frequency / thermal — the #1 heat lever
- **MinUI:** `workspace/tg5040/platform/platform.c` → `PLAT_setCPUSpeed()` (~L546). Uses the
  `userspace` governor, writes explicit kHz to `…/cpu0/cpufreq/scaling_setspeed`:
  **600 / 1200 / 1608 / 2000 MHz** (menu/powersave/normal/perf). Enum + `#define
  PWR_setCPUSpeed PLAT_setCPUSpeed` in `workspace/all/common/api.h` (~L298). Boot sets the
  governor + clock in `workspace/tg5040/install/boot.sh`. In-game speed menu in
  `workspace/all/minarch/minarch.c` (~L966).
- **NextUI:** same `PLAT_setCPUSpeed()` (~L288) but sets a **governor *mode* string** (`auto` /
  `performance` / `powersave`) — `CPU_SPEED_AUTO` hands off to a kernel `auto` governor instead
  of poking frequencies. Reads `scaling_cur_freq` + temp for an overlay. (Its auto path needed
  post-release fixes: auto-speed slowdowns; "stuck in performance on fresh install.")
- **MyMinUI:** same abstraction; `workspace/all/common/api.h` adds `CPU_SPEED_MAX` and
  `CPU_SPEED_SLEEP` to MinUI's tiers.
- **Our move:** keep MinUI's userspace + `scaling_setspeed` base (best for a closed-loop
  controller); borrow NextUI's idea of reading temp each tick. See `thermal-governor-design.md`.

## 2. Frame loop / pacing
- **MinUI:** run loop in `workspace/all/minarch/minarch.c`; pacing in `workspace/all/common/api.c`
  → `GFX_flip()` (~L208), `GFX_sync()`, `PLAT_vsync()` (frame-budget aware; `VSYNC_STRICT`).
- **NextUI:** `workspace/all/minarch/ma_runframe.c` (+ `ma_core.c`).
- **MyMinUI:** `workspace/all/common/api.c` adds `GFX_flip` / `GFX_flipNoFix` /
  `GFX_flip_fixed_rate(target_fps)` plus `MY_GetTicks()` timing.

## 3. Video / scaling — the lean-vs-GL decision
- **MinUI (software, RGB565) — KEEP THIS:** `workspace/all/common/scaler.c` / `scaler.h`,
  presented via `workspace/tg5040/platform/platform.c` → `PLAT_blitRenderer()` (~L367),
  `PLAT_flip()` (~L373); tearing/vsync hooks `PLAT_setVsync()` (~L163), `PLAT_vsync()` (~L357).
- **MyMinUI (optimized software path) — BEST REFERENCE:** a rewritten CPU rendering engine
  using NEON + multiple cores + double buffering, tear-free, **no GL.** Lives in
  `workspace/all/common/api.c` (the `GFX_flip*` family + pthreads) + `minarch.c` (NEON); uses
  `SDL_rotozoom` instead of MinUI's `scaler.c`. Caveat: its multicore blit maximizes speed —
  for minimal heat, gate multicore aggressiveness by system demand.
- **NextUI (full GLES shader pipeline) — DO NOT ADOPT:** `workspace/all/minarch/ma_video.c`
  (`GFX_GL_Swap()`, shader passes) + `workspace/all/common/generic_video.c` (GLES shader
  programs, multi-pass scale/effect/overlay, `.glsl`). Keeps the GPU lit all session.

## 4. Audio / resampler
- **MinUI:** SDL audio in `workspace/all/minarch/minarch.c` (+ `api.c` `SND_*`).
- **NextUI:** `workspace/all/minarch/ma_audio.c` — dedicated audio module, per-emulator
  quality/perf resampling (cites libsamplerate). Use a build *after* their resampler
  memory-leak fix.

## 5. Deep sleep / suspend — overheat-bug fix (high value, cleanest copy candidate)
- **Both:** `PWR_*` lives in `workspace/all/common/api.c`.
- **NextUI** added suspend-to-RAM ("suspending to RAM" / "returned from suspend", retry logic,
  battery-monitor thread, `can_autosleep`) around L3400–3710 (author: zhaofengli). Diff
  NextUI's `PWR_*` against MinUI's in the same file to isolate the delta. (GPLv3 if copied.)

## 6. Per-core patches
- **MinUI** already ships A133P core patches: `workspace/tg5040/cores/patches/` — gpsp, mgba,
  pcsx_rearmed, picodrive, snes9x2005_plus, fceumm, gambatte, mednafen_pce_fast,
  mednafen_supafaust, mednafen_vb, pokemini, race. Start core tuning from these.

## Workflow: diff in place
```
git remote add nextui https://github.com/LoveRetro/NextUI.git
git remote add mymin  https://github.com/Turro75/MyMinUI.git
git fetch --all --depth=1
git diff HEAD nextui/main -- workspace/all/common/api.c            # e.g. the PWR_*/sleep delta
git diff HEAD mymin/main  -- workspace/all/common/api.c            # the GFX_flip* render engine
```

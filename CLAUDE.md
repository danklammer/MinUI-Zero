# CLAUDE.md — performant/efficient MinUI fork for the TrimUI Brick

> Working title only — **no name chosen yet** (candidates: Brisk, Frost). This is a fork of
> `shauninman/MinUI`. Don't rename files/identifiers for branding yet.

## What this is
A fork of MinUI focused on **performance through efficiency** on the TrimUI Brick
(`tg5040` platform, Allwinner A133P). The thesis: run each emulated system at the **lowest
CPU clock that still holds its target frame rate**, so the device stays cool and sips power.
This is *not* a feature fork — it's the opposite. NextUI is the feature-rich/GL fork; this
one is the distilled, runs-cold one.

## North star / non-negotiables
- **Cool + efficient is the whole point.** Every change should serve "lowest clock that holds
  frame rate." If a change adds heat, idle power, or resident memory without earning it, don't.
- **Stay on MinUI's lean software (RGB565) render path.** Do **not** adopt NextUI's OpenGL/GLES
  engine — it keeps the GPU powered all session, which is directly against the thesis. Fix
  tearing the cheap way (page-flip + double-buffer / NEON scalers), referencing MyMinUI.
- **Stay minimal.** No box art, WiFi/NTP, Pak Store, RetroAchievements, ambient-LED modes,
  overlays, etc. Features are weight; weight is heat and idle drain.
- **Never fabricate device values.** Real CPU OPP steps and thermal-zone paths must come from
  the hardware (`tools/brick-recon.sh`). Until then, use the **clearly-labeled assumptions** in
  `docs/thermal-governor-design.md` — they're safe by construction, not guesses to hide.

## Hardware (target platform = `tg5040`)
- **SoC:** Allwinner A133P, quad-core Cortex-A53. MinUI's tg5040 code drives it via the
  `userspace` cpufreq governor, writing kHz to
  `/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed`.
- **Known reference clocks (read from tg5040 source, not assumed):** 600 / 1200 / 1608 / 2000
  MHz = menu / powersave / normal / performance. (2.0 GHz is a mild OC over the 1.8 GHz stock.)
- **GPU:** PowerVR GE8300 — irrelevant here (software render path).
- **Display:** 1024×768 IPS. **RAM:** 1 GB.
- The Brick and the TrimUI **Smart Pro** share the `tg5040` platform. (The plain Smart is
  `trimuismart` — different platform; ignore it.)

## Architecture — where things live (verify by grepping; line numbers are approximate)
- `workspace/all/minarch/minarch.c` — the libretro frontend + main run loop. **The governor
  tick goes here.** Already measures frame pacing around `GFX_flip` / `FRAME_BUDGET`.
- `workspace/all/common/api.c` — `GFX_flip` / `GFX_sync` / `PLAT_vsync` (frame pacing,
  `gfx.vsync = VSYNC_STRICT`); `PWR_*` (sleep/power); `SND_*` (audio).
- `workspace/all/common/scaler.c` / `scaler.h` — **our software RGB565 scalers.** This is the
  render path to keep/optimize.
- `workspace/all/common/api.h` — `CPU_SPEED_*` enum (~L298); `#define PWR_setCPUSpeed
  PLAT_setCPUSpeed`.
- `workspace/tg5040/platform/platform.c` — `PLAT_setCPUSpeed` (~L546, the freq write),
  `PLAT_blitRenderer` (~L367), `PLAT_flip` (~L373), `PLAT_setVsync`/`PLAT_vsync`.
- `workspace/tg5040/install/boot.sh` — sets `userspace` governor + clock at boot.
- `workspace/tg5040/cores/patches/` — per-core build patches (gpsp, mgba, pcsx_rearmed,
  picodrive, snes9x2005_plus, fceumm, gambatte, …). Start core tuning from these.
- `workspace/macos/` — **dummy desktop platform** (its own notes say so): builds the launcher
  on macOS under AddressSanitizer, no cores, stubbed input. Our zero-hardware playground for
  logic — useless for thermal/perf (no real clocks or heat).

Full three-way file map (incl. NextUI/MyMinUI equivalents): **`docs/architecture-map.md`**.

## Reference forks (git remotes — READ them, don't blindly copy)
Add as remotes; treat as reference implementations, **not** patch sources (NextUI modularized
`minarch.c` into `ma_*` modules, so almost nothing applies cleanly):
- `upstream` = `shauninman/MinUI` — the base. **No license file → all rights reserved.** Don't
  add a LICENSE over the repo; keep attribution; at most license our *own* new files.
- `mymin` = `Turro75/MyMinUI` — **best reference for the lean tear-free software render path**
  (NEON + multicore + double-buffer, no GL). No license file; contains GPLv3 bits lifted from
  NextUI, so its provenance is mixed.
- `nextui` = `LoveRetro/NextUI` — reference for the **suspend-to-RAM / deep-sleep overheat fix**
  and the dynamic-governor idea. **GPLv3** → pasting its code makes us GPLv3. To keep license
  options open, **reimplement the technique, don't copy the code.**

```
git remote add upstream https://github.com/shauninman/MinUI.git
git remote add nextui   https://github.com/LoveRetro/NextUI.git
git remote add mymin    https://github.com/Turro75/MyMinUI.git
git fetch --all
```

## Build & dev loop
**Zero-hardware (do this first — governor logic):** macOS dummy platform. Builds the launcher
under ASan; good for logic/crashes, *not* thermal/perf.
```bash
brew install sdl2 sdl2_image sdl2_ttf
cd workspace/all/minui && mkdir -p build/macos
gcc minui.c -o build/macos/minui -I. -I../common/ -I../../macos/platform/ \
  -I/opt/homebrew/include -L/opt/homebrew/lib \
  ../common/scaler.c ../common/utils.c ../common/api.c ../../macos/platform/platform.c \
  -DPLATFORM=\"macos\" -DUSE_SDL2 -Ofast -std=gnu99 \
  -ldl -lSDL2 -lSDL2_image -lSDL2_ttf -lpthread -lm -lz \
  -fsanitize=address -fno-common \
  -Wno-tautological-constant-out-of-range-compare -Wno-asm-operand-widths
./build/macos/minui
```
**Flashable Brick build (needs Docker running):**
```bash
make tg5040                 # builds tg5040 in MinUI's docker toolchain; zip → ./releases/
make shell PLATFORM=tg5040  # drop into the toolchain container
```
**On-device iteration:** MinUI runs from SD + SSH. Iterate by `scp`-ing the rebuilt
`minarch.elf` / pak and relaunching — no reflashing per change.

## Current task — closed-loop thermal/perf governor
Replace MinUI's static 4-tier CPU pick with a feedback controller: run each system at the
lowest clock that holds frame rate, capped by a conservative thermal ceiling.
**Full design + usable C: `docs/thermal-governor-design.md`.** It is safe to build entirely on
assumptions because (1) writes to `scaling_setspeed` snap to the nearest valid OPP, (2) the
loop self-corrects bad brackets at runtime, (3) a conservative ceiling bounds the downside.

Steps:
1. Add a fine-grained freq write (`PLAT_setCPUFreq(khz)` → `scaling_setspeed`; keep the
   `userspace` governor that `boot.sh` already sets).
2. Add `gov_tick(profile, frame_overrun)` to `minarch.c`'s run loop (~every 30 frames), reusing
   minarch's existing frame-budget measurement for `frame_overrun` and reading temp from the
   thermal zone.
3. Per-system `f_min`/`f_max` profile, wired through the per-pak `launch.sh` (which already sets
   a CPU tier today).
4. **No-hardware validation:** build a synthetic harness that feeds fake temp + frame-slip
   traces into `gov_tick` and asserts it converges (compile it under the macOS path / ASan).
5. **On device later (~10 min, not a blocker):** run `tools/brick-recon.sh` idle and during a
   heavy game; replace the assumed OPP ladder + thermal-zone path with the real values.

## Working rules (these complement the global ~/.claude/CLAUDE.md)
- **Read before edit.** Open the actual file and grep the symbol first; the line numbers in
  these docs are approximate.
- **No fabrication.** Mark every hardware assumption; never present an assumed value as measured.
- **Scope discipline.** One subsystem per branch. Keep `main` clean tracking `upstream`; work on
  feature branches (e.g. `feat/thermal-governor`).
- **Stay lean, stay software-rendered** unless explicitly told otherwise in chat.
- **License hygiene.** If you ever reference `nextui`/`mymin` code, note the provenance and
  license in the commit message, and prefer reimplementing over copying.

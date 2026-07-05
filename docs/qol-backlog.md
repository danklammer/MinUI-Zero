# MinUI Zero — QoL Backlog

Curated from parallel surveys of **NextUI**, **MyMinUI**, **upstream MinUI**, and **zhaofengli**
(2026-07-01), filtered through Zero's rule: **refine the existing experience, never add
feature-bloat.** Every candidate was then verified against *our actual tree* (stock MinUI +
schedutil governor) — which is why several survey "wins" fall away: they refined features or
baselines our tree doesn't share. Not porting them **is** the thesis working.

## ✅ Implemented (Tier 1)
- **#4 — Bail on failed `core.load_game`** (`minarch.c`). Stock ignored the return value; a
  bad/unsupported ROM ran an unloaded core → hang / black-screen needing a hard power-off. Now
  captured and bailed cleanly via the existing `finish:` path (clearing `core.initialized` so
  teardown doesn't touch a game that isn't there). *Crash-resistance pillar.* — commit `60930c1`
- **#6 — Handle `SET_SYSTEM_AV_INFO`(32) / `SET_GEOMETRY`(37)** (`minarch.c` env callback). Cores
  that change fps/res/aspect mid-run (PAL/NTSC toggle, some arcade) previously got `return false`
  (= "frontend rejected it"), desyncing pacing + the scaler. Now re-reads fps/sample_rate/aspect
  and resets `dst_p` so the scaler re-inits. *Frame-pacing pillar.* Known limit: a sample-rate
  change **after** `SND_init` would still need an SND re-init — deferred until a real core needs
  it (during-load changes propagate into `SND_init` automatically). — commit `60930c1`

## 🔬 Measure-first (Tier 1, pending on-device data)
- **#2 — CPU sleep-clock during faux-sleep.** MyMinUI drops to ~240 MHz on sleep because they pin
  clocks with the `userspace` governor. We use **schedutil**, which *should already* idle the
  clock during faux-sleep's `SDL_Delay` wait loop. **Test:** read `scaling_cur_freq` while in
  faux-sleep. Already ~min OPP → redundant, skip. Stuck high → implement (needs a *verified* low
  OPP value — no fabrication).
- **#3 — Close audio device on sleep vs pause.** We already `SDL_PauseAudio(1)` on sleep
  (`api.c:1729`). MyMinUI's close-vs-pause saved ~7% CPU on *their* HW. **Test:** measure the
  audio-thread CPU during faux-sleep on the A133P. ~0% → redundant. Meaningful → rework
  `SND_init`/`SND_quit` to close/reopen (integrate with `feat/deep-sleep`).

## ❌ Rejected — already present, or would be bloat
- **#1 — Instant first-press in menus.** Already ours: `api.c:1393` sets `just_repeated` on the
  initial press, so the menu already moves on tap. MyMinUI needed it for their refactored PAD
  logic; stock doesn't.
- **#5 — Favorite-toggle without menu restart.** Stock MinUI has **no favorites feature** —
  adopting = *adding* a feature = bloat.

## 🗄 Tier 2 shelf (worth considering, not scheduled)
- **RASTATE save-state import** (NextUI) — load states exported from RetroArch/other devices.
- **Split/adjustable sleep timers** (MyMinUI) — reconcile with `feat/deep-sleep`.
- **Multi-disc m3u state handling** (MyMinUI) — untangle from their boxart code.
- **`CLOCK_MONOTONIC` timing** (MyMinUI) — complements our absolute-schedule pacer.
- **Per-game config re-read on option republish** (NextUI) — cheap correctness.
- **Wake-from-sleep shutdown fix** (MyMinUI) — reference for `feat/deep-sleep`.

## Sources exhausted
- **upstream/main** — frozen at our exact sync point (`dbf89435`); we're a 100% superset.
- **zhaofengli** — deep-sleep only; already ported *and* refined (dropped a redundant
  `asound.state.post` store in the suspend script).

## 🚫 Consciously NOT adopting (bloat — out of scope by design)
Box art / art scraping · WiFi / NTP / networking · Pak Store · RetroAchievements · ambient/LED ·
shaders / overlays · cheat browsers · theme engines · added emulators (NDS, DOSBox, MAME, …) ·
rewind · debug HUD · extra aspect ratios · multiplayer.

## 💡 Key insight
MyMinUI *raised* its menu clock ~2× on weak SoCs (miyoomini/my282) because ultra-low felt
sluggish. That floor does **not** apply to the Brick's far stronger A133P — our GPU-dark menu is
already "super fast" at 600 MHz (owner-confirmed). Menu responsiveness is a solved problem on our
hardware; don't spend watts chasing it.

## Charging screen polish (v1.2 candidate, from NextUI triage 2026-07-05)
Stock's charge-while-off screen is a bare icon. NextUI's battery experience looks nicer but
is built on batmon (background daemon) + sqlite history — off-thesis, declined. The lean
slice: a charging screen with the battery PERCENTAGE, read once per draw (no daemon, no
database, no idle cost). Investigate what draws the off-state charging screen on tg5040
(vendor init vs our launch path) before scoping.

## SP rumble verify (pending device wake)
Motor-voltage fix (1.5V before enable, per NextUI) shipped in -13; verify the power-off
haptic cue buzzes on the SP after the update. If still silent: check FN mute state, then
probe the motor sysfs directly.

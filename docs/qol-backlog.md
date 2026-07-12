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
- **No Sleep tool — DEV-ONLY** (Dan, final call 2026-07-11: not user-facing; users don't
  need it and the Tools menu stays minimal). One dev pak (successor of/same as "Stay Awake",
  they were always meant to be the same tool): blocks idle autosleep, handles vendor dimtime,
  keeps WiFi awake + net-keeper self-heal, persists via boot re-arm. Stays in
  workspace/tg5040/dev-tools/, never packaged. Optional nicety if ever cheap: coffee-cup
  status icon while active (dev-visible only — flag never set on user installs). Decision
  kept for the record: NO icon for deep-sleep-disabled regardless.
- **Zero-native HUD diagnostics** (idea from NextUI's debug HUD, `ma_video.c:689-722`; decided
  2026-07-10) — extend OUR existing HUD toggle with the fields that serve the closed-loop story:
  governor state (ceiling / last signal / sink+burst events), audio buffer fill vs target +
  underrun counter, frame pacing (avg/max ms, drops), CPU usage%/clock/temp. Not a port — our
  fields, our layout. Build alongside the FF-Audio work (it reopens the buffer machinery anyway).
  Rationale: would have cut days off the BR2 saga; turns "sounds choppy" reports into two numbers.

## Sources exhausted
- **upstream/main** — frozen at our exact sync point (`dbf89435`); we're a 100% superset.
- **zhaofengli** — deep-sleep only; already ported *and* refined (dropped a redundant
  `asound.state.post` store in the suspend script).

## 🚫 Consciously NOT adopting (bloat — out of scope by design)
Box art / art scraping · WiFi / NTP / networking · Pak Store · RetroAchievements · ambient/LED ·
shaders / overlays · cheat browsers · theme engines · added emulators (NDS, DOSBox, MAME, …) ·
rewind · extra aspect ratios · multiplayer. (debug HUD moved to backlog 2026-07-10 —
Zero-native diagnostics version, see above.)

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

## Multithreaded minarch (v1.3 research frontier) — EXPERIMENT DONE 2026-07-07
Stock MinUI already ships core/render threading: the "Prioritize Audio" option
(minarch_thread_video, default off) runs core.run() on its own thread. A/B on THPS2 attract
(Brick, HUD): threading WORKS (61/60 stable, 22% total CPU) but our governor mis-instruments
it — frame work is timed on the main thread, which in threaded mode includes waiting, so the
sink gate never opens: ceiling pinned 1800/33C vs 1008/30-31C single-thread. THE PROJECT:
thread-aware governor (measure emulation time on the core thread = the true critical path).
Prize estimate: 1-2 OPP steps on heavy games (PS1 gameplay ~1008-1152 instead of ~1584) +
audio-crackle insurance. Also fix: the core thread busy-spins when paused (should cond_wait).

## Rewind prior art
NextUI ships rewind (workspace/all/minarch/ma_rewind.c) — port-or-adapt candidate for the
core-features list; evaluate its RAM/CPU cost against the efficiency charter on small cores.

## Game open/exit latency (community report: johnnyq via GitHub, 2026-07-08)
Measured by reporter: ~3s ROM open, ~1.5s close. Miyoo Mini/Plus = the benchmark (near-instant
both ways). Two threads to pull:
1. INSTRUMENT the 3s first: pak shell overhead, core dlopen, EGL/GLES init, rom read, gov/uv
   init — find the fat slice before optimizing anything.
2. GPU-dark games REVIVAL, load-time angle: we shelved software-present on a battery tie
   (drain A/B = exact break-even vs GLES) but never measured LOAD TIME — skipping EGL init
   per launch could be most of the 3s, and exit near-instant. Constraints from the shelved
   work: Brick-only (SP display scans the GL layer, not fb0 — verified), software scale costs
   ~72% CPU during play. A load-time-only measurement decides if it earns a Brick-only life.
Also theirs: 6s boot as the target (we are ~7.15s; the wifi-module-load prune remains the
known lead). Rewind demand: second community signal (they read our prior-art note as
"coming" — it is "exploring").

## Enable GitHub Discussions (johnnyq ask)
Zero-cost community home for the commit-followers. Repo Settings -> Features -> Discussions.

## MDEC NEON optimization (the BR2 endgame — next-session project, roadmap via Codex)
The D47 boundary is core-level, not physical: pcsx's MDEC decode costs 42-60ms/frame on the
A133P; below ~16ms those sections reach realtime at stock clocks and the source-audio chop
dies. Plan: (1) profiler patch in mdec.c — timers around rl2blk, idct, yuv2rgb15/24, DMA
copy; rebuild core via cores/patches, run the BR2 sequence, find the dominant cost.
(2) NEONize yuv2rgb15 if it dominates (classic per-pixel NEON win) OR implement the
row-sparsity IDCT shortcut (an explicit upstream TODO). Ruled out by Codex: Fast MDEC
(GLES-path only; we build BUILTIN_GPU=neon), MDEC_BIAS (timing knob, compat-sensitive, not
wall-clock). Success = BR2 (and all FMV-heavy PS1) flawless at stock clocks; patch is
upstreamable. Prereq receipts: D47 + addendum (frontend/audio exhausted, replicated).

## Deferred from the 2026-07-12 release audit (P2s, v1.3.1/v1.4)
Verified real, consciously not blocking v1.3 (none affects a default configuration's
correctness; each needs its own on-device re-gate):
- **Audio catch-up is a no-op on tg5040**: `api.c` clears `should_vsync` for catch-up, but
  the SDL renderer is created with `SDL_RENDERER_PRESENTVSYNC` and `PLAT_flip` ignores the
  argument; ring/stat reads on that path are also unsynchronized. Either remove the dead
  hot-path work or implement real pacing control - measure first.
- **`PLAT_uvReassert` per-flip mutex contention**: every flip takes `uv_lock`, which the
  UV hold thread holds during 200 Hz I2C traffic. Only material with UV active; fix is an
  atomic already-started fast path, then re-measure frame-time tails before shipping.
- **Pre-merge `tg3040` Extras invisible after upgrade**: `update.sh` migrates
  `.userdata/tg3040` configs but not `/Emus/tg3040` / `/Tools/tg3040` pak folders; lookup
  is tg5040-only now. Affects only ancient pre-merge Brick cards upgraded in place -
  document or migrate.

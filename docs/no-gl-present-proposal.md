# Proposal: GL-free present path (make the GPU dark) — scope, not code

Lever ① from `zero-efficiency-roadmap.md`. Goal: stop presenting through the PowerVR GE8300 so its
power domain can suspend — the one efficiency edge NextUI (fully GPU-based) structurally can't copy.
**This is a prototype-and-measure proposal, not a "just do it" — because at 1024×768 the tradeoff can
go either way, and honesty about that is the point.**

## What we do today (measured)
`workspace/tg5040/platform/platform.c`:
- `PLAT_initVideo` (L58): `SDL_CreateRenderer(..., SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC)`
  + a `STREAMING` RGB565 texture (L99/L106).
- `PLAT_flip` (L373): `SDL_UpdateTexture` (upload emu frame) → **`SDL_RenderCopy` (L436, the GPU
  scales src→1024×768 + aspect)** → `SDL_RenderPresent` (L443, GLES/KMS flip).
- `PLAT_getScaler` returns `scale1x1_c16` — a **1×1 passthrough**. So on tg5040 the **GPU does the
  scaling**, not our NEON `scaler.c`. `SDL_RENDERER_ACCELERATED` selects the KMSDRM+GLES backend.
- Device confirms it: `minarch` holds `/dev/dri/renderD128`, GPU power domain `active`, `pll_gpu`/`gpu`
  pinned at **702 MHz** during play.

So today the GPU does **texture upload + scale + present**, every frame. That's the 702 MHz we'd reclaim.

## What the device offers for a GL-free path (measured)
- **`/dev/fb0`**: present. `virtual_size = 1024,16384`, **32bpp (XRGB8888)**, stride 4096. The huge
  virtual height (21× the 768 visible) means **panning/multi-buffer is available** → tear-free
  double-buffering via `FBIOPAN_DISPLAY` + `FBIO_WAITFORVSYNC` is possible. Note: **32-bit, not
  RGB565** — a software path must convert RGB565→XRGB8888 on the way out.
- **`/dev/dri/card0`** (+ `controlD64`): full KMS. Alternative to fbdev: allocate **DRM dumb buffers**
  (CPU-mapped, no GPU) and `drmModePageFlip` + `drmWaitVBlank`. Dumb buffers can sometimes be RGB565
  (avoids the convert) depending on the plane formats the DE exposes — to be probed.

## The proposed path
Replace the GPU present with: **NEON software scale (real `scaler.c`, not 1×1) → 1024×768, RGB565→
XRGB8888 convert → write to a back buffer → page-flip (fbdev pan or DRM dumb-buffer flip) synced to
vblank.** GPU issues zero GL → `renderD128` unused → GPU power domain can suspend, `pll_gpu` gate off.
MyMinUI is the technique reference: it opens `/dev/fb0` directly and flips in `PLAT_flip`
(`mymin/main:workspace/miyoomini/platform/platform.c:297,473` and `.../m21/...:861,1173`) — **but on
640×480-class panels**, where software scaling is cheap.

## The honest tradeoff — why this must be measured, not assumed
Going GL-free **removes** GPU work (702 MHz: upload+scale+present) but **adds** CPU + DDR work:
- Software-scale each emu frame (e.g. 256×224 SNES, 160×144 GB) up to **1024×768** at 60fps — NEON,
  but ~0.79 Mpx/frame ≈ 47 Mpx/s, plus **RGB565→XRGB8888 convert** and ~3 MB/frame (180 MB/s) written
  to the framebuffer, which also loads the **DDR rail**.
- At MyMinUI's 640×480 this is ~2.5× cheaper; **at the Brick's 1024×768 it's genuinely borderline.**

This is exactly the project's own non-negotiable, applied in reverse: CLAUDE.md says *"GLES is
benchmarked, not auto-rejected — adopt only if it wins on total-device power, not CPU%."* The mirror
is true here — **software present wins only if the GPU it darkens costs more than the CPU+DDR it adds.**
On a high-res panel that is not obvious. The `charge_counter` meter exists precisely to settle it.

## Risks / things the change touches
1. **Whole UI, not just games.** `PLAT_flip` also presents `minui.elf` (launcher) + in-game menu +
   Tools. All must render correctly on the new path (menus are full-res software surfaces already —
   easier — but must be wired).
2. **Effects/sharpness done on GPU today**: `SHARPNESS_CRISP` uses a render-target upscale (L392) and
   scanline/grid `effect` overlays (L439) are GPU `SDL_RenderCopy`. Software path must reimplement or
   drop these (dropping is on-thesis: fewer features).
3. **Scale quality**: GPU bilinear vs NEON nearest. Nearest is fine (sharp pixel-art, matches SOFT/CRISP
   intent) and cheaper — but it's a visible change to vet.
4. **Tear-free correctness**: must double-buffer + block on vblank (fb pan or DRM flip). Get this wrong
   → tearing or frame-pacing regressions (pillar 3).
5. **DDR power**: the extra 180 MB/s fb writes load the memory rail — part of the "total power" question.

## Plan (decision-gated)
1. **Probe** (cheap, no commit): from a tiny standalone test on-device, confirm whether a DRM dumb
   buffer can be **RGB565** (skips the convert) and whether fb pan or DRM page-flip gives clean vblank
   sync. Pick fbdev-pan vs DRM-dumb accordingly.
2. **Prototype** behind a flag (e.g. `PLAT_useSoftwarePresent`, env-gated) — minimal: one core, native
   + one integer scale, nearest, no effects. Keep the GLES path intact as default.
3. **MEASURE with the meter** (10-min windows, per the drain-bench caveat), on light (GB/NES) *and*
   heavy (PS1) titles: (a) `charge_counter` drain vs GLES baseline, (b) does `pll_gpu` gate off /
   power domain suspend, (c) CPU load + `ddr_thermal_zone`, (d) frame pacing / tearing.
4. **Decision gate:** adopt only if **total-device drain drops** AND frame pacing holds AND it's
   tear-free. If 1024×768 software-scale eats the GPU savings, **do not ship it** — record the negative
   result (it's still a real finding) and consider the fallbacks below.

## Fallbacks if full-software present loses at 1024×768
- **GPU downclock**: if `pll_gpu` can be lowered (702→lower) while GLES still presents a simple quad,
  that's a smaller, safer partial win (the GPU cost is mostly the clock being lit). Needs clk-framework
  probing; riskier but cheaper than a full rewrite.
- **Lower present resolution**: render/scale to less than 1024×768 and let the DE upscale (if the panel
  scaler is cheaper than the GPU) — trades sharpness for power.
- **Accept GPU-lit** as the honest cost of a high-res panel, and bank the wins we *can* prove
  (governor, cores, radios/LEDs-off) — still cooler + leaner than stock MinUI, on par with NextUI.

## Effort
Prototype behind a flag: **meaningful** (new present backend + NEON scale/convert + vblank sync +
UI wiring), but bounded and reversible (flag-gated, GLES stays default until measured). The measurement
is the deliverable — this is the one lever where the *answer itself is unknown* until we run it.

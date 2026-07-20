# v1.4 launch/exit teardown profile (Brick, 2026-07-17)

## Question

A field report estimated about 3 seconds to open a game and 1.5 seconds to return to the
menu. Is the delay core/ROM work, launcher-wide `sync`, or video lifecycle teardown, and is
there a low-risk improvement consistent with Zero's lean design?

## Method

- Device: TrimUI Brick, stock clocks, release/v1.4 base `8e11ce52`.
- Baseline: profiler commit `74f203ac`; shipping platform behavior unchanged.
- Candidate: direct framebuffer clear plus invisible-menu-clear skip (`ba8d79d4` +
  `d1059218`); shipping extraction is `5d296303`, hardened by `3457ef28` to retain the
  no-mapping fb0 clear.
- Five consecutive cycles per cell through the real MinUI selection and in-game Quit paths.
- Launch interval: `menu.queue_next` to `game.first_frame`.
- Exit interval: `game.teardown.begin` to `menu.first_present`.
- Games: Aladdin (SNES, supafaust) and Bloody Roar II (PS1, pcsx_rearmed).
- Instrumentation writes only to `/tmp` and is not part of the shipping change.

## Result

| Game | Interval | Baseline median | Final candidate median | Delta |
|---|---:|---:|---:|---:|
| Aladdin | launch | 1,880 ms | 1,160 ms | **-720 ms (-38%)** |
| Aladdin | exit | 1,810 ms | 1,270 ms | **-540 ms (-30%)** |
| Bloody Roar II | launch | 2,150 ms | 1,460 ms | **-690 ms (-32%)** |
| Bloody Roar II | exit | 1,780 ms | 1,280 ms | **-500 ms (-28%)** |

The launcher-wide `sync` calls measured about 20-30 ms and are not the problem. The dominant
operation was `system("cat /dev/zero > /dev/fb0")`, which cost 600-630 ms per process exit.
The Brick menu already owns a writable fb0 mapping. The final candidate clears that mapping
directly; a game or Smart Pro path without an existing mapping briefly opens and maps fb0 so
a layer switch cannot expose stale pre-game contents. That hardened clear cost 60-90 ms in
the final ten-cycle run (80 ms median for Aladdin, 90 ms for Bloody Roar II), versus 600-630
ms for the old shell command. Three black GLES presents are skipped only for the direct-fb
Brick menu, whose panel never scans those buffers.

The final exit medians come from a second five-cycle-per-game pass after the no-mapping
hardening. Launch is unaffected because the new work exists only in `PLAT_quitVideo`; launch
medians remain from the original candidate pass.

All 20 candidate launches and graceful exits completed. Raw event streams:

- `receipts/launch-exit-snes-baseline-20260717.txt`
- `receipts/launch-exit-snes-fast-20260717.txt`
- `receipts/launch-exit-ps-baseline-20260717.txt`
- `receipts/launch-exit-ps-fast-20260717.txt`
- `receipts/launch-exit-final-preserved-fb-20260717.txt` (first five exits Aladdin, final
  five Bloody Roar II)

## Gates

- **Passed:** tg5040 cross-build of MinUI and MinArch.
- **Passed:** five repeated launch/exit cycles in each of four A/B cells.
- **Passed:** five repeated final-candidate exits per game with the no-mapping fb0 clear.
- **Pending:** human observation for stale-frame/flash behavior during both transitions.
- **Pending:** one Smart Pro launch/exit smoke test. Its code path retains the existing GLES
  black-buffer presents because `fb_present` is false.

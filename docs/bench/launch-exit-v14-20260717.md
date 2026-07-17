# v1.4 launch/exit teardown profile (Brick, 2026-07-17)

## Question

A field report estimated about 3 seconds to open a game and 1.5 seconds to return to the
menu. Is the delay core/ROM work, launcher-wide `sync`, or video lifecycle teardown, and is
there a low-risk improvement consistent with Zero's lean design?

## Method

- Device: TrimUI Brick, stock clocks, release/v1.4 base `8e11ce52`.
- Baseline: profiler commit `74f203ac`; shipping platform behavior unchanged.
- Candidate: direct framebuffer clear plus invisible-menu-clear skip (`ba8d79d4` +
  `d1059218`); shipping extraction is `cd3c2e11`.
- Five consecutive cycles per cell through the real MinUI selection and in-game Quit paths.
- Launch interval: `menu.queue_next` to `game.first_frame`.
- Exit interval: `game.teardown.begin` to `menu.first_present`.
- Games: Aladdin (SNES, supafaust) and Bloody Roar II (PS1, pcsx_rearmed).
- Instrumentation writes only to `/tmp` and is not part of the shipping change.

## Result

| Game | Interval | Baseline median | Candidate median | Delta |
|---|---:|---:|---:|---:|
| Aladdin | launch | 1,880 ms | 1,160 ms | **-720 ms (-38%)** |
| Aladdin | exit | 1,810 ms | 1,190 ms | **-620 ms (-34%)** |
| Bloody Roar II | launch | 2,150 ms | 1,460 ms | **-690 ms (-32%)** |
| Bloody Roar II | exit | 1,780 ms | 1,010 ms | **-770 ms (-43%)** |

The launcher-wide `sync` calls measured about 20-30 ms and are not the problem. The dominant
operation was `system("cat /dev/zero > /dev/fb0")`, which cost 600-630 ms per process exit.
The Brick menu already owns a writable fb0 mapping, while game and Smart Pro paths already
present black through GLES. The candidate clears the existing mapping directly, then unmaps
and closes it; it also skips three black GLES presents only for the direct-fb Brick menu,
whose panel never scans those buffers.

All 20 candidate launches and graceful exits completed. Raw event streams:

- `receipts/launch-exit-snes-baseline-20260717.txt`
- `receipts/launch-exit-snes-fast-20260717.txt`
- `receipts/launch-exit-ps-baseline-20260717.txt`
- `receipts/launch-exit-ps-fast-20260717.txt`

## Gates

- **Passed:** tg5040 cross-build of MinUI and MinArch.
- **Passed:** five repeated launch/exit cycles in each of four A/B cells.
- **Pending:** human observation for stale-frame/flash behavior during both transitions.
- **Pending:** one Smart Pro launch/exit smoke test. Its code path retains the existing GLES
  black-buffer presents because `fb_present` is false.

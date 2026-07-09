# THPS2 in-level threading A/B — new baseline (GPU thread off), 2026-07-09

First GAMEPLAY (not attract) threading measurement, run by the automated level-runner
(recorded input sequence: boot 35s → START ×2 → D-right → accept ×3 → Single Session,
The Hangar, Tony Hawk; 100s measured in-level; Brick, discharging, Crisp, HUD on).

| arm | ceiling (settled) | gen | underruns/100s | temp |
|-----|-------------------|-----|----------------|------|
| threading Off | 1008000 (bracket floor) | 60/60 | 0 | ~34.1C |
| threading On  | 1008000 (bracket floor) | 60/60 | 0 | ~34.8C |

Verdict: with pcsx_rearmed_gpu_thread_rendering=disabled (v1.3 default), THPS2 gameplay
rests on the PS1 bracket floor single-threaded. Threading adds nothing (and ~0.5C).
Historical note: the thread-campaign estimate for the single-thread gameplay floor was
~1584 — the GPU-thread-off change alone moved it to 1008. The old attract-mode matrix's
PS1 rows are superseded by this baseline.

Context for the old NextUI comparison: "THPS runs 1008-1200 on NextUI" (their commenter's
brag) is now matched single-threaded at stock clocks with the closed-loop governor.

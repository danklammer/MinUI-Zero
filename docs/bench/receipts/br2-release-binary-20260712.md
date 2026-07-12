# BR2 release-binary receipt (2026-07-12)

The 2026-07-12 re-validation's "zero audio underruns" figure originally lacked an archived
log (audit finding). Re-captured here on the shipped v1.3.0 build (20260712-3, commit
0fa83118) — on the maintainer's daily card (v1.2→v1.3 in-place upgrade), not the bench
card, so it doubles as the real-world upgrade gate.

- **Session**: Bloody Roar II attract, 9.4 min, discharging (82→80%), launched through the
  PS pak chain with full logging.
- **Underruns: 0** — `grep -ci underrun` over the full session log = 0, and the logger
  emits a line per underrun event (`br2-release-ps-log-20260712.txt`, 226 lines).
- **Generation rate held**: governor lines show gen 59.2–60.3/60 across sinks
  1800→1584→1368→1152→1008, and one correct BIGSLIP re-provision 1584→1800 at gen 57.7.
- **Clock/temp** (`../bench3-real-raw/zero-v130-final-br2-maincard.csv`, 20 s cadence):
  one 1800 MHz sample at launch, then a flat **600 MHz at ~32 °C** — this boot's attract
  mix landed on the light end (menus), and schedutil idled at 600 beneath the governor's
  1008 ceiling. Together with the 07-11 heavy-mix cell (modal 1800, 39.3 °C) this brackets
  BR2's documented scene range: the governor pays 600–1800 depending on what the scene
  actually costs, holding rate with zero underruns at both extremes.
- **Upgrade-gate side findings**: v1.2→v1.3 update consumed cleanly; configs preserved;
  the v1.2-era voltage table correctly refuses to arm (no `table.stock` recorded-stock
  file — v1.3 will not guess rail voltages) so the session ran stock volts; noted in
  release notes.

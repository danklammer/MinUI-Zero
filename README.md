# MinUI Zero

**MinUI Zero** is a performance-focused fork of [MinUI](https://github.com/shauninman/MinUI)
for the **TrimUI Brick** and **TrimUI Smart Pro**.

Run cool, run efficient, stay simple. Every system runs at the lowest clock that still holds
full frame rate, the GPU sleeps at the menu, and an idle device suspends to RAM instead of
cooking in your bag. Same appliance-simple MinUI — tuned underneath, with every change
measured on real hardware.

## What's different

| Feature | Details |
|---|---|
| **Closed-loop governor** | The lowest clock that holds frame rate, per game — never overclocks |
| **Optimize CPU** | Measures your chip's safe minimum voltage and runs it there. Up to 20% less CPU power, verified |
| **GPU-dark menu** | The launcher renders in software so the GPU powers down (~26°C) — Brick only |
| **Zero idle waste** | No polling daemons, radios and LEDs off, audio closed in sleep, USB charge-only |
| **Deep sleep** | Default-on, soak-tested — suspends to RAM, wakes instantly (opt-out tool included) |
| **Stock bugs fixed** | NES ran hot with crackling audio everywhere, quit menus hung, LEDs re-lit themselves |
| **Plays better** | Stutter-free panel-locked pacing, a frame less input lag, smoother audio resampling |
| **Tuned everything** | Cores built for the chip and pinned, `noatime` |
| **Hard to break** | Bad-ROM bail, mid-game resolution changes, crash-safe saves |

**~7.5 hours on Game Boy** (measured on the Brick) — up from ~6 before tuning.

## The governor (and why there's no CPU Speed setting)

Everyone else handles CPU speed one of two ways:

- **Static clocks** (stock MinUI, most forks) — a hand-picked speed per console, exposed as a
  "CPU Speed" menu. One number has to cover the heaviest game on the system, so it runs hot
  for everything else, and tuning it is your problem.
- **Kernel governors** (NextUI) — Linux picks the clock from CPU *utilization*. Better, but
  utilization can't see the game: it can't tell "60fps with headroom" from "55fps and
  struggling."
- **Closed loop** (MinUI Zero) — measure the game's *actual frame rate* and find the lowest
  clock that verifiably holds it, per game, continuously.

Zero closes the loop. The frontend measures the **actual outcome** — the core's real frame
rate against its target — every half second, and walks the clock ceiling down to the lowest
point where the game *verifiably* runs full speed. Every game gets its own answer (Zelda DX
settles at 408 MHz; Contra needs ~800 — same console family, different truths). Heavy scene?
It climbs within a second. A clock that failed is remembered and not re-tried. And it never
probes into saturation, because a maxed-out low clock measures *warmer* than a relaxed higher
one that finishes early and sleeps.

That's why the CPU Speed setting is gone: the machine answers the question better than a menu
can — per game, continuously, with receipts. It's also how a stock config bug was caught that
makes NES run hot on every MinUI device: a system that measures game speed notices when a
1985 console demands a 1 GHz clock.

## Optimize CPU (the self-calibrating undervolt)

Every chip is a little different. The factory voltage table is set for the worst chip ever
made — which means your specific chip almost certainly runs stable well below it. That gap
is free power. Enthusiast firmwares offer manual undervolting — you guess numbers and eat
the crashes. As far as we know, none has ever shipped a device that measures its own chip
and tunes itself. Flagship phone silicon does this with dedicated on-die hardware; Zero
does it in firmware, for a $60 handheld.

Run **Tools → Optimize CPU**, leave the device on its charger, and for about 90 minutes it
measures its own silicon: stepping the CPU voltage down under worst-case load, watchdog
armed, until it finds each clock's real limit. It restarts itself several times — that is
the measurement working. The result is a voltage table for YOUR exact chip, with a safety
margin below every measured limit.

From then on the governor runs your chip at its measured minimum. Verified on hardware:
**up to 20% less CPU power at identical clocks** and about **4°C cooler in heavy games** —
same frame rates, nothing to configure afterward.

Built safe in every direction: voltages are applied at runtime only, so any reboot, crash,
or update instantly returns to factory-safe values. There is no way to brick a device with
it. Revert anytime from the same tool. Calibration survives updates — measure once.

## What's left out

No boxart, wifi, store, achievements, LED effects, shaders, or themes. Anything that adds
heat or drain without earning it doesn't ship — several flashy features were built, measured
as break-even, and cut. `docs/DECISIONS.md` records every verdict.

## Consoles

**Ready to play:** Game Boy Color · Game Boy Advance · NES · SNES · Sega Genesis · PlayStation

**Also aboard, dormant:** Game Boy, mGBA, Super Game Boy, Game Gear, Master System,
TurboGrafx-16, Virtual Boy — create the matching Roms folder (eg. "Virtual Boy (VB)")
and the system appears, tuned core already installed.

## Install

- **Fresh:** unzip `MinUI-Zero-*-base.zip` onto a blank FAT32 SD card.
- **Update:** drop the new zip on the card root and reboot — it self-applies.

## Credits

Built on [MinUI](https://github.com/shauninman/MinUI) by Shaun Inman. Deep sleep from
[zhaofengli](https://github.com/zhaofengli/MinUI); techniques borrowed from
[MyMinUI](https://github.com/Turro75/MyMinUI) and [NextUI](https://github.com/LoveRetro/NextUI);
the dynamic rate control idea comes from [RetroArch](https://github.com/libretro/RetroArch).
An independent fork — not affiliated with any of them.

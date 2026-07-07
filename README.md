# MinUI Zero

## Same simple MinUI — Runs cooler. Lasts longer. Plays smoother.

**MinUI Zero** is a low-power [MinUI](https://github.com/shauninman/MinUI) fork for the **TrimUI Brick** and **TrimUI Smart Pro**.

It keeps MinUI's fast, distraction-free experience while tuning everything underneath to use only the power each game actually needs.

**Full speed. Zero tinkering.**

[Download the latest release](https://github.com/danklammer/MinUI-Zero/releases/latest)

---

## Why MinUI Zero?

- **Cooler gameplay** without lowering frame rates
- **Longer battery life** — ~7.5 hours on Game Boy, ~7 on PlayStation
- **Smoother gameplay** with improved frame pacing, audio resampling, and lower input latency
- **No CPU settings to manage** — every game is tuned automatically
- **Instant deep sleep and resume** without leaving the device running hot
- **The simplicity of MinUI** without box art, stores, accounts, themes, or background services

MinUI Zero is for people who want to turn on a handheld and play games — not spend their time configuring it.

## Measured results

Tests were performed on real TrimUI hardware, against stock MinUI on the same device.

| Test | Result |
|---|---|
| Gameplay vs stock MinUI's default 1608 MHz clock | **2-3°C (4-5°F) cooler** |
| Gameplay vs MinUI's 2.0GHz Performance mode | **4-5°C (7-9°F) cooler** |
| Optimize CPU, identical pinned-clock stress test | **3°C (5°F) lower temperature rise** |
| Optimize CPU power reduction | **Up to 20% less CPU power at the same clock** |
| Game Boy battery life on TrimUI Brick | **~7.5 hours**, up from ~6 hours before tuning |
| PlayStation battery life | **~6.5-7 hours** measured on tuned hardware |
| Menu idle on TrimUI Brick | **~26°C (79°F)** with the GPU powered down |
| Boot to menu | **~10 seconds** (2.3s faster than v1.1, measured) — wake from sleep is instant |
| Deep sleep | Near-zero active power, with instant resume |

The governor and Optimize CPU figures come from separate tests, so they are listed separately rather than added together; a tuned device may see a larger combined benefit. Absolute temperatures vary with the game, brightness, ambient temperature, and individual silicon. On CPU scaling alone, NextUI's auto mode and Zero measure about the same — it's the same kernel mechanism underneath. Zero's edge comes from everything else: the per-chip undervolt, the GPU-dark menu, the idle and boot work, and carrying no extras — no shaders, overlays, or background services spending power on anything but the game. See [`docs/nextui-comparison.md`](docs/nextui-comparison.md) and [`docs/DECISIONS.md`](docs/DECISIONS.md) for the test results and engineering decisions behind these claims.

## What is different?

| Feature | What it means for you |
|---|---|
| **Automatic CPU control** | Each game gets the performance it needs without running the CPU harder than necessary |
| **Optimize CPU** | Your device can measure its own safe voltage range for lower heat and power use |
| **GPU-dark menu** | The launcher renders without the GPU (TrimUI Brick), so the menu runs cool |
| **No idle waste** | Radios and LEDs are disabled, audio closes during sleep, and no polling daemons run in the background |
| **Deep sleep by default** | Suspend-to-RAM keeps your place and wakes almost instantly |
| **Stock bugs fixed** | Hot-running NES settings, crackling audio, hanging quit menus, and LEDs turning themselves back on |
| **Smoother gameplay** | Panel-matched pacing, improved audio resampling, and roughly one frame less input latency |
| **Efficiency-tuned cores** | Emulator cores are built and configured specifically for the hardware |
| **Safer failure handling** | Bad ROMs exit cleanly, mid-game resolution changes are handled, and saves are written safely |
| **Menu clock (opt-in)** | Time next to the battery, in the menu and pause screen — Tools -> Clock to enable |
| **Charging screen** | Idle on the charger shows a dim battery display, then sleeps — cooler charging, honest percentages |
| **Fast boot** | Power to menu in ~10 seconds; wake from sleep is instant |

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

How it works, in five steps:

1. The frontend measures whether the game is holding its target frame rate.
2. When there is unused headroom, it lowers the CPU ceiling.
3. The kernel picks the most efficient clock beneath that ceiling.
4. If a demanding scene needs more, the ceiling rises again within about a second.
5. A clock that failed to hold full speed is remembered and not immediately retried.

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

## Deep sleep

MinUI Zero suspends to RAM instead of leaving the OS awake behind a dark screen. Press
POWER and the device saves its state, closes audio, powers down what it can, and sleeps at
near-zero draw — then wakes almost instantly, right where you left off. An opt-out tool is
included for anyone who prefers the stock behavior.

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
the dynamic rate control idea comes from [RetroArch](https://github.com/libretro/RetroArch);
the power-off haptic cue idea from [SpruceOS](https://github.com/spruceUI/spruceOS).
An independent fork — not affiliated with any of them.

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
| Optimize CPU power reduction | **~20% less CPU power at the same clock** |
| Game Boy battery life on TrimUI Brick | **~7.5 hours**, up from ~6 hours before tuning |
| PlayStation battery life | **~6.5-7 hours** measured on tuned hardware |
| Bloody Roar II — other firmwares need the 2.0GHz overclock | **Full speed at stock clocks** |
| Tony Hawk's Pro Skater 2, in-level | **60fps at 1008 MHz** — half the stock clock |
| Menu idle on TrimUI Brick | **~26°C (79°F)** with the GPU powered down |
| Boot to menu | **~10 seconds** — wake from sleep is instant |
| Deep sleep | Near-zero active power, with instant resume |

Your games, silicon, and settings will vary. Raw data and the reasoning behind every claim live in [`docs/bench/`](docs/bench/) and [`docs/DECISIONS.md`](docs/DECISIONS.md).

## MinUI Zero or NextUI?

[NextUI](https://github.com/LoveRetro/NextUI) is the other major MinUI fork for these devices —
full-featured and polished where Zero is deliberately minimal. Both are good firmware; pick by
philosophy. Zero's measured performance numbers are in the table above.

| | **MinUI Zero** | **NextUI** |
|---|---|---|
| Philosophy | Lowest power that holds full speed | Full-featured daily driver |
| Firmware source code | ~18,600 lines | ~47,200 lines |
| Base install download | 7 MB | 85 MB |
| Rendering | Software; GPU powered down at the menu | Fully OpenGL/GPU-based, with shaders and overlays |
| CPU | Frame-aware closed loop; stock clocks only, never overclocks | Dynamic scaling; performance mode is a 2.0 GHz overclock |
| Features | None by design — no box art, WiFi, stores, or themes | Box art, WiFi, Bluetooth audio, cheats, game switcher, Pak Store, LED effects, themes |
| Deep sleep | Yes, on by default | Yes |
| Devices | Brick, Smart Pro | Brick, Smart Pro, Smart Pro S |

Measured at MinUI Zero v1.4 and NextUI v6.13.2. Source lines count each firmware's own
`.c`/`.h` (launcher, frontend, platform) and exclude the third-party emulator cores both ship;
download sizes are each project's latest base release zip. The NextUI feature list is from its
README, and the 2.0 GHz figure from its `boot.sh`. Some code flows both ways between these
projects — deep sleep shares a lineage, and NextUI is credited in this codebase.

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
| **Efficiency-tuned cores** | Emulator cores are built and configured specifically for the hardware, including NEON-accelerated PlayStation video decoding |
| **Safer failure handling** | Bad ROMs exit cleanly, mid-game resolution changes are handled, and saves are written safely |
| **Menu clock (opt-in)** | Time next to the battery, in the menu and pause screen — Tools -> Clock to enable |
| **Charging screen** | Idle on the charger shows a dim battery display, then sleeps — cooler charging, honest percentages |
| **Fast boot** | Power to menu in ~10 seconds; wake from sleep is instant |

## The governor (and why there's no CPU Speed setting)

Everyone else handles CPU speed one of two ways:

- **Static clocks** (stock MinUI, most forks) — a hand-picked speed per console, exposed as a
  "CPU Speed" menu. One number has to cover the heaviest game on the system, so it runs hot
  for everything else, and tuning it is your problem.
- **Kernel governors** (NextUI) — Linux picks the clock from CPU *utilization*. Genuinely
  good: for steady-state clock selection it ties our measurements. But utilization is
  *target-blind* — it can't know what the game is supposed to be doing. It can't know
  fast-forward wants 4x speed, that a paused emulator isn't a struggling one, or that
  the overclock should never be on the menu.
- **Closed loop** (MinUI Zero) — keep schedutil for what it's great at, and add a frame-aware
  layer above it that knows the *target*: verify the game's actual frame rate, retarget for
  fast-forward, and cap at the highest verified-stock clock.

How it works, in five steps:

1. The frontend measures whether the game is holding its target frame rate.
2. When there is unused headroom, it lowers the CPU ceiling.
3. The kernel picks the most efficient clock beneath that ceiling.
4. If a demanding scene needs more, the ceiling rises again within about a second.
5. A clock that failed to hold full speed is remembered and not immediately retried.

Every game gets its own answer — Zelda DX settles at 408 MHz, Bloody Roar II pays 1800 only
in the scenes that need it. That's why the CPU Speed setting is gone: the machine answers the
question a menu used to ask, per game, continuously.

## Optimize CPU (the self-calibrating undervolt)

Every chip is a little different. Factory voltage tables include production and operating
margin, and some individual chips can run reliably below them. Optimize CPU measures that
device-specific margin instead of applying another device's numbers.

Run **Tools → Optimize CPU**, leave the device on its charger, and for about 90 minutes it
measures its own silicon — stepping the voltage down under load until it finds each clock's
real limit, then adding a safety guard. It restarts itself several times; that is the
measurement working. Result: **~20% less CPU power at identical clocks**, measured, with
nothing to configure afterward.

Voltages apply at runtime only — any reboot returns to factory-safe values. Back up saves
before calibrating, and revert anytime from the same tool.

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
- **Update:** extract `MinUI.zip` from the release archive, place it on the card root, and reboot.

## Disclaimer

MinUI Zero is unofficial personal firmware, provided as-is, without warranty of any kind.
Use it at your own risk: custom firmware can cause data loss, failed boots, or other device
issues, and the author is not responsible for any of them. Back up your SD card before
installing.

## Credits

Built on [MinUI](https://github.com/shauninman/MinUI) by Shaun Inman. Deep sleep from
[zhaofengli](https://github.com/zhaofengli/MinUI); techniques borrowed from
[MyMinUI](https://github.com/Turro75/MyMinUI) and [NextUI](https://github.com/LoveRetro/NextUI);
the dynamic rate control idea comes from [RetroArch](https://github.com/libretro/RetroArch);
the power-off haptic cue idea from [SpruceOS](https://github.com/spruceUI/spruceOS).
An independent personal fork — not affiliated with, endorsed by, or supported by any of them.
See [`LICENSE.md`](LICENSE.md) for license, provenance, and support notes, and
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for detailed attribution.

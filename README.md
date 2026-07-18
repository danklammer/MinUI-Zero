# MinUI Zero

## Same simple MinUI — Runs cooler. Lasts longer. Plays smoother.

**MinUI Zero** is a low-power [MinUI](https://github.com/shauninman/MinUI) fork for the **TrimUI Brick** and **TrimUI Smart Pro**.

It keeps MinUI's fast, distraction-free experience while tuning everything underneath to use only the power each game actually needs.

**Full speed. Zero tinkering.**

### Install

[Download the latest release](https://github.com/danklammer/MinUI-Zero/releases/latest), then:

- **Fresh:** unzip `MinUI-Zero-*-base.zip` onto a blank FAT32 SD card.
- **Update:** extract `MinUI.zip` from the release archive, place it on the card root, and reboot.

---

## Why MinUI Zero?

- **Cooler gameplay** without lowering frame rates
- **Longer battery life** — ~7.5 hours on Game Boy, ~7 on PlayStation
- **Smoother gameplay** — panel-matched frame pacing, improved audio resampling, and roughly one frame less input latency
- **No CPU settings to manage** — every game is tuned automatically, continuously
- **Deep sleep by default** — near-zero draw, instant resume, never running hot in your bag
- **The simplicity of MinUI** — no box art, stores, accounts, or themes, and nothing running in the background but input handling

MinUI Zero is for people who want to turn on a handheld and play games — not spend their time configuring it.

## Measured results

Tests were performed on real TrimUI hardware, against stock MinUI on the same device.

| Test | Result |
|---|---|
| Gameplay vs stock MinUI's default clock | **2-3°C (4-5°F) cooler** |
| Gameplay vs MinUI's 2.0GHz Performance mode | **4-5°C (7-9°F) cooler** |
| Game Boy battery life on TrimUI Brick | **~7.5 hours**, up from ~6 hours before tuning |
| PlayStation battery life | **~6.5-7 hours** |
| Bloody Roar II — other firmwares need the 2.0GHz overclock | **Full speed at stock clocks** |
| Tony Hawk's Pro Skater 2, in-level | **60fps at 1008 MHz** — half the stock clock |
| Menu idle on TrimUI Brick | **~26°C (79°F)** with the GPU powered down |

Your games, silicon, and settings will vary. Raw data and the reasoning behind every claim live in [`docs/bench/`](docs/bench/) and [`docs/DECISIONS.md`](docs/DECISIONS.md).

## MinUI Zero or NextUI?

[NextUI](https://github.com/LoveRetro/NextUI) is the other major MinUI fork for these devices —
full-featured and polished where Zero is deliberately minimal. Both are good firmware; pick by
philosophy. Zero's measured performance numbers are in the table above.

| | **MinUI Zero** | **NextUI** |
|---|---|---|
| Philosophy | Lowest power that holds full speed | Full-featured |
| Firmware source code | ~18,600 lines | ~47,200 lines |
| Base install download | 7 MB | 85 MB |
| Rendering | Lean pipeline — GPU only displays the finished frame in-game, and powers down at the menu | Fully OpenGL/GPU-based, with shaders and overlays |
| CPU | Frame-aware closed loop; stock clocks only, never overclocks | Dynamic scaling; performance mode is a 2.0 GHz overclock |
| Features | None by design — no box art, WiFi, stores, or themes | Box art, WiFi, Bluetooth audio, cheats, game switcher, Pak Store, LED effects, themes |
| Background services in-game | keymon only, rewritten for zero idle wakeups | keymon, battery monitor, audio monitor — plus WiFi and Bluetooth stacks when enabled |
| Deep sleep | Yes | Yes |
| Devices | Brick, Smart Pro | Brick, Smart Pro, Smart Pro S |

Measured at MinUI Zero v1.4 and NextUI v6.13.2. Source lines count each firmware's own
`.c`/`.h` (launcher, frontend, platform) and exclude the third-party emulator cores both ship;
download sizes are each project's latest base release zip. The NextUI feature list is from its
README, its background services and the 2.0 GHz figure from its boot and launch scripts
(both firmwares also run the vendor's stock input daemon). Some code flows both ways between these
projects — deep sleep shares a lineage, and NextUI is credited in this codebase.

## How it works

### The governor (and why there's no CPU Speed setting)

Stock MinUI and most forks use a hand-picked static clock per console — one number that has to
cover the heaviest game on the system, so it runs hot for everything else. NextUI delegates to
the kernel's utilization-based scaling — genuinely good at steady-state clock selection, but
utilization is target-blind: it can't know fast-forward wants 4x speed or that a paused
emulator isn't a struggling one.

MinUI Zero keeps the kernel's scaling for what it's great at and adds a frame-aware layer above
it that knows the *target*:

1. The frontend measures whether the game is holding its target frame rate.
2. When there is unused headroom, it lowers the CPU ceiling.
3. The kernel picks the most efficient clock beneath that ceiling.
4. If a demanding scene needs more, the ceiling rises again within about a second.
5. A clock that failed to hold full speed is remembered and not immediately retried.

Every game gets its own answer — Zelda DX settles at 408 MHz, Bloody Roar II pays 1800 only
in the scenes that need it. That's why the CPU Speed setting is gone: the machine answers the
question a menu used to ask, per game, continuously.

### Optimize CPU (the self-calibrating undervolt)

Every chip is a little different, and factory voltage tables carry margin many individual chips
don't need. Run **Tools → Optimize CPU**, leave the device on its charger, and for about 90
minutes it measures its own silicon — stepping voltage down under load to find each clock's real
limit, then adding a safety guard. It restarts itself several times; that is the measurement
working. Result: **~20% less CPU power at identical clocks**, measured, with nothing to
configure afterward. Voltages apply at runtime only — any reboot returns to factory-safe
values. Back up saves before calibrating, and revert anytime from the same tool.

### Deep sleep

Press POWER and the device suspends to RAM instead of leaving the OS awake behind a dark
screen: state saved, audio closed, near-zero draw — then it wakes almost instantly, right where
you left off. An opt-out tool is included for anyone who prefers the stock behavior.

### Idle is truly idle

The launcher renders without the GPU (TrimUI Brick), so the menu runs cool. Radios and LEDs
are off, no daemon polls in the background, and idling on the charger shows a dim battery
screen, then sleeps — cooler charging, honest percentages.

## Quality of life

- **Stock bugs fixed** — hot-running NES settings, crackling audio, hanging quit menus, and LEDs turning themselves back on
- **Safer failure handling** — bad ROMs exit cleanly, mid-game resolution changes are handled, and saves are written atomically so a crash or full card can never destroy a good save
- **Efficiency-tuned cores** — emulator cores built and configured for this hardware, including NEON-accelerated PlayStation video decoding
- **Fast boot** — power to menu in ~10 seconds; wake from sleep is instant
- **Menu clock (opt-in)** — time next to the battery; Tools → Clock to enable

## Consoles

**Ready to play:** Game Boy Color · Game Boy Advance · NES · SNES · Sega Genesis · PlayStation

**Also aboard, dormant:** Game Boy, mGBA, Super Game Boy, Game Gear, Master System,
TurboGrafx-16, Virtual Boy — create the matching Roms folder (eg. "Virtual Boy (VB)")
and the system appears, tuned core already installed.

## What's left out

No boxart, wifi, store, achievements, LED effects, shaders, or themes. Anything that adds
heat or drain without earning it doesn't ship — several flashy features were built, measured
as break-even, and cut. `docs/DECISIONS.md` records every verdict.

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

# MinUI Zero

## Same simple MinUI. Runs cooler, lasts longer, plays smoother.

**MinUI Zero** is a low-power [MinUI](https://github.com/shauninman/MinUI) fork for the **TrimUI
Brick** and **TrimUI Smart Pro**, with alpha builds for the **Anbernic RG35XX Plus / H** and the
**Miyoo Mini family**. It keeps MinUI's fast, distraction-free experience and tunes everything
underneath to use only the power each game actually needs.

**Full speed. Zero tinkering.**

### Install

[Download the latest release](https://github.com/danklammer/MinUI-Zero/releases/latest). The two
installs are genuinely different: one copies files, the other replaces the whole card.

**TrimUI and Miyoo: copy files.** MinUI Zero rides along with the firmware already on the device and
nothing is erased.

- **Fresh:** unzip the base zip onto a blank FAT32 SD card.
- **Update:** drop `MinUI.zip` on the card root and reboot.

**Anbernic: flash an image.** Here MinUI Zero *is* the operating system. Decompress the `.img.xz`
for your device and write it with [Raspberry Pi Imager](https://www.raspberrypi.com/software/),
[balenaEtcher](https://etcher.balena.io/), or `dd`.

> **Flashing erases the card.** Back up saves and roms first. The Plus and H images are not
> interchangeable: each carries its own boot chain, device tree and kernel.

Nothing is written to the device itself, so flash a spare card and swap cards to switch systems.
First boot expands the ROMS partition to fill the card, and that partition mounts on any PC or Mac.
Wifi and ssh stay off until you ask for them, via `wifi.txt` and `authorized_keys` at the card root.

---

## Why MinUI Zero?

- **Cooler gameplay** without lowering frame rates
- **Longer battery life**, ~7.5 hours on Game Boy and ~7 on PlayStation
- **Smoother gameplay**: panel-matched frame pacing, better audio resampling, roughly one frame less
  input latency
- **No CPU settings to manage.** Every game is tuned automatically and continuously
- **Deep sleep by default**: near-zero draw, instant resume, never running hot in your bag
- **The simplicity of MinUI**: no box art, stores, accounts, or themes

For people who want to turn on a handheld and play games, not spend their time configuring one.

## Measured results

On real TrimUI hardware, against stock MinUI on the same device.

| Test | Result |
|---|---|
| Gameplay vs stock MinUI's default clock | **2-3°C cooler** |
| Gameplay vs MinUI's 2.0GHz Performance mode | **4-5°C cooler** |
| Game Boy battery life, TrimUI Brick | **~7.5 hours**, up from ~6 before tuning |
| PlayStation battery life | **~6.5-7 hours** |
| Bloody Roar II fights, ~51fps on the serial path | **Locked 60fps at stock clocks** (other firmwares reach 60 via the 2.0GHz overclock) |
| Tony Hawk's Pro Skater 2, in-level | **60fps at 1008 MHz**, half the stock clock |
| Menu idle, TrimUI Brick | **~26°C** with the GPU powered down |

Your games, silicon, and settings will vary. Raw data and the reasoning behind every claim live in
[`docs/bench/`](docs/bench/) and [`docs/DECISIONS.md`](docs/DECISIONS.md).

## MinUI Zero or NextUI?

[NextUI](https://github.com/LoveRetro/NextUI) is the other major MinUI fork for these devices,
full-featured and polished where Zero is deliberately minimal. Both are good firmware; pick by
philosophy.

| | **MinUI Zero** | **NextUI** |
|---|---|---|
| Philosophy | Lowest power that holds full speed | Full-featured |
| Firmware source code | ~21,500 lines | ~51,200 lines |
| Base install download | 7 MB | 82 MB |
| Rendering | Lean pipeline; the GPU only displays the finished frame in-game, and powers down at the menu | Fully OpenGL/GPU-based, with shaders and overlays |
| CPU | Frame-aware closed loop plus a pipelined PS1 frontend; holds full speed at stock clocks, never overclocks | Dynamic scaling; performance mode is a 2.0 GHz overclock |
| Undervolting | Self-calibrating per-chip tool, finds each chip's lowest safe voltage (opt-in) | None |
| Features | None by design | Box art, WiFi, Bluetooth audio, cheats, game switcher, Pak Store, LED effects, themes |
| Background services in-game | keymon only, rewritten for zero idle wakeups | keymon, battery monitor, audio monitor, plus WiFi and Bluetooth stacks when enabled |
| Deep sleep | Yes | Yes |
| Devices | Brick, Smart Pro (+ alpha Anbernic RG35XX Plus / H and Miyoo Mini family) | Brick, Smart Pro, Smart Pro S |

Measured at MinUI Zero v1.5 and NextUI v6.14.0. Source lines count each firmware's own `.c`/`.h` and
exclude the emulator cores both ship; download sizes are each project's base release zip. The NextUI
column comes from its README and its own boot and launch scripts. Code flows both ways between these
projects: deep sleep shares a lineage, and NextUI is credited in this codebase.

## How it works

**The governor, and why there's no CPU Speed setting.** Most forks use a hand-picked static clock per
console, one number that has to cover the heaviest game on the system, so it runs hot for everything
else. MinUI Zero measures whether the game is holding its target frame rate, lowers the CPU ceiling
when there is headroom, and raises it within about a second when a demanding scene needs more. The
kernel picks the most efficient clock beneath that ceiling, and a clock that failed is remembered
rather than immediately retried. Every game gets its own answer: Zelda DX settles at 408 MHz, Bloody
Roar II pays 1800 only in the scenes that need it.

**Optimize CPU**, the self-calibrating undervolt. Factory voltage tables carry margin most individual
chips don't need. Run **Tools → Optimize CPU**, leave the device charging for about 90 minutes, and
it measures its own silicon under load to find each clock's real limit. Result: **~20% less CPU power
at identical clocks**, with nothing to configure afterward. Voltages apply at runtime only, so any
reboot returns to factory-safe values.

**Deep sleep.** Press POWER and the device suspends to RAM instead of leaving the OS awake behind a
dark screen, then wakes almost instantly where you left off. On the RG35XX Plus that is **~1.0 %/h
suspended against 8.79 %/h while playing**, about four days of standby, replacing the only idle
behaviour that device had before, which was to power off and lose your place. An opt-out tool is
included.

**Idle is truly idle.** The launcher renders without the GPU (Brick), radios and LEDs are off, no
daemon polls in the background, and idling on the charger shows a dim battery screen and then sleeps.

## Quality of life

- **Stock bugs fixed**: hot-running NES settings, crackling audio, hanging quit menus, LEDs turning
  themselves back on
- **Safer failure handling**: bad ROMs exit cleanly, mid-game resolution changes are handled, and
  saves are written atomically so a crash or full card can never destroy a good save
- **Efficiency-tuned cores**, including NEON-accelerated PlayStation video decoding
- **Fast boot**: power to menu in ~10 seconds, and waking from sleep is instant
- **Menu clock** (opt-in) via Tools → Clock

## Consoles

**Ready to play:** Game Boy Color · Game Boy Advance · NES · SNES · Sega Genesis · PlayStation

**Also aboard, dormant:** Game Boy, mGBA, Super Game Boy, Game Gear, Master System, TurboGrafx-16,
Virtual Boy, PICO-8. Create the matching Roms folder (eg. "Virtual Boy (VB)") and the system appears,
tuned core already installed.

## Anbernic RG35XX Plus / H (alpha)

Same launcher, same closed-loop governor, same cores, but newer than the TrimUI builds and less
proven. **Each device has its own image**, carrying that board's boot chain, device tree and kernel;
the H image is what makes its analog sticks work. Other H700 handhelds are untested and need their
own image.

Owning the whole OS is what lets the device boot straight into the launcher, poll input every 5ms,
and idle with nothing else running. Measured on the Plus: **95% to 6% battery over 10.1 hours of
continuous Game Boy Color**, at the lowest clock the silicon offers, at ~36°C.

Rough edges to expect: **updates are a reflash** (no in-place updater yet, and saves live on the
ROMS partition), six of the fifteen systems are verified by launch test, L3 and R3 are unmapped, and
no third-party pak has been run end to end.

## Miyoo Mini family (alpha)

A real port for the SigmaStar SSD202D: same launcher, same governor, the same eleven cores rebuilt
for ARMv7/NEON. One card serves all three models and the firmware detects which it woke up on. The
**Plus** is the model this is developed and verified on; the **Mini** and **Flip** are code-complete
but **never tested on real hardware**.

| | TrimUI Brick / Smart Pro | Miyoo Mini Plus |
|---|---|---|
| Closed-loop governor | Yes | Yes |
| Emulator cores | 11 | 11 (same pinned versions) |
| **Deep sleep** | **Yes** | **Not possible**, see below |
| **Optimize CPU (undervolt)** | **Yes** | **No** |
| Tear-free, panel-accurate presentation | Yes | Yes |
| Measured battery/thermal figures | Yes, see above | **None yet** |

**None of the measured results above were taken on a Miyoo**, and an early port investigation found
the CPU is a much smaller share of total power on this SoC, so do not assume the battery numbers
transfer. They probably do not. PlayStation also leans on a 128 MB swapfile because the device has
~100 MB of usable RAM: games run at full speed, but the heaviest scenes push it to its limit. A
pin-verified 24% overclock changed that by 0%, so more clock cannot buy it.

**Why there's no deep sleep here.** This is a hardware limit, not a missing feature. The vendor
kernel is built without suspend support at all, so `/sys/power/state` is empty and there is no
suspend mode for any firmware to ask for. Every other Miyoo custom firmware works around it the same
way we do. Closing it for real would mean rebuilding the vendor kernel and reflashing SPI NOR, since
the kernel does not live on the SD card, which is a brick risk we will not take for a sleep mode.
Instead POWER blanks and idles, and after two minutes the device quicksaves and powers off.

## What's left out

No boxart, wifi, store, achievements, LED effects, shaders, or themes. Anything that adds heat or
drain without earning it doesn't ship, and several flashy features were built, measured as
break-even, and cut. `docs/DECISIONS.md` records every verdict.

## Disclaimer

MinUI Zero is unofficial personal firmware, provided as-is, without warranty of any kind. Use it at
your own risk: custom firmware can cause data loss, failed boots, or other device issues, and the
author is not responsible for any of them. Back up your SD card before installing.

## Credits

Built on [MinUI](https://github.com/shauninman/MinUI) by Shaun Inman. Deep sleep from
[zhaofengli](https://github.com/zhaofengli/MinUI); techniques borrowed from
[MyMinUI](https://github.com/Turro75/MyMinUI) and [NextUI](https://github.com/LoveRetro/NextUI); the
dynamic rate control idea from [RetroArch](https://github.com/libretro/RetroArch); the power-off
haptic cue idea from [SpruceOS](https://github.com/spruceUI/spruceOS). The Anbernic hardware
enablement layer is stripped from [muOS](https://muos.dev). An independent personal fork, not
affiliated with, endorsed by, or supported by any of them. See [`LICENSE.md`](LICENSE.md) for
license and provenance, and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for detailed
attribution.

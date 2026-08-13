# MinUI Zero

## Same simple MinUI — Runs cooler. Lasts longer. Plays smoother.

**MinUI Zero** is a low-power [MinUI](https://github.com/shauninman/MinUI) fork for the **TrimUI Brick** and **TrimUI Smart Pro**, with experimental builds for the **Miyoo Mini family** ([see below](#miyoo-mini-family-experimental)) and the **Anbernic RG35XX Plus / H** ([see below](#anbernic-rg35xx-plus--h-experimental)).

It keeps MinUI's fast, distraction-free experience while tuning everything underneath to use only the power each game actually needs.

**Full speed. Zero tinkering.**

### Install

[Download the latest release](https://github.com/danklammer/MinUI-Zero/releases/latest). **How you
install depends on the device, and the two ways are genuinely different** — one copies files, the
other replaces the whole card.

**TrimUI Brick / Smart Pro, and the Miyoo Mini family — copy files onto a card**

MinUI Zero rides along with the firmware already on the device. Nothing is erased.

- **Fresh:** unzip `MinUI-Zero-*-base.zip` onto a blank FAT32 SD card.
- **Update:** extract `MinUI.zip` from the release archive, place it on the card root, and reboot.

**Anbernic RG35XX Plus / H — flash a disk image**

On this device MinUI Zero **is the operating system**, so you write a whole card image instead of
copying files. Decompress `MinUI-Zero-h700-*.img.xz` and flash the `.img` with
[Raspberry Pi Imager](https://www.raspberrypi.com/software/), [balenaEtcher](https://etcher.balena.io/),
or `dd`.

> **Flashing ERASES the card.** Back up saves and roms first.

Two things make this less alarming than it sounds: the OS lives entirely on the card, so **nothing
is written to the device itself** — keep your existing card and flash a *different* one, and you
switch systems by swapping cards. And on first boot the ROMS partition expands to fill the card
automatically, so a 256GB card gives you 256GB of space without any manual resizing.

After flashing, the `ROMS` partition mounts on any PC or Mac. Drop roms into the matching folders,
and optionally add `wifi.txt` (wifi is off until you ask for it) or `authorized_keys` (ssh likewise).

---

## Why MinUI Zero?

- **Cooler gameplay** without lowering frame rates
- **Longer battery life** — ~7.5 hours on Game Boy, ~7 on PlayStation
- **Smoother gameplay** — panel-matched frame pacing, improved audio resampling, and roughly one frame less input latency
- **No CPU settings to manage** — every game is tuned automatically, continuously
- **Deep sleep by default** (TrimUI) — near-zero draw, instant resume, never running hot in your bag
- **The simplicity of MinUI** — no box art, stores, accounts, or themes, and nothing running in the background but input handling

MinUI Zero is for people who want to turn on a handheld and play games — not spend their time configuring it.

## Measured results

Tests were performed on real TrimUI hardware, against stock MinUI on the same device. **None of these
numbers are from the Miyoo Mini Plus** — that build is new and unmeasured; see its own section below.

| Test | Result |
|---|---|
| Gameplay vs stock MinUI's default clock | **2-3°C (4-5°F) cooler** |
| Gameplay vs MinUI's 2.0GHz Performance mode | **4-5°C (7-9°F) cooler** |
| Game Boy battery life on TrimUI Brick | **~7.5 hours**, up from ~6 hours before tuning |
| PlayStation battery life | **~6.5-7 hours** |
| Bloody Roar II fights — held ~51fps on the serial path | **Locked 60fps at stock clocks** (v1.5 pipelined PS1 frontend; other firmwares reach 60 via the 2.0GHz overclock) |
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
| Firmware source code | ~21,500 lines | ~51,200 lines |
| Base install download | 7 MB | 82 MB |
| Rendering | Lean pipeline — GPU only displays the finished frame in-game, and powers down at the menu | Fully OpenGL/GPU-based, with shaders and overlays |
| CPU | Frame-aware closed loop, plus a pipelined PS1 frontend — holds full speed at stock clocks, never overclocks | Dynamic scaling; performance mode is a 2.0 GHz overclock |
| Undervolting | Self-calibrating per-chip tool — finds each chip's lowest safe voltage (opt-in) | None |
| Features | None by design — no box art, WiFi, stores, or themes | Box art, WiFi, Bluetooth audio, cheats, game switcher, Pak Store, LED effects, themes |
| Background services in-game | keymon only, rewritten for zero idle wakeups | keymon, battery monitor, audio monitor — plus WiFi and Bluetooth stacks when enabled |
| Deep sleep | Yes | Yes |
| Devices | Brick, Smart Pro (+ experimental Miyoo Mini Plus) | Brick, Smart Pro, Smart Pro S |

Measured at MinUI Zero v1.5 and NextUI v6.14.0. Source lines count each firmware's own
`.c`/`.h` (launcher, frontend, platform) and exclude the third-party emulator cores both ship,
vendored libraries, and test harnesses; download sizes are each project's latest base release zip. The NextUI feature list is from its
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

On the RG35XX Plus this is measured at **~1.0 %/h suspended against 8.79 %/h while playing** —
roughly nine times cheaper, about four days of standby — and it replaces the only behaviour that
device previously had once it went idle, which was to power itself off and lose your place.

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
TurboGrafx-16, Virtual Boy, PICO-8 — create the matching Roms folder (eg. "Virtual Boy (VB)")
and the system appears, tuned core already installed.

## Anbernic RG35XX Plus / H (experimental)

An **alpha** build for the Anbernic H700 family. Same launcher, same closed-loop governor, same
emulator cores — but newer than the TrimUI builds and less proven. Back up your card and expect
rough edges.

**Developed and verified on the RG35XX Plus only.** The image is built from a Plus-specific donor:
the board config and boot chain (DTB, u-boot) are that device's, and the model is read from a file
in the image rather than detected from the hardware. Other H700 devices — the RG35XX H included —
are **untested and likely need their own image**. Do not assume the family is covered because the
SoC matches.

**This one installs differently, and it is worth understanding why.** On the TrimUI and Miyoo
devices MinUI Zero is a *guest*: the stock firmware boots, and MinUI takes over the screen. On the
RG35XX Plus there is no stock firmware to be a guest of — the device boots whatever card you give
it — so MinUI Zero ships as a complete operating system on a card image you flash. See
[Install](#install).

What that changes for you:

- **You flash an image; it erases the card.** No unzipping files onto an existing setup.
- **Nothing is written to the device.** Flash a spare card and swap cards to switch systems; your
  existing card is untouched.
- **The card resizes itself.** First boot expands the ROMS partition to fill the card.
- **Updates are a reflash today.** There is no in-place updater yet; saves live on the ROMS
  partition, so copy them off before reflashing.

Why bother owning the OS here: it is what makes the device boot straight into the launcher, drop
input latency to a 5ms poll, and idle with nothing else running. The measured result is
**95% → 6% battery over 10.1 hours of continuous Game Boy Color play**, at the lowest clock the
silicon offers, at ~36°C.

Wifi and ssh are both **off until you ask for them** (`wifi.txt` and `authorized_keys` at the card
root) — an idle radio is battery you did not agree to spend.

## Miyoo Mini family (experimental)

There is an **alpha** build for the Miyoo Mini family (SigmaStar SSD202D, dual Cortex-A7). It is a
real port — same launcher, same closed-loop governor, the same eleven emulator cores rebuilt for
ARMv7/NEON — but it is **newer and less proven than the TrimUI builds**, and it is missing
features they have. Treat it as a preview, back up your card, and expect rough edges.

One card serves all three models (new in v1.5.5); the firmware detects which device it woke up on:

- **Miyoo Mini Plus** — the model this port is developed and verified on.
- **Miyoo Mini (original)** — same binaries, correct battery/audio paths, **never tested on real
  hardware**. It should work; nobody has watched it work.
- **Miyoo Mini Flip** — boots from its own `miyoo285` folder, keeps the lid, battery, and audio
  paths appropriate to a clamshell, **never tested on real hardware**. Its panel is unmeasured, so
  it deliberately runs the safe present path rather than inheriting the Plus's measured rate.

| | TrimUI Brick / Smart Pro | Miyoo Mini Plus |
|---|---|---|
| Closed-loop governor | Yes | Yes |
| Emulator cores | 11 | 11 (same pinned versions) |
| Systems with a ready folder | 6 (+9 dormant) | 6 (+9 dormant) |
| **Deep sleep** | **Yes** | **Not possible** — see below |
| **Optimize CPU (undervolt)** | **Yes** | **No** |
| Charging screen | Yes | Yes |
| Tear-free, panel-accurate presentation | Yes | Yes (new in v1.5.4) |
| Measured battery/thermal figures | Yes, see above | **None yet** |

Apart from the PlayStation limitation described below, none of the measured results in this README
were taken on a Miyoo. The efficiency work that makes
Zero worthwhile on TrimUI hardware has not been quantified on this SoC, and an early port
investigation found the CPU is a much smaller share of total power here — so do not assume the
battery-life numbers transfer. They probably do not.

Its PlayStation support leans on a 128 MB swapfile because the device has ~100 MB of usable RAM;
expect SD-backed swapping, not the TrimUI PS1 experience.

**Frame pacing, rebuilt in v1.5.4.** The panel does not run at 60 Hz. It runs at **59.6720 Hz**,
measured directly with a purpose-built probe rather than assumed, and every emulator core targets
its own near-60 rate instead. Two things went wrong in that gap. The surplus frame a 60 fps core
produced was quietly coalesced away, and — the real culprit — the frontend tracked which page was
queued and which was being panned, but nothing tracked the page the panel was actively *scanning*,
so the hardware blitter could draw a whole frame into the picture on screen. That is visible
tearing, and no counter recorded it, because nothing was being dropped. Presentation now waits for
the panel instead of discarding frames, derives a per-core rate correction from the measured panel
rate, and tracks page ownership explicitly. NES and PlayStation sessions on-device report **0
frames dropped and 0 audio underruns**.

**Known limitation: heavy PlayStation scenes.** Games run at full speed (Bloody Roar II and Tony
Hawk's both generate a measured 60 fps), but the heaviest scenes push this device to its limit and
you may hear or see it. We have receipts for what it is *not*: a pin-verified 24% CPU overclock
changed it by 0%, so more clock cannot buy it. We have not isolated which of memory, driver, audio
daemon or I/O it actually is. Note that the frame-pacing rework above landed after those
measurements and heavy PS1 scenes have not been re-characterised under it — so treat this as
unverified rather than as either fixed or unchanged. Lighter systems are unaffected.

**Why there's no deep sleep here.** This is a hardware limit, not a missing feature. The Miyoo's
vendor kernel is built without suspend support at all — `/sys/power/state` is empty on the device,
so there is no suspend mode for any firmware to ask for. Every other Miyoo custom firmware works
around it the same way we do, with a "faux sleep" that blanks the screen, closes audio and drops
the clock; spruceOS says so in as many words in its own source. Closing it for real would mean
rebuilding the vendor kernel and reflashing SPI NOR — the kernel does not live on the SD card —
which is a brick risk we will not take for a sleep mode. Instead, POWER blanks and idles, and
after two minutes the device quicksaves and powers off, resuming where you left off.

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

# MinUI Zero

**MinUI Zero** is a performance-focused fork of [MinUI](https://github.com/shauninman/MinUI)
for the **TrimUI Brick** and **TrimUI Smart Pro**.

Run cool, run efficient, stay simple. Every system runs at the lowest clock that still holds
full frame rate, the GPU sleeps at the menu, and an idle device suspends to RAM instead of
cooking in your bag. Same appliance-simple MinUI — tuned underneath, with every change
measured on real hardware.

## What's different

- **❄️ GPU-dark menu** — the launcher renders in software, so the GPU powers down while you
  browse (~26°C, instant nav).
- **🌡️ Closed-loop governor** — finds the lowest clock that holds frame rate, per game, in
  real time. Never overclocks.
- **💤 Deep sleep** — suspend-to-RAM by default: goes cold, wakes instantly where you left
  off (50-cycle soak tested). The Deep Sleep tool in Extras turns it off.
- **⚡ Zero idle waste** — no busy-polling daemons, radios and LEDs off, audio fully closed
  during sleep, USB is charge-only.
- **🛡️ Crash-safe saves** — if a core ever crashes, your save RAM is written on the way down.
- **⚙️ Tuned everything** — cores built for the chip and pinned, drift-free frame pacing,
  `noatime` mounts.

**~7.5 hours on Game Boy** — up from ~6 before tuning.

## What's left out

No boxart, wifi, store, achievements, LED effects, shaders, or themes. Anything that adds
heat or drain without earning it doesn't ship — several flashy features were built, measured
as break-even, and cut. `docs/DECISIONS.md` records every verdict.

## Consoles

**Base:** Game Boy Color · Game Boy Advance · NES · SNES · Sega Genesis · PlayStation
**Extras:** Game Boy, mGBA, Super Game Boy, Game Gear, Master System, TurboGrafx-16,
Virtual Boy, and more.

## Install

- **Fresh:** unzip `MinUI-Zero-*-base.zip` onto a blank FAT32 SD card.
- **Update:** drop the new zip on the card root and reboot — it self-applies.

## Credits

Built on [MinUI](https://github.com/shauninman/MinUI) by Shaun Inman. Deep sleep from
[zhaofengli](https://github.com/zhaofengli/MinUI); techniques borrowed from
[MyMinUI](https://github.com/Turro75/MyMinUI) and [NextUI](https://github.com/LoveRetro/NextUI).
An independent fork — not affiliated with any of them.

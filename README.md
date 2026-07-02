# MinUI Zero

**MinUI Zero** is a performance-focused fork of [MinUI](https://github.com/shauninman/MinUI)
for the **TrimUI Brick** and **TrimUI Smart Pro**.

The thesis: **run cool, run efficient, stay simple.** Every emulated system runs at the lowest CPU clock
that still holds its frame rate, the GPU sleeps when it isn't earning its keep, and an idle device
suspends-to-RAM instead of cooking in your bag. "Minimal" describes the *experience*; underneath,
substantial engineering earns those thermals — but never a new user-facing feature that only adds weight.

> Zero keeps MinUI's whole appliance-like feel — the same launcher, the same clean SD card, the same
> "just play" simplicity. It only changes what's *underneath*.

## What Zero adds — all measured on-device, not assumed

- **❄️ GPU-dark menu** — the launcher renders in software straight to the framebuffer, so the PowerVR
  GPU power domain *suspends* while you browse. Cooler **and** snappier (measured ~26°C, instant nav).
- **🌡️ Closed-loop governor** — a frame-aware controller caps the kernel `schedutil` governor at the
  lowest clock that still holds frame rate (race-to-idle aware). ~4–5°C cooler than stock; **never
  overclocks** (capped at the verified-stock 1.8 GHz OPP).
- **💤 Deep-sleep** — real suspend-to-RAM (ported from [zhaofengli](https://github.com/zhaofengli/MinUI)),
  so an idle device goes genuinely *cold* (measured 33→27°C) and resumes where you left off instead of
  powering off. On by default; the **Deep Sleep** tool (Extras) turns it off.
- **🔋 Radios & LEDs off** — MinUI has no networking, so wifi/Bluetooth and the ambient RGB LEDs are off
  by default. Less idle drain and heat, zero features lost.
- **⚙️ Tuned cores + pacing** — stock cores rebuilt `-O3` and pinned to reproducible revisions;
  drift-free absolute-schedule frame pacer.
- **🛡️ Robustness** — bails cleanly on a bad/unsupported ROM instead of hanging; handles cores that
  change resolution/timing mid-run without desyncing pacing.

Measured battery: **~7.5 hours** on Game Boy — up from ~6 before the optimization pass
(60-min `charge_counter` drain, same conditions).

## Scope — TrimUI Brick + Smart Pro only

Zero is a **single-platform fork.** The whole thesis is A133P-specific (cpufreq / OPP / thermal / PMIC),
so only `tg5040` is built and supported. The other MinUI platforms are frozen (present for history and
upstream merges, not built). Deliberate: depth over breadth.

## What Zero deliberately does *not* add

No boxart, WiFi/NTP, Pak Store, RetroAchievements, ambient-LED effects, shaders, overlays, or themes.
If a change adds heat, idle power, or resident memory without earning it, it doesn't ship — and we hold
that line with *measurement*, not taste. (We built a GPU-dark **game** renderer, measured it as a
break-even power wash against GLES, and shelved it. Measured, not assumed.)

## Retained MinUI features

- Simple launcher, simple SD card — no settings, no configuration, no distractions
- Consistent in-emulator menu: save states, disc changing, emulator options
- Auto-sleep on idle or POWER; auto-resume right where you left off
- Streamlined libretro frontend (`minarch` + cores)

## Supported consoles

**Base:** Game Boy · Game Boy Color · Game Boy Advance · NES · SNES · Sega Genesis · PlayStation
**Extras:** Sega Game Gear · Sega Master System · and the other retained MinUI extra cores.

## Install

Grab the latest `MinUI-*-base.zip` from Releases.
- **Fresh install:** extract to a blank SD card.
- **Update:** drop the zip on the SD card root and reboot — it self-applies.

## Credits

Zero stands entirely on [**MinUI** by Shaun Inman](https://github.com/shauninman/MinUI) — the launcher,
the frontend, the philosophy. It also borrows from the community fork scene: **deep-sleep** from
[zhaofengli](https://github.com/zhaofengli/MinUI), the lean **software-render** path referenced from
[MyMinUI](https://github.com/Turro75/MyMinUI), and governor/suspend **evidence** from
[NextUI](https://github.com/LoveRetro/NextUI). See `THIRD_PARTY_NOTICES.md`. MinUI Zero is an
independent fork, not affiliated with or endorsed by any of them.

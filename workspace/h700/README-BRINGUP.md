# h700 — Anbernic RG35XX Plus / RG35XX-H bring-up

Platform for the Allwinner H700 (sun50iw9) Anbernic line. ONE platform serves both owned devices —
muOS's device trees confirm they are near-twins (same SoC, panel, WiFi; the H adds analog sticks
and a horizontal shell). Started 2026-08-04, the evening the RG35XX Plus was first probed.
Recon dossier with raw captures: `.notes/2026-08-04-rg35xx-recon/DOSSIER.md`.

## Measured / verified device facts (do not re-derive)

- SoC sun50iw9, 4x Cortex-A53 aarch64, 1 GB RAM. tg5040 toolchain family — the recon binaries were
  built with the tg5040 docker image (`/opt/aarch64-linux-gnu`), statically, and ran.
- **Panel: 640x480 @ 59.9777 Hz MEASURED** (3x600-pan panelprobe runs agreeing to 4 decimals; the
  fbdev mode line says "59" and is wrong). 32bpp, stride 2560, virtual 640x960 = 2 pages,
  `FBIOPAN_DISPLAY` with `FB_ACTIVATE_VBL` accepted. First pixels drawn 2026-08-04 (hellofb).
- Governors include **schedutil** -> the tg5040 hybrid governor model ports unchanged.
  OPP: 480/720/936/1008/1104/1200/1320/1416/1512 MHz. Brackets UNMEASURED — receipts first.
- PMIC **AXP2202, identical sysfs to the Brick**: `/sys/class/power_supply/axp2202-battery`
  (capacity/status/voltage_now; current_now reads EMPTY, treat as absent) + `axp2202-usb`.
  tg5040's PLAT_getBatteryStatus ports as-is.
- `/sys/power/state` = `freeze mem` -> suspend-to-RAM exists; deep sleep is a live candidate.
- Thermal zones: cpu/gpu/ve/ddr + battery temp.
- Audio: plain ALSA (`audiocodec` card 0). No vendor daemon.
- Input: `muOS-Keys` (event1, kbd+js0). Under muOS the kbd side emits the classic Anbernic layout
  (from muOS's own InputAutoCfg, SDL keysym convention): dpad=W/A/S/D, A=LSHIFT, B=LCTRL, X=Z? — 
  wait: Z-trig=z, L=x, R=c, C-buttons=I/J/K/L (right stick), Start=RETURN, X/Y unlisted there.
  VERIFY ON DEVICE before trusting minor buttons; dpad/A/B/Start are the load-bearing ones.
  NOTE: this is muOS's mapping of its OWN kernel; a MinUI Zero image may see raw gpio-keys codes
  instead. Re-capture evdev codes once we run outside muOS.
- WiFi RTL8821CS. Power-save DROPS INBOUND UNICAST — `iw dev wlan0 set power_save off` first, every
  session. Device must be pinned to 2.4 GHz at the router (5 GHz link goes one-way at range).
- muOS sshd needs a pty: `ssh -tt` or all output is silently dropped. root/root, key auth persists.

## Present-path decision (the important one)

**Destination: the Allwinner DE layer API via `/dev/disp` (`disp_layer_config2`).** MyMinUI's h700
platform implements exactly this — hardware layer scaling with no GPU and no CPU scale, the path
our tg5040 docs shelved as "research" because the Brick's kernel never exposed it. Here it is
exposed AND has a working reference (`git show mymin/main:workspace/h700/platform/platform.c`,
note its `sunxi_display2.h` structs and the config2 ioctls). This is the thesis-perfect present.

**v0 dev path may be simpler**: raw fbdev (mmap + pan, proven by panelprobe/hellofb) or device SDL2
(2.28.5 + ttf + image on muOS, Mali fbdev-EGL, no /dev/dri). Upstream's frozen rg35xxplus platform
used SDL2 window+renderer against the STOCK OS; on muOS the same approach is plausible for a dev
loop but keeps the GPU lit — fine for bring-up, not for shipping. Do not let v0 ossify into v1.

## Recovery + hosted-dev safety (learned the hard way, 2026-08-05)

- **There is a RESET BUTTON under the power button.** Hardware reset, works when everything else is
  frozen. This is the recovery of last resort and it makes the device effectively unbrickable at
  the process level.
- A dev session freeze happened once: the ssh link died mid-window, the restore never ran, and the
  device resumed with muxfrontend dead and frontend.sh SIGSTOPped — no input handler, so it looked
  hard-frozen, and the short power press (handled by the frozen userspace) did nothing.
- **RULE: never freeze/kill host daemons without a DEAD-MAN'S SWITCH on the device** — before
  stopping anything, start a detached script that restores the frontend after N minutes
  unconditionally. The restore must not depend on the ssh link surviving. Not yet implemented;
  required before the next hosted run.

## Dev loop (proven tonight)

1. Build: `docker run --rm -v "$PWD:/w" tg5040-toolchain /bin/bash -c \
   '/opt/aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc -O2 -static -o /w/out /w/in.c'`
2. Ship: `cat binary | ssh -tt -i ~/.ssh/tg5040_dev root@<ip> 'cat > /tmp/x && chmod +x /tmp/x'`
3. Clean window: `kill -STOP <muxfrontend pid>` … run … `kill -CONT <pid>`. muOS is the dev
   harness; no boot integration needed until the platform layer is real.

## Boot model (decided 2026-08-03, ledger)

Image-native like muOS/Knulli (Allwinner BROM boots SD first; stock stays untouched internally).
Knulli's h700 board config is the image-tooling reference. This is LATER work — everything until
then runs hosted inside muOS.

## Audio (SOLVED 2026-08-05)

muOS's **pipewire owns the audio hardware** (it holds the hw PCM; /etc/asound.conf routes ALSA
"default" through the pipewire plugin). SDL's alsa driver works through it IF the environment
carries `XDG_RUNTIME_DIR=/run` + `PIPEWIRE_RUNTIME_DIR=/run` (socket at /run/pipewire-0); without
them SDL_OpenAudio fails "Host is down". Device SDL2 ships ONLY the alsa audio backend (no native
pipewire/pulse). Verified: 32768Hz stream opened via the normal pak launch flow. Volume is
pipewire's for now (msettings stubbed). On our own image WE own audio and none of this applies.

HOSTED-DEV SESSION HYGIENE (each cost real debugging time): only ONE session.sh may run — stacked
sessions answer injected input with STALE launchers (kill all before starting); busybox sed with
backslash-n in replacements corrupts scripts — WRITE files whole, never sed shell scripts; injected
input must be HUMAN-TIMED (press blob, sleep, release blob) or same-poll press+release cancels
justRepeated; the dead-man resurrection mid-test is indistinguishable from a rendering bug — check
`pidof muxfrontend` before trusting any capture.

## Order of work

1. `platform.h` constants + input verification on device (evdev capture while pressing buttons).
   BLOCKED FINDING (2026-08-04): naive `cat /dev/input/event*` captures return ZERO bytes even with
   muxfrontend SIGSTOPped — all three nodes at once, including the polled device, which points at
   an exclusive EVIOCGRAB held by the frontend (a frozen process keeps its grab). Next attempt:
   kill muxfrontend outright and capture during the respawn gap, or read the gpio-keys codes out of
   the device tree. Note this only matters for the muOS-hosted dev loop — our own image owns input.
2. `platform.c` v0: fbdev present (2-page, sync pan first — MyMinUI model; add the async ownership
   machine only if a measured stall demands it, and then with the MMP's state machine), evdev
   input, AXP2202 battery (port from tg5040), stub audio.
3. minui.elf drawing menus, launched over ssh inside muOS.
4. Governor port (schedutil ceiling model) + per-OPP receipts.
5. DE-layer present (MyMinUI reference) — measure vs fbdev before adopting.
6. minarch + one core; panel rate 59.9777 wired into the rate match from day one.
7. Deep sleep probe (`echo mem`), battery/thermal receipts, image packaging (Knulli reference).

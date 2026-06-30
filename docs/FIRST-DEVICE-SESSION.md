# First device session — one bring-up run that unblocks everything

Everything built so far is cross-compiled and safe-by-construction but **unvalidated on real
hardware**. This is the sequenced ~30–45 min session that turns the whole pile into measured
reality. Run it on the `integration` branch (governor + deep-sleep + benchmark + undervolt
scaffold, all building together). One session feeds **four** workstreams at once.

Prereqs: a TrimUI Brick, the SD card, SSH to the device (MinUI runs from SD + SSH), Docker
running locally for `make tg5040`.

---

## 0. Build + get it on the device (~10 min)
```sh
make tg5040                  # builds the integration branch -> ./releases/*.zip
# flash the zip to the SD per MinUI install, OR for fast iteration scp just the binary:
scp workspace/all/minarch/build/tg5040/minarch.elf  root@<brick>:/mnt/SDCARD/.system/tg5040/bin/
scp tools/brick-recon.sh                            root@<brick>:/tmp/
scp tools/bench-run.sh                              root@<brick>:/tmp/
```

## 1. Recon — idle, then under load (~5 min) → unblocks the governor + undervolt
```sh
# on the brick, at the menu (idle):
sh /tmp/brick-recon.sh | tee /tmp/recon-idle.txt
# then launch a demanding game (PS1) and, over SSH, during gameplay:
sh /tmp/brick-recon.sh | tee /tmp/recon-load.txt
```
Pull both files back and read the sections. **Record these and replace the ASSUMED values:**
| Recon line | Fills | In |
|---|---|---|
| `scaling_available_frequencies` | real OPP ladder → `GOV_STEP_KHZ`, the **verified-stock max** (replaces assumed 1.8GHz `GOV_STOCK_MAX_KHZ`; never 2.0GHz) | `governor.c`, the pak `launch.sh` FMAX, `boot.sh` cap |
| `scaling_available_governors` | does **schedutil** exist? (hybrid governor depends on it) | confirm `boot.sh` |
| `thermal_zone*/type` | the CPU zone | `GOV_T_SENSOR`, `TLM_TEMP_PATH` |
| `power_supply/*/voltage_now`,`current_now` | battery telemetry paths/sign | `TLM_VOLT_PATH`/`TLM_CURR_PATH` |
| regulator `microvolts` **perms** + OPP table | **undervolt verdict**: writable → feasible; all read-only → needs a DTB | `docs/undervolt-spike-design.md`, flip `PLAT_supportsUndervolt` only if feasible |

## 2. Benchmark baseline (~10 min) → unblocks "is it actually cooler?"
Run a **fixed, deterministic scene** (attract mode / a recorded demo) per system so runs compare:
```sh
# on the brick (BENCH=1 is set by bench-run.sh):
sh /tmp/bench-run.sh "$CORES_PATH/fceumm_libretro.so"        <nes-rom>  120 nes
sh /tmp/bench-run.sh "$CORES_PATH/snes9x2005_plus_libretro.so" <snes-rom> 120 snes
sh /tmp/bench-run.sh "$CORES_PATH/pcsx_rearmed_libretro.so"   <ps1-rom>  120 ps1
# pull the CSVs back, then on the host:
tools/bench-analyze.py /tmp/bench-ps1.csv         # first real mJ/frame, p95/p99, thermals
```
This is the baseline every later change A/B's against (`bench-analyze.py before.csv after.csv`).

## 3. Validate the governor → the flagship
- Start a PS1 game from cold; watch `scaling_cur_freq` + `thermal_zone` over SSH. The ceiling
  should **climb to hold frame rate, then settle at the lowest stable OPP**; temp should plateau
  **below `GOV_T_CEIL_C` (72°C)**.
- Confirm `schedutil` drops the clock in light scenes/menus (cooler than a pin).
- Confirm **no hunting** (visible/audible frame wobble). If it hunts, raise `GOV_DN_DWELL`/`GOV_STEP_KHZ`.
- A/B: `GOV_DISABLE=1` (stock) vs governor on, same scene → `bench-analyze.py` must show **lower
  mJ/frame and peak temp with no pacing regression** (the release gate).
- Light systems (NES/GB) should sink toward `f_min` quickly.

## 4. Validate deep sleep
- Idle past `DEEP_SLEEP_DELAY` (120s) → confirm it suspends (`echo mem`; logs "suspending to
  RAM"/"returned from suspend"), wakes on the power button, and **doesn't immediately re-sleep**
  (the resume debounce).
- Confirm `skeleton/SYSTEM/tg5040/bin/suspend` service names match the Brick (`wpa_supplicant`,
  `wlan0`, `/sys/class/rfkill/rfkill0`, bluetooth) — and that suspend isn't refused (`EBUSY`)
  with wifi configured-but-no-AP.
- Measure idle power before/after deep sleep; tune `DEEP_SLEEP_DELAY` if shorter is meaningfully
  cooler without hurting resume.
- Stress: ~20 suspend/resume cycles, verify save/state integrity each time.

## 5. Undervolt verdict (from step 1)
- If a CPU-rail regulator `microvolts` (or an OPP voltage) is **writable**: a bounded runtime
  undervolt is feasible — scope the per-tier validation (soak each offset with the benchmark +
  a crash counter; keep only tiers that pass every gate) before flipping `PLAT_supportsUndervolt`.
- If everything is **read-only**: record that undervolt needs a custom DTB; leave the spike OFF.

## 6. Close the loop — record real values
Update the design docs' ASSUMED placeholders with the measured values, tick
`docs/ON-DEVICE-CHECKLIST.md`, and commit the recon outputs (`recon-idle.txt`, `recon-load.txt`)
so the numbers are sourced from hardware, not guesses. Then the next features (audio/pacing,
reliability, undervolt-if-feasible) are driven by data — including the honest possibility that
the biggest remaining lever is the **backlight**, not the CPU (measure it here too).

---
**One session, four unlocks:** real OPP ladder (governor cap + undervolt verdict) · schedutil/
thermal/battery paths confirmed · first mJ/frame baseline · governor + deep-sleep proven on
silicon. Everything after this is measured, not assumed.

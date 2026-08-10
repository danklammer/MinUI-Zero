# Pak compatibility — running the community's paks on MinUI Zero / MinOS

**Goal (Dan, 2026-08-10): keep the core lean, let users opt into features, and use the pak
ecosystem that already exists.** NextUI and MinUI pak authors have shipped tools and emulator
paks for years. MinUI Zero inherits the same pak contract by heritage, so most of that work
runs here unmodified — features arrive as *files a user chooses to install*, never as weight in
the base image.

## The contract (what a pak can rely on)

A pak is a folder named `<TAG>.pak` containing `launch.sh`. Emulator paks live in `Emus/`, tool
paks in `Tools/`, both under a **platform folder** at the card root. Roms map to paks by the
parenthesised tag on the rom's parent folder (`Roms/Game Boy Color (GBC)/` → `GBC.pak`).

Resolution order (`getEmuPath`, `workspace/all/common/utils.c`; `hasEmu` + Tools scan,
`workspace/all/minui/minui.c`):

1. `<card>/Emus/<PLATFORM>/<TAG>.pak/launch.sh` — our platform name
2. `<card>/Emus/<PLATFORM_ALIAS>/<TAG>.pak/launch.sh` — the scene's name, when they differ
3. `<card>/.system/<PLATFORM>/paks/Emus/<TAG>.pak/launch.sh` — shipped paks

### Platform folder names

| Device | MinOS platform | Scene folder | Needs alias |
|---|---|---|---|
| TrimUI Brick / Smart Pro | `tg5040` | `tg5040` | no — identical |
| Miyoo Mini Plus | `miyoomini` | `miyoomini` | no — identical |
| Anbernic RG35XX Plus/H (+ H700 family) | `h700` | `rg35xxplus` | **yes** (`PLATFORM_ALIAS`) |

A new port declares `#define PLATFORM_ALIAS "<scene name>"` in its `platform.h` **only when the
scene publishes under a different name**; internal identifiers are never renamed for branding
(upstream-merge cleanliness). That one define is the entire wiring.

### Environment a pak receives

`SDCARD_PATH`, `SYSTEM_PATH`, `USERDATA_PATH`, `SHARED_USERDATA_PATH`, `LOGS_PATH`, `SAVES_PATH`,
`BIOS_PATH`, `CHEATS_PATH`, `CORES_PATH`, `PLATFORM`, `DEVICE`, `LD_LIBRARY_PATH`.

`DEVICE` is the sub-device discriminator (h700: `plus` / `h`), consumed by minarch
(`config.device_tag`) and by paks that vary per model. Paks may ignore it.

### Runtime helpers present on the h700 image

`jq`, `curl`, `wget`, `unzip`, `zip`, `tar`, `gzip`, `openssl`, `sed`, `awk`, `grep`, `find`,
`xargs`, `ping`. **Absent by design:** `python3` and `dialog` (stripped — see
`tools/build-h700-stripped.sh`). A pak needing those should bundle them or ship as an opt-in
compat pak; they do not belong in a base image whose whole thesis is running cold.

Verify on any target before promising compatibility — the donor OS decides this, not us.

## What does NOT come for free

Pak compatibility is a *launching* contract. NextUI features implemented **inside its launcher
and minarch** do not arrive with a pak:

- box art / game art display
- RetroAchievements
- the Pak Store client UI
- shader/overlay pipelines

Each of those is a real port, judged against `docs/project-direction.md` (user-facing features
are weight). The rule this fork uses:

> **If it can be a pak or a hook, it ships as a pak or a hook. Only what cannot be either may
> ask to live in the core — and then it has to earn it.**

Standalone-emulator paks bundle their own binaries and link against donor-OS libraries. Our
strip pass may have removed a library a stock device has. Policy: test, and if a popular pak
needs a stripped library, ship an opt-in compat pak rather than fattening the base image.

## Testing a pak (the check before claiming support)

1. Copy the pak to `Emus/<scene folder>/` or `Tools/<scene folder>/` on the card.
2. Confirm it appears (Tools) or that a matching rom folder launches it (Emus).
3. Check `$LOGS_PATH/<TAG>.txt` for missing-binary or missing-library errors.
4. For emulator paks: verify resume, quicksave/auto-resume, and that MENU still works.

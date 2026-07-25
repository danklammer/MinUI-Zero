# miyoomini (Miyoo Mini Plus) — SigmaStar SSD202D, dual Cortex-A7, ARMv7ve, NEON-VFPv4.
#
# Only the cores this device actually ships are built. The Emus paks map to exactly six:
#   FC->fceumm  GB/GBC->gambatte  GBA->gpsp  MD->picodrive  PS->pcsx_rearmed  SFC/SUPA->supafaust
# snes9x2005_plus is deliberately NOT built: SFC.pak moved to supafaust, so nothing loads it and
# building it is pure time. tg5040's extra systems (PCE/VB/GG/SMS/MGBA/P8) have no paks here.
CORES = fceumm gambatte gpsp pcsx_rearmed picodrive mednafen_supafaust

###############################

# optional core vars
# *_REPO=
# *_HASH=
# *_CORE=
# *_FLAGS=
# *_MAKEFILE=
# *_BUILD_PATH=

###############################
# PINNED core versions. These are the SAME tested-working commits tg5040 ships (pinned 2026-06-30),
# reused deliberately: they are already validated in this fork, and the tg5040 patches apply
# cleanly to them — which is what the miyoomini patches are generated from
# (tools/gen-miyoomini-core-patches.sh).
#
# Before this, miyoomini pinned NOTHING — `git clone --depth 1` of whatever HEAD happened to be —
# so the build was unreproducible AND the inherited upstream patches had drifted out of applying
# at all (fceumm failed first). Setting *_HASH switches the fetch to a full clone + checkout.
fceumm_HASH             = 6e00afac498903586330492cdd81354a6c4c0d4c
gambatte_HASH           = fc59959a30b40d74875f18ac4bc617d81b42d782
gpsp_HASH               = 69e86ebe89f14c3f5f75b809c12c0a953b3d6ce4
pcsx_rearmed_HASH       = 050981b6eeb715f142854f57c68086f62921f027
picodrive_HASH          = 9e011174842a098a99290622b14db7ffafa717d1
mednafen_supafaust_HASH = 2b93c0d7dff5b8f6c4e60e049d66849923fa8bba

###############################

# The miyoomini toolchain image is amd64-only and therefore runs emulated on an arm64 host, where
# gcc 8.3 intermittently segfaults mid-compile at -j4 (hit on picodrive tremor/mdct.c; the same
# command succeeded serially immediately after). Trade a little wall-clock for a build that
# actually finishes every time.
PROCS = -j2

###############################

fceumm_REPO = https://github.com/libretro/libretro-fceumm

gambatte_REPO = https://github.com/libretro/gambatte-libretro

mednafen_supafaust_REPO = https://github.com/libretro/supafaust

pcsx_rearmed_MAKEFILE = Makefile.libretro

picodrive_REPO = https://github.com/irixxxx/picodrive
picodrive_MAKEFILE = Makefile.libretro
# picodrive recurses into cpu/cyclone to GENERATE Cyclone.s (the ARM32 M68K JIT). That sub-make
# mishandles the jobserver and dies with "write jobserver: Bad file descriptor" under any -jN,
# deterministically. Serial for this core only — the ARM32 Cyclone/SH2 DRC path is worth the wait,
# it is the one big win this device has that the aarch64 Brick cannot use.
picodrive_PROCS = -j1
# Upstream tracks two prebuilt GP2X objects that its broad clean rule deletes; irrelevant here.
picodrive_VERIFY_IGNORE = platform/gp2x/warm_2.4.25.o platform/gp2x/warm_2.4.26-open2x.o

###############################

include ../../all/cores/makefile

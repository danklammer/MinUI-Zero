# miyoomini (Miyoo Mini Plus) — SigmaStar SSD202D, dual Cortex-A7, ARMv7ve, NEON-VFPv4.
#
# The core set is deliberately IDENTICAL to tg5040's, including cores no pak on this device loads
# yet (snes9x2005_plus, mednafen_pce_fast, mednafen_vb, mgba, fake-08). Keeping the two platforms
# on one list means a system added to the Miyoo later needs no core work, and it removes the class
# of bug where a pak references a core this platform silently never built — SFC.pak was switched to
# supafaust purely because snes9x2005_plus was missing here, and kept a comment describing the core
# it no longer loaded.
CORES = fceumm gambatte gpsp pcsx_rearmed picodrive snes9x2005_plus
CORES+= mednafen_pce_fast mednafen_vb mednafen_supafaust mgba fake-08

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
snes9x2005_plus_HASH    = b60356971fc9caae02cd0853676dced886a08be7
mednafen_pce_fast_HASH  = 4727b96c10f50405167d98d364844469307fcc41
mednafen_vb_HASH        = 38e7a0ec9ac7079ca1c1e3dd9aaf5b56f527efca
mgba_HASH               = 6dce57eef127dc4cc292644f38196e0e7c58590c
fake-08_HASH            = 814991a2571ad3970e386cef48f3b148aa1c27b9

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
snes9x2005_plus_REPO = https://github.com/libretro/snes9x2005
snes9x2005_plus_FLAGS = USE_BLARGG_APU=1
mednafen_pce_fast_REPO = https://github.com/libretro/beetle-pce-fast-libretro
mednafen_vb_REPO = https://github.com/libretro/beetle-vb-libretro
fake-08_REPO = https://github.com/jtothebell/fake-08
fake-08_CORE = fake08_libretro.so
fake-08_BUILD_PATH = fake-08/platform/libretro
# picodrive recurses into cpu/cyclone to GENERATE Cyclone.s (the ARM32 M68K JIT). That sub-make
# mishandles the jobserver and dies with "write jobserver: Bad file descriptor" under any -jN,
# deterministically. Serial for this core only — the ARM32 Cyclone/SH2 DRC path is worth the wait,
# it is the one big win this device has that the aarch64 Brick cannot use.
picodrive_PROCS = -j1
# -j1 alone is not enough, because the outer make exports MAKEFLAGS and the recursive sub-make links
# before platform/libretro/libretro.o exists (undefined lprintf/rfread/rfseek) — it builds fine
# standalone and via `make picodrive`, and ONLY fails inside the full platform build, which is what
# made this take so long to see. That isolation is the template default in ../../all/cores/makefile
# and covers every core, so only the -j1 above is picodrive-specific (2026-08-14).
# Upstream tracks two prebuilt GP2X objects that its broad clean rule deletes; irrelevant here.
picodrive_VERIFY_IGNORE = platform/gp2x/warm_2.4.25.o platform/gp2x/warm_2.4.26-open2x.o

###############################

include ../../all/cores/makefile

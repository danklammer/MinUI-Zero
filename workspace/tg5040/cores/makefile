CORES = fceumm gambatte gpsp pcsx_rearmed picodrive snes9x2005_plus
CORES+= mednafen_pce_fast mednafen_vb mednafen_supafaust mgba # skipped (upstream drift): fake-08, pokemini, race

###############################

# optional core vars
# *_REPO=
# *_HASH=
# *_CORE=
# *_FLAGS=
# *_MAKEFILE=
# *_BUILD_PATH=

###############################
# PINNED core versions — tested-working HEADs (2026-06-30). Setting *_HASH switches the fetch to a
# full clone + `git checkout` (see all/cores/makefile), locking reproducible, regression-free thermal
# behavior — an unpinned upstream commit could silently raise CPU cost or break frame pacing.
fceumm_HASH             = 6e00afac498903586330492cdd81354a6c4c0d4c
gambatte_HASH           = fc59959a30b40d74875f18ac4bc617d81b42d782
gpsp_HASH               = 69e86ebe89f14c3f5f75b809c12c0a953b3d6ce4
pcsx_rearmed_HASH       = 050981b6eeb715f142854f57c68086f62921f027
picodrive_HASH          = 9e011174842a098a99290622b14db7ffafa717d1
snes9x2005_plus_HASH    = b60356971fc9caae02cd0853676dced886a08be7
mednafen_pce_fast_HASH  = 4727b96c10f50405167d98d364844469307fcc41
mednafen_vb_HASH        = 38e7a0ec9ac7079ca1c1e3dd9aaf5b56f527efca
mednafen_supafaust_HASH = 2b93c0d7dff5b8f6c4e60e049d66849923fa8bba
mgba_HASH               = 6dce57eef127dc4cc292644f38196e0e7c58590c

mednafen_pce_fast_REPO = https://github.com/libretro/beetle-pce-fast-libretro
mednafen_vb_REPO = https://github.com/libretro/beetle-vb-libretro

fake-08_REPO = https://github.com/jtothebell/fake-08
fake-08_CORE = fake08_libretro.so
fake-08_BUILD_PATH = fake-08/platform/libretro

fceumm_REPO = https://github.com/libretro/libretro-fceumm

gambatte_REPO = https://github.com/libretro/gambatte-libretro

mednafen_supafaust_REPO = https://github.com/libretro/supafaust

pcsx_rearmed_MAKEFILE = Makefile.libretro

picodrive_REPO = https://github.com/irixxxx/picodrive
picodrive_MAKEFILE = Makefile.libretro
# Upstream tracks two prebuilt GP2X objects and its broad clean rule deleted them before
# this release audit. They are irrelevant to tg5040; ignore only those exact non-source paths
# while old dependency checkouts converge to the patched clean rule.
picodrive_VERIFY_IGNORE = platform/gp2x/warm_2.4.25.o platform/gp2x/warm_2.4.26-open2x.o
# NOTE: no ARCH override — picodrive's M68K JIT (Cyclone) is ARM32-only, so aarch64 uses the FAMEC C
# core regardless; ARCH=aarch64 was verified byte-identical (SH2 DRC + NEON draw already on). Inert.

pokemini_REPO = https://github.com/libretro/PokeMini
pokemini_MAKEFILE = Makefile.libretro

snes9x2005_plus_REPO = https://github.com/libretro/snes9x2005
snes9x2005_plus_FLAGS = USE_BLARGG_APU=1

###############################

include ../../all/cores/makefile

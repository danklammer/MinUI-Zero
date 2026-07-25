#!/bin/bash
# Generate workspace/miyoomini/cores/patches/*.patch from the tg5040 patches, retargeted to
# Cortex-A7. RUNS INSIDE THE TOOLCHAIN CONTAINER (needs git + network).
#
# WHY generate instead of hand-editing:
#   The inherited miyoomini patches were upstream MinUI's, unpinned and drifted out of applying
#   (fceumm failed first). Our tg5040 patches DO apply to the pinned commits and carry this fork's
#   own fixes -- e.g. fceumm's GetKeyboard out-of-bounds read (a 1-byte literal was being read as
#   256 bytes of keyboard state every IRQ tick) and -O2 -> -O3 on the generic path. Deriving the
#   miyoomini patch from them inherits those fixes instead of silently losing them.
#
#   Emitting via `git diff` against the real pinned tree means the patch is CORRECT BY
#   CONSTRUCTION -- hunk offsets and line counts come from git, not from hand arithmetic.
#
# The tree is reset afterwards so the normal build can apply the generated patch to a clean clone.
set -e

CORES=/root/workspace/miyoomini/cores
TG=/root/workspace/tg5040/cores/patches
cd "$CORES"
mkdir -p src patches

ARCHFLAGS='-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm'

# Every one of these cores builds with -flto, which means the OBJECT FILES ARE GIMPLE BYTECODE and
# real code generation happens at LINK time. Arch flags in CFLAGS alone are therefore not enough:
# the link is driven by a bare `gcc -shared ... $(LDFLAGS)`, so codegen falls back to the
# compiler's default arch.
#
# MEASURED: with the flags only in CFLAGS, fceumm and gambatte came out
#   Tag_CPU_name: "7-A"   Tag_FP_arch: VFPv3
# i.e. GENERIC ARMv7-A with no fused multiply-add -- strictly worse than the foreign binaries we
# were replacing, which correctly reported Cortex-A7. Putting the same arch flags on LDFLAGS is
# the documented fix, and there is precedent in these very Makefiles: the cortex-a35 platform in
# fceumm's Makefile.libretro does exactly this.
inject_ldflags() { # $1 = makefile to patch
  awk -v flags="$ARCHFLAGS" '
    {print}
    /else ifeq \(\$\(platform\), miyoomini\)/ {print "\tLDFLAGS += " flags}
  ' "$1" > /tmp/ld.$$
  cat /tmp/ld.$$ > "$1"
}

# Several of these cores append a GENERIC release block -- `CFLAGS += -O2 -DNDEBUG` -- *after* all
# the platform blocks, so it silently overrides whatever optimisation level the platform chose.
#
# MEASURED, and this is why it matters: gambatte built at -O2 (every other core landed -O3/-Ofast)
# and GBC regressed from 59.9 to 58.9 fps with min 55.3 -> 49.0, reproducibly across two runs, vs
# the foreign binary it replaced. The A53 in the Brick absorbs -O2; this A7 does not.
#
# This fork already fixes exactly this pattern in the tg5040 fceumm patch; doing it generically
# here means the next core added can't quietly reintroduce it.
bump_o2_to_o3() { # $1 = makefile to patch
  sed 's/-O2 -DNDEBUG/-O3 -DNDEBUG/g' "$1" > /tmp/o3.$$
  cat /tmp/o3.$$ > "$1"
}

repo_for() { case "$1" in
  fceumm)             echo https://github.com/libretro/libretro-fceumm ;;
  gambatte)           echo https://github.com/libretro/gambatte-libretro ;;
  gpsp)               echo https://github.com/libretro/gpsp ;;
  pcsx_rearmed)       echo https://github.com/libretro/pcsx_rearmed ;;
  picodrive)          echo https://github.com/irixxxx/picodrive ;;
  mednafen_supafaust) echo https://github.com/libretro/supafaust ;;
esac; }

hash_for() { case "$1" in
  fceumm)             echo 6e00afac498903586330492cdd81354a6c4c0d4c ;;
  gambatte)           echo fc59959a30b40d74875f18ac4bc617d81b42d782 ;;
  gpsp)               echo 69e86ebe89f14c3f5f75b809c12c0a953b3d6ce4 ;;
  pcsx_rearmed)       echo 050981b6eeb715f142854f57c68086f62921f027 ;;
  picodrive)          echo 9e011174842a098a99290622b14db7ffafa717d1 ;;
  mednafen_supafaust) echo 2b93c0d7dff5b8f6c4e60e049d66849923fa8bba ;;
esac; }

for C in fceumm gambatte gpsp pcsx_rearmed picodrive mednafen_supafaust; do
  R=$(repo_for "$C"); H=$(hash_for "$C")
  echo "=============================================================="
  echo "$C  @ ${H:0:10}"
  echo "=============================================================="

  if [ ! -d "src/$C/.git" ]; then
    rm -rf "src/$C"
    git clone -q --recursive "$R" "src/$C"
  fi
  # A previous build may have left a `--depth 1` shallow clone, which does NOT contain the pinned
  # commit ("reference is not a tree"). Deepen on demand rather than silently building the wrong
  # source.
  if ! ( cd "src/$C" && git cat-file -e "$H^{commit}" 2>/dev/null ); then
    echo "  pinned commit absent (shallow clone) — deepening"
    ( cd "src/$C" && git fetch -q --unshallow 2>/dev/null || git fetch -q origin "$H" 2>/dev/null || true )
    ( cd "src/$C" && git cat-file -e "$H^{commit}" 2>/dev/null ) || {
      echo "  re-cloning $C in full"; rm -rf "src/$C"; git clone -q --recursive "$R" "src/$C"; }
  fi
  ( cd "src/$C" && git checkout -q --detach "$H" )
  ( cd "src/$C" && git submodule -q update --init --recursive 2>/dev/null || true )
  ( cd "src/$C" && git reset -q --hard && git clean -qfd )

  # gpsp is the one core whose upstream Makefile ALREADY defines a `miyoomini` platform, and it is
  # a good block: -Ofast, -flto=4 -fwhole-program, dynarec + NEON, A7 flags. Appending a second
  # `else ifeq ($(platform), miyoomini)` would be DEAD CODE (first match in the chain wins), so we
  # repair the existing block in place instead of deriving from tg5040.
  # Upstream MinUI's miyoomini patch replaced this block wholesale and in doing so dropped -Ofast
  # AND all the LTO flags — a silent optimisation regression on the heaviest system this device
  # runs. Its three actual defects are fixed here and nothing else is touched:
  #   1. TARGET emitted `gpsp_plus_libretro.so`; the pak loads `gpsp_libretro.so`
  #   2. CC/CXX/AR hardcode /opt/miyoomini-toolchain/usr/bin/arm-linux-*, which does not exist in
  #      our SDL2 image (its prefix is arm-linux-gnueabihf-)
  #   3. -mtune=cortex-a7 with no -march leaves the arch at the toolchain default
  if [ "$C" = "gpsp" ]; then
    sed \
      -e 's|^\([[:space:]]*\)TARGET := \$(TARGET_NAME)_plus_libretro\.so|\1TARGET := $(TARGET_NAME)_libretro.so|' \
      -e 's|^\([[:space:]]*\)CC = /opt/miyoomini-toolchain/usr/bin/arm-linux-gcc|\1CC = $(CROSS_COMPILE)gcc|' \
      -e 's|^\([[:space:]]*\)CXX = /opt/miyoomini-toolchain/usr/bin/arm-linux-g++|\1CXX = $(CROSS_COMPILE)g++|' \
      -e 's|^\([[:space:]]*\)AR = /opt/miyoomini-toolchain/usr/bin/arm-linux-ar|\1AR = $(CROSS_COMPILE)ar|' \
      -e 's/-marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard/-marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard/' \
      src/gpsp/Makefile > /tmp/mk.$$
    cat /tmp/mk.$$ > src/gpsp/Makefile
    inject_ldflags src/gpsp/Makefile
    bump_o2_to_o3 src/gpsp/Makefile
    ( cd "src/$C" && git diff > "$CORES/patches/$C.patch" )
    echo "  wrote patches/$C.patch ($(wc -l < "patches/$C.patch") lines) [repaired upstream block]"
    ( cd "src/$C" && git reset -q --hard && git clean -qfd )
    continue
  fi

  # Start from the tg5040 patch so this fork's non-platform fixes come along.
  ( cd "src/$C" && git apply --unidiff-zero "$TG/$C.patch" )

  # Retarget the platform block: tg5040/Cortex-A53/arm64  ->  miyoomini/Cortex-A7/arm.
  # Only the makefiles are rewritten; C-source fixes from the tg5040 patch are left untouched.
  # NOTE: `sed -i` fails on this bind mount ("couldn't open temporary file") because it wants to
  # create its temp alongside the target. Filter through /tmp and write back with `cat >` instead.
  for MK in Makefile Makefile.libretro; do
    [ -f "src/$C/$MK" ] || continue
    sed \
      -e 's/^\(#[[:space:]]*\)TRIMUI.*$/\1MIYOO MINI PLUS (SSD202D, Cortex-A7)/' \
      -e 's/\$(platform), tg5040)/$(platform), miyoomini)/' \
      -e 's/-mtune=cortex-a53 -mcpu=cortex-a53 -march=armv8-a/-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm/' \
      -e 's/^\([[:space:]]*\)ARCH = arm64/\1ARCH = arm/' \
      -e 's/^\([[:space:]]*\)CPU_ARCH := arm64/\1CPU_ARCH := arm/' \
      "src/$C/$MK" > /tmp/mk.$$
    cat /tmp/mk.$$ > "src/$C/$MK"
    inject_ldflags "src/$C/$MK"
    bump_o2_to_o3 "src/$C/$MK"
  done

  # Prove the retarget actually landed before emitting anything. Checks are scoped to OUR block:
  # these Makefiles carry many other platform blocks (rpi4_64, classic_*, ...) that legitimately
  # mention arm64, and grepping the whole file just cries wolf.
  MK=Makefile; [ -f "src/$C/Makefile.libretro" ] && MK=Makefile.libretro
  BLOCK=$(awk '/platform\), miyoomini\)/{f=1;print;next} f&&/^else ifeq/{exit} f{print}' "src/$C/$MK")
  [ -n "$BLOCK" ] || { echo "FAILED: no miyoomini block in $C/$MK"; exit 1; }
  echo "$BLOCK" | grep -q 'mcpu=cortex-a7' \
    || echo "  WARNING: $C block has no -mcpu=cortex-a7 (would inherit generic armv7-a/VFPv3)"
  # With -flto the link is what generates code, so the arch flags MUST also be on LDFLAGS or the
  # core silently comes out generic armv7-a/VFPv3. This is not a style check.
  if ! echo "$BLOCK" | grep -q 'LDFLAGS += -mcpu=cortex-a7'; then
    echo "FAILED: $C miyoomini block has no arch flags on LDFLAGS — LTO codegen would be generic"
    exit 1
  fi
  if echo "$BLOCK" | grep -q 'arm64'; then
    echo "FAILED: $C miyoomini block still mentions arm64"; exit 1
  fi
  if [ "$(grep -c 'platform), miyoomini)' "src/$C/$MK")" != "1" ]; then
    echo "FAILED: $C has more than one miyoomini block — the later one would be dead code"; exit 1
  fi

  ( cd "src/$C" && git diff > "$CORES/patches/$C.patch" )
  echo "  wrote patches/$C.patch ($(wc -l < "patches/$C.patch") lines)"

  # Reset so the ordinary build applies the generated patch to a clean tree.
  ( cd "src/$C" && git reset -q --hard && git clean -qfd )
done

echo
echo "All patches regenerated."

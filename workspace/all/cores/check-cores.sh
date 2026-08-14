#!/bin/sh
# Fail a build that produced a missing or stub-sized core.
#
# WHY: a racing link can "succeed" and leave a ~10KB shared object behind. That is exactly how a
# 10,840-byte pcsx_rearmed shipped inside the h700 images, and how a tg5040 zip was built tonight
# with a broken PlayStation core and no error anywhere (2026-08-14). A core that fails to build is
# obvious; a core that half-builds is not, and nothing was checking.
#
# Usage: check-cores.sh <output-dir> <core> [core...]
set -e
OUT="$1"; shift
MIN=102400        # 100KB. The smallest real core here is mednafen_vb at ~167KB; stubs are ~10KB.
BAD=""
for c in "$@"; do
	so="$OUT/${c}_libretro.so"
	if [ ! -f "$so" ]; then
		BAD="$BAD $c(missing)"
		continue
	fi
	sz=$(stat -f%z "$so" 2>/dev/null || stat -c%s "$so" 2>/dev/null || echo 0)
	if [ "$sz" -lt "$MIN" ]; then
		BAD="$BAD $c(${sz}B)"
	fi
done
if [ -n "$BAD" ]; then
	echo "ERROR: core build produced missing or stub artifacts:$BAD"
	echo "       a stub is usually a raced link — check that the core sub-make is not inheriting"
	echo "       the parent jobserver, and rebuild that core from a clean tree."
	exit 1
fi
echo "  cores OK: $# built, none stub-sized"

#!/bin/sh
# Fail a build that produced a missing or stub-sized core.
#
# WHY: a racing link can "succeed" and leave a ~10KB shared object behind. That is exactly how a
# 10,840-byte pcsx_rearmed shipped inside the h700 images, and how a tg5040 zip was built tonight
# with a broken PlayStation core and no error anywhere (2026-08-14). A core that fails to build is
# obvious; a core that half-builds is not, and nothing was checking.
#
# ARGUMENTS ARE ARTIFACT FILENAMES, not core names. The two differ: the core `fake-08` produces
# `fake08_libretro.so` via its *_CORE override, so deriving the filename here would reject every
# valid build. The makefile passes the same $(if $(C_CORE),...) expression the template uses to move
# the artifact, which keeps this checker and the build agreeing by construction.
#
# Usage: check-cores.sh <output-dir> <artifact.so> [artifact.so...]
set -e
OUT="$1"; shift
. "$(dirname "$0")/core-limits.sh"
MIN=$CORE_MIN_SIZE

if [ $# -eq 0 ]; then
	echo "ERROR: check-cores.sh called with no cores to check (vacuous pass)"
	exit 1
fi

BAD=""
for so_name in "$@"; do
	so="$OUT/$so_name"
	if [ ! -f "$so" ]; then
		BAD="$BAD $so_name(missing)"
		continue
	fi
	sz=$(stat -f%z "$so" 2>/dev/null || stat -c%s "$so" 2>/dev/null || echo 0)
	if [ "$sz" -lt "$MIN" ]; then
		BAD="$BAD $so_name(${sz}B)"
	fi
done
if [ -n "$BAD" ]; then
	echo "ERROR: core build produced missing or stub artifacts:$BAD"
	echo "       a stub is usually a raced link — check that the core sub-make is not inheriting"
	echo "       the parent jobserver, and rebuild that core from a clean tree."
	exit 1
fi
echo "  cores OK: $# built, none stub-sized"

#!/bin/sh
# Verify the cores STAGED FOR PACKAGING are real, loadable artifacts for this platform.
#
# check-cores.sh guards the core BUILD; this guards what is about to be zipped, which is not the
# same thing: a stale core can be copied from a previous build, and `make package` can be run
# against a manually staged tree the build gate never saw (Codex review 2026-08-14).
#
# Size alone is too weak. A raced link can drop a subset of objects and still exceed any byte
# threshold, and a core built for the wrong architecture is exactly the right size while being
# unusable. So check the ELF header directly:
#   - ELF magic, and ET_DYN (a shared object, not an executable or a truncated file)
#   - e_machine matches the platform (AArch64 0xB7 for tg5040/h700, ARM 0x28 for miyoomini)
#   - the libretro entry point `retro_run` appears in the file, which a stub link omits
#
# Usage: check-payload.sh <emus-dir> <platform>
set -e
DIR="$1"
PLATFORM="$2"
[ -d "$DIR" ] || { echo "  check-payload: no staged Emus dir at $DIR, nothing to verify"; exit 0; }

case "$PLATFORM" in
	miyoomini) WANT_MACHINE=40;  WANT_NAME="ARM" ;;      # 0x28
	*)         WANT_MACHINE=183; WANT_NAME="AArch64" ;;  # 0xB7
esac

BAD=""
N=0
for so in $(find "$DIR" -name "*_libretro.so" 2>/dev/null); do
	N=$((N + 1))
	name=$(basename "$so")

	# Size first. The ELF checks below do NOT catch a stub: a raced link still emits a valid
	# aarch64 ET_DYN with retro_run in it, just missing most of the code (measured: the 10,840-byte
	# pcsx_rearmed passes every header check). Size is the only signal that separates those.
	sz=$(stat -f%z "$so" 2>/dev/null || stat -c%s "$so" 2>/dev/null || echo 0)
	if [ "$sz" -lt 102400 ]; then
		BAD="$BAD $name(stub ${sz}B)"
		continue
	fi

	# ELF magic: 0x7F 'E' 'L' 'F'
	magic=$(od -An -t u1 -j 0 -N 4 "$so" 2>/dev/null | tr -s ' ')
	case "$magic" in
		*" 127 69 76 70"*) ;;
		*) BAD="$BAD $name(not-ELF)"; continue ;;
	esac

	# e_type at offset 16, little-endian u16. 3 = ET_DYN (shared object).
	etype=$(od -An -t u2 -j 16 -N 2 "$so" 2>/dev/null | tr -d ' ')
	[ "$etype" = "3" ] || { BAD="$BAD $name(not-shared-object)"; continue; }

	# e_machine at offset 18, little-endian u16.
	mach=$(od -An -t u2 -j 18 -N 2 "$so" 2>/dev/null | tr -d ' ')
	[ "$mach" = "$WANT_MACHINE" ] || { BAD="$BAD $name(arch=$mach want=$WANT_MACHINE)"; continue; }

	# The libretro entry point must be present. A raced/stub link omits it.
	grep -q "retro_run" "$so" 2>/dev/null || BAD="$BAD $name(no-retro_run)"
done

if [ "$N" -eq 0 ]; then
	echo "  check-payload: no cores staged under $DIR (nothing to verify)"
	exit 0
fi
if [ -n "$BAD" ]; then
	echo "ERROR: staged cores are not valid $WANT_NAME libretro objects:$BAD"
	echo "       refusing to package. Rebuild the affected cores from a clean tree."
	exit 1
fi
echo "  check-payload: $N staged cores are valid $WANT_NAME libretro objects"

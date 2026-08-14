#!/bin/sh
# Verify the cores STAGED FOR PACKAGING are real, loadable artifacts for this platform.
#
# check-cores.sh guards the core BUILD; this guards what is about to be zipped, which is not the
# same thing: a stale core can be copied from a previous build, and `make package` can be run
# against a manually staged tree the build gate never saw (Codex review 2026-08-14).
#
# SCANS THE WHOLE STAGED PLATFORM TREE, not one subdirectory. The first version took the Emus pak
# dir, which is where only the seven DORMANT extras live — the primary cores (including the
# pcsx_rearmed this file was written for) stage in <platform>/cores, and on miyoomini every core
# does, so the gate inspected nothing at all on the platform it named (2026-08-14, review round 2).
#
# Two independent failure modes, and neither check finds the other:
#   - a stub. A raced link still emits a valid ET_DYN of the right architecture WITH retro_run in
#     it, just missing most of the code (measured: pcsx_rearmed at 10,840 bytes passes every header
#     check below). Only size separates those, so the size check is load-bearing — do not remove it
#     as redundant with the ELF checks.
#   - a wrong or non-core file: right size, unusable. ELF magic + ET_DYN + e_machine catch a core
#     built for another architecture, and retro_run catches a shared object that is not a libretro
#     core at all.
#
# Usage: check-payload.sh <staged-platform-dir> <platform>
set -e
DIR="$1"
PLATFORM="$2"
. "$(dirname "$0")/core-limits.sh"

# FAILS CLOSED, like check-cores.sh. A missing directory or an empty scan means the gate was wired
# wrong, not that the payload is clean, and a gate that reassures you when it inspected nothing is
# worse than no gate. That is precisely how the first version passed a build it never looked at.
[ -n "$PLATFORM" ] || { echo "ERROR: check-payload.sh called with no platform (vacuous pass)"; exit 1; }
[ -d "$DIR" ] || { echo "ERROR: check-payload: no staged tree at $DIR (vacuous pass)"; exit 1; }

# No catch-all default. Guessing an architecture is the mistake this gate exists to catch, so an
# unknown platform is an error — a new platform must state its own arch here.
case "$PLATFORM" in
	tg5040|h700) WANT_MACHINE=183; WANT_NAME="AArch64" ;;  # 0xB7
	miyoomini)   WANT_MACHINE=40;  WANT_NAME="ARM"     ;;  # 0x28
	*) echo "ERROR: check-payload: unknown platform '$PLATFORM', refusing to guess its architecture"; exit 1 ;;
esac

LIST=$(mktemp) || exit 1
BAD=$(mktemp)  || exit 1
trap 'rm -f "$LIST" "$BAD"' EXIT INT TERM

# -print into a file and read line-by-line: `for so in $(find ...)` word-splits, so one pak with a
# space in its name (this tree already ships "Optimize CPU.pak") reports two bogus stubs and fails
# a good release. No -type f, so a broken symlink reaches the missing branch below instead of being
# silently skipped.
find "$DIR" -name "*_libretro.so" -print > "$LIST"

N=0
while IFS= read -r so; do
	N=$((N + 1))
	name=${so#$DIR/}

	if [ ! -f "$so" ] || [ ! -r "$so" ]; then
		echo "$name(missing or unreadable)" >> "$BAD"
		continue
	fi

	sz=$(stat -f%z "$so" 2>/dev/null || stat -c%s "$so" 2>/dev/null || echo 0)
	if [ "$sz" -lt "$CORE_MIN_SIZE" ]; then
		echo "$name(stub ${sz}B)" >> "$BAD"
		continue
	fi

	# ELF magic: 0x7F 'E' 'L' 'F'
	magic=$(od -An -t u1 -j 0 -N 4 "$so" 2>/dev/null | tr -s ' ')
	case "$magic" in
		*" 127 69 76 70"*) ;;
		*) echo "$name(not-ELF)" >> "$BAD"; continue ;;
	esac

	# e_type at offset 16, little-endian u16. 3 = ET_DYN (shared object).
	etype=$(od -An -t u2 -j 16 -N 2 "$so" 2>/dev/null | tr -d ' ')
	[ "$etype" = "3" ] || { echo "$name(not-shared-object)" >> "$BAD"; continue; }

	# e_machine at offset 18, little-endian u16.
	mach=$(od -An -t u2 -j 18 -N 2 "$so" 2>/dev/null | tr -d ' ')
	[ "$mach" = "$WANT_MACHINE" ] || { echo "$name(arch=$mach want=$WANT_MACHINE)" >> "$BAD"; continue; }

	# Not a stub check — see the header. This catches a shared object that is not a libretro core:
	# a host library or an unrelated .so copied into the payload by a bad path.
	grep -q "retro_run" "$so" 2>/dev/null || echo "$name(no-retro_run)" >> "$BAD"
done < "$LIST"

if [ "$N" -eq 0 ]; then
	echo "ERROR: check-payload: no cores staged under $DIR (vacuous pass)"
	exit 1
fi
if [ -s "$BAD" ]; then
	echo "ERROR: staged cores are not valid $WANT_NAME libretro objects:"
	sed 's/^/         /' "$BAD"
	echo "       refusing to package. Rebuild the affected cores from a clean tree."
	exit 1
fi
echo "  check-payload: $N staged core(s) valid $WANT_NAME libretro objects"

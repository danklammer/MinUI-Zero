#!/bin/sh
# Host test for minarch's saved-cfg migration. No device, no toolchain, no SDL.
#
# The pieces are EXTRACTED from minarch.c rather than duplicated, so the test can never drift from
# the shipping implementation: the version constants, the stale-key table, Config_isStaleKey, and
# Config_getValue (the parser the version check runs through -- a substring bug there would silently
# make every save look current, which is the failure that matters most).
#
# Built under AddressSanitizer: Config_getValue does raw strstr/strncpy into a char[256].
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../minarch/minarch.c"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

[ -f "$SRC" ] || { echo "cannot find minarch.c at $SRC"; exit 1; }

{
	grep -E '^#define (CFG_VERSION|CFG_VERSION_KEY|CFG_STALE_MAX) ' "$SRC"
	awk '/^static const char\* cfg_stale_keys\[\] = \{/,/^\};/' "$SRC"
	awk '/^static int Config_staleIndex\(/,/^}/' "$SRC"
	grep -E '^static int Config_isStaleKey\(.*\}$' "$SRC"
	awk '/^static int Config_getValue\(/,/^}/' "$SRC"
} > "$OUT/cfg_migrate_extracted.h"

for SYM in CFG_VERSION_KEY CFG_STALE_MAX cfg_stale_keys Config_staleIndex Config_isStaleKey Config_getValue; do
	grep -q "$SYM" "$OUT/cfg_migrate_extracted.h" || { echo "extraction missed $SYM"; exit 1; }
done

cc -std=gnu99 -g -fsanitize=address -fno-omit-frame-pointer \
   -I"$OUT" "$HERE/cfg_migrate_test.c" -o "$OUT/cfg_migrate_test"
"$OUT/cfg_migrate_test"

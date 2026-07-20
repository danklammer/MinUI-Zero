#!/bin/sh
# Forbidden-globals audit (threading v2 verification gate): framering.c and
# frontend_core.c must carry NO mutable file-scope globals — all shared state lives
# INSIDE the fr_ring / fc objects (passed by pointer), never in loose globals that
# could bypass the ring's ownership model. Read-only constants and thread-local
# (per-thread) storage are allowed; only genuinely writable data/bss is forbidden.
set -e
cd "$(dirname "$0")"
CC="${CC:-cc}"
mkdir -p build
BAD=0
for src in framering.c frontend_core.c; do
	obj="build/${src%.c}.audit.o"
	$CC -std=c11 -Wall -Wextra -Werror -c -o "$obj" "$src"
	# nm type letters: d/D = writable data, b/B = bss. (s/S = read-only small-data &
	# compiler local labels; t/T = text.) TLS is per-thread, excluded by name marker.
	# A genuinely forbidden mutable global is exactly a d/D/b/B symbol that is not TLS.
	syms=$(nm "$obj" 2>/dev/null | awk '$2 ~ /^[dDbB]$/ {print $3}' \
	       | grep -Ev '^$|tl_|__tls|_thread' || true)
	if [ -n "$syms" ]; then
		echo "FORBIDDEN MUTABLE GLOBAL(S) in $src:"
		echo "$syms"
		BAD=1
	fi
done
if [ "$BAD" != "0" ]; then
	echo "== forbidden-globals audit: FAIL =="
	exit 1
fi
echo "== forbidden-globals audit: PASS (no mutable file-scope globals; shared state is ring-owned) =="

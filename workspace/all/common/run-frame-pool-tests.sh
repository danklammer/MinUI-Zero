#!/bin/sh
set -e
cd "$(dirname "$0")"
mkdir -p build
CC="${CC:-cc}"
for mode in plain asan; do
	case "$mode" in
		plain) flags="-O2" ;;
		asan) flags="-O1 -g -fsanitize=address" ;;
	esac
	$CC -std=c11 -Wall -Wextra -Werror $flags -o "build/frame_pool_test_$mode" frame_pool_test.c
	"build/frame_pool_test_$mode"
done

#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
mkdir -p "$ROOT/.notes"
TMP=$(mktemp -d "$ROOT/.notes/source-verifier-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

REPO="$TMP/core"
VERIFY="$ROOT/workspace/all/cores/verify-source.sh"
mkdir -p "$REPO"
git -C "$REPO" init -q
git -C "$REPO" config user.name test
git -C "$REPO" config user.email test@example.invalid
printf '%s\n' base > "$REPO/source.c"
printf '%s\n' legacy > "$REPO/ignored.bin"
git -C "$REPO" add source.c ignored.bin
git -C "$REPO" commit -qm base
PIN=$(git -C "$REPO" rev-parse HEAD)

printf '%s\n' patched > "$REPO/source.c"
git -C "$REPO" diff -- source.c > "$TMP/core.patch"
sh "$VERIFY" "$REPO" "$PIN" "$TMP/core.patch"

printf '%s\n' extra >> "$REPO/source.c"
if sh "$VERIFY" "$REPO" "$PIN" "$TMP/core.patch" >/dev/null 2>&1; then
	echo "source verifier accepted an undeclared tracked edit" >&2
	exit 1
fi

printf '%s\n' patched > "$REPO/source.c"
rm "$REPO/ignored.bin"
sh "$VERIFY" "$REPO" "$PIN" "$TMP/core.patch" --ignore=ignored.bin

git -C "$REPO" add source.c
if sh "$VERIFY" "$REPO" "$PIN" "$TMP/core.patch" --ignore=ignored.bin >/dev/null 2>&1; then
	echo "source verifier accepted staged dependency changes" >&2
	exit 1
fi

echo "== source reproducibility tests: ALL PASS =="

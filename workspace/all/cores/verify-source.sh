#!/bin/sh
set -eu

REPO=$1
PIN=$2
shift 2

PIN=$(git -C "$REPO" rev-parse "$PIN^{commit}")
HEAD=$(git -C "$REPO" rev-parse HEAD)
if [ "$HEAD" != "$PIN" ]; then
	echo "$(basename "$REPO"): source HEAD does not match pinned commit" >&2
	exit 1
fi

# Patches are intentionally left as unstaged changes in the dependency checkout. Reject
# staged edits, then compare the complete tracked worktree delta with a clean index built
# from the declared patches. This catches a stale .patched marker and unrelated local edits.
git -C "$REPO" diff --cached --quiet -- || {
	echo "$(basename "$REPO"): staged source changes are not reproducible" >&2
	exit 1
}

EXPECTED=$(mktemp)
ACTUAL=$(mktemp)
rm -f "$EXPECTED" "$ACTUAL"
trap 'rm -f "$EXPECTED" "$ACTUAL"' EXIT HUP INT TERM

GIT_INDEX_FILE="$EXPECTED" git -C "$REPO" read-tree "$PIN"
IGNORES=
for PATCH_FILE in "$@"; do
	case "$PATCH_FILE" in
		--ignore=*) IGNORES="$IGNORES ${PATCH_FILE#--ignore=}"; continue ;;
	esac
	case "$PATCH_FILE" in
		/*) ;;
		*) PATCH_FILE="$PWD/$PATCH_FILE" ;;
	esac
	GIT_INDEX_FILE="$EXPECTED" git -C "$REPO" apply --cached --unidiff-zero --whitespace=nowarn -p1 < "$PATCH_FILE"
done
EXPECTED_TREE=$(GIT_INDEX_FILE="$EXPECTED" git -C "$REPO" write-tree)

GIT_INDEX_FILE="$ACTUAL" git -C "$REPO" read-tree "$PIN"
git -C "$REPO" diff --binary --no-ext-diff | \
	GIT_INDEX_FILE="$ACTUAL" git -C "$REPO" apply --cached --whitespace=nowarn
for IGNORE in $IGNORES; do
	# Reset only the temporary index. The real dependency worktree is never modified.
	GIT_INDEX_FILE="$ACTUAL" git -C "$REPO" reset -q "$PIN" -- "$IGNORE"
done
ACTUAL_TREE=$(GIT_INDEX_FILE="$ACTUAL" git -C "$REPO" write-tree)

if [ "$ACTUAL_TREE" != "$EXPECTED_TREE" ]; then
	echo "$(basename "$REPO"): tracked source changes do not match declared patches" >&2
	exit 1
fi

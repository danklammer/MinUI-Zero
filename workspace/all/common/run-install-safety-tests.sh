#!/bin/sh
# Host tests for the Miyoo install/update path (Codex blocker #9).
#
# The failure this guards is the worst one we ship: the installer used to delete BOTH escape routes
# — the update zip and the previous bootstrap — without ever checking that the extract worked. A
# corrupt download, a full card or power loss then left a half-written system that could not boot
# and could not repair itself.
#
# These tests run the REAL script body against a fake SD tree, so they exercise the shipped logic
# rather than a copy of it. Each case asserts the two things that actually matter after a failure:
#   * the update zip still exists (the payload can be retried)
#   * the previous bootstrap is back in place (the device still boots)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../../miyoomini/install/boot.sh"
[ -f "$SRC" ] || { echo "cannot find boot.sh at $SRC"; exit 1; }

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); echo "    ok   $1"; }
bad()  { FAIL=$((FAIL+1)); echo "    FAIL $1"; }

# Extract just the install stanza and run it with SDCARD_PATH pointed at a scratch tree.
# `sed` pulls the block between the update test and the launch loop.
run_install() {
	ROOT="$1"
	sed -n '/^# install\/update$/,/^# or launch/p' "$SRC" \
		| sed -e '/^# or launch/d' > "$ROOT/stanza.sh"
	# neutralise device-only bits: backlight sysfs, lcd init, the splash binary, install.sh
	sed -i.bak \
		-e 's|^\techo .* > /sys/class/pwm.*$|\t:|' \
		-e 's|^\tcat /proc/ls$|\t:|' \
		-e 's|^\tsleep 1$|\t:|' \
		-e 's|\./show\.elf [^ ]*|:|' \
		-e 's|"\$SYSTEM_PATH/\$PLATFORM/bin/install\.sh"|:|' \
		-e 's|cd \$(dirname "\$0")/\$PLATFORM|cd "$ROOT/.tmp_update/miyoomini"|' \
		"$ROOT/stanza.sh"
	( cd "$ROOT" && PLATFORM=miyoomini SDCARD_PATH="$ROOT" ROOT="$ROOT" \
	    UPDATE_PATH="$ROOT/MinUI.zip" SYSTEM_PATH="$ROOT/.system" \
	    sh "$ROOT/stanza.sh" >/dev/null 2>&1 ) || true
}

# Build a fake card: an installed system + a bootstrap + an update zip.
make_card() {
	ROOT=$(mktemp -d)
	mkdir -p "$ROOT/.tmp_update/miyoomini"
	echo OLD > "$ROOT/.tmp_update/marker"
	mkdir -p "$ROOT/.system/miyoomini/paks/MinUI.pak" "$ROOT/.system/miyoomini/bin"
	echo OLD > "$ROOT/.system/miyoomini/paks/MinUI.pak/launch.sh"
	echo "$ROOT"
}

# A VALID update zip containing a new bootstrap + a new system with the launcher present.
make_good_zip() {
	ROOT="$1"; STAGE=$(mktemp -d)
	mkdir -p "$STAGE/.tmp_update/miyoomini" "$STAGE/.system/miyoomini/paks/MinUI.pak"
	echo NEW > "$STAGE/.tmp_update/marker"
	echo NEW > "$STAGE/.system/miyoomini/paks/MinUI.pak/launch.sh"
	( cd "$STAGE" && zip -qr "$ROOT/MinUI.zip" .tmp_update .system )
	rm -rf "$STAGE"
}

echo "=== case 1: healthy update applies and cleans up ==="
R=$(make_card); make_good_zip "$R"; run_install "$R"
[ ! -f "$R/MinUI.zip" ] && ok "zip consumed" || bad "zip should be gone after success"
[ ! -d "$R/.tmp_update-old" ] && ok "rollback cleaned up" || bad "rollback left behind"
[ "$(cat "$R/.system/miyoomini/paks/MinUI.pak/launch.sh")" = "NEW" ] && ok "new system installed" || bad "system not updated"
rm -rf "$R"

echo "=== case 2: CORRUPT zip — must change nothing ==="
R=$(make_card); head -c 2048 /dev/urandom > "$R/MinUI.zip"; run_install "$R"
[ -f "$R/MinUI.zip" ] && ok "zip KEPT for retry" || bad "zip deleted — payload lost"
[ -d "$R/.tmp_update" ] && ok "bootstrap intact" || bad "bootstrap destroyed — card cannot boot"
[ "$(cat "$R/.system/miyoomini/paks/MinUI.pak/launch.sh")" = "OLD" ] && ok "old system untouched" || bad "live system was modified by a corrupt zip"
[ -f "$R/MinUI-update-failed.txt" ] && ok "failure explained on the card" || bad "no explanation written"
rm -rf "$R"

echo "=== case 3: TRUNCATED zip (interrupted download) ==="
R=$(make_card); make_good_zip "$R"
SZ=$(wc -c < "$R/MinUI.zip"); head -c $((SZ/3)) "$R/MinUI.zip" > "$R/t" && mv "$R/t" "$R/MinUI.zip"
run_install "$R"
[ -f "$R/MinUI.zip" ] && ok "zip KEPT for retry" || bad "zip deleted — payload lost"
[ -d "$R/.tmp_update" ] && ok "bootstrap intact" || bad "bootstrap destroyed — card cannot boot"
[ ! -d "$R/.tmp_update-old" ] && ok "no stale rollback left" || bad "rollback dir left behind"
rm -rf "$R"

echo "=== case 4: zip extracts but payload is WRONG (no launcher) ==="
R=$(make_card); STAGE=$(mktemp -d)
mkdir -p "$STAGE/.tmp_update"; echo JUNK > "$STAGE/.tmp_update/marker"
( cd "$STAGE" && zip -qr "$R/MinUI.zip" .tmp_update ); rm -rf "$STAGE"
run_install "$R"
[ -f "$R/MinUI.zip" ] && ok "zip KEPT for retry" || bad "zip deleted after bad payload"
[ -d "$R/.tmp_update" ] && ok "bootstrap restored" || bad "bootstrap destroyed"
[ "$(cat "$R/.tmp_update/marker")" = "OLD" ] && ok "ORIGINAL bootstrap restored, not the junk one" || bad "junk bootstrap left in place"
rm -rf "$R"

echo ""
echo "=== $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]

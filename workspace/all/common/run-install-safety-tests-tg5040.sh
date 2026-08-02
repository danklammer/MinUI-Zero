#!/bin/sh
# Host tests for the tg5040 install/update path.
#
# Sibling of run-install-safety-tests.sh, which covers Miyoo only. The failure this guards is one a
# user reaches by following our own README: "extract MinUI.zip from the release archive, place it on
# the card root, and reboot" — while the release page offers one archive PER DEVICE. A valid archive
# for the wrong device passes the CRC test, and before this check it was extracted over the live
# .system and then DELETED, turning a working console into one that needs a PC.
#
# As in the Miyoo harness, these run the REAL script body against a fake SD tree rather than a copy
# of the logic, and assert the two things that matter after a rejection:
#   * the archive still exists (the user can replace it and retry)
#   * the live system was not touched
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../../tg5040/install/boot.sh"
[ -f "$SRC" ] || { echo "cannot find boot.sh at $SRC"; exit 1; }
command -v zip >/dev/null 2>&1 || { echo "SKIP: host has no zip"; exit 0; }

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "    ok   $1"; }
bad() { FAIL=$((FAIL+1)); echo "    FAIL $1"; }

# Pull the archive-handling stanza (CRC test through the trailing sync) and run it against a scratch
# tree. The bundled ./unzip is replaced with the host's, and the splash binary neutralised.
run_install() {
	ROOT="$1"
	sed -n '/^	UPDATED=$/,/^	sync$/p' "$SRC" > "$ROOT/stanza.sh"
	sed -i.bak -e 's|\./unzip|unzip|g' -e 's|\./show\.elf [^ ]*|:|' "$ROOT/stanza.sh"
	( cd "$ROOT" && PLATFORM=tg5040 SDCARD_PATH="$ROOT" \
	  UPDATE_PATH="$ROOT/MinUI.zip" SYSTEM_PATH="$ROOT/.system" \
	  sh "$ROOT/stanza.sh" >/dev/null 2>&1 ) || true
}

# A card with MinUI already installed, plus an archive to apply.
make_card() {
	ROOT="$1"
	rm -rf "$ROOT"
	mkdir -p "$ROOT/.system/tg5040/paks/MinUI.pak" "$ROOT/.system/tg5040/bin" "$ROOT/.tmp_update"
	echo "LIVE-LAUNCHER" > "$ROOT/.system/tg5040/paks/MinUI.pak/launch.sh"
	echo "LIVE-BINARY"   > "$ROOT/.system/tg5040/bin/minui.elf"
}

# Build a payload zip for an arbitrary platform name.
make_zip() {
	ROOT="$1"; PLAT="$2"
	B="$ROOT/.build"; rm -rf "$B"
	mkdir -p "$B/.system/$PLAT/paks/MinUI.pak"
	echo "NEW-LAUNCHER-$PLAT" > "$B/.system/$PLAT/paks/MinUI.pak/launch.sh"
	( cd "$B" && zip -qr "$ROOT/MinUI.zip" .system )
	rm -rf "$B"
}

T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

echo "=== case 1: correct tg5040 archive installs and is consumed ==="
make_card "$T/c1"; make_zip "$T/c1" tg5040
run_install "$T/c1"
[ ! -f "$T/c1/MinUI.zip" ] && ok "archive consumed on success" || bad "archive should be gone after a good install"
grep -q "NEW-LAUNCHER-tg5040" "$T/c1/.system/tg5040/paks/MinUI.pak/launch.sh" 2>/dev/null \
	&& ok "new system installed" || bad "new launcher was not installed"

echo "=== case 2: valid archive for the WRONG DEVICE — must change nothing ==="
make_card "$T/c2"; make_zip "$T/c2" miyoomini
run_install "$T/c2"
[ -f "$T/c2/MinUI.zip" ] && ok "archive KEPT so the user can replace it" || bad "archive was consumed or renamed away"
grep -q "LIVE-LAUNCHER" "$T/c2/.system/tg5040/paks/MinUI.pak/launch.sh" 2>/dev/null \
	&& ok "live system untouched" || bad "live system was overwritten by another device's payload"
[ ! -d "$T/c2/.system/miyoomini" ] && ok "foreign payload not extracted" || bad "the other device's .system was written"
[ -f "$T/c2/MinUI-update-failed.txt" ] && ok "failure explained to the user" || bad "no explanation written"

echo "=== case 3: corrupt archive — must change nothing and not be kept as live ==="
make_card "$T/c3"; head -c 2048 /dev/urandom > "$T/c3/MinUI.zip"
run_install "$T/c3"
grep -q "LIVE-LAUNCHER" "$T/c3/.system/tg5040/paks/MinUI.pak/launch.sh" 2>/dev/null \
	&& ok "live system untouched by a corrupt archive" || bad "corrupt archive damaged the system"
[ -f "$T/c3/MinUI.zip.bad" ] && ok "corrupt archive set aside, cannot install-loop" || bad "corrupt archive not set aside"

echo ""
echo "=== $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]

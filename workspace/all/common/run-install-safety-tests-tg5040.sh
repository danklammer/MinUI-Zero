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
	mkdir -p "$B/.system/$PLAT/paks/MinUI.pak" "$B/.tmp_update"
	echo "NEW-LAUNCHER-$PLAT" > "$B/.system/$PLAT/paks/MinUI.pak/launch.sh"
	echo "NEW-BOOTSTRAP" > "$B/.tmp_update/$PLAT.sh"
	( cd "$B" && zip -qr "$ROOT/MinUI.zip" .system .tmp_update )
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
# The bootstrap is a DOTFILE. A glob of "$STAGE"/* does not match it, so it was silently skipped:
# the install reported success, ate the archive, and left the old updater in place forever.
grep -q "NEW-BOOTSTRAP" "$T/c1/.tmp_update/tg5040.sh" 2>/dev/null \
	&& ok ".tmp_update installed too (bootstrap can self-update)" \
	|| bad ".tmp_update was NOT installed — updater self-update silently broken"

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

echo "=== case 4: extraction fails PARTWAY — live system must be untouched ==="
# The card fills (or power is cut) after unzip has written some entries. Miyoo extracts to an inert
# staging directory precisely so a half-extract cannot touch the installed system; tg5040 extracted
# straight over it with -o, so a failure left .system a mix of old and new binaries and the archive
# was renamed aside, removing the only way to retry.
make_card "$T/c4"; make_zip "$T/c4" tg5040
mkdir -p "$T/c4/bin"
cat > "$T/c4/bin/unzip" <<'STUB'
#!/bin/sh
# -tqq (the CRC preflight) and -l (the platform listing) must behave normally.
case "$1" in
	-tqq|-l) exec /usr/bin/unzip "$@" ;;
esac
# The real extract: write ONE entry into the destination, then fail as if the card filled.
# The call under test is `unzip -o <zip> -d <dest>`, so the destination is the LAST argument.
DEST=""
for a in "$@"; do DEST="$a"; done
if [ -n "$DEST" ] && [ -d "$DEST" ]; then
	mkdir -p "$DEST/.system/tg5040/paks/MinUI.pak"
	echo "HALF-WRITTEN" > "$DEST/.system/tg5040/paks/MinUI.pak/launch.sh"
fi
exit 1
STUB
chmod +x "$T/c4/bin/unzip"
( cd "$T/c4" && PATH="$T/c4/bin:$PATH" sh -c '
	ROOT="'"$T/c4"'"
	sed -n "/^	UPDATED=\$/,/^	sync\$/p" "'"$SRC"'" | sed -e "s|\./unzip|unzip|g" -e "s|\./show\.elf [^ ]*|:|" > "$ROOT/stanza.sh"
	cd "$ROOT" && PLATFORM=tg5040 SDCARD_PATH="$ROOT" UPDATE_PATH="$ROOT/MinUI.zip" \
	SYSTEM_PATH="$ROOT/.system" sh "$ROOT/stanza.sh"
' ) >/dev/null 2>&1
grep -q "LIVE-LAUNCHER" "$T/c4/.system/tg5040/paks/MinUI.pak/launch.sh" 2>/dev/null \
	&& ok "live system untouched by a half-extract" \
	|| bad "half-extract overwrote the installed system"
[ -f "$T/c4/MinUI.zip" ] || [ -f "$T/c4/MinUI.zip.failed" ] \
	&& ok "archive retained for diagnosis or retry" || bad "archive lost after a failed extract"

echo "=== case 5: the stock fallback is real code, not a comment ==="
# This used to assert with a bare grep over the source, which a comment mentioning the name would
# satisfy. Check for the executable statement instead.
grep -qE '^[[:space:]]*exec /usr/trimui/bin/runtrimui-original\.sh' "$SRC" \
	&& ok "execs the stock launcher when MinUI cannot run" \
	|| bad "no executable stock fallback — a failed install leaves a black screen"
LAUNCH_LN=$(grep -n "^LAUNCH_PATH=" "$SRC" | head -1 | cut -d: -f1)
FALLBACK_LN=$(grep -nE '^[[:space:]]*exec /usr/trimui/bin/runtrimui-original\.sh' "$SRC" | tail -1 | cut -d: -f1)
if [ -n "$LAUNCH_LN" ] && [ -n "$FALLBACK_LN" ] && [ "$FALLBACK_LN" -gt "$LAUNCH_LN" ]; then
	ok "fallback comes after the launcher attempt, not instead of it"
else
	bad "fallback ordering wrong — it could pre-empt a working MinUI"
fi

echo "=== case 6: recovery copies must not be dropped before the merge is durable ==="
# The card is remounted async, so deleting the stage and the archive can hit the disk before the
# merged data. The sync must come first.
SYNC_LN=$(grep -nE '^[[:space:]]+sync$' "$SRC" | head -1 | cut -d: -f1)
RMSTAGE_LN=$(grep -nE '^[[:space:]]+rm -rf "\$STAGE"$' "$SRC" | sed -n 2p | cut -d: -f1)
if [ -n "$SYNC_LN" ] && [ -n "$RMSTAGE_LN" ] && [ "$SYNC_LN" -lt "$RMSTAGE_LN" ]; then
	ok "merge is synced before the staging tree is removed"
else
	bad "stage removed before the merge was made durable"
fi

echo ""
echo "=== $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]

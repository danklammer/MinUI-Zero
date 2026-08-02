#!/bin/sh
# Host tests for the OUTER bootstrap scripts — the first thing the stock firmware runs off the card.
#
# The other two install harnesses cover the platform installers (boot.sh). Nothing covered these,
# and they are the more dangerous layer: they delete the stock entry point, which is the only way
# the device can be made to run our code again. Get it wrong and the user needs a PC and a reader.
#
# The invariant under test, for both families:
#
#   If the bootstrap payload did not arrive COMPLETE on the card, the stock entry point must still
#   be there on the next boot, so the install can simply be retried. And the firmware's own launcher
#   must never be absent, not even for an instant.
#
# These run the REAL script bodies against a fake card, never a re-implementation — a harness that
# tests a copy of the logic proves nothing about what ships. The interesting failure is a PARTIAL
# copy (a card that fills, or an interrupted write). `cp` is stubbed via PATH to reproduce that
# deterministically instead of trying to fill a real filesystem.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT_REPO=$(cd "$HERE/../../.." && pwd)
MIYOO="$ROOT_REPO/skeleton/BOOT/miyoo/app/miyoomini.sh"
TRIMUI="$ROOT_REPO/skeleton/BOOT/trimui/app/MainUI"
[ -f "$MIYOO" ]  || { echo "cannot find $MIYOO"; exit 1; }
[ -f "$TRIMUI" ] || { echo "cannot find $TRIMUI"; exit 1; }

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "    ok   $1"; }
bad() { FAIL=$((FAIL+1)); echo "    FAIL $1"; }

# A stub `cp` that forwards to the real one and then removes a named file from the DESTINATION —
# the file that "did not fit". Deleting by absolute path avoids parsing cp's own arguments.
# `poweroff`/`reboot`/`sync` are stubbed so the bail-out path is inert on the host.
make_stubs() {
	BIN="$1"; OMIT_PATH="$2"
	mkdir -p "$BIN"
	cat > "$BIN/cp" <<-STUB
	#!/bin/sh
	/bin/cp "\$@"; RC=\$?
	[ -n "$OMIT_PATH" ] && rm -f "$OMIT_PATH"
	exit \$RC
	STUB
	for s in poweroff reboot sync; do printf '#!/bin/sh\nexit 0\n' > "$BIN/$s"; done
	chmod +x "$BIN"/*
}

# Fake card holding the stock-firmware entry directory and its bootstrap payload.
make_card() {
	R="$1"; BOOTDIR="$2"; PLAT="$3"
	rm -rf "$R"
	mkdir -p "$R/$BOOTDIR/app/.tmp_update/$PLAT"
	echo "updater-contents"  > "$R/$BOOTDIR/app/.tmp_update/updater"
	echo "platform-script"   > "$R/$BOOTDIR/app/.tmp_update/$PLAT.sh"
	echo "show-binary"       > "$R/$BOOTDIR/app/.tmp_update/$PLAT/show.elf"
}

echo "=== MIYOO: partial payload must NOT cost the model entry point ==="
T1=$(mktemp -d) || exit 1
make_card "$T1" miyoo354 miyoomini
make_stubs "$T1/bin" "$T1/.tmp_update.new/miyoomini.sh"
# Real stanza: from the REQUIRED list through the entry-point deletion.
sed -n '/^REQUIRED=/,/^rm -rf "\$MIYOO_PATH"$/p' "$MIYOO" > "$T1/stanza.sh"
[ -s "$T1/stanza.sh" ] || { echo "    FAIL could not extract the miyoo stanza"; FAIL=$((FAIL+1)); }
(
	cd "$T1/miyoo354/app" \
	&& PATH="$T1/bin:$PATH" SDCARD_PATH="$T1" MIYOO_PATH="$T1/miyoo354" IS_PLUS=true \
	   sh "$T1/stanza.sh"
) >/dev/null 2>&1
[ ! -f "$T1/.tmp_update/miyoomini.sh" ] \
	&& ok "precondition: the copy really was partial" \
	|| bad "test setup wrong — payload arrived complete, so this case proves nothing"
[ -d "$T1/miyoo354" ] \
	&& ok "model entry point survives an incomplete copy" \
	|| bad "entry point DELETED though the payload was incomplete (device needs a PC)"
[ ! -f "$T1/.tmp_update/updater" ] \
	&& ok "no dispatchable half-payload published" \
	|| bad "a partial .tmp_update was published under the live name"
rm -rf "$T1"

echo "=== MIYOO: a COMPLETE copy still installs normally ==="
T2=$(mktemp -d) || exit 1
make_card "$T2" miyoo354 miyoomini
make_stubs "$T2/bin" ""   # omit nothing
sed -n '/^REQUIRED=/,/^rm -rf "\$MIYOO_PATH"$/p' "$MIYOO" > "$T2/stanza.sh"
(
	cd "$T2/miyoo354/app" \
	&& PATH="$T2/bin:$PATH" SDCARD_PATH="$T2" MIYOO_PATH="$T2/miyoo354" IS_PLUS=true \
	   sh "$T2/stanza.sh"
) >/dev/null 2>&1
[ -f "$T2/.tmp_update/updater" ] && [ -f "$T2/.tmp_update/miyoomini.sh" ] \
	&& ok "complete payload promoted to the live name" || bad "payload not promoted"
[ ! -d "$T2/miyoo354" ] && ok "entry point removed once the payload is committed" \
	|| bad "entry point should be gone after a good install"
[ ! -d "$T2/.tmp_update.new" ] && ok "staging cleaned up" || bad "staging directory left behind"
rm -rf "$T2"

echo "=== TRIMUI: the firmware launcher is never absent during the swap ==="
T3=$(mktemp -d) || exit 1
mkdir -p "$T3/fw"
echo "STOCK-LAUNCHER" > "$T3/fw/runtrimui.sh"
# The shipped swap, with the firmware path redirected at the scratch tree.
sed -n '/^# smart pro\/brick$/,/^fi$/p' "$TRIMUI" | sed "s|/usr/trimui/bin|$T3/fw|g" > "$T3/swap.sh"
[ -s "$T3/swap.sh" ] || { echo "    FAIL could not extract the trimui swap"; FAIL=$((FAIL+1)); }
grep -q "runtrimui.sh.new" "$T3/swap.sh" \
	&& ok "swap writes a temporary and renames it over the live name" \
	|| bad "swap still moves the live launcher aside before writing its replacement"
grep -q "^	mv /usr/trimui/bin/runtrimui.sh /usr/trimui/bin/runtrimui-original.sh$" "$TRIMUI" \
	&& bad "the non-atomic mv of the live launcher is still present" \
	|| ok "no non-atomic move of the live firmware launcher"
# Ordering: the payload must be staged before the hook is installed.
PAY=$(grep -n '^STAGE=' "$TRIMUI" | head -1 | cut -d: -f1)
HOOK=$(grep -n '^# smart pro/brick$' "$TRIMUI" | head -1 | cut -d: -f1)
[ -n "$PAY" ] && [ -n "$HOOK" ] && [ "$PAY" -lt "$HOOK" ] \
	&& ok "payload staged before the boot hook is installed" \
	|| bad "boot hook is installed before the payload exists"
rm -rf "$T3"

echo ""
echo "=== $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]

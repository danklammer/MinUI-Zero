#!/bin/sh
# PLAT_* surface check across the three families (CLAUDE.md "Multi-platform parity"; three-family
# scope, Dan 2026-08-10).
#
# The shared code calls ~57 PLAT_ entry points. 16 of them are declared FALLBACK_IMPLEMENTATION
# (weak) in api.c, so a platform that omits one still LINKS — it just silently gets stock
# behaviour. That is exactly how a capability goes missing without anyone noticing: the h700
# shipped for days with PLAT_setRumble a no-op stub ("no rumble hardware reported in recon") while
# muOS's own halt.sh was buzzing the motor on that very device.
#
# This makes the gaps explicit: every PLAT_ symbol the reference platform (tg5040) implements must
# either be implemented by the peer, or be declared here with a reason. No silent omissions.
set -u

REF=workspace/tg5040/platform/platform.c
FAIL=0
ALLOW=tools/plat-surface-allowlist.txt

bad() { echo "  FAIL $1"; FAIL=1; }
ok()  { echo "  ok   $1"; }

# Definitions, not calls: a leading type then PLAT_name(
defs() {
	grep -oE "^[A-Za-z_][A-Za-z0-9_ *]*[ *]PLAT_[A-Za-z_]+\(" "$1" 2>/dev/null \
		| grep -oE "PLAT_[A-Za-z_]+" | sort -u
}

REF_SYMS=$(defs "$REF")
echo "reference: tg5040 implements $(echo "$REF_SYMS" | wc -l | tr -d ' ') PLAT_ entry points"

for PEER in miyoomini h700; do
	echo "-- $PEER --"
	PEER_FILE=workspace/$PEER/platform/platform.c
	[ -f "$PEER_FILE" ] || { bad "$PEER: no platform.c"; continue; }
	PEER_SYMS=$(defs "$PEER_FILE")
	MISSING=""
	for s in $REF_SYMS; do
		echo "$PEER_SYMS" | grep -qx "$s" && continue
		grep -q "^$PEER:$s\$" "$ALLOW" 2>/dev/null && continue
		MISSING="$MISSING $s"
	done
	if [ -z "$MISSING" ]; then
		ok "$PEER: implements every tg5040 PLAT_ entry point (or declares the gap)"
	else
		for s in $MISSING; do bad "$PEER: missing $s — implement it or declare it in $ALLOW"; done
	fi
done

# A declared gap that has since been implemented is stale: it would hide a future regression.
if [ -f "$ALLOW" ]; then
	while IFS= read -r line; do
		case "$line" in ''|'#'*) continue ;; esac
		p=${line%%:*}; s=${line#*:}
		if defs "workspace/$p/platform/platform.c" | grep -qx "$s"; then
			bad "allowlist entry '$line' is stale — $p implements $s now; remove it"
		fi
	done < "$ALLOW"
fi

echo
if [ "$FAIL" -eq 0 ]; then echo "== PLAT surface: ALL PASS =="; else echo "== PLAT surface: FAILURES =="; exit 1; fi

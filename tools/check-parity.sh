#!/bin/sh
# Cross-platform parity check: tg5040 vs miyoomini (CLAUDE.md "Multi-platform parity").
#
# Drift between the platform skeletons is silent — tg5040 grew systems for months while the MMP
# kept its port-day pak set, leaving 7 systems unlaunchable with their cores already shipping
# (2026-07-27). This makes that class of gap a FAILING CHECK instead of an archaeology find.
#
# What is compared:
#   1. Emus pak sets (directory listing)
#   2. EMU_EXE per pak (same system -> same core)
#   3. core hash pins (*_HASH) — same core must build from the same commit on both platforms
#   4. core patch filename sets
#   5. default.cfg option keys and values per pak, and system.cfg — ignoring binds and comments
#
# Deliberate divergences live in tools/parity-allowlist.txt as `<pak>:<key>` with a reason
# comment. Anything not listed fails. `default-<device>.cfg` variants are tg5040-internal
# (device_tag specialisation) and are not compared.
set -u

A=skeleton/SYSTEM/tg5040
B=skeleton/SYSTEM/miyoomini
ALLOW=tools/parity-allowlist.txt
FAIL=0
TMPD=$(mktemp -d); trap 'rm -rf "$TMPD"' EXIT

allow() { # allow <pak> <key> -> 0 if declared
	grep -q "^$1:$2\$" "$ALLOW" 2>/dev/null
}
bad() {
	echo "  FAIL $1"
	FAIL=1
}
ok() {
	echo "  ok   $1"
}

# --- 1. pak sets ---
PAKS_A=$(ls "$A/paks/Emus" | sort)
PAKS_B=$(ls "$B/paks/Emus" | sort)
if [ "$PAKS_A" = "$PAKS_B" ]; then
	ok "pak sets match ($(echo "$PAKS_A" | wc -l | tr -d ' ') paks)"
else
	bad "pak sets differ:"
	echo "$PAKS_A" > "$TMPD/a"; echo "$PAKS_B" > "$TMPD/b"
	diff "$TMPD/a" "$TMPD/b" | sed 's/^/       /'
fi

# --- 2. EMU_EXE per pak ---
for p in $PAKS_A; do
	[ -f "$A/paks/Emus/$p/launch.sh" ] && [ -f "$B/paks/Emus/$p/launch.sh" ] || continue
	ea=$(grep -m1 "^EMU_EXE=" "$A/paks/Emus/$p/launch.sh" || true)
	eb=$(grep -m1 "^EMU_EXE=" "$B/paks/Emus/$p/launch.sh" || true)
	[ "$ea" = "$eb" ] || bad "$p: core differs ($ea vs $eb)"
done
ok "EMU_EXE checked for every shared pak"

# --- 3. core hash pins ---
pins() { grep -E "^[a-z0-9_-]+_HASH" "$1" | tr -d ' \t' | sort; }
PA=$(pins workspace/tg5040/cores/makefile)
PB=$(pins workspace/miyoomini/cores/makefile)
if [ "$PA" = "$PB" ]; then
	ok "core hash pins identical ($(echo "$PA" | wc -l | tr -d ' ') cores)"
else
	bad "core hash pins differ:"
	echo "$PA" > "$TMPD/a"; echo "$PB" > "$TMPD/b"
	diff "$TMPD/a" "$TMPD/b" | sed 's/^/       /'
fi

# --- 4. patch sets ---
LA=$(ls workspace/tg5040/cores/patches | sort)
LB=$(ls workspace/miyoomini/cores/patches | sort)
if [ "$LA" = "$LB" ]; then
	ok "core patch sets match"
else
	bad "core patch file sets differ:"
	echo "$LA" > "$TMPD/a"; echo "$LB" > "$TMPD/b"
	diff "$TMPD/a" "$TMPD/b" | sed 's/^/       /'
fi

# --- 5. cfg keys/values ---
# "key = value" lines minus binds/comments; the lock prefix (-) is part of the value semantics but
# not of the key, so strip it for key identity and compare it as part of the value.
cfg_pairs() { # file -> "key<TAB>lock+value"
	[ -f "$1" ] || return 0
	grep -E "^-?[a-z0-9_]+ = " "$1" | grep -v "^bind" | sed 's/^\(-\{0,1\}\)\([a-z0-9_]*\) = \(.*\)$/\2\t\1\3/' | sort
}
check_cfg() { # <label> <fileA> <fileB>
	label=$1; fa=$2; fb=$3
	pa=$(cfg_pairs "$fa"); pb=$(cfg_pairs "$fb")
	keys=$( { echo "$pa"; echo "$pb"; } | cut -f1 | grep -v "^$" | sort -u )
	for k in $keys; do
		va=$(echo "$pa" | grep "^$k	" | cut -f2- || true)
		vb=$(echo "$pb" | grep "^$k	" | cut -f2- || true)
		if [ "$va" = "$vb" ]; then continue; fi
		if allow "$label" "$k"; then continue; fi
		if [ -z "$va" ]; then bad "$label: '$k' only on miyoomini (= $vb) — declare or sync"
		elif [ -z "$vb" ]; then bad "$label: '$k' only on tg5040 (= $va) — declare or sync"
		else bad "$label: '$k' differs (tg5040 '$va' vs miyoomini '$vb') — declare or sync"
		fi
	done
}
for p in $PAKS_A; do
	check_cfg "$p" "$A/paks/Emus/$p/default.cfg" "$B/paks/Emus/$p/default.cfg"
done
check_cfg "system" "$A/system.cfg" "$B/system.cfg"
ok "cfg keys/values checked against the allowlist"

# Every allowlist entry must still be a live divergence — a stale entry hides future drift on
# that key. (Comments and blanks skipped.)
while IFS= read -r line; do
	case "$line" in ''|'#'*) continue ;; esac
	l=${line%%:*}; k=${line#*:}
	if [ "$l" = system ]; then fa="$A/system.cfg"; fb="$B/system.cfg"
	else fa="$A/paks/Emus/$l/default.cfg"; fb="$B/paks/Emus/$l/default.cfg"; fi
	va=$(cfg_pairs "$fa" | grep "^$k	" | cut -f2- || true)
	vb=$(cfg_pairs "$fb" | grep "^$k	" | cut -f2- || true)
	[ "$va" = "$vb" ] && bad "allowlist entry '$line' is stale — values now match; remove it"
done < "$ALLOW"

echo
if [ "$FAIL" -eq 0 ]; then echo "== parity: ALL PASS =="; else echo "== parity: FAILURES =="; exit 1; fi

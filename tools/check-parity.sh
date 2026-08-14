#!/bin/sh
# Cross-platform parity check: tg5040 (reference) vs miyoomini AND h700 (CLAUDE.md
# "Multi-platform parity"; three-family scope, Dan 2026-08-10).
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
# comment (h700 entries are prefixed `h700/` so the two comparisons cannot mask each other).
# Anything not listed fails. `default-<device>.cfg` variants are tg5040-internal (device_tag
# specialisation) and are not compared.
#
# h700 runs a REDUCED set: it has no cores makefile or patch dir of its own — build-h700-stripped.sh
# installs tg5040's built cores verbatim, so pins/patches are identical by construction and there is
# nothing to drift. Pak sets and cfgs are compared exactly as for miyoomini.
set -u

A=skeleton/SYSTEM/tg5040
B=skeleton/SYSTEM/miyoomini
ALLOW=tools/parity-allowlist.txt
PEER=miyoomini      # label used in messages + allowlist prefixes; reset for the h700 pass
FAIL=0
TMPD=$(mktemp -d); trap 'rm -rf "$TMPD"' EXIT

allow() { # allow <pak> <key> -> 0 if declared (h700 entries are namespaced "h700/<pak>:<key>")
	if [ "$PEER" = miyoomini ]; then grep -q "^$1:$2\$" "$ALLOW" 2>/dev/null
	else grep -q "^$PEER/$1:$2\$" "$ALLOW" 2>/dev/null; fi
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
echo "$PAKS_A" > "$TMPD/paks_a"   # reused by the h700 pass below
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
		if [ -z "$va" ]; then bad "[$PEER] $label: '$k' only on $PEER (= $vb) — declare or sync"
		elif [ -z "$vb" ]; then bad "[$PEER] $label: '$k' only on tg5040 (= $va) — declare or sync"
		else bad "[$PEER] $label: '$k' differs (tg5040 '$va' vs $PEER '$vb') — declare or sync"
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
	# h700 entries are namespaced "h700/<pak>:<key>" and compare tg5040 vs h700, not vs miyoomini.
	peer_root="$B"
	case "$l" in
		h700/*) peer_root=skeleton/SYSTEM/h700; l=${l#h700/} ;;
	esac
	if [ "$l" = system ]; then fa="$A/system.cfg"; fb="$peer_root/system.cfg"
	else fa="$A/paks/Emus/$l/default.cfg"; fb="$peer_root/paks/Emus/$l/default.cfg"; fi
	va=$(cfg_pairs "$fa" | grep "^$k	" | cut -f2- || true)
	vb=$(cfg_pairs "$fb" | grep "^$k	" | cut -f2- || true)
	[ "$va" = "$vb" ] && bad "allowlist entry '$line' is stale — values now match; remove it"
done < "$ALLOW"

# --- h700 pass -------------------------------------------------------------------------------
# Reduced set (see header): pak sets + cfgs. h700 ships tg5040's built cores verbatim, so core
# pins and patch sets cannot drift. Earned structural divergences are declared here, once, with
# the reason — anything NOT listed is a failure exactly as on the miyoomini pass.
echo
echo "-- h700 --"
PEER=h700
C=skeleton/SYSTEM/h700

# Paks the h700 legitimately does not ship yet. Each needs a reason; delete the line when it lands.
H700_PAK_EXEMPT="PAK.pak"   # native-ports launcher: no native port exists for h700 yet

PAKS_C=$(ls "$C/paks/Emus" | sort)
echo "$PAKS_C" > "$TMPD/paks_c"   # POSIX sh: no process substitution (a <(...) here silently
                                  # produced empty sets and the check printed a FALSE PASS)
MISSING=$(comm -23 "$TMPD/paks_a" "$TMPD/paks_c" 2>/dev/null || true)
UNEXPECTED=""
for p in $MISSING; do
	case " $H700_PAK_EXEMPT " in *" $p "*) continue ;; esac
	UNEXPECTED="$UNEXPECTED $p"
done
EXTRA=$(comm -13 "$TMPD/paks_a" "$TMPD/paks_c" 2>/dev/null || true)
if [ -z "$UNEXPECTED" ] && [ -z "$EXTRA" ]; then
	ok "[h700] pak set matches tg5040 ($(echo "$PAKS_C" | wc -l | tr -d ' ') paks; exempt:$H700_PAK_EXEMPT)"
else
	[ -n "$UNEXPECTED" ] && bad "[h700] paks missing without an exemption:$UNEXPECTED"
	[ -n "$EXTRA" ] && bad "[h700] paks present only on h700:$EXTRA"
fi

for p in $PAKS_C; do
	[ -f "$A/paks/Emus/$p/launch.sh" ] && [ -f "$C/paks/Emus/$p/launch.sh" ] || continue
	# h700 paks are currently bespoke scripts rather than the standard EMU_EXE boilerplate, so
	# read the core from either form: `EMU_EXE=<core>` or the inline `<core>_libretro.so` argument.
	core_of() {
		c=$(grep -m1 "^EMU_EXE=" "$1" 2>/dev/null | cut -d= -f2)
		[ -n "$c" ] || c=$(grep -o -m1 "[a-z0-9_-]*_libretro\.so" "$1" 2>/dev/null | head -1 | sed 's/_libretro\.so$//')
		echo "$c"
	}
	ea=$(core_of "$A/paks/Emus/$p/launch.sh")
	ec=$(core_of "$C/paks/Emus/$p/launch.sh")
	[ "$ea" = "$ec" ] || bad "[h700] $p: core differs ($ea vs $ec)"
done
ok "[h700] EMU_EXE checked for every shared pak"

for p in $PAKS_C; do
	check_cfg "$p" "$A/paks/Emus/$p/default.cfg" "$C/paks/Emus/$p/default.cfg"
done
ok "[h700] cfg keys/values checked against the allowlist"

echo
if [ "$FAIL" -eq 0 ]; then echo "== parity: ALL PASS =="; else echo "== parity: FAILURES =="; exit 1; fi

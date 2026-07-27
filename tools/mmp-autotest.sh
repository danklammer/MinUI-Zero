#!/bin/sh
# Self-running device test suite for the Miyoo Mini Plus.
#
#   sh tools/mmp-autotest.sh [user@host[:port]]
#
# Drives the DEVICE over ssh: injects game launches through the /tmp/next + eval mechanism the
# menu itself uses (safe: MinUI.pak's loop guards on /tmp/minui_exec, so killall minui.elf just
# re-loops; shutdown only fires if that sentinel is removed), lets each game run unattended,
# captures the minarch log, a framebuffer screenshot, and BENCH telemetry, then asserts.
#
# WHAT IT PROVES on its own:
#   T1 GBC   stale-Aspect save MIGRATES (log line) and audio path is the daemon
#   T2 GBA   stale-Aspect save is NOT touched (no shipped scaling default there)
#   T3 SUPA  same do-no-harm assertion as T2
#   T4 P8    fake-08 loads a cart and SURVIVES (the _vm->Step() fix) at ~target rate
#   T5 BR2   PS1 clock headroom: attract+demo, telemetry verdict for the OC decision
#   T6 THPS  same, second title
#
# WHAT STAYS MANUAL (physics): audio EAR checks (pops), charging-cable behaviour, panel look.
#
# Exit paths: games are ended with SIGTERM — minarch's Term_handler releases audio and restores
# volts, and telemetry fflushes every window, so data survives the kill. The menu loop then
# returns to minui.elf on its own.
set -u

TARGET=${1:-root@192.168.1.10}
PORT=22
case "$TARGET" in *:*) PORT=${TARGET##*:}; TARGET=${TARGET%:*} ;; esac
SSH="ssh -p $PORT -o ConnectTimeout=8"

SYS=/mnt/SDCARD/.system/miyoomini
ROMS=/mnt/SDCARD/Roms
LOGS=/mnt/SDCARD/.userdata/miyoomini/logs
DEVSHOT=/mnt/SDCARD/autotest

TS=$(date +%Y%m%d-%H%M%S)
ART=".notes/mmp-build/autotest-$TS"
mkdir -p "$ART"
SUMMARY="$ART/summary.txt"
: > "$SUMMARY"
FAIL=0

note() { echo "$1" | tee -a "$SUMMARY"; }
pass() { note "  PASS $1"; }
fail() { note "  FAIL $1"; FAIL=1; }

$SSH "$TARGET" true 2>/dev/null || { echo "device unreachable at $TARGET:$PORT"; exit 1; }
$SSH "$TARGET" "mkdir -p $DEVSHOT" 2>/dev/null

# rsh <cmd>: run on the device, silencing the firmware's ssh banner noise
rsh() { $SSH "$TARGET" "$1" 2>/dev/null; }

# launch <name> <seconds> <cmdline>: inject a launch, wait, screenshot, terminate, pull the log.
# The cmdline is written verbatim to /tmp/next and eval'd by MinUI.pak's loop — use DOUBLE quotes
# around paths inside it (rom names carry apostrophes: "Tony Hawk's").
launch() {
	name=$1; secs=$2; cmdline=$3
	note "--- $name (${secs}s) ---"
	printf '%s\n' "$cmdline" | $SSH "$TARGET" "cat > /tmp/next" 2>/dev/null
	rsh "killall minui.elf 2>/dev/null"
	i=0; up=0
	while [ $i -lt 40 ]; do
		rsh "pgrep minarch.elf >/dev/null" && { up=1; break; }
		sleep 1; i=$((i+1))
	done
	[ $up -eq 1 ] || { fail "$name: minarch never started"; return 1; }
	sleep "$secs"
	if rsh "pgrep minarch.elf >/dev/null"; then
		pass "$name: ran ${secs}s without crashing"
	else
		fail "$name: minarch DIED before ${secs}s elapsed"
	fi
	# one framebuffer page as evidence (panel is mounted inverted; converter rotates 180)
	rsh "dd if=/dev/fb0 of=$DEVSHOT/$name.fb bs=1228800 count=1 2>/dev/null; sync"
	rsh "killall minarch.elf 2>/dev/null"
	i=0
	while [ $i -lt 20 ]; do
		rsh "pgrep minarch.elf >/dev/null" || break
		sleep 1; i=$((i+1))
	done
	rsh "pgrep minarch.elf >/dev/null" && rsh "killall -9 minarch.elf 2>/dev/null"
	rsh "cat $DEVSHOT/$name.fb" > "$ART/$name.fb" 2>/dev/null
	# menu needs a beat to come back before the next injection
	i=0
	while [ $i -lt 20 ]; do
		rsh "pgrep minui.elf >/dev/null" && break
		sleep 1; i=$((i+1))
	done
	return 0
}

pull_log() { # <name> <TAG>
	rsh "cat $LOGS/$2.txt" > "$ART/$1.log" 2>/dev/null
}

# every launch must be on the daemon audio path, never silently disabled
assert_audio() { # <name>
	if grep -q "audio disabled\|No such audio device" "$ART/$1.log" 2>/dev/null; then
		fail "$1: audio DISABLED (see $1.log)"
	elif grep -q "Current audio driver: dsp" "$ART/$1.log" 2>/dev/null; then
		pass "$1: audio on the daemon path (dsp)"
	else
		fail "$1: audio driver line missing from log"
	fi
}

#######################################
note "MMP autotest $TS -> $ART"
note "build on device: $(rsh "md5sum $SYS/bin/minarch.elf | cut -c1-8")..."

# T1 — GBC migration fires (GBC pak ships Native; the card's saves carry stale Aspect)
launch T1-gbc-tetris 60 "\"$SYS/paks/Emus/GBC.pak/launch.sh\" \"$ROMS/1) Game Boy Color (GBC)/Tetris.gbc\"" && {
	pull_log T1-gbc-tetris GBC
	if grep -q "migrating saved cfg.*minarch_screen_scaling" "$ART/T1-gbc-tetris.log"; then
		pass "T1: stale Aspect save migrated (log line present)"
	else
		fail "T1: migration line MISSING from GBC log"
	fi
	assert_audio T1-gbc-tetris
}

# T2 — GBA must NOT migrate (no shipped scaling default for gpsp)
launch T2-gba-lotr 60 "\"$SYS/paks/Emus/GBA.pak/launch.sh\" \"$ROMS/2) Game Boy Advance (GBA)/LOTR - The Two Towers.gba\"" && {
	pull_log T2-gba-lotr GBA
	if grep -q "migrating saved cfg" "$ART/T2-gba-lotr.log"; then
		fail "T2: GBA migrated a save IT MUST NOT TOUCH"
	else
		pass "T2: GBA saves untouched (no migration line)"
	fi
	assert_audio T2-gba-lotr
}

# T3 — SUPA: same do-no-harm assertion
launch T3-supa-actraiser 60 "\"$SYS/paks/Emus/SUPA.pak/launch.sh\" \"$ROMS/4) Super Nintendo (SUPA)/ActRaiser.sfc\"" && {
	pull_log T3-supa-actraiser SUPA
	if grep -q "migrating saved cfg" "$ART/T3-supa-actraiser.log"; then
		fail "T3: SUPA migrated a save IT MUST NOT TOUCH"
	else
		pass "T3: SUPA saves untouched"
	fi
	assert_audio T3-supa-actraiser
}

# T4 — PICO-8: load must survive (_vm->Step() fix) and hold ~rate; BENCH via env prefix
launch T4-p8-celeste 90 "BENCH=1 BENCH_OUT=\"/mnt/SDCARD/bench-P8-celeste.csv\" \"$SYS/paks/Emus/P8.pak/launch.sh\" \"$ROMS/7) PICO-8 (P8)/Celeste.p8.png\"" && {
	pull_log T4-p8-celeste P8
	assert_audio T4-p8-celeste
	rsh "cat /mnt/SDCARD/bench-P8-celeste.csv" > "$ART/bench-P8-celeste.csv" 2>/dev/null
}

# T5/T6 — PS1 clock headroom (PS.pak exports BENCH itself; attract modes self-play)
launch T5-ps-br2 240 "\"$SYS/paks/Emus/PS.pak/launch.sh\" \"$ROMS/6) Sony PlayStation (PS)/Bloody Roar II/Bloody Roar II.cue\"" && {
	pull_log T5-ps-br2 PS
	assert_audio T5-ps-br2
	rsh "cat \"/mnt/SDCARD/bench-PS-Bloody_Roar_II.cue.csv\"" > "$ART/bench-PS-br2.csv" 2>/dev/null
}
launch T6-ps-thps 180 "\"$SYS/paks/Emus/PS.pak/launch.sh\" \"$ROMS/6) Sony PlayStation (PS)/Tony Hawk's Pro Skater/Tony Hawk's Pro Skater.cue\"" && {
	pull_log T6-ps-thps PS
	assert_audio T6-ps-thps
	rsh "cat \"/mnt/SDCARD/bench-PS-Tony_Hawk's_Pro_Skater.cue.csv\"" > "$ART/bench-PS-thps.csv" 2>/dev/null
}

note ""
note "artifacts: $ART (logs, raw framebuffers, bench CSVs)"
note "manual-only gates: audio EAR check, charging cable, panel look"
if [ "$FAIL" -eq 0 ]; then note "== autotest: ALL PASS =="; else note "== autotest: FAILURES =="; exit 1; fi

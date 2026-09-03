#!/bin/sh
# brick-test.sh — automated on-device test harness for the tg5040 (Brick / Brick Pro / Smart Pro).
# Runs from the Mac over ssh, keeps the device awake, runs a battery of checks, and prints a
# PASS/FAIL line plus a receipt for each. Full transcript is saved under .notes/.
#
#   sh tools/brick-test.sh root@<ip> [-i ~/.ssh/tg5040_dev]
#
# Design: every check is a shell function that echoes exactly one "RESULT <name> PASS|FAIL|INFO
# <receipt>" line. Non-gameplay checks are fully automated. Gameplay checks (governor-under-load,
# underruns) need a human or the injector and are marked SKIP with the reason — this harness never
# fakes a measurement (see .notes/2026-09-02: two nights lost to contaminated/absent baselines).
set -u

TARGET="${1:?usage: brick-test.sh root@<ip> [-i keyfile]}"; shift || true
KEY=""
while [ $# -gt 0 ]; do case "$1" in -i) KEY="-i $2"; shift 2;; *) shift;; esac; done
SSH="ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR $KEY $TARGET"
OUTDIR="$(cd "$(dirname "$0")/.." && pwd)/.notes/$(date +%Y-%m-%d)-brick-test"
mkdir -p "$OUTDIR"
LOG="$OUTDIR/run-$(date +%H%M%S).log"

sshc() { $SSH "$@" 2>/dev/null; }
awake() { sshc 'touch /tmp/stay_awake' >/dev/null 2>&1; }

wait_up() {
	i=0; while [ $i -lt 30 ]; do
		if sshc 'echo ok' | grep -q ok; then awake; return 0; fi
		sleep 10; i=$((i+1))
	done
	echo "RESULT device_reachable FAIL (no ssh after 5 min)"; exit 1
}

# ---- checks (each echoes one RESULT line) --------------------------------------------------

check_clock_state() {
	awake
	D=$(sshc 'S=$(date +%s); R=$(date -d "$(hwclock -r 2>/dev/null | sed s/\\..*//)" +%s 2>/dev/null || echo NA);
		echo "sys=$(date +%H:%M) utc=$(date -u +%H:%M) tz=[${TZ:-unset}] etc_tz=$(cat /etc/TZ 2>/dev/null || echo none) localtime=$(readlink /etc/localtime 2>/dev/null || echo none) zonename=$(uci -q get system.@system[0].zonename 2>/dev/null || echo none) offset=$([ "$R" != NA ] && echo $((S-R))s || echo NA)"')
	# PASS only if a timezone is actually pinned somewhere; a bare "unset/none" TZ is the known bug.
	if echo "$D" | grep -q 'tz=\[unset\] etc_tz=none .*zonename=none'; then
		echo "RESULT clock_timezone_pinned FAIL no TZ anywhere -> $D"
	else
		echo "RESULT clock_timezone_pinned PASS $D"
	fi
}

# Sets a known time, reboots, and re-reads it. Proves the round-trip end to end. Slow (one reboot).
check_clock_roundtrip() {
	awake
	sshc 'date -s "2026-06-15 09:00:00" >/dev/null 2>&1; hwclock -w -u 2>/dev/null'
	BEFORE=$(sshc 'echo "$(date +%H:%M) rtc=$(hwclock -r 2>/dev/null | sed s/\\..*//)"')
	sshc 'sync; touch /tmp/prereboot; (sleep 1; reboot) >/dev/null 2>&1 &' >/dev/null 2>&1
	sleep 20; wait_up
	AFTER=$(sshc 'echo "$(date +%H:%M) rtc=$(hwclock -r 2>/dev/null | sed s/\\..*//)"')
	BH=$(echo "$BEFORE" | grep -o '^[0-9]*'); AH=$(echo "$AFTER" | grep -o '^[0-9]*')
	if [ "$BH" = "$AH" ]; then
		echo "RESULT clock_survives_reboot PASS set 09:xx -> before[$BEFORE] after[$AFTER]"
	else
		echo "RESULT clock_survives_reboot FAIL hour drifted set 09 -> before[$BEFORE] after[$AFTER]"
	fi
}

check_idle_wakeups() {
	awake
	R=$(sshc 'cp /proc/interrupts /tmp/i0; sleep 10; awk "NR==FNR{for(i=2;i<=5;i++)a[\$1]+=\$i;next}{s=0;for(i=2;i<=5;i++)s+=\$i;d=s-a[\$1];if(d>30)printf \"%s=%d/s \",\$NF,d/10}" /tmp/i0 /proc/interrupts')
	TOTAL=$(echo "$R" | grep -oE '[0-9]+/s' | grep -oE '^[0-9]+' | awk '{s+=$1} END{print s+0}')
	# INFO not PASS/FAIL: this is a measurement to track, not a pass gate. twi3 storm shows here.
	echo "RESULT idle_wakeups INFO total~${TOTAL}/s movers: $R"
}

check_radios_leds() {
	awake
	R=$(sshc 'echo "bt=$(rfkill list 2>/dev/null | grep -ic bluetooth || echo ?) leds=$(for l in /sys/class/leds/*/brightness; do cat $l 2>/dev/null; done | paste -sd, -)"')
	echo "RESULT radios_leds INFO $R"
}

check_governor_profile() {
	awake
	R=$(sshc 'C=/sys/devices/system/cpu/cpu0/cpufreq; echo "gov=$(cat $C/scaling_governor) min=$(cat $C/scaling_min_freq) max=$(cat $C/scaling_max_freq) opps=[$(cat $C/scaling_available_frequencies)]"')
	echo "$R" | grep -q 'gov=schedutil' && V=PASS || V=FAIL
	echo "RESULT governor_schedutil $V $R"
}

# reads the newest game log for the binding + underrun receipts, if a game has been played
check_last_game_receipts() {
	awake
	R=$(sshc 'F=$(ls -t /mnt/SDCARD/.userdata/tg5040/logs/*.txt 2>/dev/null | grep -vE "minui|model" | head -1);
		[ -n "$F" ] || { echo "no game log yet"; exit 0; }
		echo "$(basename $F): $(grep -oE "gov-cap: at-ceil [0-9]+/[0-9]+ ticks ceil=[0-9]+ actual=[0-9]+" $F | tail -1); $(grep -E "servo-stats" $F | tail -1 | sed s/.INFO.//)"')
	echo "RESULT last_game_receipts INFO $R"
}

check_gameplay_gated() {
	echo "RESULT gameplay_under_load SKIP needs a driver: light-scene injection reaches only calm scenes (0 underruns on any build); a real heavy-scene run needs the human. Do it live and read gov-cap/servo-stats."
}

# ---- run -----------------------------------------------------------------------------------
{
	echo "brick-test $(date) target=$TARGET"
	wait_up
	echo "RESULT device_reachable PASS"
	check_clock_state
	check_idle_wakeups
	check_radios_leds
	check_governor_profile
	check_last_game_receipts
	check_gameplay_gated
	# clock_roundtrip runs LAST because it reboots the device
	if [ "${BRICK_TEST_REBOOT:-0}" = "1" ]; then check_clock_roundtrip
	else echo "RESULT clock_survives_reboot SKIP set BRICK_TEST_REBOOT=1 to run (reboots the device)"; fi
} | tee "$LOG"

echo ""
echo "=== SUMMARY ==="
grep '^RESULT' "$LOG" | awk '{printf "%-26s %s\n", $2, $3}'
echo "full log: $LOG"

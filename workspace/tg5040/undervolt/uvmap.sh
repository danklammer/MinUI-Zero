#!/bin/sh
# uvmap.sh — self-resuming per-chip voltage margin mapper (undervolt P2).
#
# HOW IT WORKS: for each OPP, pin the clock, step the CPU rail down 12.5mV at a time
# (RAM-only, via uvtool), run the stressor under a petted hardware watchdog. A crash or
# hang reboots the device; on boot, auto.sh re-runs this script, which sees the state
# file still says "testing OPP X at Y uV" and concludes THAT POINT CRASHED — records the
# cliff, moves to the next OPP at stock voltage. When every OPP is mapped it disarms
# itself. The crash boundary IS the data; the watchdog IS the safety.
#
# ARM:    mkdir -p $UV_DIR && touch $UV_DIR/ARMED && sh uvmap.sh &   (device plugged in!)
# DISARM: rm $UV_DIR/ARMED  (takes effect next boot; harness also disarms when done)
#
# Nothing here persists a voltage: every reboot returns to the kernel's stock table.

UV_DIR=/mnt/SDCARD/.userdata/tg5040/undervolt
STATE=$UV_DIR/state          # "<opp_khz> <testing_uv>"
LOG=$UV_DIR/margins.log
BIN=$UV_DIR
P=/sys/devices/system/cpu/cpu0/cpufreq

OPPS="1800000 1608000 1416000 1200000 1008000 816000 600000 408000"
STEP=12500
FLOOR=762500
STRESS_SECS=75

[ -f "$UV_DIR/ARMED" ] || exit 0
mkdir -p "$UV_DIR"

# refuse to run on battery — a mid-campaign battery death looks like a crash datapoint
STATUS=$(cat /sys/class/power_supply/axp2202-battery/status 2>/dev/null)
if [ "$STATUS" = "Discharging" ]; then
	echo "$(date +%T) not charging — campaign paused this boot" >> "$LOG"
	exit 0
fi

# keep the device awake and out of MinUI's way for the whole campaign
touch /tmp/stay_awake
killall -9 minarch.elf minui.elf 2>/dev/null

stock_uv_for() { # read the rail after pinning (kernel applies stock OPP voltage)
	for r in /sys/class/regulator/regulator.*; do
		[ "$(cat $r/name 2>/dev/null)" = "tcs4838-dcdc0" ] && cat $r/microvolts && return
	done
}

pin() {
	echo "$1" > $P/scaling_max_freq 2>/dev/null
	echo "$1" > $P/scaling_min_freq 2>/dev/null
	sleep 1
}

# watchdog petter: keep-alive every 5s; if the machine locks, the dog fires (~16s).
# REFUSE to calibrate without it — a marginal voltage can hang the machine indefinitely
# instead of rebooting-and-recording-a-cliff (Codex audit 2026-07-09).
if ! ( exec 3<> /dev/watchdog ) 2>/dev/null; then
	# exclusive open failed — on procd systems (this BSP) PID 1 owns and feeds the dog
	# full-time, which provides the same hang-reset guarantee: a lockup stops procd's
	# feeding and the hardware dog fires. Proceed under procd protection; refuse only
	# when NOBODY is petting the hardware.
	if ls -l /proc/1/fd 2>/dev/null | grep -q watchdog; then
		echo "uvmap: watchdog held by procd (PID 1) — proceeding under its feeder"
		touch /tmp/uvmap.running
		WDPID=""
	else
		echo "uvmap: /dev/watchdog unavailable — refusing to run an unprotected calibration"
		exit 1
	fi
else
	( exec 3<> /dev/watchdog || exit 0
	  while [ -f /tmp/uvmap.running ]; do echo k >&3; sleep 5; done
	  printf 'V' >&3 # magic close: disarm cleanly when the campaign exits on its own
	) &
	touch /tmp/uvmap.running
	WDPID=$!
fi

GUARD=50000 # -50mV guardband below each measured cliff

gen_table() {
	# Build table.conf from the log: for each OPP, guardband below its CLIFF; for OPPs that
	# reached the floor without cracking (explicit DONE), guardband above the tested floor.
	# REFUSE to publish unless every OPP carries a verdict: a refused uvtool write or
	# malformed state can leave an OPP with neither CLIFF nor DONE, and guessing "floor"
	# for an untested OPP would apply an unproven voltage (audit 2026-07-11).
	OUT="$UV_DIR/table.conf"
	for OPP in 408000 600000 816000 1008000 1200000 1416000 1608000 1800000; do
		grep -q "^$OPP CLIFF\|^$OPP DONE" "$LOG" 2>/dev/null || {
			echo "$(date +%T) gen_table: $OPP has no CLIFF/DONE verdict — refusing to publish" >> "$LOG"
			return 1
		}
	done
	: > "$OUT.tmp"
	for OPP in 408000 600000 816000 1008000 1200000 1416000 1608000 1800000; do
		CLIFF=$(grep "^$OPP CLIFF" "$LOG" 2>/dev/null | tail -1 | awk "{print \$3}")
		if [ -n "$CLIFF" ]; then
			UV=$((CLIFF + GUARD))
		else
			# explicit DONE at floor without cracking (verified present above)
			UV=$((FLOOR + GUARD))
		fi
		# quantize down to a 12.5mV step and clamp to [FLOOR..stock-safe]
		UV=$(( (UV - 712500) / 12500 * 12500 + 712500 ))
		[ "$UV" -lt "$FLOOR" ] && UV=$FLOOR
		echo "$OPP $UV" >> "$OUT.tmp"
	done
	mv "$OUT.tmp" "$OUT"
	# bind the table to THIS device model (card-swap guard: a Brick table must never
	# apply on a Smart Pro — the chips have different margins)
	MODEL="$TRIMUI_MODEL"
	[ -z "$MODEL" ] && MODEL=$(strings /usr/trimui/bin/MainUI 2>/dev/null | grep ^Trimui | head -1)
	echo "$MODEL" > "$UV_DIR/table.model"
	# chip binding: the eFUSE serial is unique per die — the table belongs to THIS chip only
	CHIP=$(grep sunxi_serial /sys/class/sunxi_info/sys_info 2>/dev/null | awk -F: "{print \$2}" | tr -d " \t")
	[ -n "$CHIP" ] && echo "$CHIP" > "$UV_DIR/table.chip"
	# state file for the tool UI: worst-case APPLIED reduction at the high OPPs —
	# stock voltage minus the PUBLISHED table row (cliff+guard), not stock-to-cliff.
	# The UI's benefit claims must describe what production actually applies
	# (audit 2026-07-11: the old stock-to-cliff figure overstated the benefit).
	MINSAVE=999000
	for OPP in 1200000 1416000 1608000 1800000; do
		APPLIED=$(grep "^$OPP " "$OUT" | awk "{print \$2}")
		[ -z "$APPLIED" ] && continue
		STOCK=$(grep "opp $OPP: stock" "$LOG" | tail -1 | awk "{print \$5}")
		case "$STOCK" in *[!0-9]*|"") continue;; esac
		M=$((STOCK - APPLIED))
		[ "$M" -lt 0 ] && M=0
		[ "$M" -lt "$MINSAVE" ] && MINSAVE=$M
	done
	{
		echo "calibrated=$(date +%Y-%m-%d)"
		echo "min_margin_mv=$((MINSAVE/1000))" # key kept for compat; value = applied reduction
	} > "$UV_DIR/calibration"
	sync
}

finish() {
	rm -f /tmp/uvmap.running
	if gen_table; then
		echo "$(date +%T) campaign COMPLETE — table.conf written, disarming" >> "$LOG"
		rm -f "$UV_DIR/ARMED" "$STATE" "$UV_DIR/RETRIES"
	else
		# incomplete: some OPP has no verdict. Stay armed so the resume loop re-runs
		# just the missing OPPs next boot — but cap retries so a persistently failing
		# OPP can't reboot-loop the device forever.
		N=$(cat "$UV_DIR/RETRIES" 2>/dev/null); [ -n "$N" ] || N=0
		N=$((N + 1)); echo "$N" > "$UV_DIR/RETRIES"
		if [ "$N" -ge 3 ]; then
			echo "$(date +%T) campaign INCOMPLETE after $N attempts — giving up, no table published (stock voltages remain)" >> "$LOG"
			rm -f "$UV_DIR/ARMED" "$STATE" "$UV_DIR/RETRIES"
		else
			echo "$(date +%T) campaign incomplete (attempt $N) — staying armed to retry missing OPPs" >> "$LOG"
			rm -f "$STATE"
		fi
	fi
	sync
	reboot
}

# --- crash bookkeeping: a leftover state line means that point rebooted us ---
if [ -f "$STATE" ]; then
	read CRASH_OPP CRASH_UV < "$STATE"
	echo "$CRASH_OPP CLIFF $CRASH_UV (crashed)" >> "$LOG"
	echo "$(date +%T) opp $CRASH_OPP: cliff at $CRASH_UV uV" >> "$LOG"
	rm -f "$STATE"; sync
	# resume from the OPP AFTER the crashed one
	SKIP=1
else
	SKIP=0
	echo "=== uvmap campaign start $(date +%T) ===" >> "$LOG"
fi

PAST_CRASH=0
for OPP in $OPPS; do
	if [ "$SKIP" = "1" ] && [ "$PAST_CRASH" = "0" ]; then
		[ "$OPP" = "$CRASH_OPP" ] && PAST_CRASH=1
		grep -q "^$OPP CLIFF\|^$OPP DONE" "$LOG" 2>/dev/null && continue
		[ "$PAST_CRASH" = "0" ] && continue
		continue # the crashed OPP itself is finished (cliff recorded)
	fi
	grep -q "^$OPP CLIFF\|^$OPP DONE" "$LOG" 2>/dev/null && continue

	pin "$OPP"
	UV=$(stock_uv_for)
	echo "$(date +%T) opp $OPP: stock $UV uV, stepping down" >> "$LOG"; sync

	while [ "$UV" -gt "$FLOOR" ]; do
		# re-check disarm and charger EVERY round (audit 2026-07-11: cancel used to be
		# read only at startup, so a running campaign outlived the user's cancel; and a
		# yanked charger mid-campaign kept stressing on battery)
		if [ ! -f "$UV_DIR/ARMED" ]; then
			echo "$(date +%T) opp $OPP: cancelled mid-campaign — stopping cleanly" >> "$LOG"
			rm -f "$STATE" /tmp/uvmap.running; sync
			exit 0
		fi
		CHG=$(cat /sys/class/power_supply/axp2202-battery/status 2>/dev/null)
		if [ "$CHG" != "Charging" ] && [ "$CHG" != "Full" ]; then
			echo "$(date +%T) opp $OPP: charger lost mid-campaign — pausing (resumes next charged boot)" >> "$LOG"
			rm -f "$STATE" /tmp/uvmap.running; sync
			exit 0
		fi
		UV=$((UV - STEP))
		echo "$OPP $UV" > "$STATE"; sync   # if we reboot past here, this point crashed
		# userspace deadman: a SOFT wedge (CPU stuck but procd still feeding the hw dog)
		# hangs the campaign forever — the procd-held-watchdog path can't catch it.
		# If this round runs far past budget, force the reboot ourselves; the breadcrumb
		# above then records this voltage as the cliff. (Live-fired 2026-07-11: chip
		# wedged mid-campaign and needed a manual power cut.)
		( sleep $((STRESS_SECS + 120)) && reboot -f ) &
		DEADMAN=$!
		"$BIN/uvtool" set "$UV" >> "$LOG" 2>&1 || { kill $DEADMAN 2>/dev/null; echo "$OPP uvtool-refused at $UV" >> "$LOG"; break; }
		if ! "$BIN/stress" $STRESS_SECS >> "$LOG" 2>&1; then
			kill $DEADMAN 2>/dev/null
			echo "$OPP CLIFF $UV (stress-fail)" >> "$LOG"; sync
			break
		fi
		kill $DEADMAN 2>/dev/null
		echo "$(date +%T) opp $OPP: $UV uV survived" >> "$LOG"; sync
	done
	[ "$UV" -le "$FLOOR" ] && echo "$OPP DONE floor $FLOOR reached, no cliff" >> "$LOG"
	rm -f "$STATE"; sync
	# restore stock voltage for this OPP by bouncing the clock (kernel re-asserts)
	pin 408000; pin "$OPP"; pin 408000
done

finish

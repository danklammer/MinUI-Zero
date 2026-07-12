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
CAMPAIGN_CHIP=$UV_DIR/campaign.chip
CAMPAIGN_MODEL=$UV_DIR/campaign.model

OPPS="1800000 1608000 1416000 1200000 1008000 816000 600000 408000"
ASC_OPPS="408000 600000 816000 1008000 1200000 1416000 1608000 1800000"
BASE=712500
STEP=12500
FLOOR=762500
STOCK_MAX=1187500
STRESS_SECS=75

[ -f "$UV_DIR/ARMED" ] || exit 0
mkdir -p "$UV_DIR"

# auto.sh and the tool can be entered close together. Only one process may own the
# watchdog, cpufreq policy, or voltage rail; /tmp is cleared by every reboot.
LOCK=/tmp/uvmap.lock
if ! mkdir "$LOCK" 2>/dev/null; then
	echo "$(date +%T) campaign already running -- duplicate start ignored" >> "$LOG"
	exit 0
fi
trap 'rmdir "$LOCK" 2>/dev/null' EXIT
trap 'exit 1' HUP INT TERM

read_chip() {
	grep sunxi_serial /sys/class/sunxi_info/sys_info 2>/dev/null | awk -F: '{print $2}' | tr -d ' \t\r\n'
}

read_model() {
	if [ -n "$TRIMUI_MODEL" ]; then
		printf '%s\n' "$TRIMUI_MODEL"
	else
		strings /usr/trimui/bin/MainUI 2>/dev/null | grep '^Trimui' | head -1
	fi
}

verify_campaign_identity() {
	CURRENT_CHIP=$(read_chip)
	CURRENT_MODEL=$(read_model)
	SAVED_CHIP=$(cat "$CAMPAIGN_CHIP" 2>/dev/null)
	SAVED_MODEL=$(cat "$CAMPAIGN_MODEL" 2>/dev/null)
	[ -n "$CURRENT_CHIP" ] && [ -n "$CURRENT_MODEL" ] &&
		[ "$CURRENT_CHIP" = "$SAVED_CHIP" ] && [ "$CURRENT_MODEL" = "$SAVED_MODEL" ]
}

campaign_abort() {
	echo "$(date +%T) campaign ABORTED: $*" >> "$LOG"
	touch "$UV_DIR/INVALID"
	rm -f "$UV_DIR/ARMED" "$STATE" "$UV_DIR/RETRIES" "$UV_DIR/table.conf.tmp" \
		"$UV_DIR/table.chip.tmp" "$UV_DIR/table.model.tmp" "$UV_DIR/table.stock.tmp" \
		"$UV_DIR/calibration.tmp" \
		/tmp/uvmap.running
	sync
	reboot
	exit 1
}

# A campaign belongs to the chip and model recorded before its first voltage test. Never
# let a moved SD card turn another device's partial margins into a publishable table.
verify_campaign_identity || campaign_abort "device identity is missing or changed"

# Refuse anything except an affirmative charging state: an unknown PMIC response must not
# be interpreted as permission, because a battery death looks exactly like a voltage cliff.
STATUS=$(cat /sys/class/power_supply/axp2202-battery/status 2>/dev/null)
if [ "$STATUS" != "Charging" ] && [ "$STATUS" != "Full" ]; then
	echo "$(date +%T) not charging — campaign paused this boot" >> "$LOG"
	exit 0
fi

# keep the device awake and out of MinUI's way for the whole campaign
touch /tmp/stay_awake
killall -9 minarch.elf minui.elf 2>/dev/null

stock_uv_for() { # read the rail after pinning (kernel applies stock OPP voltage)
	for r in /sys/class/regulator/regulator.*; do
		[ "$(cat "$r/name" 2>/dev/null)" = "tcs4838-dcdc0" ] && cat "$r/microvolts" && return
	done
	return 1
}

is_uint() { case "$1" in ''|*[!0-9]*) return 1;; *) return 0;; esac; }
opp_known() { case "$1" in 408000|600000|816000|1008000|1200000|1416000|1608000|1800000) return 0;; *) return 1;; esac; }
on_step() { is_uint "$1" && [ "$1" -ge "$FLOOR" ] && [ $(( ($1 - BASE) % STEP )) -eq 0 ]; }

pin() {
	TARGET=$1
	opp_known "$TARGET" || return 1
	CURRENT_MIN=$(cat "$P/scaling_min_freq" 2>/dev/null)
	is_uint "$CURRENT_MIN" || return 1
	# Raising: open max before min. Lowering: lower min before max. The reverse lowering
	# order is rejected while min is still above the requested max on this cpufreq driver.
	if [ "$TARGET" -gt "$CURRENT_MIN" ]; then
		printf '%s\n' "$TARGET" > "$P/scaling_max_freq" 2>/dev/null || return 1
		printf '%s\n' "$TARGET" > "$P/scaling_min_freq" 2>/dev/null || return 1
	else
		printf '%s\n' "$TARGET" > "$P/scaling_min_freq" 2>/dev/null || return 1
		printf '%s\n' "$TARGET" > "$P/scaling_max_freq" 2>/dev/null || return 1
	fi
	sleep 1
	[ "$(cat "$P/scaling_min_freq" 2>/dev/null)" = "$TARGET" ] &&
		[ "$(cat "$P/scaling_max_freq" 2>/dev/null)" = "$TARGET" ] &&
		[ "$(cat "$P/scaling_cur_freq" 2>/dev/null)" = "$TARGET" ]
}

# Watchdog petter: open once and keep that descriptor alive. A probe open followed by an
# ordinary close can arm some watchdog drivers without disarming them, so readiness is
# reported through a marker by the long-lived petter itself.
rm -f /tmp/uvmap.watchdog-open
touch /tmp/uvmap.running
( exec 3<> /dev/watchdog || exit 1
  touch /tmp/uvmap.watchdog-open
  while [ -f /tmp/uvmap.running ]; do echo k >&3; sleep 5; done
  printf 'V' >&3
) 2>/dev/null &
WDPID=$!
sleep 1
if [ ! -f /tmp/uvmap.watchdog-open ] || ! kill -0 "$WDPID" 2>/dev/null; then
	wait "$WDPID" 2>/dev/null
	rm -f /tmp/uvmap.watchdog-open
	# exclusive open failed — on procd systems (this BSP) PID 1 owns and feeds the dog
	# full-time, which provides the same hang-reset guarantee: a lockup stops procd's
	# feeding and the hardware dog fires. Proceed under procd protection; refuse only
	# when NOBODY is petting the hardware.
	if ls -l /proc/1/fd 2>/dev/null | grep -q watchdog; then
		echo "uvmap: watchdog held by procd (PID 1) — proceeding under its feeder"
		WDPID=""
	else
		campaign_abort "no hardware watchdog owner was found"
	fi
else
	rm -f /tmp/uvmap.watchdog-open
fi

GUARD=50000 # production stays 50mV above each measured failure cliff

stock_for_opp() {
	awk -v opp="$1" '
		$2 == "opp" && $3 == opp ":" && $4 == "stock" && $5 ~ /^[0-9]+$/ {
			if (!n) value=$5
			else if ($5 != value) bad=1
			n++
		}
		END { if (n && !bad) print value; else exit 1 }
	' "$LOG" 2>/dev/null
}

has_any_verdict() {
	grep -Eq "^$1 (CLIFF|DONE)( |$)" "$LOG" 2>/dev/null
}

record_floor_done() {
	RD_OPP=$1
	RD_PROVEN=$2
	[ "$RD_PROVEN" = "1" ] || return 1
	# Never append DONE over a CLIFF. In particular, a stress failure at FLOOR must
	# remain a CLIFF rather than becoming a duplicate, malformed verdict.
	has_any_verdict "$RD_OPP" && return 1
	echo "$RD_OPP DONE floor $FLOOR reached, no cliff" >> "$LOG"
}

verdict_for() {
	VF_OPP=$1
	VF_RAW=$(awk -v opp="$VF_OPP" '
		$1 == opp && ($2 == "CLIFF" || $2 == "DONE") {
			n++
			if ($2 == "CLIFF" && $3 ~ /^[0-9]+$/) { kind="CLIFF"; value=$3 }
			else if ($2 == "DONE" && $3 == "floor" && $4 ~ /^[0-9]+$/) { kind="DONE"; value=$4 }
			else bad=1
		}
		END { printf "%d|%d|%s|%s\n", n, bad, kind, value }
	' "$LOG" 2>/dev/null)
	VF_N=${VF_RAW%%|*}; VF_REST=${VF_RAW#*|}
	VF_BAD=${VF_REST%%|*}; VF_REST=${VF_REST#*|}
	VF_KIND=${VF_REST%%|*}; VF_VALUE=${VF_REST#*|}
	[ "$VF_N" = "1" ] && [ "$VF_BAD" = "0" ] && [ -n "$VF_KIND" ] && [ -n "$VF_VALUE" ] || return 1
	case "$VF_VALUE" in *'|'*) return 1;; esac
	on_step "$VF_VALUE" || return 1
	VF_STOCK=$(stock_for_opp "$VF_OPP") || return 1
	on_step "$VF_STOCK" || return 1
	[ "$VF_STOCK" -le "$STOCK_MAX" ] || return 1
	if [ "$VF_KIND" = "CLIFF" ]; then
		[ "$VF_VALUE" -lt "$VF_STOCK" ] || return 1
	else
		[ "$VF_VALUE" = "$FLOOR" ] || return 1
	fi
	printf '%s %s\n' "$VF_KIND" "$VF_VALUE"
}

gen_table() {
	# Build table.conf from the log: for each OPP, guardband below its CLIFF; for OPPs that
	# reached the floor without cracking (explicit DONE), guardband above the tested floor.
	# REFUSE to publish unless every OPP carries a verdict: a refused uvtool write or
	# malformed state can leave an OPP with neither CLIFF nor DONE, and guessing "floor"
	# for an untested OPP would apply an unproven voltage (audit 2026-07-11).
	OUT="$UV_DIR/table.conf"
	for OPP in $ASC_OPPS; do
		verdict_for "$OPP" >/dev/null || {
			echo "$(date +%T) gen_table: $OPP lacks one valid verdict — refusing to publish" >> "$LOG"
			return 1
		}
	done
	rm -f "$OUT.tmp" "$UV_DIR/table.stock.tmp"
	RUNMAX=0
	for OPP in $ASC_OPPS; do
		VERDICT=$(verdict_for "$OPP") || { rm -f "$OUT.tmp"; return 1; }
		KIND=${VERDICT%% *}; VALUE=${VERDICT#* }
		[ -n "$KIND" ] && [ -n "$VALUE" ] && [ "$KIND $VALUE" = "$VERDICT" ] || { rm -f "$OUT.tmp"; return 1; }
		STOCK=$(stock_for_opp "$OPP") || { rm -f "$OUT.tmp"; return 1; }
		if [ "$OPP" -le 816000 ]; then
			# Runtime deliberately stands down at light-load clocks. Publish their exact
			# recorded stock rows so the monotonic envelope also covers a clock raise.
			UV=$STOCK
		elif [ "$KIND" = "CLIFF" ]; then
			UV=$((VALUE + GUARD))
		else
			# A floor survivor still gets the same positive guard, capped to this OPP's
			# measured stock voltage so low rows can never become an overvolt.
			UV=$((FLOOR + GUARD))
		fi
		UV=$(( (UV - BASE) / STEP * STEP + BASE ))
		[ "$UV" -lt "$FLOOR" ] && UV=$FLOOR
		[ "$UV" -gt "$STOCK" ] && UV=$STOCK
		# The held rail for a ceiling covers every lower OPP, so publish the running
		# maximum rather than relying on a hidden runtime normalization.
		[ "$UV" -lt "$RUNMAX" ] && UV=$RUNMAX
		if [ "$UV" -gt "$STOCK" ]; then
			echo "$(date +%T) gen_table: envelope $UV exceeds $OPP stock $STOCK — refusing" >> "$LOG"
			rm -f "$OUT.tmp"
			return 1
		fi
		RUNMAX=$UV
		echo "$OPP $UV" >> "$OUT.tmp"
		echo "$OPP $STOCK" >> "$UV_DIR/table.stock.tmp"
	done
	verify_campaign_identity || { rm -f "$OUT.tmp"; return 1; }
	# state file for the tool UI: worst-case APPLIED reduction at the high OPPs —
	# stock voltage minus the PUBLISHED table row (cliff+guard), not stock-to-cliff.
	# The UI's benefit claims must describe what production actually applies
	# (audit 2026-07-11: the old stock-to-cliff figure overstated the benefit).
	MINSAVE=999000
	TOPSAVE=0
	for OPP in 1200000 1416000 1608000 1800000; do
		APPLIED=$(awk -v opp="$OPP" '$1 == opp { n++; value=$2 } END { if (n == 1) print value; else exit 1 }' "$OUT.tmp") || return 1
		STOCK=$(stock_for_opp "$OPP") || return 1
		M=$((STOCK - APPLIED))
		[ "$M" -lt 0 ] && M=0
		[ "$M" -lt "$MINSAVE" ] && MINSAVE=$M
		[ "$OPP" = "1800000" ] && TOPSAVE=$M
	done
	[ "$MINSAVE" -lt 999000 ] && [ "$TOPSAVE" -gt 0 ] || return 1
	{
		echo "calibrated=$(date +%Y-%m-%d)"
		echo "min_margin_mv=$((MINSAVE/1000))" # key kept for compat; value = applied reduction
		echo "top_reduction_mv=$((TOPSAVE/1000))"
	} > "$UV_DIR/calibration.tmp"
	printf '%s\n' "$SAVED_MODEL" > "$UV_DIR/table.model.tmp"
	printf '%s\n' "$SAVED_CHIP" > "$UV_DIR/table.chip.tmp"
	# Publish table.conf last. Runtime also requires table.chip and table.stock, so a
	# power loss during these renames can only leave the voltage authority disabled.
	sync
	mv "$UV_DIR/table.model.tmp" "$UV_DIR/table.model" || return 1
	mv "$UV_DIR/table.chip.tmp" "$UV_DIR/table.chip" || return 1
	mv "$UV_DIR/table.stock.tmp" "$UV_DIR/table.stock" || return 1
	mv "$UV_DIR/calibration.tmp" "$UV_DIR/calibration" || return 1
	mv "$OUT.tmp" "$OUT" || return 1
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
		N=$(cat "$UV_DIR/RETRIES" 2>/dev/null)
		case "$N" in ''|*[!0-9]*) N=0;; esac
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
	set -- $(cat "$STATE" 2>/dev/null)
	[ "$#" = "2" ] || campaign_abort "malformed crash breadcrumb"
	CRASH_OPP=$1; CRASH_UV=$2
	opp_known "$CRASH_OPP" && on_step "$CRASH_UV" || campaign_abort "invalid crash breadcrumb '$CRASH_OPP $CRASH_UV'"
	CRASH_STOCK=$(stock_for_opp "$CRASH_OPP") || campaign_abort "crash breadcrumb has no measured stock row"
	[ "$CRASH_UV" -lt "$CRASH_STOCK" ] || campaign_abort "crash voltage is not below stock"
	has_any_verdict "$CRASH_OPP" && campaign_abort "duplicate verdict for crashed OPP $CRASH_OPP"
	echo "$CRASH_OPP CLIFF $CRASH_UV (crashed)" >> "$LOG"
	echo "$(date +%T) opp $CRASH_OPP: cliff at $CRASH_UV uV" >> "$LOG"
	rm -f "$STATE"; sync
else
	echo "=== uvmap campaign start $(date +%T) ===" >> "$LOG"
fi

for OPP in $OPPS; do
	if has_any_verdict "$OPP"; then
		verdict_for "$OPP" >/dev/null || campaign_abort "malformed or duplicate verdict for OPP $OPP"
		continue
	fi

	pin "$OPP" || campaign_abort "could not pin and verify OPP $OPP"
	UV=$(stock_uv_for) || campaign_abort "could not read stock voltage at OPP $OPP"
	on_step "$UV" && [ "$UV" -le "$STOCK_MAX" ] || campaign_abort "invalid stock voltage '$UV' at OPP $OPP"
	echo "$(date +%T) opp $OPP: stock $UV uV, stepping down" >> "$LOG"; sync
	FLOOR_PROVEN=0
	# If stock itself is the campaign floor there is no undervolt point to test. Stock
	# remains authoritative, and the generated light-load row is kept at that exact value.
	[ "$UV" -le "$FLOOR" ] && FLOOR_PROVEN=1

	while [ "$UV" -gt "$FLOOR" ]; do
		# re-check disarm and charger EVERY round (audit 2026-07-11: cancel used to be
		# read only at startup, so a running campaign outlived the user's cancel; and a
		# yanked charger mid-campaign kept stressing on battery)
		if [ ! -f "$UV_DIR/ARMED" ]; then
			echo "$(date +%T) opp $OPP: cancelled mid-campaign — rebooting to restore stock" >> "$LOG"
			rm -f "$STATE" /tmp/uvmap.running; sync
			reboot
			exit 0
		fi
		CHG=$(cat /sys/class/power_supply/axp2202-battery/status 2>/dev/null)
		if [ "$CHG" != "Charging" ] && [ "$CHG" != "Full" ]; then
			echo "$(date +%T) opp $OPP: charger lost mid-campaign — rebooting to stock and pausing" >> "$LOG"
			rm -f "$STATE" /tmp/uvmap.running; sync
			reboot
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
		[ "$UV" -le "$FLOOR" ] && FLOOR_PROVEN=1
	done
	# A refused uvtool write or failed stress at FLOOR did not prove the floor. Leave
	# the OPP without a verdict so finish() retries it; never manufacture DONE from UV alone.
	record_floor_done "$OPP" "$FLOOR_PROVEN" || true
	rm -f "$STATE"; sync
	# restore stock voltage for this OPP by bouncing the clock (kernel re-asserts)
	pin 408000 && pin "$OPP" && pin 408000 || campaign_abort "failed to restore stock rail after OPP $OPP"
done

finish

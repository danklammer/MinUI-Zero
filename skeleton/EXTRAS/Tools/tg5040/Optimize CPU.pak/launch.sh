#!/bin/sh
# Optimize CPU — measures THIS chip's lowest safe operating voltage and runs it
# lean forever after. Per-device (silicon varies unit to unit); RAM-only at runtime, so
# any reboot returns to factory-safe voltages. See docs/dtb-undervolt-primer.md.

UV_DIR="$SHARED_USERDATA_PATH/../tg5040/undervolt"
[ -d "$UV_DIR" ] || UV_DIR="/mnt/SDCARD/.userdata/tg5040/undervolt"
mkdir -p "$UV_DIR"
BIN="$UV_DIR"
PAK_DIR="$(dirname "$0")"

# self-install the harness binaries into the (persistent) working dir so the auto.sh
# resume hook can run them across the campaign's reboots.
for f in uvtool stress uvmap.sh; do
	if [ ! -f "$UV_DIR/$f" ] || [ "$PAK_DIR/bin/$f" -nt "$UV_DIR/$f" ]; then
		cp "$PAK_DIR/bin/$f" "$UV_DIR/$f" 2>/dev/null && chmod +x "$UV_DIR/$f"
	fi
done

# archive_campaign: retire EVERY artifact of a previous campaign before starting a new
# one. Leaving any behind is unsafe: table.conf+table.model without table.chip lets the
# runtime's model-string fallback apply another chip's voltages after a card swap, and a
# stale margins.log/state would seed gen_table with the old chip's cliffs (audit 2026-07-11).
archive_campaign() {
	[ -f "$UV_DIR/margins.log" ] && mv "$UV_DIR/margins.log" "$UV_DIR/margins.log.prev" 2>/dev/null
	rm -f "$UV_DIR/table.conf" "$UV_DIR/table.model" "$UV_DIR/table.chip" \
	      "$UV_DIR/calibration" "$UV_DIR/RETRIES" "$UV_DIR/state" "$UV_DIR/ARMED"
	sync
}

STATUS=$(cat /sys/class/power_supply/axp2202-battery/status 2>/dev/null)

# ---------- STATE 3b: calibrated, but for a DIFFERENT chip (card swapped) ----------
DEV_CHIP=$(grep sunxi_serial /sys/class/sunxi_info/sys_info 2>/dev/null | awk -F: '{print $2}' | tr -d " \t")
TAB_CHIP=$(cat "$UV_DIR/table.chip" 2>/dev/null)
if [ -f "$UV_DIR/calibration" ] && [ -n "$DEV_CHIP" ] && [ -n "$TAB_CHIP" ] && [ "$DEV_CHIP" != "$TAB_CHIP" ]; then
	confirm.elf "Different Device Detected

This card was optimized on another
device. Every chip is different, so
the tuning is not applied here.

Optimize this device now?" "OPTIMIZE" "BACK" || exit 0
	archive_campaign # a different chip: EVERYTHING from the old campaign must go
	# fall through to the charger check + arming below
elif [ -f "$UV_DIR/calibration" ] && [ -f "$UV_DIR/table.conf" ]; then
	. "$UV_DIR/calibration" 2>/dev/null
	# headline from the APPLIED reduction (V^2 -> ~1.6x the mV% in rail power). No
	# temperature claim: degrees depend on the game and chassis, not derivable here.
	MV=${min_margin_mv:-0}
	PCT=$(( MV * 16 / 120 ))          # ~% CPU-rail power saved at the top clock
	[ "$PCT" -lt 1 ] && PCT=1
	confirm.elf --ok "CPU Optimized" "About ${PCT}% less CPU power
at full load. Tuned to this
exact chip." "" "BACK" "MANAGE"
	RC=$?
	[ "$RC" != "2" ] && exit 0 # B (or anything but X): back to the menu

	# MANAGE: the deliberate second level
	confirm.elf "Manage Tuning

Calibrated ${calibrated:-unknown}
Headroom: ${min_margin_mv:-?}mV" "RE-RUN" "BACK" "REVERT"
	RC=$?
	if [ "$RC" = "2" ]; then
		confirm.elf "Revert to Factory?

Stock voltage resumes next launch.
Re-tune anytime." "REVERT" "BACK" || exit 0
		mv "$UV_DIR/table.conf" "$UV_DIR/table.conf.reverted" 2>/dev/null
		rm -f "$UV_DIR/calibration"
		sync
		say.elf "Reverted to factory voltage.

Launch a game to apply."
		exit 0
	elif [ "$RC" != "0" ]; then
		exit 0 # B: back
	fi
	# A: re-run measurement
	confirm.elf "Re-measure This Chip?

Takes ~90 min. Keep it charging." "RE-RUN" "BACK" || exit 0
	archive_campaign # fresh campaign: no stale cliffs/table may leak into the new table
	# fall through to arm
fi

# ---------- STATE 2: calibration in progress (armed, resuming across reboots) ----------
if [ -f "$UV_DIR/ARMED" ]; then
	MLOG="$UV_DIR/margins.log"
	DONE_N=$(grep -cE "^[0-9]+ (CLIFF|DONE)" "$MLOG" 2>/dev/null)
	[ -n "$DONE_N" ] || DONE_N=0
	LAST=$(grep "uV survived" "$MLOG" 2>/dev/null | tail -1 | awk '{f=$3; sub(/:/,"",f); printf "%d MHz: %.1f mV ok", f/1000, $4/1000}')
	if [ "$STATUS" != "Charging" ] && [ "$STATUS" != "Full" ]; then
		confirm.elf --ok "Measurement PAUSED

Not charging. Progress: $DONE_N of 8
steps done. Plug in the charger and
restart to resume." "" "BACK" "CANCEL"
		if [ "$?" = "2" ]; then
			confirm.elf "Cancel Measurement?

Progress is kept; re-run
Optimize CPU to resume later." "CANCEL IT" "BACK" || exit 0
			rm -f "$UV_DIR/ARMED"
			sync
			say.elf "Measurement cancelled.

Stops within about a minute.
Factory voltages remain active."
		fi
	else
		say.elf "Measuring... $DONE_N of 8 steps done.
${LAST:-Warming up.}

Keep it charging. The device
restarts itself several times.
Check back here any time."
	fi
	exit 0
fi

# ---------- STATE 1: not calibrated -> the pitch + disclaimer ----------
confirm.elf "Optimize CPU

Undervolt the CPU to your chip's
measured safe minimum. Cooler,
longer battery, same speed." "NEXT" "BACK" || exit 0

confirm.elf "Before You Start

* Takes ~90 min. Keep it charging.
* It restarts itself several times.
  This is how it measures.
* Reboots are always factory-safe.
  This cannot damage your device." "START" "BACK" || exit 0

# require an affirmative charging state — the PMIC also reports "Not charging",
# which the old != Discharging test let through (armed-but-paused-forever trap)
if [ "$STATUS" != "Charging" ] && [ "$STATUS" != "Full" ]; then
	say.elf "Connect the charger first,
then run Optimize CPU again."
	exit 0
fi

# arm it: install the auto.sh resume hook + the flag, then kick off round one
AUTO="$SHARED_USERDATA_PATH/../tg5040/auto.sh"
[ -d "$(dirname "$AUTO")" ] || AUTO="/mnt/SDCARD/.userdata/tg5040/auto.sh"
if ! grep -q uvmap "$AUTO" 2>/dev/null; then
	[ -f "$AUTO" ] || echo "#!/bin/sh" > "$AUTO"   # only shebang a NEW file (audit nit)
	{
		echo "# Optimize CPU resume hook (inert once calibration completes)"
		echo "[ -f \"$UV_DIR/ARMED\" ] && ( sh \"$UV_DIR/uvmap.sh\" > /dev/null 2>&1 & )"
	} >> "$AUTO"
	chmod +x "$AUTO"
fi
touch "$UV_DIR/ARMED"
sync

say.elf "Measurement started.

Leave it charging.
It restarts itself several times and
finishes in about 90 minutes.
Check back here any time."

( sh "$UV_DIR/uvmap.sh" > /dev/null 2>&1 & )
exit 0

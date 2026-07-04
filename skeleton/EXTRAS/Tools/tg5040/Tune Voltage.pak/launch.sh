#!/bin/sh
# Tune Device Voltage — measures THIS chip's lowest safe operating voltage and runs it
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

STATUS=$(cat /sys/class/power_supply/axp2202-battery/status 2>/dev/null)

# ---------- STATE 3: already calibrated ----------
if [ -f "$UV_DIR/calibration" ] && [ -f "$UV_DIR/table.conf" ]; then
	. "$UV_DIR/calibration" 2>/dev/null
	# derive rough headline benefits from the measured margin (V^2 -> ~1.6x the mV% in power)
	MV=${min_margin_mv:-0}
	PCT=$(( MV * 16 / 120 ))          # ~% CPU-rail power saved at the top clock
	COOL=$(( MV / 30 ))               # ~degrees C cooler in heavy games
	[ "$COOL" -lt 1 ] && COOL=1
	confirm.elf --ok "Device Voltage Optimized" "${PCT}% less CPU power. ${COOL}C cooler.
Tuned to this exact chip." "" "BACK" "MANAGE"
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
	rm -f "$UV_DIR/calibration"
	# fall through to arm
fi

# ---------- STATE 2: calibration in progress (armed, resuming across reboots) ----------
if [ -f "$UV_DIR/ARMED" ]; then
	say.elf "Measuring...

Keep it charging.
The device restarts itself several times.
Done in about 90 minutes."
	exit 0
fi

# ---------- STATE 1: not calibrated -> the pitch + disclaimer ----------
confirm.elf "Tune Device Voltage

Every chip is a little different. This finds
YOUR chip's lowest safe voltage and runs
there. Cooler and longer battery, same speed." "NEXT" "BACK" || exit 0

confirm.elf "Before You Start

* Takes ~90 min. Keep it charging.
* It restarts itself several times.
  This is how it measures.
* Reboots are always factory-safe.
  This cannot damage your device." "START" "BACK" || exit 0

if [ "$STATUS" = "Discharging" ]; then
	say.elf "Connect the charger first,
then run Tune Voltage again."
	exit 0
fi

# arm it: install the auto.sh resume hook + the flag, then kick off round one
AUTO="$SHARED_USERDATA_PATH/../tg5040/auto.sh"
[ -d "$(dirname "$AUTO")" ] || AUTO="/mnt/SDCARD/.userdata/tg5040/auto.sh"
if ! grep -q uvmap "$AUTO" 2>/dev/null; then
	{
		echo "#!/bin/sh"
		echo "# Tune Voltage resume hook (self-removes when calibration completes)"
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

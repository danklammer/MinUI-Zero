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
	confirm.elf "Device Tuned  [OK]

Calibrated: ${calibrated:-unknown}
Your chip's margin: ${min_margin_mv:-?}mV (measured)

Running lean saves power and heat
in every game -- most on PlayStation.
Any reboot is always factory-safe." "RE-RUN" "BACK" "REVERT"
	RC=$?
	if [ "$RC" = "1" ]; then
		exit 0 # B: straight back to the menu, no questions
	elif [ "$RC" = "2" ]; then
		confirm.elf "Revert to factory voltages?

Removes the tuning. Your device
goes back to stock voltages on the
next game launch. You can re-tune
any time." "REVERT" "BACK" || exit 0
		mv "$UV_DIR/table.conf" "$UV_DIR/table.conf.reverted" 2>/dev/null
		rm -f "$UV_DIR/calibration"
		sync
		say.elf "Reverted to factory voltages.

Launch a game to apply."
		exit 0
	fi
	# A: re-run measurement
	confirm.elf "Re-measure this device?

This runs the ~90-minute calibration
again. Keep it on the charger." "RE-RUN" "BACK" || exit 0
	rm -f "$UV_DIR/calibration"
	# fall through to arm
fi

# ---------- STATE 2: calibration in progress (armed, resuming across reboots) ----------
if [ -f "$UV_DIR/ARMED" ]; then
	say.elf "Measurement in progress...

Keep the device on its charger.
It restarts itself several times --
that is the measurement working.
Done in about 90 minutes; you'll
see a summary here when it finishes."
	exit 0
fi

# ---------- STATE 1: not calibrated -> the pitch + disclaimer ----------
confirm.elf "Tune Device Voltage

Every chip is a little different. This
measures YOUR device's lowest safe
voltage and runs it there -- cooler
hands, more play time per charge
(most on PlayStation). Frame rates
are unchanged.

Continue to the details?" "NEXT" "BACK" || exit 0

confirm.elf "How it works  (please read)

* Takes about 90 minutes.
* MUST stay on the charger.
* The device WILL restart itself
  several times -- this is normal,
  it's how the measurement works.
* Play is unaffected afterward.
* Any reboot always returns to
  factory-safe voltages -- this
  cannot damage your device.

Start the measurement?" "START" "CANCEL" || exit 0

if [ "$STATUS" = "Discharging" ]; then
	say.elf "Please connect the charger first,
then run Tune Voltage again.

(The measurement needs steady power
so a low battery can't be mistaken
for a voltage limit.)"
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

Leave the device on its charger.
It will restart itself several times
and finish in about 90 minutes.

You can check back here any time."

( sh "$UV_DIR/uvmap.sh" > /dev/null 2>&1 & )
exit 0

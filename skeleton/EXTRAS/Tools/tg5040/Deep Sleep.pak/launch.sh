#!/bin/sh
# Deep Sleep status + toggle, styled like Tune Voltage. Deep sleep is ON by default;
# the opt-out flag disables it (see DEEP_SLEEP_OFF_PATH in defines.h). No reboot needed.

FLAG="$SHARED_USERDATA_PATH/disable-deep-sleep"

if [ ! -f "$FLAG" ]; then
	# ON = the good default: green-check status, X turns it off
	confirm.elf --ok "Deep Sleep On" "Suspends to RAM when idle.
Near-zero power, wakes instantly." "" "BACK" "TURN OFF"
	[ "$?" = "2" ] || exit 0
	confirm.elf "Turn Deep Sleep Off?

Sleep will work like stock: the screen
turns off and the device powers off
on a timer. Uses more standby power." "TURN OFF" "BACK" || exit 0
	touch "$FLAG"
	sync
	say.elf "Deep Sleep is off.

Takes effect at the next sleep."
else
	# OFF: plain status, A turns it back on
	confirm.elf "Deep Sleep Off

Sleep works like stock MinUI.
The screen turns off and the device
powers off on a timer." "TURN ON" "BACK" || exit 0
	rm -f "$FLAG"
	sync
	say.elf "Deep Sleep is on.

Takes effect at the next sleep."
fi

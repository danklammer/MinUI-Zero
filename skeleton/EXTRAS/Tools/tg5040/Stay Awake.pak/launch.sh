#!/bin/sh
# Stay Awake status + toggle, styled like Deep Sleep. Holds off the 30s autosleep so a device
# stays reachable over SSH while it is being worked on.
#
# Two files, on purpose:
#   /tmp/stay_awake                       what PWR_preventAutosleep() actually reads (api.c)
#   $SHARED_USERDATA_PATH/dev-stay-awake  survives a reboot; MinUI.pak/launch.sh re-arms /tmp from it
# Writing both means the toggle takes effect NOW and after a reboot, with no relaunch needed.

FLAG="$SHARED_USERDATA_PATH/dev-stay-awake"

if [ ! -f "$FLAG" ]; then
	# OFF = the good default: the whole point of this fork is not burning power while idle
	confirm.elf --ok "Stay Awake Off" "The device sleeps after 30 seconds
idle, as it should." "" "BACK" "TURN ON"
	[ "$?" = "2" ] || exit 0
	confirm.elf "Keep The Device Awake?

For debugging over SSH. The screen and
CPU stay live until you turn this off,
so it WILL cost battery. Not for play." "TURN ON" "BACK" || exit 0
	touch "$FLAG"
	touch /tmp/stay_awake
	# a sleeping wifi radio drops SSH just as effectively as a sleeping CPU
	iw dev wlan0 set power_save off 2>/dev/null
	iwconfig wlan0 power off 2>/dev/null
	sync
	say.elf "Staying awake.

Turn this off when you are done."
else
	confirm.elf "Stay Awake On

The device will NOT sleep on its own
and is using more battery than usual." "TURN OFF" "BACK" || exit 0
	rm -f "$FLAG"
	rm -f /tmp/stay_awake
	sync
	say.elf "Stay Awake is off.

Normal sleep resumes."
fi

#!/bin/sh
# Toggle deep sleep (suspend-to-RAM). When ON, a device left sleeping for 2 minutes
# escalates from light sleep to true suspend-to-RAM: it goes properly cold and resumes
# right where you left off. When OFF, sleep works like stock MinUI (light sleep, then
# scheduled power-off). The flag is read at the sleep timeout — no reboot needed.

FLAG="$SHARED_USERDATA_PATH/enable-deep-sleep"
if [ -f "$FLAG" ]; then
	rm -f "$FLAG"
	sync
	say.elf "Deep Sleep: OFF"
else
	touch "$FLAG"
	sync
	say.elf "Deep Sleep: ON"
fi

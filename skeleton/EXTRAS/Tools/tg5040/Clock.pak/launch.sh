#!/bin/sh
# Clock: set the time, and (opt-in) show HH:MM in the menu next to the battery.

cd $(dirname "$0")
FLAG="$SHARED_USERDATA_PATH/show-clock"

while :; do
	if [ -f "$FLAG" ]; then STATE="on"; TOGGLE="HIDE CLOCK"; else STATE="off"; TOGGLE="SHOW CLOCK"; fi
	confirm.elf "Clock

Menu clock: $STATE" "SET TIME" "BACK" "$TOGGLE"
	RC=$?
	if [ "$RC" = "0" ]; then
		./clock.elf
	elif [ "$RC" = "2" ]; then
		if [ -f "$FLAG" ]; then rm -f "$FLAG"; else touch "$FLAG"; fi
		sync
	else
		exit 0
	fi
done

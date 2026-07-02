#!/bin/sh
# Toggle deep sleep (suspend-to-RAM) and explain it. The flag is read at the sleep
# timeout (see DEEP_SLEEP_PATH in defines.h) — takes effect immediately, no reboot.

FLAG="$SHARED_USERDATA_PATH/enable-deep-sleep"
if [ -f "$FLAG" ]; then
	rm -f "$FLAG"
	sync
	say.elf "Deep Sleep is now OFF

Sleep works like stock MinUI:
the screen turns off, and after a
while the device fully powers off.

Run this tool again to turn
Deep Sleep back on."
else
	touch "$FLAG"
	sync
	say.elf "Deep Sleep is now ON

After 2 minutes of sleep the device
suspends to RAM: near-zero power,
runs cold, and wakes instantly
right where you left off.

Run this tool again to turn it off."
fi

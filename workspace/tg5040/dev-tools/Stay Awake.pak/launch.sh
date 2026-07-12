#!/bin/sh
# Stay Awake (DEV TOOL — lives in workspace/tg5040/dev-tools/, never packaged into
# releases). Blocks ALL autosleep (menu + in-game; the /tmp/stay_awake flag is honored
# by PWR_preventAutosleep in both minui and minarch) and disables WiFi power-save so
# SSH/dev sessions stop dying to radio naps. Persists across reboots via a shared
# flag re-armed by MinUI.pak/launch.sh at boot. Styled like the Deep Sleep tool.

FLAG_PERSIST="$SHARED_USERDATA_PATH/dev-stay-awake"

wifi_awake() {
	iw dev wlan0 set power_save off 2>/dev/null
	iwconfig wlan0 power off 2>/dev/null
	echo on > /sys/class/net/wlan0/device/power/control 2>/dev/null
}
wifi_normal() {
	iw dev wlan0 set power_save on 2>/dev/null
	iwconfig wlan0 power on 2>/dev/null
	echo auto > /sys/class/net/wlan0/device/power/control 2>/dev/null
}

if [ ! -f "$FLAG_PERSIST" ]; then
	confirm.elf "Stay Awake Off

Normal sleep behavior.
Turn on to block all autosleep and
keep WiFi active (dev/testing)." "TURN ON" "BACK" || exit 0
	touch "$FLAG_PERSIST"
	touch /tmp/stay_awake
	wifi_awake
	# net-keeper: pings the gateway and bounces wlan0 when the driver wedges
	( /bin/sh "$(dirname "$0")/net-keeper.sh" </dev/null >/dev/null 2>&1 ) &
	sync
	say.elf "Stay Awake is ON.

No autosleep, WiFi stays active
and self-heals if it stalls.
Survives reboots. Battery will drain."
else
	confirm.elf --ok "Stay Awake On" "No autosleep, WiFi stays active.
Battery drains at the full rate." "" "BACK" "TURN OFF"
	[ "$?" = "2" ] || exit 0
	rm -f "$FLAG_PERSIST" /tmp/stay_awake
	# net-keeper exits on its own when the flag disappears (checked each loop)
	wifi_normal
	sync
	say.elf "Stay Awake is off.

Normal sleep behavior restored."
fi

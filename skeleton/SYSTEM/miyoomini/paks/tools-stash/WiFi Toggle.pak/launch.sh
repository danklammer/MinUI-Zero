#!/bin/sh
# Wifi status + toggle, styled like Deep Sleep.pak. ONE script, duplicated per platform
# (parity rule: change one, diff the others) — the radio-down lines are guarded per device. The card's wifi.txt stays the ONE source
# of truth (SSID:password, opt-in by existing); this pak is hands for it: it SHOWS the real
# state (Dan on 2026-08-31, after scanning his LAN to learn whether a device had joined: the
# status question needs an on-screen answer) and flips the file between wifi.txt and
# wifi.txt.off. A missing file is reported, never created — the password is the user's to type.
#
# The radio change applies LIVE where possible; the boot path picks the file up regardless.

WTXT="$SDCARD_PATH/wifi.txt"
WOFF="$SDCARD_PATH/wifi.txt.off"

wifi_state() { # prints: up <ip> | joined | down
	S=$(cat /sys/class/net/wlan0/operstate 2>/dev/null)
	if [ "$S" = "up" ]; then
		IP=$(ip -4 addr show wlan0 2>/dev/null | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -1)
		[ -n "$IP" ] && echo "up $IP" || echo "joined"
	else
		echo "down"
	fi
}

if [ -f "$WTXT" ]; then
	SSID=$(sed '/^#/d;/^[[:space:]]*$/d' "$WTXT" | head -1 | cut -d: -f1)
	case "$(wifi_state)" in
		up*)  STATE="Connected to \"$SSID\"
IP: $(wifi_state | cut -d' ' -f2)" ;;
		joined) STATE="Joining \"$SSID\"..." ;;
		down)   STATE="Enabled for \"$SSID\"
Not connected (takes ~1-2 min
after boot, or wrong password)." ;;
	esac
	confirm.elf --ok "WiFi On" "$STATE" "" "BACK" "TURN OFF"
	[ "$?" = "2" ] || exit 0
	mv "$WTXT" "$WOFF"
	sync
	# take the radio down NOW, not just at next boot
	killall wpa_supplicant 2>/dev/null
	killall udhcpc 2>/dev/null
	ifconfig wlan0 down 2>/dev/null
	command -v rfkill >/dev/null 2>&1 && rfkill block wifi 2>/dev/null
	# Miyoo: the PMIC can cut the radio's power rail entirely (guarded no-op elsewhere)
	[ -x /customer/app/axp_test ] && /customer/app/axp_test wifioff >/dev/null 2>&1
	say.elf "WiFi is off.

The radio is down. Your network
stays saved in wifi.txt.off."
elif [ -f "$WOFF" ]; then
	SSID=$(sed '/^#/d;/^[[:space:]]*$/d' "$WOFF" | head -1 | cut -d: -f1)
	confirm.elf "WiFi Off

Network \"$SSID\" is saved.
Turning on starts SSH access." "TURN ON" "BACK" || exit 0
	mv "$WOFF" "$WTXT"
	sync
	say.elf "WiFi is on.

Takes effect at the next boot.
(Reboot now to connect.)"
else
	confirm.elf --ok "WiFi Not Set Up" "No wifi.txt on the card.

On a computer, create wifi.txt at
the card root with one line:
YourNetwork:password" "" "OKAY" ""
fi

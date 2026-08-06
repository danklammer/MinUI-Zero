#!/bin/sh
# MinUI Zero — the "frontend" of the stripped-muOS OS. muOS's startup.sh brings up ALL the hard
# parts (kernel modules via udev, wifi via network.sh, ssh, mounts, power) exactly as it always
# has; startup.sh's `FRONTEND start` line is swapped for this, so MinUI is the UI instead of
# muxfrontend. Everything below the UI is muOS's proven stack — we reinvent nothing.
#
# muOS mounts the ROMS/data partition at /mnt/mmc (via /opt/muos/script/mount). Our payload lives
# there under .system/h700, same as the piggyback/hosted-dev loop.

export PLATFORM=h700
export SDCARD_PATH=/mnt/mmc
export SYSTEM_PATH=/mnt/mmc/.system/h700
export USERDATA_PATH=/mnt/mmc/.userdata/h700
export LOGS_PATH=/mnt/mmc/.userdata/h700/logs
export SHARED_USERDATA_PATH=/mnt/mmc/.userdata/shared
export SAVES_PATH=/mnt/mmc/Saves
export BIOS_PATH=/mnt/mmc/Bios
export CORES_PATH=/mnt/mmc/.system/h700/cores
export LD_LIBRARY_PATH=/mnt/mmc/.system/h700/lib:/usr/lib:/lib
# audio: muOS's pipewire is still running (kept), so the pak launchers' pipewire env applies —
# the exact working path from the hosted-dev loop.
export SDL_VIDEODRIVER=dummy

LOG=/mnt/mmc/minui-zero.log
: > "$LOG" 2>/dev/null
echo "MinUI Zero frontend $(date 2>/dev/null)" >> "$LOG"

# THE THESIS: own the governor. schedutil + our minui/minarch write the ceiling on top.
echo schedutil > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)" >> "$LOG"

# WiFi — brought up HERE, not via muOS's network.sh. muOS keeps its saved SSID/password on the
# ROMS partition, which our strip replaced with a fresh one, so muOS's connect has no creds. We
# ship our own wpa_supplicant.conf and drive the same tools muOS uses (all kept): wait for wlan0
# (udev loads the driver), power-save OFF (the RTL8821CS 5GHz deafness), associate, DHCP.
( if [ -f /etc/wpa_supplicant.conf ]; then
	SSID=$(sed -n 's/^[[:space:]]*ssid="\(.*\)"/\1/p' /etc/wpa_supplicant.conf | head -1)
	for _ in 1 2 3 4 5 6 7 8; do [ -e /sys/class/net/wlan0 ] && break; sleep 2; done
	if [ -e /sys/class/net/wlan0 ]; then
		ifconfig wlan0 up 2>/dev/null
		iw dev wlan0 set power_save off 2>/dev/null
		iwconfig wlan0 essid -- "$SSID" 2>/dev/null
		killall wpa_supplicant 2>/dev/null; sleep 1
		wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf -D nl80211 2>/dev/null
		sleep 3
		mkdir -p /var/db/dhcpcd /run
		dhcpcd -t 25 wlan0 2>/dev/null
		echo "wifi: $(ip -4 -o addr show wlan0 2>/dev/null | awk '{print $4}')" >> "$LOG"
	else
		echo "wifi: wlan0 never appeared" >> "$LOG"
	fi
  fi ) &

mkdir -p "$LOGS_PATH" "$SAVES_PATH" "$SHARED_USERDATA_PATH/.minui" 2>/dev/null

# power-off the muOS way (AXP register; plain poweroff reboots) — reused from halt.sh
power_off() {
	sync
	echo 0x1801 > /sys/class/axp/axp_reg 2>/dev/null
	/opt/muos/script/system/halt.sh poweroff 2>/dev/null
	poweroff -f
}

cd /tmp
FAILS=0
while : ; do
	rm -f /tmp/next
	"$SYSTEM_PATH/bin/minui.elf" >> "$LOG" 2>&1
	RC=$?
	if [ -f /tmp/next ]; then
		FAILS=0
		CMD=$(cat /tmp/next)
		echo "launch: $CMD" >> "$LOG"
		sh -c "$CMD"
		echo "game exited rc=$?" >> "$LOG"
	elif [ "$RC" = "0" ]; then
		echo "clean exit — power off" >> "$LOG"
		power_off
	else
		FAILS=$((FAILS+1))
		echo "minui exited rc=$RC (fail $FAILS)" >> "$LOG"
		[ $FAILS -ge 3 ] && { echo "parking" >> "$LOG"; while : ; do sleep 60; done; }
		sleep 2
	fi
done

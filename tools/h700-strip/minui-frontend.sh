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

# WiFi + SSH bring-up. IDEMPOTENT: if wlan0 already has an IP we touch nothing — a frontend
# restart must never drop a live link (that dropped the dev ssh mid-session, 2026-08-06). Our
# wpa config carries freq_list=2.4GHz-only (the RTL8821CS can't hold a usable 5GHz link even with
# power_save off) + power_save off. SSH: the build resets openssh host-key perms to world-readable
# and sshd then refuses to start ("no hostkeys available"), so fix perms + start it here.
( if [ -f /etc/wpa_supplicant.conf ] && ! ip -4 -o addr show wlan0 2>/dev/null | grep -q inet; then
	for _ in 1 2 3 4 5 6 7 8; do [ -e /sys/class/net/wlan0 ] && break; sleep 2; done
	if [ -e /sys/class/net/wlan0 ]; then
		ifconfig wlan0 up 2>/dev/null
		iw dev wlan0 set power_save off 2>/dev/null
		killall wpa_supplicant 2>/dev/null; sleep 1
		mkdir -p /var/run/wpa_supplicant /var/db/dhcpcd /run
		wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf -D nl80211 2>/dev/null
		sleep 3
		dhcpcd -t 25 wlan0 2>/dev/null
	fi
  fi
  chmod 700 /opt/openssh /opt/openssh/etc 2>/dev/null
  chmod 600 /opt/openssh/etc/ssh_host_*_key 2>/dev/null
  chmod 700 /root/.ssh 2>/dev/null; chmod 600 /root/.ssh/authorized_keys 2>/dev/null
  pidof sshd >/dev/null || /opt/openssh/sbin/sshd 2>/dev/null
  echo "wifi: $(ip -4 -o addr show wlan0 2>/dev/null | awk '{print $4}') ssh=$(pidof sshd >/dev/null && echo up || echo down)" >> "$LOG"
) &

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

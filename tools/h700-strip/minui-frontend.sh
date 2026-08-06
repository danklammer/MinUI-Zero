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

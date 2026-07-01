#!/bin/sh
# NOTE: becomes .tmp_update/tg5040.sh

PLATFORM="tg5040"
SDCARD_PATH="/mnt/SDCARD"
UPDATE_PATH="$SDCARD_PATH/MinUI.zip"
SYSTEM_PATH="$SDCARD_PATH/.system"

# for Brick (noatime: nothing reads atime, so skip the per-access metadata writeback to SD)
mount -o remount,rw,async,noatime "$SDCARD_PATH"
mount -o remount,rw,async,noatime "/mnt/UDISK"

# Hybrid CPU control: prefer schedutil (the governor sets a scaling_max_freq cap and the
# kernel picks beneath it). Verify by read-back; if schedutil is unavailable, fall back to
# the known-good userspace path at a NON-OC clock. Either way: never 2.0GHz (overclock).
GOV=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo schedutil > "$GOV" 2>/dev/null
if [ "$(cat "$GOV" 2>/dev/null)" = "schedutil" ]; then
	echo 1800000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null || true
else
	echo userspace > "$GOV" 2>/dev/null || true
	echo 1608000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed 2>/dev/null || true
fi

# MinUI Zero: radios off by default. MinUI has no networking, so full-time wifi/BT is pure wasted
# power. Bluetooth is always disabled (no feature uses it); wifi stays up ONLY when the dev
# enable-ssh flag is present (SSH access). Zero user-facing loss, less idle drain + heat.
SHARED_UD="$SDCARD_PATH/.userdata/shared"
killall -q bluetoothd bluealsa hciattach 2>/dev/null
[ -x /etc/init.d/hciattach ] && /etc/init.d/hciattach stop >/dev/null 2>&1
for r in /sys/class/rfkill/rfkill*; do
	[ "$(cat "$r/type" 2>/dev/null)" = "bluetooth" ] && echo 0 > "$r/state" 2>/dev/null
done
if [ ! -f "$SHARED_UD/enable-ssh" ]; then
	killall -q wpa_supplicant 2>/dev/null
	ifconfig wlan0 down 2>/dev/null
	for r in /sys/class/rfkill/rfkill*; do
		[ "$(cat "$r/type" 2>/dev/null)" = "wlan" ] && echo 0 > "$r/state" 2>/dev/null
	done
fi

# Ambient RGB LEDs off every boot (not just on install) — Zero has no ambient-LED feature, so they're
# a rail we never use. Cheap insurance in case an OFW update re-enables them.
echo 0 > /sys/class/led_anim/max_scale 2>/dev/null
echo 0 > /sys/class/led_anim/max_scale_lr 2>/dev/null
echo 0 > /sys/class/led_anim/max_scale_f1f2 2>/dev/null

# install/update
if [ -f "$UPDATE_PATH" ]; then
	export LD_LIBRARY_PATH=/usr/trimui/lib:$LD_LIBRARY_PATH
	export PATH=/usr/trimui/bin:$PATH

	TRIMUI_MODEL=`strings /usr/trimui/bin/MainUI | grep ^Trimui`
	if [ "$TRIMUI_MODEL" = "Trimui Brick" ]; then
		DEVICE="brick"
	fi

	# leds_off
	echo 0 > /sys/class/led_anim/max_scale
	if [ "$DEVICE" = "brick" ]; then
		echo 0 > /sys/class/led_anim/max_scale_lr
		echo 0 > /sys/class/led_anim/max_scale_f1f2
	fi
	
	cd $(dirname "$0")/$PLATFORM
	if [ -d "$SYSTEM_PATH" ]; then
		ACTION=updating
	else
		ACTION=installing
	fi
	./show.elf ./$DEVICE/$ACTION.png
	
	./unzip -o "$UPDATE_PATH" -d "$SDCARD_PATH" # &> /mnt/SDCARD/unzip.txt
	rm -f "$UPDATE_PATH"
	sync
	
	# the updated system finishes the install/update
	if [ -f $SYSTEM_PATH/$PLATFORM/bin/install.sh ]; then
		$SYSTEM_PATH/$PLATFORM/bin/install.sh # &> $SDCARD_PATH/log.txt
	fi
	
	if [ "$ACTION" = "installing" ]; then
		reboot
	fi
fi

LAUNCH_PATH="$SYSTEM_PATH/$PLATFORM/paks/MinUI.pak/launch.sh"
if [ -f "$LAUNCH_PATH" ] ; then
	exec "$LAUNCH_PATH"
fi

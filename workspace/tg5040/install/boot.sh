#!/bin/sh
# NOTE: becomes .tmp_update/tg5040.sh

PLATFORM="tg5040"
SDCARD_PATH="/mnt/SDCARD"
UPDATE_PATH="$SDCARD_PATH/MinUI.zip"
SYSTEM_PATH="$SDCARD_PATH/.system"

# for Brick
mount -o remount,rw,async "$SDCARD_PATH"
mount -o remount,rw,async "/mnt/UDISK"

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

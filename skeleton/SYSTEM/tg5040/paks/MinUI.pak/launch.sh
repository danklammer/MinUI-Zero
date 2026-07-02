#!/bin/sh
# MiniUI.pak

#######################################

export PLATFORM="tg5040"
export SDCARD_PATH="/mnt/SDCARD"
export BIOS_PATH="$SDCARD_PATH/Bios"
export ROMS_PATH="$SDCARD_PATH/Roms"
export SAVES_PATH="$SDCARD_PATH/Saves"
export SYSTEM_PATH="$SDCARD_PATH/.system/$PLATFORM"
export CORES_PATH="$SYSTEM_PATH/cores"
export USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
export SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/shared"
export LOGS_PATH="$USERDATA_PATH/logs"
export DATETIME_PATH="$SHARED_USERDATA_PATH/datetime.txt"

mkdir -p "$BIOS_PATH"
mkdir -p "$ROMS_PATH"
mkdir -p "$SAVES_PATH"
mkdir -p "$USERDATA_PATH"
mkdir -p "$LOGS_PATH"
mkdir -p "$SHARED_USERDATA_PATH/.minui"

# model detection: `strings` reads the entire MainUI binary — do it once and cache
# (the model can't change out from under a device); later boots skip the read
MODEL_CACHE="$SHARED_USERDATA_PATH/.minui/model"
if [ -s "$MODEL_CACHE" ]; then
	export TRIMUI_MODEL=`cat "$MODEL_CACHE"`
else
	export TRIMUI_MODEL=`strings /usr/trimui/bin/MainUI | grep ^Trimui`
	echo "$TRIMUI_MODEL" > "$MODEL_CACHE"
fi
if [ "$TRIMUI_MODEL" = "Trimui Brick" ]; then
	export DEVICE="brick"
fi

#######################################

#rumble motor PH3
echo 227 > /sys/class/gpio/export
echo -n out > /sys/class/gpio/gpio227/direction
echo -n 0 > /sys/class/gpio/gpio227/value

if [ "$TRIMUI_MODEL" = "Trimui Smart Pro" ]; then
	#Left/Right Pad PD14/PD18
	echo 110 > /sys/class/gpio/export
	echo -n out > /sys/class/gpio/gpio110/direction
	echo -n 1 > /sys/class/gpio/gpio110/value

	echo 114 > /sys/class/gpio/export
	echo -n out > /sys/class/gpio/gpio114/direction
	echo -n 1 > /sys/class/gpio/gpio114/value
fi

#DIP Switch PH19
echo 243 > /sys/class/gpio/export
echo -n in > /sys/class/gpio/gpio243/direction

#######################################

export LD_LIBRARY_PATH=$SYSTEM_PATH/lib:/usr/trimui/lib:$LD_LIBRARY_PATH
export PATH=$SYSTEM_PATH/bin:/usr/trimui/bin:$PATH

#######################################

# leds_off
echo 0 > /sys/class/led_anim/max_scale
if [ "$TRIMUI_MODEL" = "Trimui Brick" ]; then
	echo 0 > /sys/class/led_anim/max_scale_lr
	echo 0 > /sys/class/led_anim/max_scale_f1f2
fi
# ...and keep them off: trimui_inputd re-arms the scales from /mnt/UDISK/system.json on every
# button press (verified on-device 2026-07-01 — first keypress after boot relit the LEDs at
# scale 60). Zero the stock config too, BEFORE inputd starts, so it reads "off" at startup.
STOCK_JSON=/mnt/UDISK/system.json
if [ -f "$STOCK_JSON" ]; then
	sed -i -e "s/\"topled\":[[:space:]]*[0-9]*/\"topled\":\t0/" \
	       -e "s/\"shoulderled\":[[:space:]]*[0-9]*/\"shoulderled\":\t0/" \
	       -e "s/\"f1f2led\":[[:space:]]*[0-9]*/\"f1f2led\":\t0/" \
	       -e "s/\"ledswitch\":[[:space:]]*[0-9]*/\"ledswitch\":\t0/" "$STOCK_JSON"
fi

# USB: charge-only unless dev mode. Stock "data" mode enumerates MTP + adb when plugged into a
# computer — MtpDaemon then indexes the whole SD while games stream from it (measured 2026-07-02:
# the I/O contention visibly stretches frame times), plus two resident daemons of pure waste.
# MinUI has no USB-data feature, so a plugged-in Zero should just charge. The enable-ssh dev flag
# keeps stock "data" mode (adb as a wifi-less fallback for development).
if [ ! -f "$SHARED_USERDATA_PATH/enable-ssh" ] && [ -f "$STOCK_JSON" ]; then
	sed -i -e "s/\"usbmode\":[[:space:]]*\"[a-z]*\"/\"usbmode\":\t\"charge\"/" "$STOCK_JSON"
fi
# set default usb mode (reads usbmode from the stock config)
usb_device.sh

# match stock audio
tinymix set 9 1
tinymix set 1 0

# start stock gpio input daemon
mkdir -p /tmp/trimui_inputd
trimui_inputd &

# Hybrid CPU control (see boot.sh): prefer schedutil + a scaling_max_freq cap; fall back to
# known-good userspace at a non-OC clock if schedutil is unavailable. Never 2.0GHz (OC).
GOV=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo schedutil > "$GOV" 2>/dev/null
if [ "$(cat "$GOV" 2>/dev/null)" = "schedutil" ]; then
	CPU_PATH=/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
	CPU_SPEED_PERF=1800000
else
	echo userspace > "$GOV" 2>/dev/null || true
	CPU_PATH=/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed
	CPU_SPEED_PERF=1608000
fi
echo $CPU_SPEED_PERF > $CPU_PATH 2>/dev/null || true

# networking: DEV MODE (wifi + SSH for testing) only if the opt-in flag exists,
# otherwise block all radios (default runs-cold behavior).
if [ -f "$SHARED_USERDATA_PATH/enable-ssh" ]; then
	sh "$SYSTEM_PATH/bin/dev-net.sh" &
else
	killall MtpDaemon
	killall adbd 2>/dev/null
	killall ntpd 2>/dev/null # MinUI keeps its own clock; no network to sync from anyway
	killall wpa_supplicant
	killall udhcpc
	rfkill block bluetooth
	rfkill block wifi
fi

keymon.elf & # &> $SDCARD_PATH/keymon.txt &

#######################################

AUTO_PATH=$USERDATA_PATH/auto.sh
if [ -f "$AUTO_PATH" ]; then
	"$AUTO_PATH"
fi

cd $(dirname "$0")

#######################################

EXEC_PATH="/tmp/minui_exec"
NEXT_PATH="/tmp/next"
touch "$EXEC_PATH" # tmpfs; a sync here would flush every filesystem for a file that never touches disk
while [ -f $EXEC_PATH ]; do
	ZERO_FB_PRESENT=1 minui.elf &> $LOGS_PATH/minui.txt
	[ -f $EXEC_PATH ] && echo $CPU_SPEED_PERF > $CPU_PATH
	echo `date +'%F %T'` > "$DATETIME_PATH"
	sync
	
	if [ -f $NEXT_PATH ]; then
		CMD=`cat $NEXT_PATH`
		eval $CMD
		rm -f $NEXT_PATH
		[ -f $EXEC_PATH ] && echo $CPU_SPEED_PERF > $CPU_PATH
		echo `date +'%F %T'` > "$DATETIME_PATH"
		sync
	fi
done

exec shutdown

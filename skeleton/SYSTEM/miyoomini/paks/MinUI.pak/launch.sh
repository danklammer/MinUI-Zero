#!/bin/sh
# MiniUI.pak

if [ -z "$LCD_INIT" ]; then
	# an update may have already initilized the LCD
	/mnt/SDCARD/.system/miyoomini/bin/blank.elf

	# init backlight
	echo 0 > /sys/class/pwm/pwmchip0/export
	echo 800 > /sys/class/pwm/pwmchip0/pwm0/period
	echo 6 > /sys/class/pwm/pwmchip0/pwm0/duty_cycle
	echo 1 > /sys/class/pwm/pwmchip0/pwm0/enable

	# init lcd
	cat /proc/ls
	sleep 0.5
fi

# init charger detection
if [ ! -f /sys/devices/gpiochip0/gpio/gpio59/direction ]; then
	echo 59 > /sys/class/gpio/export
	echo in > /sys/devices/gpiochip0/gpio/gpio59/direction
fi


#######################################

if [ -f /customer/app/axp_test ]; then
	IS_PLUS=true
else
	IS_PLUS=false
fi
export IS_PLUS
export PLATFORM="miyoomini"
export SDCARD_PATH="/mnt/SDCARD"
export BIOS_PATH="$SDCARD_PATH/Bios"
export SAVES_PATH="$SDCARD_PATH/Saves"
export SYSTEM_PATH="$SDCARD_PATH/.system/$PLATFORM"
export CORES_PATH="$SYSTEM_PATH/cores"
export USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
export SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/shared"
export LOGS_PATH="$USERDATA_PATH/logs"
export DATETIME_PATH="$SHARED_USERDATA_PATH/datetime.txt" # used by bin/shutdown

mkdir -p "$USERDATA_PATH"
mkdir -p "$LOGS_PATH"
mkdir -p "$SHARED_USERDATA_PATH/.minui"

#######################################

# The closed-loop governor owns the clock during gameplay (userspace + scaling_setspeed).
# These stay for the menu//tmp/next path, but note 1296/1488 exceed the top STOCK OPP (1200) --
# overclock.elf reaches them by poking the MPLL directly, which we do not do. Menu runs at a
# real OPP instead.
export CPU_SPEED_MENU=600000
export CPU_SPEED_GAME=1200000
export CPU_SPEED_PERF=1200000
echo userspace > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo $CPU_SPEED_MENU > /sys/devices/system/cpu/cpufreq/policy0/scaling_setspeed 2>/dev/null

# `strings` over the 1.4MB vendor MainUI binary costs a MEASURED 130ms on every single boot, and
# the answer cannot change without a firmware reflash. Cache it. (Same fix as the tg5040
# model-detect cache.) Delete $USERDATA_PATH/model.txt to force a re-probe.
MODEL_CACHE="$USERDATA_PATH/model.txt"
if [ -s "$MODEL_CACHE" ]; then
	export MY_MODEL=`cat "$MODEL_CACHE"`
else
	export MY_MODEL=`strings -n 5 /customer/app/MainUI | grep MY`
	echo "$MY_MODEL" > "$MODEL_CACHE"
fi

MIYOO_VERSION=`/etc/fw_printenv miyoo_version`
export MIYOO_VERSION=${MIYOO_VERSION#miyoo_version=}

#######################################

# killall tee # NOTE: killing tee is somehow responsible for audioserver crashes
rm -f "$SDCARD_PATH/update.log"

#######################################

export LD_LIBRARY_PATH=$SYSTEM_PATH/lib:$LD_LIBRARY_PATH
export PATH=$SYSTEM_PATH/bin:$PATH

#######################################

# MinUI Zero is an SDL2 build: its MMIYOO audio driver opens MI_AO directly, so we neither
# start audioserver (which would OWN MI_AO and lock us out) nor preload libpadsp.
# libpadsp is the SDL 1.2 /dev/dsp shim and it SEGFAULTS binaries from this toolchain
# (verified with a minimal open("/dev/dsp") test), so exporting it would break every app here.
unset LD_PRELOAD

#######################################

lumon.elf & # adjust lcd luma and saturation

# Charging screen. Runs in the foreground by design — it is what you see INSTEAD of booting when
# the device is powered on plugged in — but it is now safe to do that:
#   POWER          -> exits, boot continues
#   charger pulled -> exits, boot continues  (the old one ran `shutdown` here, so unplugging
#                     powered the device off)
#   idle           -> dims only, stays responsive  (the old one blanked after 3s and blocked,
#                     which was indistinguishable from a hang and got reported as a crash twice)
# It draws a real fill level and percentage from AXP reg 0xB9, not a static png, and no longer
# busy-spins a core for the whole charging session.
if [ -f /customer/app/axp_test ]; then
	# Plus: AXP reg 0x00 bit2 = battery current direction (1 = charging)
	CHARGING=`/customer/app/axp_test | awk -F'[,: {}]+' '{print $7}'`
	[ "$CHARGING" = "3" ] && batmon.elf
else
	CHARGING=`cat /sys/devices/gpiochip0/gpio/gpio59/value 2>/dev/null`
	[ "$CHARGING" = "1" ] && batmon.elf
fi

keymon.elf & # &> /mnt/SDCARD/keymon.txt &

#######################################

# init datetime
if [ -f "$DATETIME_PATH" ] && [ ! -f "$USERDATA_PATH/enable-rtc" ]; then
	DATETIME=`cat "$DATETIME_PATH"`
	date +'%F %T' -s "$DATETIME"
	DATETIME=`date +'%s'`
	date -u -s "@$DATETIME"
fi

#######################################

AUTO_PATH=$USERDATA_PATH/auto.sh
if [ -f "$AUTO_PATH" ]; then
	"$AUTO_PATH"
fi

cd $(dirname "$0")

#######################################

EXEC_PATH=/tmp/minui_exec
NEXT_PATH="/tmp/next"
touch "$EXEC_PATH"  && sync
while [ -f "$EXEC_PATH" ]; do
	minui.elf &> $LOGS_PATH/minui.txt
	
	echo `date +'%F %T'` > "$DATETIME_PATH"
	sync
	
	if [ -f $NEXT_PATH ]; then
		CMD=`cat $NEXT_PATH`
		eval $CMD
		rm -f $NEXT_PATH
		if [ -f "/tmp/using-swap" ]; then
			swapoff $USERDATA_PATH/swapfile
			rm -f "/tmp/using-swap"
		fi
		
		echo `date +'%F %T'` > "$DATETIME_PATH"
		sync
	fi
done

shutdown # just in case

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

# RTC sanity (the Onion/stock-MinUI trick, restore half): a fully dead battery resets the
# RTC to epoch. If the clock reads obvious nonsense, restore the last-known-good time that
# the launch loop already saves after every game — the clock loses only the time spent
# dead, not four decades.
if [ "$(date +%Y)" -lt 2025 ] && [ -f "$DATETIME_PATH" ]; then
	date -s "$(cat "$DATETIME_PATH")" >/dev/null 2>&1
	hwclock -w 2>/dev/null
fi

mkdir -p "$BIOS_PATH"
mkdir -p "$ROMS_PATH"
mkdir -p "$SAVES_PATH"
mkdir -p "$USERDATA_PATH"
mkdir -p "$LOGS_PATH"
mkdir -p "$SHARED_USERDATA_PATH/.minui"

# macs and windows leave droppings on any card they mount (Spotlight index, fsevents logs,
# ._* resource forks, .DS_Store, System Volume Information). Sweep them each boot; the
# .metadata_never_index and .fseventsd/no_log markers shipped in the zip prevent most of it.
rm -rf "$SDCARD_PATH/.Spotlight-V100" "$SDCARD_PATH/.Trashes" "$SDCARD_PATH/System Volume Information" 2>/dev/null
touch "$SDCARD_PATH/.metadata_never_index" 2>/dev/null
mkdir -p "$SDCARD_PATH/.fseventsd" && touch "$SDCARD_PATH/.fseventsd/no_log" 2>/dev/null
find "$SDCARD_PATH/.fseventsd" -type f ! -name no_log -delete 2>/dev/null
rm -f "$SDCARD_PATH/.DS_Store" "$SDCARD_PATH"/._* 2>/dev/null
( find "$ROMS_PATH" "$BIOS_PATH" "$SAVES_PATH" \( -name "._*" -o -name ".DS_Store" \) -delete 2>/dev/null & )

# v1.3 migration: saved PS cfgs snapshot ALL core options, so pre-v1.3 saves pin
# pcsx_rearmed_gpu_thread_rendering=auto forever, silently overriding the new disabled
# default (the async GPU thread serializes on the A133P; DECISIONS D48 — this trap bit
# both dev devices within hours). Idempotent + cheap: only rewrites files still carrying
# the old value.
for _cfg in "$USERDATA_PATH/PS-pcsx_rearmed"/*.cfg; do
	[ -f "$_cfg" ] && grep -q "pcsx_rearmed_gpu_thread_rendering = auto" "$_cfg" && \
		sed -i "s/pcsx_rearmed_gpu_thread_rendering = auto/pcsx_rearmed_gpu_thread_rendering = disabled/" "$_cfg"
done 2>/dev/null

# dev: Stay Awake tool persistence (workspace/tg5040/dev-tools/, never shipped) — the
# /tmp flag resets each boot, so re-arm from the shared flag. Inert unless a dev armed it.
if [ -f "$SHARED_USERDATA_PATH/dev-stay-awake" ]; then
	touch /tmp/stay_awake
	iw dev wlan0 set power_save off 2>/dev/null
	iwconfig wlan0 power off 2>/dev/null
	# net-keeper self-heals a wedged wifi driver (dev tool file; absent on shipped cards)
	NK="$SDCARD_PATH/Tools/$PLATFORM/Stay Awake.pak/net-keeper.sh"
	[ -f "$NK" ] && ( /bin/sh "$NK" </dev/null >/dev/null 2>&1 ) &
fi

# model detection: NO caching — the SD card can move between a Brick and a Smart Pro
# (same tg5040 platform), and a cached model would misdetect after the swap (wrong pad
# init, wrong LED/keymon handling). The MainUI binary lives on the DEVICE, not the card,
# so it must be read fresh each boot. (A cache shipped briefly on 2026-07-02 — reverted.)
export TRIMUI_MODEL=`strings /usr/trimui/bin/MainUI | grep ^Trimui`
if [ "$TRIMUI_MODEL" = "Trimui Brick" ]; then
	export DEVICE="brick"
fi
rm -f "$SHARED_USERDATA_PATH/.minui/model" # clean up the briefly-shipped cache

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
# set default usb mode (reads usbmode from the stock config). Backgrounded: the final
# kernel-attribute read blocks ~2.3s while the USB stack initializes — measured as the
# single biggest cost in our whole boot path, and nothing before the menu depends on it.
usb_device.sh &

# First-boot polish (baked in from the old Bootlogo + Remove Loading tools — see git history).
# Runs once, guarded by a flag; every step is non-fatal so a failure never blocks boot.
FIRSTRUN="$SHARED_USERDATA_PATH/.minui/zero-firstrun-done"
if [ ! -f "$FIRSTRUN" ]; then
	# Remove Loading: drop the stock splash line so boot goes straight to MinUI (no flash).
	[ -f /etc/init.d/runtrimui ] && sed -i '/^\/usr\/sbin\/pic2fb \/etc\/splash.png/d' /etc/init.d/runtrimui 2>/dev/null
	# Bootlogo: replace the vendor boot logo on the eMMC boot partition (once).
	# MODEL-SPECIFIC assets: the two bootloaders expect different BMP formats (Brick:
	# 216x237x24; Smart Pro: 396x66x32). Writing the wrong one renders garbled (learned
	# the hard way on the SP, 2026-07-05) — never write a logo the model did not ask for.
	if [ "$TRIMUI_MODEL" = "Trimui Brick" ]; then
		LOGO="$SYSTEM_PATH/dat/bootlogo.bmp"
	elif [ "$TRIMUI_MODEL" = "Trimui Smart Pro" ]; then
		LOGO="$SYSTEM_PATH/dat/bootlogo-smartpro.bmp"
	else
		LOGO="" # unknown model: leave the vendor logo alone
	fi
	if [ -n "$LOGO" ] && [ -f "$LOGO" ]; then
		BOOTMNT=/tmp/zero-boot
		mkdir -p "$BOOTMNT"
		if mount -t vfat /dev/mmcblk0p1 "$BOOTMNT" 2>/dev/null; then
			cp "$LOGO" "$BOOTMNT/bootlogo.bmp" 2>/dev/null
			sync
			umount "$BOOTMNT" 2>/dev/null
		fi
		rmdir "$BOOTMNT" 2>/dev/null
	fi
	touch "$FIRSTRUN"
	sync
fi

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
	if [ "$DEVICE" = "brick" ]; then
		# GPU-dark menu is BRICK-ONLY, for real (re-verified 2026-07-03 after a bad ungate):
		# writing fb0 produces correct CONTENTS on both devices, but only the Brick panel
		# SCANS OUT fb0 — the Smart Pro display pipeline shows the GLES layer, so fb-present
		# there = black screen while fb0 quietly holds a perfect menu. Do not ungate again
		# unless the SP scanout is actually rebound to fb0.
		ZERO_FB_PRESENT=1 minui.elf &> $LOGS_PATH/minui.txt
	else
		minui.elf &> $LOGS_PATH/minui.txt
	fi
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

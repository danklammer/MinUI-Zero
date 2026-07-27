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
# OPT-IN shell-side boot timing (ZERO_BOOT_TIMING=1), same switch minui.elf uses for its own
# startup probes. /proc/uptime is monotonic seconds since kernel start, so these stamps line up
# with the kernel's own clock and cost one read each. Off by default: this writes to the SD card.
bt() { [ "$ZERO_BOOT_TIMING" = "1" ] && echo "$(cut -d' ' -f1 /proc/uptime) $1" >> /mnt/SDCARD/boot-timing.txt; }
bt "launch.sh start"

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

# AUDIO: one system-lifetime codec owner.
#
# Every pop on this device is an MI_AO power transition -- boot enable, per-game enable/disable,
# poweroff. Muting cannot hide one: MI_AO_SETMUTE gates the DATA path only (proven on device --
# muting never fixed the exit pop; not calling MI_AO_Disable did). One long-lived owner means the
# codec powers up once and never cycles, so the game-boundary pops have nothing to fire on.
#
# SDL2 uses its STOCK OSS backend (src/audio/dsp), selected per-game with SDL_AUDIODRIVER=dsp,
# and libpadsp redirects /dev/dsp into audioserver. This is what MyMinUI actually ships; their
# other/sdl2.patch is an unused draft and applying it here broke game launch.
# The Mini PLUS uses the STOCK /customer/app/audioserver (audioserver.mod is the non-Plus path).
AUDIO_SHIM=/customer/lib/libpadsp.so
AUDIO_FIFO=/tmp/audio_fifo_server

# Is the daemon USABLE right now? Every condition is checked at the moment of use, never cached:
# a boot-time verdict goes stale the instant the daemon dies, and acting on a stale one forces
# games onto a dead OSS path, which is silence.
audio_daemon_ok() {
	# Plus ONLY. The stock /customer/app/audioserver is the qualified path for THIS model; other
	# Miyoo models use audioserver.mod, which is not qualified here. Testing for the binary is not
	# enough -- keep everything else on explicit direct MMIYOO.
	[ "$IS_PLUS" = "true" ] || return 1
	[ -x /customer/app/audioserver ] || return 1
	# The client shim is what actually routes /dev/dsp into the daemon. Without it, selecting the
	# OSS driver just opens the raw device and the game is silent.
	[ -f "$AUDIO_SHIM" ] || return 1
	pgrep audioserver >/dev/null 2>&1 || return 1
	[ -e "$AUDIO_FIFO" ] || return 1
	return 0
}

# Bring the daemon up and WAIT until it is genuinely serving. Bounded (~5s) so a broken daemon can
# never hang the launcher; returns non-zero instead, and the caller falls back.
audio_daemon_start() {
	[ "$IS_PLUS" = "true" ] || return 1
	[ -x /customer/app/audioserver ] || return 1
	# Check the CLIENT SHIM *before* spawning anything. Without libpadsp we cannot route games to
	# the daemon — but a spawned daemon would still CLAIM MI_AO, and then the direct-MMIYOO fallback
	# could not open the codec and the game would be silent. Never start an owner we cannot use.
	[ -f "$AUDIO_SHIM" ] || return 1
	if ! pgrep audioserver >/dev/null 2>&1; then
		# The FIFOs OUTLIVE the daemon (measured: SIGKILL it and /tmp/audio_fifo_server is still
		# there). A leftover would make readiness pass instantly against a daemon that has not
		# started serving yet. Safe to remove -- nothing owns them when no daemon is running.
		rm -f /tmp/audio_fifo_server /tmp/audio_fifo_ioctl_req /tmp/audio_fifo_ioctl_res
		# idempotent: this branch only runs when no owner exists, so we never stack a second one
		/customer/app/audioserver -60 &
	fi
	i=0
	while [ $i -lt 50 ]; do
		audio_daemon_ok && return 0
		sleep 0.1
		i=$((i+1))
	done
	return 1
}

# Release the codec from a daemon we are NOT going to use. A daemon that is alive but not serving
# (no FIFO, or no shim to reach it with) still OWNS MI_AO, so handing the device to a direct-MMIYOO
# game would leave that game unable to open the codec — silent, with nothing in the logs to say why.
# Stop it and confirm it is gone before selecting direct audio.
audio_daemon_release() {
	pgrep audioserver >/dev/null 2>&1 || return 0
	killall audioserver 2>/dev/null
	i=0
	while [ $i -lt 20 ]; do
		pgrep audioserver >/dev/null 2>&1 || break
		sleep 0.1
		i=$((i+1))
	done
	pgrep audioserver >/dev/null 2>&1 && killall -9 audioserver 2>/dev/null
	sleep 0.3
	# its FIFOs outlive it; leave nothing that could later look like readiness
	rm -f /tmp/audio_fifo_server /tmp/audio_fifo_ioctl_req /tmp/audio_fifo_ioctl_res
	pgrep audioserver >/dev/null 2>&1 && return 1
	return 0
}

#######################################

bt "lumon"
lumon.elf & # adjust lcd luma and saturation

# Restore the USER's brightness before the charge screen.
#
# LCD init above parks the backlight at duty 6 — the value SetBrightness(0) maps to, i.e. the
# dimmest step — and the saved level is only applied later by keymon's InitSettings(). batmon runs
# in between, so it both DREW and RESTORED at duty 6: plugging in a powered-off device showed the
# battery screen as dim as the panel goes, whatever the user had set.
#
# Brightness only, deliberately: syncsettings.elf would do this but also calls SetVolume(), which
# touches MI_AO — and audioserver has not claimed the codec yet at this point. Enabling MI_AO here
# would risk the 0xa0052009 "cannot reconfigure an enabled device" wall that stops the daemon
# starting at all. This is pure PWM sysfs and cannot interfere.
#
# msettings.bin is a struct of ints: version, brightness, headphones, speaker, jack.
# Brightness is the second, at byte offset 4. Mapping matches msettings.c SetBrightness():
# 0 -> duty 6, otherwise value*10.
MSETTINGS="$USERDATA_PATH/msettings.bin"
if [ -s "$MSETTINGS" ]; then
	B=$(od -An -tu4 -j4 -N4 "$MSETTINGS" 2>/dev/null | tr -d ' ')
	case "$B" in
		''|*[!0-9]*) B="" ;;
		*) [ "$B" -gt 10 ] && B="" ;;   # out of range: leave the boot default alone
	esac
	if [ -n "$B" ]; then
		[ "$B" = "0" ] && DUTY=6 || DUTY=$((B * 10))
		echo $DUTY > /sys/class/pwm/pwmchip0/pwm0/duty_cycle 2>/dev/null
	fi
fi

# CHARGE-ONLY MODE — deliberately the FIRST thing after backlight setup, and ahead of audioserver,
# keymon, minui, the governor and any core. Plugging in a powered-off device should light a battery
# screen, not boot a games console in the dark: nothing below this line has any business running
# while the user is only charging.
#
# Behaviour (matches the Brick/SP screen this renders):
#   POWER          -> exits, boot continues (exactly once)
#   charger pulled -> exits, boot continues  (the old one ran `shutdown` here, so unplugging
#                     powered the device off)
#   idle           -> dims only, stays responsive  (the old one blanked after 3s and blocked,
#                     which was indistinguishable from a hang and got reported as a crash twice)
#
# ONE predicate, and it lives in batmon. The launcher used to gate on `axp_test` field 7 == 3,
# which is AXP reg 0x00 BIT 2 — battery current direction. That bit CLEARS when the battery is
# full, so a plugged-in device at 100% read as "not charging" and booted straight to the menu,
# while batmon (which gates on ACIN/VBUS presence) would have stayed. Two notions of "charging" in
# two places is how that divergence survived. batmon now decides, and exits immediately when the
# device is not externally powered.
# batmon owns the decision on AXP hardware: its preflight reads the PMIC once and returns
# immediately unless external power is actually present (measured: 20 invocations on battery in 0s).
#
# But batmon is AXP-ONLY -- isPowered() is a raw i2c read with no gpio path. On the original Mini
# (no AXP) that read always fails, so "one predicate in batmon" silently meant NO CHARGE SCREEN AT
# ALL on that model, even though platform.c and keymon both read gpio59 there correctly. The gate
# below is therefore not a duplicate hardware rule, it is the non-AXP branch batmon does not have.
if [ -e /dev/i2c-1 ]; then
	batmon.elf
else
	CHARGING=`cat /sys/devices/gpiochip0/gpio/gpio59/value 2>/dev/null`
	[ "$CHARGING" = "1" ] && batmon.elf
fi

#######################################

# Audio comes up AFTER charge mode exits: a charging device needs no codec, and starting the daemon
# first would have it hold MI_AO through the whole charging session for nothing.
# Still before minui.elf, and that ordering is load-bearing: the daemon must claim MI_AO before
# anything else opens it, or MI_AO_SetPubAttr fails with 0xa0052009 and it dies. This is the only
# moment in the session when it can win the codec.
bt "audio daemon start"
audio_daemon_start
bt "audio daemon ready"
AUDIO_DAEMON=0
audio_daemon_ok && AUDIO_DAEMON=1
AUDIO_RETRY=1   # one revival attempt is allowed if the daemon later dies; see the launch site

# Do NOT export the shim globally. libpadsp is fragile for non-SDL clients -- a program that
# actually drives /dev/dsp segfaults under it (measured) -- and keymon, the shutdown helper and
# every other child inherit whatever is exported here. The menu plays no audio and does not need
# it; only the game process tree does (applied at the launch site below).
unset LD_PRELOAD
export AUDIO_DAEMON

bt "keymon"
keymon.elf & # &> /mnt/SDCARD/keymon.txt &

#######################################

# init datetime — RTC-FIRST on this device (inverted vs upstream, which restores from a file
# unless `enable-rtc` exists).
#
# MEASURED 2026-07-25: this board has a working battery-backed RTC and it is ACCURATE.
#   /sys/class/rtc/rtc0/time  22:16:18     <- correct to the second
#   system clock              22:11:19     <- 5 minutes behind
#   datetime.txt              22:08:32     <- what upstream's restore had written back
# Restoring from datetime.txt therefore OVERWRITES a good clock with the timestamp of the last
# shutdown, so the clock walks backwards every boot. Verified after inverting: system, RTC and
# wall clock all read 22:18:03.
#
# IMPORTANT — this must stay safe on the OTHER models in this family. The Plus has an RTC; the
# original Miyoo Mini is widely reported not to (and any unit's backup cell can die). Trusting the
# RTC unconditionally would leave such a device sitting at the kernel's 1970 epoch with the
# file-restore fallback disabled — a worse clock than before.
#
# So this does NOT assume an RTC exists: it asks whether the clock the kernel came up with is
# PLAUSIBLE. The kernel seeds the system clock from the RTC at boot when one is present, so a
# sane year means a working RTC, and 1970 means there isn't one (or it lost power).
#   RTC present + sane  -> keep it (correct time, no rewind)
#   no RTC / dead cell  -> fall back to datetime.txt exactly as upstream does
# `disable-rtc` forces the file-restore path regardless.
RTC_SANE=0
[ "$(date +%Y)" -ge 2024 ] 2>/dev/null && RTC_SANE=1
if [ -f "$DATETIME_PATH" ] && { [ "$RTC_SANE" = "0" ] || [ -f "$USERDATA_PATH/disable-rtc" ]; }; then
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
		# Re-decide ownership at EVERY launch. The daemon can die between games, and a verdict
		# computed at boot would keep routing games to an OSS endpoint that no longer exists — the
		# game would run silently while looking perfectly healthy.
		#
		# Revival is attempted AT MOST ONCE per session, and usually cannot work. MEASURED: once
		# minui.elf has the codec open, a restarted daemon dies immediately with
		#     MI_AO_SetPubAttr[3364]: Dev0 failed to set pub attr!!! error number:0xa0052009
		# the same "cannot reconfigure an ENABLED device" wall this project keeps hitting. The
		# daemon only wins the codec at boot, before the menu opens it. So retrying on every launch
		# would stall each one for the full readiness timeout and still fail; after one failure we
		# stay on direct MMIYOO for the rest of the session and recover at the next boot.
		if audio_daemon_ok || { [ "$AUDIO_RETRY" = "1" ] && audio_daemon_start; }; then
			AUDIO_DAEMON=1
			export SDL_AUDIODRIVER=dsp
			# NOT exported. Exporting put the shim on every helper the pak spawns -- needs-swap,
			# dd, mkswap, say.elf -- and this file states two screens up that libpadsp segfaults
			# non-SDL clients that drive /dev/dsp. Applied to the minarch process ALONE, at its
			# exec, via MINARCH_PRELOAD (see the pak launch.sh invocation).
			MINARCH_PRELOAD=$AUDIO_SHIM
		else
			# EXPLICIT fallback, and it must name the driver. Merely unsetting SDL_AUDIODRIVER is
			# not enough: OSS is registered BEFORE MMIYOO in SDL2 bootstrap[] (src/audio/SDL_audio.c
			# -- DSP at 114, MMIYOO at 126), so SDL would auto-pick dsp again and route into
			# nothing. The pop returns on this path; silence would be the worse failure.
			# Take the codec back first. Reaching here with a daemon still running means it is
			# wedged or unreachable; it would keep MI_AO and this game would open nothing.
			audio_daemon_release
			AUDIO_DAEMON=0
			AUDIO_RETRY=0   # do not stall every later launch on a revival that cannot succeed
			export SDL_AUDIODRIVER=MMIYOO
			MINARCH_PRELOAD=
		fi
		# The game needs its OWN ownership mode: in daemon mode minarch is only an OSS client and
		# must NOT disable the codec on exit/crash (PLAT_resetAudio honours this).
		export AUDIO_DAEMON MINARCH_PRELOAD
		eval $CMD
		unset SDL_AUDIODRIVER MINARCH_PRELOAD
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

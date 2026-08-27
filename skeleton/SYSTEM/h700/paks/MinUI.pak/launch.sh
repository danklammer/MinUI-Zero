#!/bin/sh
# MinUI Zero launcher entry point for h700 (Anbernic RG35XX Plus / H).
#
# WHY THIS FILE EXISTS. tg5040 and miyoomini have always had a MinUI.pak/launch.sh; h700 did not,
# because our own image starts the launcher by patching muOS's startup.sh instead
# (tools/build-h700-stripped.sh swaps its FRONTEND line for /opt/minui-zero/minui-frontend.sh).
# That works, but it is the only platform where the entry point lives in the OS rather than on the
# card, which (a) breaks the MinUI pak contract every other platform honours and (b) makes the
# frontend un-runnable on any OS layer other than our own muOS strip.
#
# PORTABLE ON PURPOSE. BaseOS (github.com/pvaibhav/BaseOS) is a minimal H700 OS that hands off with
#     exec /bin/sh "$SD/.system/h700/paks/MinUI.pak/launch.sh"
# which is exactly this path. Everything muOS-specific below is guarded by a file test, so this
# script runs unmodified on our image and on a bare OS that only mounts the card and execs us.
# Things the OS layer owns and this script must NOT duplicate: wifi, ssh, kernel modules, mounts.

# --- where is the card? muOS mounts it at /mnt/mmc, BaseOS at /mnt/sdcard --------------------
if [ -d /mnt/mmc/.system/h700 ]; then
	export SDCARD_PATH="/mnt/mmc"
elif [ -d /mnt/sdcard/.system/h700 ]; then
	export SDCARD_PATH="/mnt/sdcard"
else
	# last resort: resolve from our own location, so an unknown mount point still works
	export SDCARD_PATH="$(cd "$(dirname "$0")/../../../.." 2>/dev/null && pwd)"
fi

export PLATFORM="h700"
export SYSTEM_PATH="$SDCARD_PATH/.system/$PLATFORM"
export CORES_PATH="$SYSTEM_PATH/cores"
export BIOS_PATH="$SDCARD_PATH/Bios"
export ROMS_PATH="$SDCARD_PATH/Roms"
export SAVES_PATH="$SDCARD_PATH/Saves"
export CHEATS_PATH="$SDCARD_PATH/Cheats"
export USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
export SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/shared"
export LOGS_PATH="$USERDATA_PATH/logs"
export DATETIME_PATH="$SHARED_USERDATA_PATH/datetime.txt"

# plus vs h are near-twins; the board name is how we tell them apart. muOS publishes it; BaseOS
# does not, so fall back to the device tree model and finally to plus.
if [ -f /opt/muos/device/config/board/name ]; then
	export DEVICE=$(sed 's/^rg35xx-//' /opt/muos/device/config/board/name 2>/dev/null)
fi
[ -n "$DEVICE" ] || DEVICE=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null | grep -oiE "h$|plus" | tr 'A-Z' 'a-z')
[ -n "$DEVICE" ] || DEVICE=plus
export DEVICE

export LD_LIBRARY_PATH="$SYSTEM_PATH/lib:/usr/lib:/lib:$LD_LIBRARY_PATH"
export PATH="$SYSTEM_PATH/bin:$PATH"
# SDL2 here has exactly one video driver ("mali"); the launcher presents through the DE layer, so
# the dummy driver is correct and keeps SDL from initialising a GL context we never use.
export SDL_VIDEODRIVER=dummy
# DEVICE properties, so they live here rather than being repeated by all 15 emu paks (2026-08-26).
# Panel measured 59.9777 Hz (panelprobe 2026-08-04); minarch paces against the real rate, not 60.
export MINARCH_PANEL_FPS=59.9777
# ALSA-direct: pipewire is stripped, and asound.conf routes "default" straight to the codec
# (plug -> hw:0,0). SDL must not go looking for a sound server that is not there.
export SDL_AUDIODRIVER=alsa

mkdir -p "$LOGS_PATH" "$SAVES_PATH" "$SHARED_USERDATA_PATH/.minui" "$USERDATA_PATH" 2>/dev/null
LOG="$LOGS_PATH/launch.txt"
: > "$LOG" 2>/dev/null

# --- clock: the board has no battery-backed RTC, so a cold boot starts in 1970 ----------------
if [ "$(date +%Y)" -lt 2025 ] && [ -f "$DATETIME_PATH" ]; then
	date -s "$(cat "$DATETIME_PATH")" >/dev/null 2>&1
fi

# --- audio codec: unmute + restore the mixer baseline BEFORE the launcher reads it ------------
# minui reads the codec volume exactly once at startup. muOS restores the mixer in a BACKGROUNDED
# pipewire.sh, which races that read and loses often enough to boot dead silent (found live
# 2026-08-06). Do it here, synchronously, before anything reads. Absent on a non-muOS OS layer,
# which is fine: alsactl restore with no state file is a no-op.
for _st in /opt/muos/device/control/asound.state /var/lib/alsa/asound.state; do
	[ -f "$_st" ] && { alsactl -U -f "$_st" restore >/dev/null 2>&1; break; }
done

# Re-assert the USER's saved volume over that baseline, at every process boundary below. The only
# muter is PWR_enterSleep (raw 0) and its un-mute lives in the SAME process; if that process is
# replaced while muted (faux-sleep then relaunch, a crash, a dev deploy) nobody writes the level
# back and the codec stays at 0 into the next game. Captured live 2026-08-10.
VOL_FILE="$USERDATA_PATH/volume"   # UI 0-20, written by libmsettings SetVolume
apply_volume() {
	[ -f "$VOL_FILE" ] || return 0
	_ui=$(cat "$VOL_FILE" 2>/dev/null)
	case "$_ui" in ''|*[!0-9]*) return 0 ;; esac   # ignore a garbage or partial file
	[ "$_ui" -gt 20 ] && _ui=20
	amixer -c 0 sset 'digital volume' $(( _ui * 63 / 20 )) >/dev/null 2>&1   # UI 0-20 -> raw 0-63
}
apply_volume

# --- the thesis: own the governor. schedutil, and minui/minarch write the ceiling on top -------
echo schedutil > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)" >> "$LOG"

# --- boot-time readahead ----------------------------------------------------------------------
# The FIRST launch after a boot is the slow one: everything it touches is cold on a ~10MB/s card.
# MEASURED 2026-08-10: 4348ms cold vs 707ms warm, and the biggest single item is libmali.so
# (42.5MB), which SDL dlopens because "mali" is its only video driver. Pull that fixed cost into
# the seconds after boot while the user is reading the menu. Page cache only: nothing stays
# resident, the kernel evicts under pressure, and it costs nothing the thesis measures.
( nice -n 19 sh -c '
	sleep 3
	for f in /usr/lib/libmali.so /usr/lib/libSDL2-2.0.so.0 /usr/lib/libSDL2_image-2.0.so.0 \
	         /usr/lib/libSDL2_ttf-2.0.so.0 "$SYSTEM_PATH/bin/minarch.elf" \
	         "$SYSTEM_PATH/lib/libmsettings.so"; do
		[ -f "$f" ] && cat "$f" > /dev/null 2>&1
	done
	R="$SHARED_USERDATA_PATH/.minui/recent.txt"
	if [ -f "$R" ]; then
		T=$(sed -n "1p" "$R" | sed -n "s/.*(\([A-Z0-9]*\)).*/\1/p")
		[ -n "$T" ] && [ -f "$SYSTEM_PATH/paks/Emus/$T.pak/launch.sh" ] && \
			C=$(grep -o "[a-z0-9_-]*_libretro\.so" "$SYSTEM_PATH/paks/Emus/$T.pak/launch.sh" | head -1) && \
			[ -n "$C" ] && [ -f "$CORES_PATH/$C" ] && cat "$CORES_PATH/$C" > /dev/null 2>&1
	fi
' >/dev/null 2>&1 ) &

# --- community pak compat ---------------------------------------------------------------------
# The scene's canonical mount is /mnt/SDCARD and paks hardcode it constantly (NextUI HOOKS.md
# documents that literal path). Ours is elsewhere, so a hardcoded pak would write into a
# nonexistent tree and silently do nothing. Only when the real mount exists and the name is free.
[ -d "$SDCARD_PATH" ] && [ ! -e /mnt/SDCARD ] && ln -s "$SDCARD_PATH" /mnt/SDCARD 2>/dev/null

# --- power off ---------------------------------------------------------------------------------
# The AXP register write is how this board actually powers down; a plain poweroff reboots it.
power_off() {
	sync
	echo 0x1801 > /sys/class/axp/axp_reg 2>/dev/null
	[ -x /opt/muos/script/system/halt.sh ] && /opt/muos/script/system/halt.sh poweroff 2>/dev/null
	poweroff -f
}

# --- boot straight into the game ---------------------------------------------------------------
# MinUI quicksaves on power-off and resumes on the next boot, but the resume normally goes through
# the launcher: it starts, reads the marker, writes /tmp/next and exits, so a resume pays a full
# launcher startup and a menu frame the user never wanted. Do it here instead.
#
# The contract is the launcher's own (minui.c autoResume): the marker holds a card-relative rom
# path, it is consumed EXACTLY ONCE (unlink before launching, so a crash cannot resume-loop), and
# slot 9 tells minarch to load the auto-save. Every failure falls through to the normal path.
AUTO_RESUME="$SHARED_USERDATA_PATH/.minui/auto_resume.txt"
if [ -f "$AUTO_RESUME" ]; then
	_rel=$(head -1 "$AUTO_RESUME" 2>/dev/null)
	rm -f "$AUTO_RESUME"; sync
	_rom="$SDCARD_PATH$_rel"
	_tag=$(dirname "$_rel" | sed -n 's/.*(\([A-Za-z0-9]*\))$/\1/p')
	_pak="$SYSTEM_PATH/paks/Emus/$_tag.pak/launch.sh"
	if [ -n "$_rel" ] && [ -f "$_rom" ] && [ -n "$_tag" ] && [ -f "$_pak" ]; then
		echo "boot-to-game: $_tag <- $_rel" >> "$LOG"
		echo 9 > /tmp/resume_slot.txt
		apply_volume
		sh "$_pak" "$_rom" >> "$LOG" 2>&1
		[ -f /tmp/poweroff ] && { echo "poweroff requested (boot-to-game)" >> "$LOG"; power_off; }
	fi
fi

AUTO_PATH="$USERDATA_PATH/auto.sh"
[ -f "$AUTO_PATH" ] && "$AUTO_PATH"

cd "$(dirname "$0")"

#######################################

# THE LAUNCH LOOP. Ported verbatim from the h700 frontend script rather than from tg5040, because
# the two platforms genuinely differ and the h700 semantics are the audited ones:
#   - tg5040 ends the loop by unlinking /tmp/minui_exec in PLAT_powerOff.
#   - h700 PLAT_powerOff drops /tmp/poweroff instead, so the loop can tell a real power-off request
#     from an ordinary game or menu exit. Without that distinction an in-game power-off looked like
#     a quit and relaunched the same game (audit 2026-08-07).
# A first draft of this file used the tg5040 marker, which would have left the power-off path
# working only by accident and dropped the fail-retry below entirely (caught 2026-08-26).
FAILS=0
while : ; do
	rm -f /tmp/next /tmp/poweroff
	minui.elf >> "$LOG" 2>&1
	RC=$?
	[ -f /tmp/poweroff ] && { echo "poweroff requested" >> "$LOG"; power_off; }
	if [ -f /tmp/next ]; then
		FAILS=0
		CMD=$(cat /tmp/next)
		echo "launch: $CMD" >> "$LOG"
		apply_volume   # a mute left behind by the menu process must not follow us into the game
		sh -c "$CMD"
		echo "game exited rc=$?" >> "$LOG"
		apply_volume   # ...nor back into the menu if the game was killed while muted
		[ -f /tmp/poweroff ] && { echo "poweroff requested (in-game)" >> "$LOG"; power_off; }
		echo "$(date +'%F %T')" > "$DATETIME_PATH"
		sync
	elif [ "$RC" = "0" ]; then
		echo "clean exit, power off" >> "$LOG"
		power_off
	else
		FAILS=$((FAILS+1))
		echo "minui exited rc=$RC (fail $FAILS)" >> "$LOG"
		# Retry so transient faults self-heal; persistent failure powers OFF rather than parking in
		# an infinite sleep, which left a black, draining, unrecoverable device (audit).
		[ $FAILS -ge 5 ] && { echo "$FAILS consecutive fails, powering off" >> "$LOG"; power_off; }
		sleep 2
	fi
done

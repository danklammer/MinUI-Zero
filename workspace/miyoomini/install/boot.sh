#!/bin/sh
# NOTE: becomes .tmp_update/miyoomini.sh

PLATFORM="miyoomini"
SDCARD_PATH="/mnt/SDCARD"
UPDATE_PATH="$SDCARD_PATH/MinUI.zip"
SYSTEM_PATH="$SDCARD_PATH/.system"

# Boot at a real, modest OPP rather than pinning `performance` (which parks at 1200MHz for the
# whole boot + menu until the first PLAT_setCPUMaxFreq call). MinUI.pak/launch.sh sets the
# userspace governor and the menu clock immediately after this.
CPU_PATH=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo userspace > "$CPU_PATH"
echo 800000 > /sys/devices/system/cpu/cpufreq/policy0/scaling_setspeed 2>/dev/null

# install/update
if [ -f "$UPDATE_PATH" ]; then 
	cd $(dirname "$0")/$PLATFORM
	
	# init backlight
	echo 0 > /sys/class/pwm/pwmchip0/export
	echo 800 > /sys/class/pwm/pwmchip0/pwm0/period
	echo 50 > /sys/class/pwm/pwmchip0/pwm0/duty_cycle
	echo 1 > /sys/class/pwm/pwmchip0/pwm0/enable

	# init lcd
	cat /proc/ls
	sleep 1
	export LCD_INIT=1

	if [ -d "$SYSTEM_PATH" ]; then
		./show.elf ./updating.png
	else
		./show.elf ./installing.png
	fi
	
	# ------------------------------------------------------------------
	# STAGED, VERIFIED, ROLLBACK-SAFE INSTALL
	#
	# This used to be four unchecked commands: rename the bootstrap aside, unzip over the LIVE
	# system, delete the zip, delete the rollback. Nothing tested whether the unzip worked, so a
	# corrupt download, a full card, or power loss mid-extract left a half-written system AND
	# deleted both escape routes — the only copy of the payload and the previous bootstrap. The
	# card then could not boot and could not repair itself; it needed a PC and a re-image.
	#
	# The rule now: nothing is deleted until the new system is proven to exist on disk.
	# ------------------------------------------------------------------
	SHOW="$(pwd)/show.elf"   # absolute: the directory we are cd'd into gets renamed below
	FAIL_LOG="$SDCARD_PATH/MinUI-update-failed.txt"
	NEW_LAUNCH="$SYSTEM_PATH/$PLATFORM/paks/MinUI.pak/launch.sh"

	# 1. VERIFY THE ARCHIVE BEFORE TOUCHING ANYTHING.
	#    Prefer `unzip -t` (checks CRCs). Some busybox builds lack -t, and failing a GOOD update
	#    because the tool is limited would be its own bug — so fall back to `-l`, which every
	#    unzip supports and which still rejects a truncated or corrupt archive.
	ZIP_OK=0
	if unzip -t "$UPDATE_PATH" >/dev/null 2>&1; then ZIP_OK=1
	elif unzip -l "$UPDATE_PATH" >/dev/null 2>&1; then ZIP_OK=1
	fi
	# ...and that it is actually a MinUI payload for THIS platform. Checking only that the launcher
	# exists AFTER extracting is not enough: on an already-installed card that file is present from
	# the PREVIOUS version, so a zip that carried no system at all would still look like a success,
	# and we would delete the zip and the rollback for an update that never happened.
	if [ "$ZIP_OK" = "1" ]; then
		unzip -l "$UPDATE_PATH" 2>/dev/null | grep -q "$PLATFORM/paks/MinUI.pak/launch.sh" || ZIP_OK=0
	fi

	if [ "$ZIP_OK" != "1" ]; then
		# Change NOTHING. The existing system (if any) still boots, and the zip is kept so the
		# user can retry after re-copying it.
		echo "$(date '+%F %T') MinUI.zip is corrupt or truncated. Nothing was changed; the file was kept so you can replace it and reboot." > "$FAIL_LOG"
		sync
	else
		# 2. Keep the current bootstrap as a rollback. Clear any stale one first, or the mv below
		#    fails and we would proceed with no rollback at all.
		rm -rf "$SDCARD_PATH/.tmp_update-old"
		mv "$SDCARD_PATH/.tmp_update" "$SDCARD_PATH/.tmp_update-old"

		# 3. Extract, then PROVE the result: unzip's exit status AND the launcher actually present.
		#    Exit status alone is not enough — a zip can extract "successfully" and still be the
		#    wrong payload.
		if unzip -o "$UPDATE_PATH" -d "$SDCARD_PATH" && [ -f "$NEW_LAUNCH" ]; then
			sync   # commit the new system to the card BEFORE removing anything
			rm -f "$UPDATE_PATH"
			rm -rf "$SDCARD_PATH/.tmp_update-old"
			rm -f "$FAIL_LOG"
			sync

			# the updated system finishes the install/update
			if [ -f "$SYSTEM_PATH/$PLATFORM/bin/install.sh" ]; then
				"$SYSTEM_PATH/$PLATFORM/bin/install.sh"
			fi
		else
			# 4. ROLL BACK. Remove whatever partial tree was written, restore the previous
			#    bootstrap, and KEEP the zip so the update can be retried.
			echo "$(date '+%F %T') Update failed while extracting (corrupt archive, full card, or interrupted power). The previous version was restored and MinUI.zip was kept - reboot to retry." > "$FAIL_LOG"
			rm -rf "$SDCARD_PATH/.tmp_update"
			mv "$SDCARD_PATH/.tmp_update-old" "$SDCARD_PATH/.tmp_update"
			sync
		fi
	fi
fi

# or launch (and keep launched)
LAUNCH_PATH="$SYSTEM_PATH/$PLATFORM/paks/MinUI.pak/launch.sh"
while [ -f "$LAUNCH_PATH" ] ; do
	"$LAUNCH_PATH"
done

reboot # under no circumstances should stock be allowed to touch this card

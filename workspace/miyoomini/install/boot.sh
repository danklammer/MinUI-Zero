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
		# 2. STAGE the extract somewhere harmless. Extracting straight over the LIVE system meant a
		#    failure part-way left .system as a mix of old and new binaries — and the rollback below
		#    only ever restored .tmp_update, so the message "the previous version was restored" was
		#    not true of the system itself. Nothing touches the live tree until the payload is
		#    complete on disk.
		STAGE="$SDCARD_PATH/.minui-staging"
		rm -rf "$STAGE"
		if ! mkdir -p "$STAGE"; then
			echo "$(date '+%F %T') Could not create a staging folder on the card - it may be full or write-protected. Nothing was changed." > "$FAIL_LOG"
			sync
		elif unzip -o "$UPDATE_PATH" -d "$STAGE" && [ -f "$STAGE/.system/$PLATFORM/paks/MinUI.pak/launch.sh" ] && [ -d "$STAGE/.tmp_update" ]; then
			# 3. The payload is fully extracted and verified. Only now touch the live system, and do
			#    it in the order that always leaves something bootable:
			#      a) keep the current bootstrap as a rollback
			#      b) move the new system in
			#      c) move the new bootstrap in
			sync
			rm -rf "$SDCARD_PATH/.tmp_update-old"
			mv "$SDCARD_PATH/.tmp_update" "$SDCARD_PATH/.tmp_update-old"

			cp -rf "$STAGE/.system" "$SDCARD_PATH/" && cp -rf "$STAGE/.tmp_update" "$SDCARD_PATH/"
			COPY_RC=$?
			# anything else the zip carried (Bios/Roms/Saves skeletons, Tools) — best effort, and
			# never fatal: the system itself is already in place.
			for extra in "$STAGE"/*; do
				case "$(basename "$extra")" in .system|.tmp_update) continue;; esac
				cp -rf "$extra" "$SDCARD_PATH/" 2>/dev/null
			done
			sync

			if [ "$COPY_RC" = "0" ] && [ -f "$NEW_LAUNCH" ]; then
				rm -rf "$STAGE"
				rm -f "$UPDATE_PATH"
				rm -rf "$SDCARD_PATH/.tmp_update-old"
				rm -f "$FAIL_LOG"
				sync
				# the updated system finishes the install/update
				if [ -f "$SYSTEM_PATH/$PLATFORM/bin/install.sh" ]; then
					"$SYSTEM_PATH/$PLATFORM/bin/install.sh"
				fi
			else
				# The copy itself failed (card filled between staging and install). Put the old
				# bootstrap back and KEEP both the staging tree and the zip for a retry.
				echo "$(date '+%F %T') Update failed while installing (the card may be full). The previous bootstrap was restored and MinUI.zip was kept - free some space and reboot to retry." > "$FAIL_LOG"
				rm -rf "$SDCARD_PATH/.tmp_update"
				mv "$SDCARD_PATH/.tmp_update-old" "$SDCARD_PATH/.tmp_update"
				sync
			fi
		else
			# 4. Extract failed or produced the wrong payload. The LIVE SYSTEM WAS NEVER TOUCHED —
			#    that is the point of staging. Drop the staging tree and keep the zip to retry.
			echo "$(date '+%F %T') Update failed while extracting (corrupt archive, full card, or interrupted power). Nothing was changed and MinUI.zip was kept - reboot to retry." > "$FAIL_LOG"
			rm -rf "$STAGE"
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

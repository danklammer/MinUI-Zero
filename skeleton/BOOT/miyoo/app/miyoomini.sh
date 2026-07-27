#!/bin/sh

IS_PLUS=false
if [ -f "/customer/app/axp_test" ]; then
	IS_PLUS=true
fi

SDCARD_PATH=/mnt/SDCARD
MIYOO_PATH=$(cd "$(dirname "$0")"/.. && pwd)

cd "$MIYOO_PATH/app"

export LD_LIBRARY_PATH=/lib:/config/lib:/customer/lib

# update bootcmd if necessary
contains() { [ -z "${2##*$1*}" ]; }
MIYOO_VERSION=`/etc/fw_printenv miyoo_version`
MIYOO_VERSION=${MIYOO_VERSION#miyoo_version=}
SUPPORTED_VERSION="202205010000" # date after latest known version
# TODO: pretty sure this bricks a subsequent update
if [ $MIYOO_VERSION -lt $SUPPORTED_VERSION ]; then
	OLD_CMD=`/etc/fw_printenv bootcmd`
	NEW_CMD="gpio output 85 1; bootlogo 0 0 0 0 0; mw 1f001cc0 11; gpio out 8 0; sf probe 0;sf read 0x22000000 \${sf_kernel_start} \${sf_kernel_size}; gpio out 8 1; gpio output 4 1; bootm 0x22000000"
	if contains "sleepms" "$OLD_CMD"; then
		/etc/fw_setenv bootcmd $NEW_CMD
		sleep 1
	fi
fi

# .tmp_update/updater does the actual installation (and later, updating)
#
# ORDER MATTERS AND IT USED TO BE WRONG. This copied the bootstrap, then deleted $MIYOO_PATH — the
# stock-firmware entry point, i.e. the only thing that re-runs this on the next boot — WITHOUT ever
# checking that the copy succeeded. On a full card or an interrupted write that left no bootstrap
# and no way back in.
#
# Now: copy, prove the updater is actually there, sync it to the card, and only then remove the
# entry point. If the copy failed, leave everything alone and let the stock path run again.
cp -rf .tmp_update $SDCARD_PATH/
if [ ! -f "$SDCARD_PATH/.tmp_update/updater" ]; then
	echo "$(date '+%F %T') Could not copy the MinUI bootstrap to the SD card (full card, or a write error). Nothing was removed - free some space and try again." > "$SDCARD_PATH/MinUI-install-failed.txt"
	sync
	if $IS_PLUS; then poweroff; else reboot; fi
fi
sync                      # commit the bootstrap BEFORE deleting our way back to it
rm -rf "$MIYOO_PATH"
sync
$SDCARD_PATH/.tmp_update/updater

# under no circumstances should stock be allowed to touch this card
if $IS_PLUS; then
	poweroff
else
	reboot
fi
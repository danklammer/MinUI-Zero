#!/bin/sh

SDCARD_PATH=/mnt/SDCARD

# --------------------------------------
# update runtrimui.sh
OLD_PATH=/usr/trimui/bin/runtrimui.sh
NEW_PATH=${SDCARD_PATH}/.system/tg5040/dat/runtrimui.sh
echo "check for outdated $OLD_PATH"
if [ -f $NEW_PATH ] && ! grep -q exec $OLD_PATH; then
	echo "replacing with updated version"
	# never rm-then-cp a boot-critical file: a failed cp would leave the device unbootable.
	# stage on the same filesystem, then swap (audit 2026-07-12)
	cp $NEW_PATH $OLD_PATH.new && mv -f $OLD_PATH.new $OLD_PATH
fi

# --------------------------------------
# remove old brick system folder
BRICK_PATH=${SDCARD_PATH}/.system/tg3040
echo "check for $BRICK_PATH"
# this might always exist so we can pull up old cards
if [ -d $BRICK_PATH ]; then
	echo "deleting brick system folder $BRICK_PATH"
	rm -rf "$BRICK_PATH"
	
	# copy brick configs from userdata
	SRC_PATH=${SDCARD_PATH}/.userdata/tg3040
	if [ -d $SRC_PATH ]; then
		DST_PATH=${SDCARD_PATH}/.userdata/tg5040
		mkdir -p $DST_PATH # just in case
	
		for SUB_PATH in $SRC_PATH/*; do
			if [ -d $SUB_PATH ]; then
				SUB_NAME=$(basename $SUB_PATH)
				NEW_PATH=$DST_PATH/$SUB_NAME
			
				if [ ! -d $NEW_PATH ]; then
					echo "creating new path $NEW_PATH"
					mkdir -p $NEW_PATH
				fi
			
				for CFG_PATH in $SUB_PATH/*.cfg; do
					if [ -f $CFG_PATH ]; then
						CFG_NAME=$(basename $CFG_PATH .cfg)
						echo "copying $CFG_PATH to $NEW_PATH/$CFG_NAME-brick.cfg"
						cp $CFG_PATH $NEW_PATH/$CFG_NAME-brick.cfg || MIGRATE_FAILED=1
					fi
				done
			fi
		done
		# only delete the originals if every copy landed (audit 2026-07-12)
		if [ "$MIGRATE_FAILED" = "1" ]; then
			echo "config migration incomplete — keeping $SRC_PATH"
		else
			echo "deleting brick userdata $SRC_PATH"
			rm -rf $SRC_PATH
		fi
		
		UPDATE_PATH=${SDCARD_PATH}/.tmp_update/tg3040
		rm -rf $UPDATE_PATH.sh
		rm -rf $UPDATE_PATH
		
		reboot
		# we need to sleep until reboot otherwise 
		# it will poweroff without rebooting
		while :; do
			sleep 1
		done
	fi
fi
# --------------------------------------

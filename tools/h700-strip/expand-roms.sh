#!/bin/sh
# One-shot ROMS expansion for the MinUI Zero h700 image. Runs from startup.sh BEFORE muOS mounts
# the card (mount/start.sh) — that timing is the whole design, see WHY below.
#
# The flashed image is ~1.4GB, so on a bigger card everything past the ROMS partition is
# unreachable: a 32GB card shows 256MB of ROM space (measured on-device: 58,130,432 free sectors,
# ~28GB, 2026-08-10).
#
# There is no in-place FAT32 grow here (parted/sfdisk/mkfs.vfat present; NO fatresize, NO
# resize2fs), so this does what muOS's own factory reset does — resize the partition, reformat,
# restore — except it restores OUR payload from a copy instead of an archive.
#
# WHY PRE-MOUNT (learned the hard way 2026-08-10): the first version ran from the frontend, after
# the card was mounted. `umount -l` made /proc/mounts look clean while the filesystem was still
# busy, so `parted resizepart` silently prompted "Partition is being used... Yes/No?" and did
# NOTHING; mkfs then reformatted at the OLD size and the script declared success because it never
# checked whether the partition actually grew. Data survived (the backup/restore path is sound),
# but nothing was gained. Two rules came out of that:
#   * do this while the card is UNMOUNTED and nothing can hold it — i.e. before mount/start.sh
#   * never claim success without measuring the result
#
# Kernel table reload: with the rootfs mounted from the same disk the kernel will not re-read the
# partition table, so growth lands in two phases across one automatic reboot (PHASE marker).
set -u

DEV=/dev/mmcblk0
PART_NUM=6
PART="${DEV}p${PART_NUM}"
MOUNT=/mnt/mmc
STATE=/opt/minui-zero
DONE_MARK="$STATE/roms-expanded"
PHASE2_MARK="$STATE/roms-resized"
FAILED_MARK="$STATE/roms-expand-failed"
BACKUP=/var/minui-zero-payload
LOG=/var/minui-zero-expand.log   # rootfs: the card is not mounted while this runs

say() { echo "$(date +%H:%M:%S) $*" >> "$LOG"; }

[ -f "$DONE_MARK" ] && exit 0
[ -f "$FAILED_MARK" ] && exit 0   # a previous run stopped somewhere unsafe: leave it to a human

command -v parted >/dev/null 2>&1 || exit 0
command -v mkfs.vfat >/dev/null 2>&1 || exit 0
[ -b "$PART" ] || exit 0
grep -q " $MOUNT " /proc/mounts && { say "card already mounted — wrong hook point, skipping"; exit 0; }

DISK_SECTORS=$(cat /sys/block/mmcblk0/size 2>/dev/null || echo 0)
PART_START=$(cat /sys/block/mmcblk0/mmcblk0p6/start 2>/dev/null || echo 0)
PART_SIZE=$(cat /sys/block/mmcblk0/mmcblk0p6/size 2>/dev/null || echo 0)
[ "$DISK_SECTORS" -gt 0 ] && [ "$PART_SIZE" -gt 0 ] || exit 0
FREE=$(( DISK_SECTORS - PART_START - PART_SIZE ))

# ---------- PHASE 1: grow the partition entry, then reboot so the kernel re-reads it ----------
if [ ! -f "$PHASE2_MARK" ]; then
	[ "$FREE" -gt 2097152 ] || { say "only ${FREE}s free past ROMS — nothing to do"; touch "$DONE_MARK"; exit 0; }
	say "phase 1: ${FREE}s (~$(( FREE / 2048 ))MB) to reclaim; resizing partition $PART_NUM"
	printf "w\nw\n" | fdisk "$DEV" >> "$LOG" 2>&1
	printf "Yes\n" | parted ---pretend-input-tty "$DEV" resizepart "$PART_NUM" 100% >> "$LOG" 2>&1
	parted ---pretend-input-tty "$DEV" set "$PART_NUM" boot off    >> "$LOG" 2>&1
	parted ---pretend-input-tty "$DEV" set "$PART_NUM" hidden off  >> "$LOG" 2>&1
	parted ---pretend-input-tty "$DEV" set "$PART_NUM" msftdata on >> "$LOG" 2>&1
	sync
	# VERIFY the on-disk table actually changed before going any further
	NEW_END=$(parted -s "$DEV" unit s print 2>/dev/null | awk -v n="$PART_NUM" '$1==n {gsub("s","",$3); print $3}')
	if [ -z "$NEW_END" ] || [ "$NEW_END" -le $(( PART_START + PART_SIZE )) ]; then
		say "phase 1 FAILED: table unchanged (end=$NEW_END) — leaving the card exactly as it was"
		exit 0    # no marker: harmless to retry next boot
	fi
	say "phase 1 ok: partition now ends at ${NEW_END}s; rebooting so the kernel re-reads the table"
	touch "$PHASE2_MARK"; sync
	sleep 1
	reboot -f
	exit 0
fi

# ---------- PHASE 2: kernel now sees the big partition; reformat at full size + restore ----------
say "phase 2: kernel sees ${PART_SIZE}s; formatting and restoring"
mkdir -p "$MOUNT" 2>/dev/null
if ! mount -t vfat -o rw,utf8,noatime,nofail "$PART" "$MOUNT" 2>>"$LOG"; then
	say "phase 2: cannot mount the old filesystem to back it up — aborting"
	touch "$FAILED_MARK"; exit 0
fi
PAYLOAD_KB=$(du -sk "$MOUNT" 2>/dev/null | cut -f1)
ROOT_FREE_KB=$(df -k / 2>/dev/null | awk 'NR==2 {print $4}')
if [ -z "$PAYLOAD_KB" ] || [ -z "$ROOT_FREE_KB" ] || [ "$ROOT_FREE_KB" -lt $(( PAYLOAD_KB * 2 + 65536 )) ]; then
	say "phase 2: payload ${PAYLOAD_KB}KB will not fit safely in ${ROOT_FREE_KB}KB rootfs — leaving card as is"
	umount "$MOUNT" 2>/dev/null; exit 0
fi
rm -rf "$BACKUP"; mkdir -p "$BACKUP"
if ! cp -a "$MOUNT"/. "$BACKUP"/ 2>>"$LOG"; then
	say "phase 2: backup failed — card untouched"
	rm -rf "$BACKUP"; umount "$MOUNT" 2>/dev/null; exit 0
fi
SRC=$(find "$MOUNT" | wc -l); BAK=$(find "$BACKUP" | wc -l)
if [ "$SRC" != "$BAK" ]; then
	say "phase 2: backup verify failed ($SRC vs $BAK) — card untouched"
	rm -rf "$BACKUP"; umount "$MOUNT" 2>/dev/null; exit 0
fi
sync
say "phase 2: backup verified ($BAK entries), reformatting"
if ! umount "$MOUNT" 2>>"$LOG"; then
	say "phase 2: real unmount failed — refusing to format a busy partition (this is the bug that bit us)"
	rm -rf "$BACKUP"; exit 0
fi
mkfs.vfat -F 32 -n ROMS "$PART" >> "$LOG" 2>&1
sync
if ! mount -t vfat -o rw,utf8,noatime,nofail "$PART" "$MOUNT" 2>>"$LOG"; then
	say "phase 2: REMOUNT FAILED after format — payload is safe at $BACKUP"
	touch "$FAILED_MARK"; exit 0
fi
if ! cp -a "$BACKUP"/. "$MOUNT"/ 2>>"$LOG"; then
	say "phase 2: restore failed — payload still at $BACKUP"
	touch "$FAILED_MARK"; exit 0
fi
NEW=$(find "$MOUNT" | wc -l); sync
if [ "$NEW" != "$BAK" ]; then
	say "phase 2: restore verify mismatch ($NEW vs $BAK) — payload kept at $BACKUP"
	touch "$FAILED_MARK"; exit 0
fi
# MEASURE the result: only now is this a success
SIZE=$(df -k "$MOUNT" 2>/dev/null | awk 'NR==2 {print $2}')
say "phase 2: done — ROMS is now $(( SIZE / 1024 ))MB with $NEW entries restored"
umount "$MOUNT" 2>/dev/null    # hand a clean, unmounted card back to muOS's own mount step
rm -f "$PHASE2_MARK"
touch "$DONE_MARK"
rm -rf "$BACKUP"
sync
exit 0

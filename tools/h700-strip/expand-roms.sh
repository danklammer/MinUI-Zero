#!/bin/sh
# One-shot ROMS expansion for the MinUI Zero h700 image.
#
# The flashed image is ~1.4GB, so on any larger card the space past the ROMS partition is simply
# unreachable — a 32GB card shows 256MB of ROM space (measured on Dan's: 58,130,429 free sectors,
# ~29GB, 2026-08-10).
#
# There is no in-place FAT32 grow available: this rootfs has parted/sfdisk/mkfs.vfat but NO
# fatresize and NO resize2fs. muOS solves the same problem in its own factory reset
# (/opt/muos/script/system/reset.sh) by resizing the PARTITION and then REFORMATTING — it can
# afford to, because a factory reset restores its payload from an archive afterwards. We do the
# same three steps and restore OUR payload from a copy we make first.
#
# This is the one genuinely destructive thing MinUI Zero does, so it is fenced:
#   * runs only when the DONE marker is absent AND real free space exists past ROMS
#   * runs only BEFORE minui starts (nothing may hold /mnt/mmc open — the frontend calls this
#     before its launch loop; the frontend itself lives on the rootfs)
#   * backs the whole card payload up to the rootfs and VERIFIES the copy before touching anything
#   * refuses if the payload would not fit in rootfs free space with headroom
#   * writes an ATTEMPT marker before the destructive step: if we ever boot and find an attempt
#     without a done, we stop and leave it to a human rather than retry a half-finished disk
# On any refusal it returns 0 and leaves the card exactly as it was — a small ROMS partition is a
# nuisance, a corrupted one is a reflash.
set -u

LOG="${LOG:-/mnt/mmc/minui-zero.log}"
DEV=/dev/mmcblk0
PART_NUM=6
PART="${DEV}p${PART_NUM}"
MOUNT=/mnt/mmc
STATE=/opt/minui-zero
DONE_MARK="$STATE/roms-expanded"
ATTEMPT_MARK="$STATE/roms-expanding"
BACKUP=/var/minui-zero-payload

say() { echo "expand: $*" >> "$LOG"; }

[ -f "$DONE_MARK" ] && exit 0
if [ -f "$ATTEMPT_MARK" ]; then
	say "a previous attempt did not finish — refusing to retry, card needs a look (or a reflash)"
	exit 0
fi

command -v parted >/dev/null 2>&1 || { say "no parted, skipping"; exit 0; }
command -v mkfs.vfat >/dev/null 2>&1 || { say "no mkfs.vfat, skipping"; exit 0; }
mountpoint -q "$MOUNT" 2>/dev/null || grep -q " $MOUNT " /proc/mounts || { say "$MOUNT not mounted, skipping"; exit 0; }

# --- is there anything to gain? (sectors past the end of ROMS) ---
DISK_SECTORS=$(cat /sys/block/mmcblk0/size 2>/dev/null || echo 0)
PART_START=$(cat /sys/block/mmcblk0/mmcblk0p6/start 2>/dev/null || echo 0)
PART_SIZE=$(cat /sys/block/mmcblk0/mmcblk0p6/size 2>/dev/null || echo 0)
[ "$DISK_SECTORS" -gt 0 ] && [ "$PART_SIZE" -gt 0 ] || { say "cannot read geometry, skipping"; exit 0; }
FREE=$(( DISK_SECTORS - PART_START - PART_SIZE ))
# less than ~1GB to reclaim is not worth a reformat
[ "$FREE" -gt 2097152 ] || { say "only ${FREE}s free past ROMS, nothing worth doing"; exit 0; }

# --- can the payload be parked safely? ---
PAYLOAD_KB=$(du -sk "$MOUNT" 2>/dev/null | cut -f1)
ROOT_FREE_KB=$(df -k / 2>/dev/null | awk 'NR==2 {print $4}')
[ -n "$PAYLOAD_KB" ] && [ -n "$ROOT_FREE_KB" ] || { say "cannot size payload/rootfs, skipping"; exit 0; }
# require the payload to fit twice over: the copy plus working room
if [ "$ROOT_FREE_KB" -lt $(( PAYLOAD_KB * 2 + 65536 )) ]; then
	say "payload ${PAYLOAD_KB}KB will not fit safely in ${ROOT_FREE_KB}KB of rootfs — skipping"
	exit 0
fi

say "expanding ROMS: ${FREE}s (~$(( FREE / 2048 ))MB) to reclaim; parking ${PAYLOAD_KB}KB"

# --- 1. back up + verify BEFORE anything destructive ---
rm -rf "$BACKUP"; mkdir -p "$BACKUP" || { say "cannot create backup dir"; exit 0; }
if ! cp -a "$MOUNT"/. "$BACKUP"/ 2>/dev/null; then
	say "backup copy failed — aborting, card untouched"
	rm -rf "$BACKUP"; exit 0
fi
SRC_FILES=$(find "$MOUNT" | wc -l)
BAK_FILES=$(find "$BACKUP" | wc -l)
if [ "$SRC_FILES" != "$BAK_FILES" ]; then
	say "backup verify failed ($SRC_FILES vs $BAK_FILES entries) — aborting, card untouched"
	rm -rf "$BACKUP"; exit 0
fi
sync
say "backup verified ($BAK_FILES entries)"

# --- 2. the destructive part, in muOS's own order (reset.sh) ---
touch "$ATTEMPT_MARK"; sync
cd /
umount "$MOUNT" 2>/dev/null || umount -l "$MOUNT" 2>/dev/null
if grep -q " $MOUNT " /proc/mounts; then
	say "could not unmount $MOUNT — aborting before any write; restoring nothing (card untouched)"
	rm -f "$ATTEMPT_MARK"; rm -rf "$BACKUP"; exit 0
fi

printf "w\nw\n" | fdisk "$DEV" >/dev/null 2>&1          # rewrite the table so parted sees the disk end
parted ---pretend-input-tty "$DEV" resizepart "$PART_NUM" 100% >/dev/null 2>&1
parted ---pretend-input-tty "$DEV" set "$PART_NUM" boot off >/dev/null 2>&1
parted ---pretend-input-tty "$DEV" set "$PART_NUM" hidden off >/dev/null 2>&1
parted ---pretend-input-tty "$DEV" set "$PART_NUM" msftdata on >/dev/null 2>&1
mkfs.vfat -F 32 -n ROMS "$PART" >/dev/null 2>&1
sync

# --- 3. remount + restore ---
if ! mount -t vfat -o rw,utf8,noatime,nofail "$PART" "$MOUNT" 2>/dev/null; then
	say "REMOUNT FAILED after reformat — payload is safe at $BACKUP, needs manual restore"
	exit 0
fi
if ! cp -a "$BACKUP"/. "$MOUNT"/ 2>/dev/null; then
	say "restore copy failed — payload still at $BACKUP, needs manual restore"
	exit 0
fi
NEW_FILES=$(find "$MOUNT" | wc -l)
sync
if [ "$NEW_FILES" != "$BAK_FILES" ]; then
	say "restore verify mismatch ($NEW_FILES vs $BAK_FILES) — payload kept at $BACKUP"
	exit 0
fi

rm -f "$ATTEMPT_MARK"
touch "$DONE_MARK"
rm -rf "$BACKUP"
sync
say "done — ROMS is now $(df -h "$MOUNT" 2>/dev/null | awk 'NR==2 {print $2}')"
exit 0

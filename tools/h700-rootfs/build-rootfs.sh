#!/bin/sh
# Build an h700 rootfs by ALLOWLIST from a donor, instead of by denylist from muOS.
#
# WHY THIS EXISTS. tools/build-h700-stripped.sh rdumps the muOS rootfs and deletes a named bloat
# list. That only removes what someone remembered to name: bluetoothd (1.4MB) and dbus-daemon are
# still in the shipping image today, on a device with no Bluetooth feature. This builds the other
# way round, from tools/h700-rootfs/harvest.list, so anything not enumerated is simply absent and
# a missing dependency fails the BUILD instead of the device. Approach borrowed from BaseOS
# (github.com/pvaibhav/BaseOS); the contents are ours, computed from our own binaries.
#
# Usage:
#   sh tools/h700-rootfs/build-rootfs.sh                 # normal build, GPU blob included
#   H700_NO_GPU=1 sh tools/h700-rootfs/build-rootfs.sh   # omit libmali + mali_kbase (~63MB -> ~20MB)
#
# Produces $OUT/p5-lean.img, a drop-in replacement for the p5 that build-h700-stripped.sh makes.
# It does NOT touch the boot chain: bootchain.py and gpt.py stay exactly as they are.
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)
ASSETS="${H700_ASSETS:-$REPO/.notes/2026-08-05-h700-image}"
OUT="${H700_OUT:-$ASSETS/out}"
LIST="$REPO/tools/h700-rootfs/harvest.list"
OVERLAY="$REPO/tools/h700-rootfs/overlay"
DONOR="${H700_DONOR:-$ASSETS/muos-p5.img}"
SIZE_KB="${H700_ROOTFS_KB:-262144}"   # 256MB slot; the payload should land near 63MB (or 20MB no-GPU)

for f in "$LIST" "$DONOR"; do
	[ -e "$f" ] || { echo "ERROR: missing required input: $f"; exit 1; }
done
[ -d "$OVERLAY" ] || { echo "ERROR: missing overlay dir: $OVERLAY"; exit 1; }
mkdir -p "$OUT"

# The allowlist is filtered HERE rather than inside the container, so the no-GPU variant is visible
# in the build log and cannot silently disagree with what got installed.
if [ -n "$H700_NO_GPU" ]; then
	echo "== H700_NO_GPU: omitting the GPU blob and its module =="
	grep -vE '^#|^[[:space:]]*$' "$LIST" \
		| grep -vE 'libmali|libEGL|libGLES|mali_kbase' > "$ASSETS/harvest.active"
else
	grep -vE '^#|^[[:space:]]*$' "$LIST" > "$ASSETS/harvest.active"
fi
echo "  allowlist entries: $(wc -l < "$ASSETS/harvest.active" | tr -d ' ')"

cp "$REPO/tools/h700-strip/expand-roms.sh" "$ASSETS/expand-roms.sh"
rm -rf "$ASSETS/overlay"; cp -R "$OVERLAY" "$ASSETS/overlay"

# NOTE: no apostrophes anywhere inside the docker block below. It is one single-quoted string and
# an apostrophe closes it early, which is a confusing failure a long way from its cause.
docker run --rm --platform linux/amd64 -v "$ASSETS:/a" -e SIZE_KB="$SIZE_KB" tg5040-toolchain /bin/bash -c '
set -e
D=/work/donor
R=/work/lean
rm -rf "$D" "$R"; mkdir -p "$D" "$R"

echo "  rdump donor (2.6GB, be patient)..."
debugfs -R "rdump / $D" /a/muos-p5.img 2>/dev/null

echo "  copying the allowlist..."
MISSING=0
while IFS= read -r p; do
	src="$D$p"
	if [ ! -e "$src" ]; then
		echo "    MISSING FROM DONOR: $p"
		MISSING=$((MISSING + 1))
		continue
	fi
	mkdir -p "$R$(dirname "$p")"
	# -a preserves the symlinks the loader depends on; -H follows a top-level symlinked dir so a
	# soname path becomes a real file rather than a dangling link into a tree we did not copy.
	cp -aH "$src" "$R$p" 2>/dev/null || cp -a "$src" "$R$p"
done < /a/harvest.active
if [ "$MISSING" -gt 0 ]; then
	echo "ERROR: $MISSING allowlist entries are absent from the donor; the list and the donor disagree"
	exit 1
fi

echo "  busybox applet symlinks..."
mkdir -p "$R/bin" "$R/sbin" "$R/usr/bin" "$R/usr/sbin" "$R/etc/init.d" "$R/proc" "$R/sys" "$R/dev" \
         "$R/tmp" "$R/run" "$R/mnt/mmc" "$R/root" "$R/var/log" "$R/opt/minui-zero"
# Only the applets our init and launcher actually invoke. busybox --install would spray hundreds.
for a in sh mount umount mkdir ln rm cp mv cat echo sleep usleep sync hostname insmod lsmod \
         killall pgrep pkill ps grep sed awk head tail cut tr basename dirname date df du \
         mountpoint poweroff reboot halt init udhcpc ip ifconfig route nc wc sort uniq find touch \
         chmod stat readlink env printf test true false uname; do
	ln -sf /bin/busybox "$R/bin/$a" 2>/dev/null || true
done
ln -sf /bin/busybox "$R/sbin/init"

echo "  overlay (wins over everything)..."
cp -a /a/overlay/. "$R/"
cp /a/expand-roms.sh "$R/opt/minui-zero/expand-roms.sh"
chmod +x "$R/init" "$R/etc/init.d/rcS" "$R/opt/minui-zero/expand-roms.sh"

# ld.so.cache is REGENERATED, never harvested: a donor cache describes libraries we did not copy.
echo "  ldconfig..."
printf "/lib\n/usr/lib\n" > "$R/etc/ld.so.conf"
ldconfig -r "$R" 2>/dev/null || echo "    (ldconfig unavailable for the target; cache will build at first boot)"

echo "  closure check: every retained ELF must resolve inside this rootfs..."
# Read into a list first: an unquoted $(find) word-splits, and one path with a space would report
# two bogus unresolved binaries and fail a good build (same bug fixed in check-payload.sh).
find "$R" -type f -perm -u+x > /tmp/elflist
BAD=0
SEEN=0
while IFS= read -r f; do
	head -c 4 "$f" | grep -q ELF 2>/dev/null || continue
	SEEN=$((SEEN + 1))
	for need in $(readelf -d "$f" 2>/dev/null | sed -nE "s/.*NEEDED.*\[(.+)\]/\1/p"); do
		if [ ! -e "$R/lib/$need" ] && [ ! -e "$R/usr/lib/$need" ]; then
			echo "    UNRESOLVED: $(echo "$f" | sed "s|$R||") needs $need"
			BAD=$((BAD + 1))
		fi
	done
done < /tmp/elflist
if [ "$BAD" -gt 0 ]; then
	echo "ERROR: $BAD unresolved library dependencies. Add them to harvest.list."
	exit 1
fi
# FAIL CLOSED. A closure check that inspected nothing and reported success is worse than no check;
# that is exactly how check-payload passed a release it never looked at (2026-08-15).
if [ "$SEEN" -lt 10 ]; then
	echo "ERROR: closure check only inspected $SEEN ELF files, which cannot be right (vacuous pass)"
	exit 1
fi
echo "    all $SEEN retained binaries resolve"

echo "  boot-critical scripts must be present AND executable..."
for c in /init /etc/init.d/rcS /etc/inittab /bin/busybox; do
	[ -e "$R$c" ] || { echo "ERROR: missing boot-critical file: $c"; exit 1; }
done
for c in /init /etc/init.d/rcS /bin/busybox; do
	[ -x "$R$c" ] || { echo "ERROR: not executable: $c (a guard would skip it and boot would die silently)"; exit 1; }
done
# /init must be a REGULAR FILE. The vendor initramfs switch_root fails on a symlink and says nothing.
[ -L "$R/init" ] && { echo "ERROR: /init is a symlink; switch_root will fail silently"; exit 1; }
echo "    ok"

echo "  size: $(du -sh $R | cut -f1)"

# 4.9-SAFE EXT4. mke2fs 1.47 defaults (metadata_csum, metadata_csum_seed, 64bit) are not mountable
# by this vendor kernel, and the journal is REQUIRED because the vendor initramfs mounts root
# data=ordered and the kernel rejects that on a journal-less fs. Both learned from BaseOS docs/00.
mke2fs -q -F -t ext4 \
	-O ^metadata_csum,^metadata_csum_seed,^64bit,has_journal \
	-d "$R" -L rootfs /a/out/p5-lean.img ${SIZE_KB}k
echo "  wrote p5-lean.img"
'

ls -l "$OUT/p5-lean.img" 2>/dev/null | awk '{printf "  %s  %.1f MB\n", $9, $5/1048576}'
echo "== done =="

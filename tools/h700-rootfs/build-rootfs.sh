#!/bin/sh
# Build an h700 rootfs by ALLOWLIST from a donor, instead of by denylist from muOS.
#
# WHY THIS EXISTS. tools/build-h700-stripped.sh rdumps the muOS rootfs and deletes a named bloat
# list. That only removes what someone remembered to name: bluetoothd (1.4MB) and dbus-daemon are
# still in the shipping image today, on a device with no Bluetooth feature. This builds the other
# way round, from tools/h700-rootfs/harvest.list, so anything not enumerated is simply absent and
# a missing dependency fails the BUILD instead of the device. The list is ours, computed from our
# own binaries.
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
# Artifact name carries the mode. Both builds used to write p5-lean.img, so building a release
# silently replaced the dev rootfs at the same path and the next image shipped with no ssh key
# (caught 2026-08-27 before it cost a third flash cycle).
MODE="${H700_MODE:-dev}"
OUTNAME="p5-lean.img"
[ "$MODE" = release ] && OUTNAME="p5-lean-release.img"

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

# DEV SSH KEY, same contract as build-h700-stripped.sh. A dev image bakes in the builder public key
# so a freshly flashed card is reachable immediately; a release image ships none and the launcher
# installs whatever key the user drops at the card root instead.
#
# This exists because H700_P5 skips the branch of build-h700-stripped.sh that used to do it, so an
# allowlist image had no key, dropbear therefore refused to start (by design, it is key-auth only),
# and every flash produced a device that ran perfectly and could not be reached (2026-08-27). Two
# wasted flash cycles before the cause was obvious.
rm -f "$ASSETS/authorized_keys"
if [ "$MODE" = dev ]; then
	cp "$HOME/.ssh/tg5040_dev.pub" "$ASSETS/authorized_keys" 2>/dev/null || true
	[ -s "$ASSETS/authorized_keys" ] && echo "  dev ssh key staged" || echo "  WARNING: no dev key at ~/.ssh/tg5040_dev.pub; image will be unreachable"
fi
# Collect the binaries that will live on the CARD so the container can check them against the
# rootfs it is about to build (see the card-payload closure check).
rm -rf "$ASSETS/cardbins"; mkdir -p "$ASSETS/cardbins"
for _b in "$REPO/workspace/all/minui/build/h700/minui.elf" \
          "$REPO/workspace/all/minarch/build/h700/minarch.elf" \
          "$REPO/workspace/h700/libmsettings/libmsettings.so" \
          "$REPO/skeleton/SYSTEM/tg5040/bin/dropbearmulti" \
          "$REPO"/workspace/tg5040/cores/output/*_libretro.so; do
	[ -f "$_b" ] && cp "$_b" "$ASSETS/cardbins/" 2>/dev/null
done
echo "  card binaries staged for the closure check: $(ls "$ASSETS/cardbins" | wc -l | tr -d ' ')"
rm -rf "$ASSETS/overlay"; cp -R "$OVERLAY" "$ASSETS/overlay"

# NOTE: no apostrophes anywhere inside the docker block below. It is one single-quoted string and
# an apostrophe closes it early, which is a confusing failure a long way from its cause.
docker run --rm --platform linux/amd64 -v "$ASSETS:/a" -e SIZE_KB="$SIZE_KB" -e H700_MODE_IN="$MODE" -e OUTNAME="$OUTNAME" tg5040-toolchain /bin/bash -c '
set -e
D=/work/donor
R=/work/lean
rm -rf "$D" "$R"; mkdir -p "$D" "$R"

# CACHE THE RDUMP, AS A TAR. Extracting the 8GB donor takes minutes and its contents never change,
# so it is done once and reused; iterating on the allowlist is the actual work and it must cost
# seconds, not minutes.
#
# The cache is a TAR, not a directory tree, and that is deliberate. The first version rdumped
# straight into the bind-mounted /a and SILENTLY LOST the real 44MB /usr/lib/libmali.so, leaving
# only its dangling .so.0/.so.1 symlinks, so the next build reported seven allowlist entries as
# "missing from the donor" that were present all along. A single tar written inside the container
# and only then moved onto the shared volume avoids per-file behaviour differences entirely.
# Errors from debugfs are NO LONGER discarded: hiding them is what made the loss silent.
if [ -f /a/donor-cache.tar ]; then
	echo "  restoring cached donor extract..."
	mkdir -p "$D"
	tar -xf /a/donor-cache.tar -C "$D"
else
	echo "  rdump donor (2.6GB, be patient; cached for next time)..."
	debugfs -R "rdump / $D" /a/muos-p5.img 2> /tmp/rdump.err || true
	if [ -s /tmp/rdump.err ]; then
		echo "    debugfs reported:"; head -5 /tmp/rdump.err | sed "s/^/      /"
	fi
	echo "  caching the extract..."
	tar -cf /tmp/donor-cache.tar -C "$D" . && mv /tmp/donor-cache.tar /a/donor-cache.tar
fi

# VALIDATE THE DONOR before trusting it. A truncated or partial extract must fail here, loudly,
# rather than turn into a rootfs that is missing files nobody notices until the device will not boot.
_want_big=$(find "$D/usr/lib" -maxdepth 1 -type f -size +30M 2>/dev/null | wc -l)
_files=$(find "$D" -type f 2>/dev/null | wc -l)
echo "  donor: $_files files, $_want_big large libs in /usr/lib"
if [ "$_files" -lt 10000 ]; then
	echo "ERROR: donor extract has only $_files files, which cannot be a full rootfs."
	echo "       Delete /a/donor-cache.tar and re-run to re-extract."
	exit 1
fi

echo "  copying the allowlist..."
# SYMLINKS ARE KEPT AS SYMLINKS, AND THEIR TARGETS COME ALONG.
# Every soname in this donor is a link to a versioned file (libz.so.1 -> libz.so.1.3.1), and
# libmali.so has six aliases (libEGL.so, libGLESv2.so, ...) all pointing at the same 42.5MB blob.
# cp -aH dereferences, which fixed the sonames but wrote SEVEN copies of libmali (297MB, overflowed
# the partition). Plain cp -a fixed that and left 20 sonames dangling. So: copy the link verbatim,
# then follow it and copy what it points at, recursively. One real file, links beside it, nothing
# dangling, no duplication.
MISSING=0
copy_one() {
	_p="$1"; _depth="${2:-0}"
	[ "$_depth" -gt 8 ] && return 0          # symlink loop guard
	_src="$D$_p"
	[ -e "$_src" ] || [ -L "$_src" ] || { return 1; }
	[ -e "$R$_p" ] || [ -L "$R$_p" ] && [ "$_depth" -gt 0 ] && return 0
	mkdir -p "$R$(dirname "$_p")"
	cp -a "$_src" "$R$_p" 2>/dev/null || true
	if [ -L "$_src" ]; then
		_t=$(readlink "$_src")
		case "$_t" in
			/*) _tp="$_t" ;;
			*)  _tp="$(dirname "$_p")/$_t" ;;
		esac
		copy_one "$_tp" $((_depth + 1))
	fi
	return 0
}
while IFS= read -r p; do
	if ! copy_one "$p" 0; then
		echo "    MISSING FROM DONOR: $p"
		MISSING=$((MISSING + 1))
	fi
done < /a/harvest.active
if [ "$MISSING" -gt 0 ]; then
	echo "ERROR: $MISSING allowlist entries are absent from the donor; the list and the donor disagree"
	exit 1
fi

# NOTHING MAY DANGLE. Preserving symlinks is only safe if the allowlist also carries their targets;
# a dangling libEGL.so is a third-party pak that dies at dlopen with a confusing message.
echo "  dangling symlink check..."
DANGLE=0
find "$R" -type l > /tmp/links
while IFS= read -r l; do
	[ -e "$l" ] || { echo "    DANGLING: $(echo "$l" | sed "s|$R||") -> $(readlink "$l")"; DANGLE=$((DANGLE + 1)); }
done < /tmp/links
if [ "$DANGLE" -gt 0 ]; then
	echo "ERROR: $DANGLE dangling symlinks. Add their targets to harvest.list."
	exit 1
fi
echo "    no dangling symlinks"

echo "  busybox applet symlinks..."
mkdir -p "$R/bin" "$R/sbin" "$R/usr/bin" "$R/usr/sbin" "$R/etc/init.d" "$R/proc" "$R/sys" "$R/dev" \
         "$R/tmp" "$R/run" "$R/mnt/mmc" "$R/root" "$R/var/log" "$R/opt/minui-zero"
# Only the applets our init and launcher actually invoke. busybox --install would spray hundreds.
for a in sh mount umount mkdir ln rm cp mv cat echo sleep usleep sync hostname insmod lsmod \
         killall pgrep pkill ps grep sed awk head tail cut tr basename dirname date df du \
         mountpoint poweroff reboot halt init udhcpc ip ifconfig route nc wc sort uniq find touch \
         chmod stat readlink env printf test true false uname \
         fdisk mkfs.vfat partprobe blockdev dd \
         ls md5sum tee xargs seq expr kill more vi; do
	ln -sf /bin/busybox "$R/bin/$a" 2>/dev/null || true
done
ln -sf /bin/busybox "$R/sbin/init"

# /var/run -> /run and /var/lock -> /run/lock. wpa_supplicant opens its control socket under
# /var/run/wpa_supplicant and simply fails without the path; the donor had these and the allowlist
# did not carry them, because a denylist inherits the whole directory skeleton for free. Found on
# the first boot of this rootfs (2026-08-27): device up, wifi dead.
mkdir -p "$R/var" "$R/run/lock"
ln -sfn /run "$R/var/run"
ln -sfn /run/lock "$R/var/lock"
# resolv.conf is written at DHCP time by usr/share/udhcpc/default.script, but the file has to exist
# and be writable first, and /etc is on the read-write rootfs so a plain empty file is enough.
: > "$R/etc/resolv.conf"
chmod 644 "$R/etc/resolv.conf"

# STRIP KERNEL MODULE DEBUG SYMBOLS. The donor ships them unstripped: mali_kbase.ko alone is
# 17,586,616 bytes and becomes 720,624 (the size the running device actually has). The existing
# denylist build has always done this (build-h700-stripped.sh:262); this builder did not, which is
# the entire reason /lib/modules measured 20.5MB here against ~4MB on-device. Same tool, same flag.
KOSTRIP=$(command -v aarch64-linux-gnu-strip 2>/dev/null || echo /opt/aarch64-linux-gnu/bin/aarch64-linux-gnu-strip)
if [ -x "$KOSTRIP" ] || command -v "$KOSTRIP" >/dev/null 2>&1; then
	_before=$(du -sk "$R/lib/modules" 2>/dev/null | cut -f1)
	find "$R/lib/modules" -name "*.ko" -exec "$KOSTRIP" --strip-debug {} + 2>/dev/null || true
	_after=$(du -sk "$R/lib/modules" 2>/dev/null | cut -f1)
	echo "  kernel modules: ${_before}k -> ${_after}k after debug-strip"
	# A strip that silently did nothing would quietly cost 17MB, so say so rather than assume.
	if [ "${_after:-0}" -ge "${_before:-0}" ]; then
		echo "  WARNING: strip had no effect; modules are still carrying debug symbols"
	fi
else
	echo "  WARNING: no aarch64 strip found; modules keep their debug symbols (+17MB)"
fi

# The donor alsa.conf.d carries pipewire configs (50-pipewire.conf, 99-pipewire-default.conf)
# that redefine "default" to a plugin we do not ship. The overlay already replaces /etc/asound.conf;
# these must go too or the conf.d definitions win the merge and audio dies exactly as before.
rm -f "$R/usr/share/alsa/alsa.conf.d/50-pipewire.conf" \
      "$R/usr/share/alsa/alsa.conf.d/99-pipewire-default.conf"
echo "  pipewire alsa configs removed"

echo "  ssh key..."
mkdir -p "$R/root/.ssh"
chmod 700 "$R/root/.ssh"
if [ -s /a/authorized_keys ]; then
	cp /a/authorized_keys "$R/root/.ssh/authorized_keys"
	chmod 600 "$R/root/.ssh/authorized_keys"
	echo "    dev key installed (image is reachable over ssh)"
else
	echo "    no key baked in (release, or none staged)"
fi

echo "  overlay (wins over everything)..."
cp -a /a/overlay/. "$R/"
cp /a/expand-roms.sh "$R/opt/minui-zero/expand-roms.sh"
chmod +x "$R/init" "$R/etc/init.d/rcS" "$R/opt/minui-zero/expand-roms.sh"

# ld.so.cache: use the DONOR cache, never a host-generated one, and never none.
# Three states were tried and two are wrong (both found on-device 2026-08-27):
#   - amd64 ldconfig output: unreadable by the aarch64 loader, and WORSE than nothing, because with
#     a bad cache present the loader failed to find even libm.
#   - no cache at all: this Buildroot glibc 2.38 does not fall back to /usr/lib without one, so
#     bare binaries broke while the launcher survived only via its explicit LD_LIBRARY_PATH.
#   - the donor own cache: built on-device by the vendor, aarch64-native, and it maps exactly the
#     paths we preserve. Entries for libraries we deleted are harmless: nothing we ship links them.
echo "  ld.so.cache: harvesting the donor native cache..."
printf "/lib\n/usr/lib\n" > "$R/etc/ld.so.conf"
cp "$D/etc/ld.so.cache" "$R/etc/ld.so.cache"
[ -s "$R/etc/ld.so.cache" ] || { echo "ERROR: donor ld.so.cache missing or empty"; exit 1; }

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

# THE CARD IS PART OF THE CLOSURE. The check above scans the ROOTFS, but the payload on the FAT
# partition carries executables of its own (minui.elf, minarch.elf, the libretro cores,
# dropbearmulti) and the rootfs is what has to satisfy them. Missing that cost a boot: dropbearmulti
# needs libutil and libcrypt, neither was harvested, so ssh never started and the device came up on
# wifi with no way in (2026-08-27). A rootfs is not "complete" on its own terms; it is complete
# relative to what will run against it.
echo "  card-payload closure check..."
CBAD=0
CSEEN=0
for f in /a/cardbins/*; do
	[ -f "$f" ] || continue
	head -c 4 "$f" | grep -q ELF 2>/dev/null || continue
	CSEEN=$((CSEEN + 1))
	for need in $(readelf -d "$f" 2>/dev/null | sed -nE "s/.*NEEDED.*\[(.+)\]/\1/p"); do
		# libmsettings ships on the card beside these binaries, not in the rootfs.
		[ "$need" = "libmsettings.so" ] && continue
		if [ ! -e "$R/lib/$need" ] && [ ! -e "$R/usr/lib/$need" ]; then
			echo "    UNRESOLVED: $(basename "$f") needs $need"
			CBAD=$((CBAD + 1))
		fi
	done
done
if [ "$CBAD" -gt 0 ]; then
	echo "ERROR: $CBAD card-side dependencies are absent from the rootfs. Add them to harvest.list."
	exit 1
fi
if [ "$CSEEN" -lt 3 ]; then
	echo "ERROR: card-payload check only inspected $CSEEN binaries (vacuous pass)"
	exit 1
fi
echo "    all $CSEEN card binaries resolve against this rootfs"

if [ "$H700_MODE_IN" = release ] && [ -e "$R/root/.ssh/authorized_keys" ]; then
	echo "ERROR: release rootfs carries an authorized_keys; that would authorize one developer key"
	echo "       on every user device. Refusing to build."
	exit 1
fi

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
echo "  biggest contributors:"
du -a "$R" 2>/dev/null | sort -rn | head -12 | awk -v r="$R" "{ sub(r, \"\", \$2); printf \"    %8.1f MB  %s\\n\", \$1/1024, \$2 }"

# 4.9-SAFE EXT4. mke2fs 1.47 defaults (metadata_csum, metadata_csum_seed, 64bit) are not mountable
# by this vendor kernel, and the journal is REQUIRED because the vendor initramfs mounts root
# data=ordered and the kernel rejects that on a journal-less fs. Both are hard requirements of this
# boot chain, discovered the expensive way.
mke2fs -q -F -t ext4 \
	-O ^metadata_csum,^metadata_csum_seed,^64bit,has_journal \
	-d "$R" -L rootfs "/a/out/$OUTNAME" ${SIZE_KB}k
echo "  wrote $OUTNAME"
'

ls -l "$OUT/p5-lean.img" 2>/dev/null | awk '{printf "  %s  %.1f MB\n", $9, $5/1048576}'
echo "== done =="

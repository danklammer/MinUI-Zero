#!/bin/sh
# Build the MinUI Zero h700 SD image (Anbernic RG35XX Plus/H, image-native boot).
#
# Boot chain (kept VERBATIM from the muOS card — same kernel every ABI receipt was probed
# against): raw preamble (boot0@8KB + U-Boot) | p1 spare | p2 boot-resource (logos) | p3 env |
# p4 ANDROID! bootimg (kernel 4.9.170 + ramdisk). The ramdisk mounts p5 and
# `exec switch_root /mnt /init` — our whole OS is tools/h700-image/init on p5.
# GPT: 8-entry Allwinner layout patched by tools/h700-image/gpt.py (see its header comment).
#
# v1 DECISIONS (documented in .notes/2026-08-05-h700-image/PLAN.md):
#   * p6 (roms) is EXT4 — the toolchain container has no FAT tooling; a PC cannot read it yet.
#     Fine for the boot test; FAT/exFAT + PC-visible roms is v2.
#   * wifi ships dormant (modules + wpa_supplicant present, not started). dropbear starts on
#     boot so ssh works the moment networking exists.
#   * volume buttons are wpctl-based (guest era) — inert on this image until the direct-ALSA
#     mixer lands. Brightness (dispdbg) works.
#
# Needs: docker (tg5040-toolchain image), the extracted assets dir (from the muOS card), and
# built h700 binaries (make h700-build).
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
ASSETS="${H700_ASSETS:-$REPO/.notes/2026-08-05-h700-image}"
OUT_DIR="$ASSETS/out"
STAGE="$OUT_DIR/stage"
IMG="$OUT_DIR/MinUI-Zero-h700-$(date +%Y%m%d).img"

# image geometry (sectors). p1-p4 = EXACT muOS offsets (U-Boot may address by sector) — those
# first 156MB are the fixed cost of the verbatim boot chain. p5/p6 are sized to CONTENT
# (rootfs ~17MB after module pruning, payload ~22MB): ~476MB total, most of it the boot chain.
# Growing p6 to fill the real card is the v2 first-boot expansion job.
P5_FIRST=319488
P5_SECTORS=131072       # 64MB rootfs partition
P5_LAST=$((P5_FIRST + P5_SECTORS - 1))
P6_FIRST=$((P5_LAST + 1))
P6_SECTORS=262144       # 128MB roms partition (boot-test size; first-boot expansion is v2)
P6_LAST=$((P6_FIRST + P6_SECTORS - 1))
TOTAL=$((P6_LAST + 40)) # + backup GPT tail

[ -f "$ASSETS/parts/raw-36mb.img.gz" ] || { echo "ERROR: assets not found at $ASSETS"; exit 1; }
[ -f "$REPO/workspace/all/minui/build/h700/minui.elf" ] || { echo "ERROR: run 'make h700-build' first"; exit 1; }

rm -rf "$STAGE"
mkdir -p "$OUT_DIR" "$STAGE/rootfs" "$STAGE/card"

echo "== rootfs (p5) =="
R="$STAGE/rootfs"
mkdir -p "$R/bin" "$R/sbin" "$R/dev" "$R/proc" "$R/sys" "$R/tmp" "$R/run" "$R/mnt/mmc" \
         "$R/etc/dropbear" "$R/usr/sbin" "$R/sys/kernel/debug" "$R/dev/pts"
tar xzf "$ASSETS/userland-closure.tar.gz" -C "$R"        # /lib /usr/lib closure + /bin/busybox
tar xzf "$ASSETS/modules-4.9.170.tar.gz" -C "$R"          # /lib/modules
# prune to the ONE module we can ever need: wifi (8821cs). Display/ION/audio are built-in
# (verified: lsmod on muOS shows only 8821cs + mali, and we do not use the GPU). 30MB -> ~3MB.
find "$R/lib/modules/4.9.170/kernel" -name '*.ko' ! -name '8821cs.ko' -delete
find "$R/lib/modules/4.9.170/kernel" -type d -empty -delete 2>/dev/null || true
tar xzf "$ASSETS/alsa-wifi-ssh.tar.gz" -C "$R"            # /usr/share/alsa + wpa_supplicant + sshd
tar xzf "$ASSETS/wifi-libs.tar.gz" -C "$R"                # ssl/crypto for wpa_supplicant
# the libnl trio in wifi-libs are DANGLING SYMLINKS (pre-dereference-lesson extraction);
# libnl-real.tar.gz carries the actual files pulled from the muOS rootfs dump — extract AFTER
# so real files replace the husks
rm -f "$R/usr/lib/libnl-3.so.200" "$R/usr/lib/libnl-genl-3.so.200" "$R/usr/lib/libnl-route-3.so.200"
tar xzf "$ASSETS/libnl-real.tar.gz" -C "$R/usr/lib"
cp "$REPO/skeleton/SYSTEM/tg5040/bin/dropbearmulti" "$R/usr/sbin/dropbearmulti"
cp "$REPO/tools/h700-image/init" "$R/init"
# wifi tools from the muOS rootfs dump (dhcpcd/iw/iwconfig — busybox has none of these)
tar xzf "$ASSETS/wifi-tools.tar.gz" -C "$R/sbin"
chmod +x "$R/sbin/dhcpcd" "$R/sbin/iw" "$R/sbin/iwconfig" 2>/dev/null || true
ln -sf ../sbin/iw "$R/usr/sbin/iw" 2>/dev/null || true
# WiFi credentials: OPTIONAL, never in git — drop a wpa_supplicant.conf in the assets dir
if [ -f "$ASSETS/wifi.conf" ]; then
	cp "$ASSETS/wifi.conf" "$R/etc/wpa_supplicant.conf"
	echo "   wifi config aboard"
fi
chmod +x "$R/init" "$R/usr/sbin/dropbearmulti" "$R/bin/busybox" 2>/dev/null || true
ln -sf busybox "$R/bin/sh"
echo "root::0:0:root:/:/bin/sh" > "$R/etc/passwd"   # root home = /
# ssh access: the dev key (same one every device uses); dropbear reads /.ssh for root home /
mkdir -p "$R/.ssh"
cp "$HOME/.ssh/tg5040_dev.pub" "$R/.ssh/authorized_keys" 2>/dev/null || true
chmod 700 "$R/.ssh"; chmod 600 "$R/.ssh/authorized_keys" 2>/dev/null || true

echo "== card payload (p6) =="
C="$STAGE/card"
mkdir -p "$C/.system/h700/bin" "$C/.system/h700/lib" "$C/.system/h700/cores" \
         "$C/.system/res" "$C/.userdata/h700/logs" "$C/.userdata/shared/.minui" \
         "$C/Saves" "$C/Bios" "$C/Roms/Game Boy Color (GBC)"
cp "$REPO/workspace/all/minui/build/h700/minui.elf"     "$C/.system/h700/bin/"
cp "$REPO/workspace/all/minarch/build/h700/minarch.elf" "$C/.system/h700/bin/"
cp "$REPO/workspace/h700/libmsettings/libmsettings.so"  "$C/.system/h700/lib/"
cp "$REPO"/workspace/tg5040/cores/output/*.so           "$C/.system/h700/cores/"
cp "$REPO"/skeleton/SYSTEM/res/*                        "$C/.system/res/"
cp -R "$REPO"/skeleton/SYSTEM/h700/paks                 "$C/.system/h700/paks"
if [ -n "$H700_TEST_ROM" ] && [ -f "$H700_TEST_ROM" ]; then
	cp "$H700_TEST_ROM" "$C/Roms/Game Boy Color (GBC)/"
fi

echo "== filesystems (container: mke2fs -d, no loop mounts) =="
# stage trees are TARRED into the container and unpacked onto its native fs first: mke2fs -d
# reading the virtiofs bind mount SEGFAULTS on some files (bisected 2026-08-05; the same trees
# build fine from /tmp). Bonus: tar also strips macOS .DS_Store/._* detritus here.
# COPYFILE_DISABLE stops macOS bsdtar EMITTING AppleDouble ._* entries for xattrs — they are
# generated during archiving, so --exclude alone cannot catch them (first build shipped ._init)
COPYFILE_DISABLE=1 tar c -C "$STAGE/rootfs" --exclude '.DS_Store' --exclude '._*' . > "$OUT_DIR/rootfs.tar"
COPYFILE_DISABLE=1 tar c -C "$STAGE/card"   --exclude '.DS_Store' --exclude '._*' . > "$OUT_DIR/card.tar"
docker run --rm --platform linux/amd64 -v "$ASSETS:/a" tg5040-toolchain /bin/bash -c "
	set -e
	mkdir -p /tmp/rootfs /tmp/card
	tar x -C /tmp/rootfs -f /a/out/rootfs.tar
	tar x -C /tmp/card   -f /a/out/card.tar
	mke2fs -q -F -t ext4 -d /tmp/rootfs -L rootfs /a/out/p5.img $((P5_SECTORS / 2))k
	mke2fs -q -F -t ext4 -d /tmp/card   -L roms   /a/out/p6.img $(( (P6_LAST - P6_FIRST + 1) / 2 ))k
"
rm -f "$OUT_DIR/rootfs.tar" "$OUT_DIR/card.tar"

echo "== assemble =="
rm -f "$IMG"
gunzip -c "$ASSETS/parts/raw-36mb.img.gz" > "$IMG"                       # boot0+uboot+GPT verbatim
gunzip -c "$ASSETS/parts/p2-boot.img.gz"   | dd of="$IMG" bs=512 seek=90112  conv=notrunc status=none
gunzip -c "$ASSETS/parts/p3-env.img.gz"    | dd of="$IMG" bs=512 seek=155648 conv=notrunc status=none
# p4: write only the TRUE Android bootimg length — the rest of the 64MB partition on the
# donor card is old flash residue (incompressible, +40MB of download for nothing)
BOOTIMG_BYTES=$(gunzip -c "$ASSETS/parts/p4-kernel.img.gz" | head -c 48 | python3 -c '
import sys, struct
d = sys.stdin.buffer.read(48)
k, _, r = struct.unpack("<III", d[8:20])
pg = struct.unpack("<I", d[36:40])[0]
print(pg * (1 + (k + pg - 1)//pg + (r + pg - 1)//pg))
')
gunzip -c "$ASSETS/parts/p4-kernel.img.gz" | head -c "$BOOTIMG_BYTES" | dd of="$IMG" bs=512 seek=188416 conv=notrunc status=none
dd if="$ASSETS/out/p5.img" of="$IMG" bs=512 seek=$P5_FIRST conv=notrunc status=none
dd if="$ASSETS/out/p6.img" of="$IMG" bs=512 seek=$P6_FIRST conv=notrunc status=none
python3 "$REPO/tools/h700-image/gpt.py" "$IMG" $TOTAL $P5_LAST $P6_FIRST $P6_LAST
rm -f "$ASSETS/out/p5.img" "$ASSETS/out/p6.img"

xz -9 -T0 -f -k "$IMG"

echo ""
echo "IMAGE: $IMG ($(du -h "$IMG" | cut -f1) on disk)  DOWNLOAD: $IMG.xz ($(du -h "$IMG.xz" | cut -f1))"
echo "Flash (macOS): diskutil unmountDisk /dev/diskN && sudo dd if=$IMG of=/dev/rdiskN bs=4m"
echo "NEVER flash the muOS card."

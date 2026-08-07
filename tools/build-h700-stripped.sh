#!/bin/sh
# Build MinUI Zero h700 by STRIPPING muOS (Dan's call, 2026-08-06: "why reinvent the wheel").
#
# Base = muOS's proven rootfs (from the local dump): working wifi, ssh, udev module autoload,
# display init, power, every library, the correct kernel + modules. We DELETE the frontend/apps/
# theme bloat and swap the frontend-launch for MinUI. Everything below the UI stays muOS's.
#
# This replaces the from-scratch approach (tools/build-h700-image.sh), which failed by rebuilding
# a whole userland one missing library at a time. See .notes/2026-08-06-h700-architecture/.
#
# Pipeline (all offline, no device): debugfs rdump the muOS rootfs -> strip the bloat list ->
# swap FRONTEND->minui in startup.sh + drop our launcher -> mke2fs -d a lean p5 -> assemble with
# the verbatim boot chain + our FAT ROMS payload.
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
ASSETS="${H700_ASSETS:-$REPO/.notes/2026-08-05-h700-image}"
OUT_DIR="$ASSETS/out"
MUOS_ROOTFS="$ASSETS/muos-p5.img"
IMG="$OUT_DIR/MinUI-Zero-h700-stripped-$(date +%Y%m%d).img"
MT=/opt/homebrew/bin

# geometry (sectors) — p1-p4 verbatim muOS offsets; p5 sized to the STRIPPED rootfs
P5_FIRST=319488
P5_SECTORS=2097152      # 1GB rootfs (stripped muOS lands ~500-900MB; headroom for now)
P5_LAST=$((P5_FIRST + P5_SECTORS - 1))
P6_FIRST=$((P5_LAST + 1))
P6_SECTORS=524288       # 256MB ROMS (FAT32)
P6_LAST=$((P6_FIRST + P6_SECTORS - 1))
TOTAL=$((P6_LAST + 40))

[ -f "$MUOS_ROOTFS" ] || { echo "ERROR: muOS rootfs dump not at $MUOS_ROOTFS"; exit 1; }
[ -f "$REPO/workspace/all/minui/build/h700/minui.elf" ] || { echo "ERROR: run 'make h700-build' first"; exit 1; }

mkdir -p "$OUT_DIR"

# ---- p5: extract muOS rootfs, strip, inject our launcher, rebuild (all in the container) ----
echo "== extracting + stripping muOS rootfs (this takes a few minutes) =="
cp "$REPO/tools/h700-strip/minui-frontend.sh" "$ASSETS/minui-frontend.sh"
docker run --rm --platform linux/amd64 -v "$ASSETS:/a" -e P5_KB=$((P5_SECTORS / 2)) tg5040-toolchain /bin/bash -c '
set -e
R=/work/root
rm -rf "$R"; mkdir -p "$R"
echo "  rdump (2.6GB, be patient)..."
debugfs -R "rdump / $R" /a/muos-p5.img 2>/dev/null

echo "  stripping bloat..."
# frontend UI + its 1.2GB of theme/asset data (we replace the UI with minui)
rm -rf "$R/opt/muos/share" "$R/opt/muos/frontend" "$R/opt/muos/bin" "$R/opt/muos/kiosk"
# heavyweight extras nothing in our boot needs
rm -rf "$R/opt/java" "$R/opt/zulu" "$R/opt/sftpgo" "$R/opt/fish" "$R/opt/micro"
rm -f  "$R/usr/bin/retroarch" "$R/usr/lib/libavcodec."* "$R/usr/lib/libavformat."* "$R/usr/lib/libavfilter."* "$R/usr/lib/libavdevice."*
rm -rf "$R/usr/lib/python3.11" "$R/usr/bin/python3."* "$R/usr/lib/libpython3."*   # muOS scripts are POSIX sh, not python
# KEEP: /opt/muos/script (init + network.sh + halt.sh + func.sh), /opt/muos/device + config,
#       udev, wifi stack, openssh, SDL, kernel modules, libmali (SDL may dlopen it)

echo "  swapping FRONTEND -> minui in startup.sh..."
SU="$R/opt/muos/script/system/startup.sh"
# replace the frontend launch with our loop; skip the hotkey daemon (minui reads input directly)
sed -i "s|^FRONTEND start|sh /opt/minui-zero/minui-frontend.sh \&|" "$SU"
sed -i "s|^HOTKEY start|true # hotkey daemon disabled (minui owns input)|" "$SU"
mkdir -p "$R/opt/minui-zero"
cp /a/minui-frontend.sh "$R/opt/minui-zero/minui-frontend.sh"
chmod +x "$R/opt/minui-zero/minui-frontend.sh"
# NO wifi credentials baked into the image (privacy): the frontend reads a USER-SUPPLIED
# wifi.txt ("SSID:password") from the SD-card root at boot, MinUI-style. Ship a commented example.
# FIX the ssh host-key perms: debugfs rdump + mke2fs -d reset them to 0755, and sshd refuses
# world-readable private keys ("no hostkeys available -- exiting"). 600 = sshd starts on boot.
chmod 700 "$R/opt/openssh" "$R/opt/openssh/etc" 2>/dev/null || true
chmod 600 "$R/opt/openssh/etc/ssh_host_"*_key 2>/dev/null || true
chmod 644 "$R/opt/openssh/etc/ssh_host_"*_key.pub 2>/dev/null || true
# ssh key for our access (muOS keeps its openssh sshd; drop our authorized_keys in root)
mkdir -p "$R/root/.ssh"
cp /a/authorized_keys "$R/root/.ssh/authorized_keys" 2>/dev/null || true
chmod 700 "$R/root/.ssh" 2>/dev/null || true

echo "  stripped rootfs size: $(du -sh $R | cut -f1)"
echo "  building lean p5 (${P5_KB}k)..."
mke2fs -q -F -t ext4 -d "$R" -L rootfs /a/out/p5.img ${P5_KB}k
'

# ---- p6: FAT32 ROMS + our minui payload (mtools, no mount) ----
echo "== building FAT ROMS payload (p6) =="
STAGE="$OUT_DIR/card"; rm -rf "$STAGE"
mkdir -p "$STAGE/.system/h700/bin" "$STAGE/.system/h700/lib" "$STAGE/.system/h700/cores" \
         "$STAGE/.system/res" "$STAGE/.userdata/h700/logs" "$STAGE/.userdata/shared/.minui" \
         "$STAGE/Saves" "$STAGE/Bios" "$STAGE/Roms/Game Boy Color (GBC)"
cp "$REPO/workspace/all/minui/build/h700/minui.elf"     "$STAGE/.system/h700/bin/"
cp "$REPO/workspace/all/minarch/build/h700/minarch.elf" "$STAGE/.system/h700/bin/"
cp "$REPO/workspace/h700/libmsettings/libmsettings.so"  "$STAGE/.system/h700/lib/"
cp "$REPO"/workspace/tg5040/cores/output/*.so           "$STAGE/.system/h700/cores/"
cp "$REPO"/skeleton/SYSTEM/res/*                        "$STAGE/.system/res/"
cp -R "$REPO"/skeleton/SYSTEM/h700/paks                 "$STAGE/.system/h700/paks"
# version.txt (minui about screen reads .system/version.txt as "release" + "commit"; a missing
# file crashed minui on a home-screen MENU tap before the code guard, 2026-08-06)
printf 'MinUI Zero (%s)\n%s\n' "$(date +%Y%m%d)" "$(cd "$REPO" && git rev-parse --short HEAD)" > "$STAGE/.system/version.txt"
[ -n "$H700_TEST_ROM" ] && [ -f "$H700_TEST_ROM" ] && cp "$H700_TEST_ROM" "$STAGE/Roms/Game Boy Color (GBC)/"
# example wifi.txt at the card root (commented out; user adds their own "SSID:password")
printf '# WiFi: one network per line as SSID:password (# comments ignored). Example:\n# MyNetwork:mypassword\n' > "$STAGE/wifi.txt.example"

cp "$HOME/.ssh/tg5040_dev.pub" "$ASSETS/authorized_keys" 2>/dev/null || true
rm -f "$OUT_DIR/p6.img"
dd if=/dev/zero of="$OUT_DIR/p6.img" bs=512 count=$P6_SECTORS status=none
"$MT/mformat" -i "$OUT_DIR/p6.img" -F -v ROMS ::
for entry in "$STAGE"/* "$STAGE"/.[!.]*; do
	[ -e "$entry" ] || continue
	COPYFILE_DISABLE=1 "$MT/mcopy" -i "$OUT_DIR/p6.img" -s "$entry" ::
done

# ---- assemble ----
echo "== assembling image =="
rm -f "$IMG"
gunzip -c "$ASSETS/parts/raw-36mb.img.gz" > "$IMG"
gunzip -c "$ASSETS/parts/p2-boot.img.gz"   | dd of="$IMG" bs=512 seek=90112  conv=notrunc status=none
gunzip -c "$ASSETS/parts/p3-env.img.gz"    | dd of="$IMG" bs=512 seek=155648 conv=notrunc status=none
BOOTIMG_BYTES=$(gunzip -c "$ASSETS/parts/p4-kernel.img.gz" | head -c 48 | python3 -c 'import sys,struct;d=sys.stdin.buffer.read(48);k,_,r=struct.unpack("<III",d[8:20]);pg=struct.unpack("<I",d[36:40])[0];print(pg*(1+(k+pg-1)//pg+(r+pg-1)//pg))')
gunzip -c "$ASSETS/parts/p4-kernel.img.gz" | head -c "$BOOTIMG_BYTES" | dd of="$IMG" bs=512 seek=188416 conv=notrunc status=none
dd if="$OUT_DIR/p5.img" of="$IMG" bs=512 seek=$P5_FIRST conv=notrunc status=none
dd if="$OUT_DIR/p6.img" of="$IMG" bs=512 seek=$P6_FIRST conv=notrunc status=none
python3 "$REPO/tools/h700-image/gpt.py" "$IMG" $TOTAL $P5_LAST $P6_FIRST $P6_LAST
rm -f "$OUT_DIR/p5.img" "$OUT_DIR/p6.img"
xz -9 -T0 -f -k "$IMG"
echo ""
echo "IMAGE: $IMG ($(du -h "$IMG" | cut -f1) on disk)  DOWNLOAD: $IMG.xz ($(du -h "$IMG.xz" | cut -f1))"
echo "Flash: diskutil unmountDisk /dev/diskN && sudo dd if=$IMG of=/dev/rdiskN bs=4m  (NEVER the muOS card)"

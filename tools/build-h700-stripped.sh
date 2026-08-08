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
# Unused libraries (VERIFIED on-device 2026-08-06: minui+minarch launch cleanly without them,
# linker resolves every dependency): GPU (we render via the DE layer, no GL), ICU unicode data
# (was for ImageMagick/heavy apps), ImageMagick (muOS theme rendering), and the EasyRPG game
# engine libs. ~87MB.
rm -f "$R/usr/lib/libmali.so"
rm -f "$R/usr/lib/libicudata.so."* "$R/usr/lib/libicui18n.so."* "$R/usr/lib/libicuuc.so."*
rm -f "$R/usr/lib/libMagickCore-"* "$R/usr/lib/libMagickWand-"* "$R/usr/bin/magick"* "$R/usr/bin/convert" 2>/dev/null || true
rm -f "$R/usr/lib/libzmusic.so."* "$R/usr/lib/liblcf.so."*
# Audio daemon (9.5MB): pipewire/wireplumber cannot autolaunch a D-Bus session bus on this headless
# rootfs (no X11/DISPLAY) so they never start, and stripping /opt/muos/share below removes their
# config anyway. MinUI Zero routes ALSA straight to the codec instead (asound.conf, added below).
# Only the pipewire tools themselves link libpipewire, and SDL uses the alsa driver — VERIFIED safe.
rm -rf "$R/usr/bin/pipewire"* "$R/usr/bin/wireplumber" "$R/usr/bin/pw-"* \
       "$R/usr/lib/libpipewire-0.3.so."* "$R/usr/lib/libwireplumber-0.4.so."* \
       "$R/usr/lib/spa-0.2" "$R/usr/lib/pipewire-0.3" "$R/usr/lib/wireplumber-0.4" \
       "$R/usr/lib/alsa-lib/libasound_module_pcm_pipewire.so" 2>/dev/null || true
# AGGRESSIVE LEAN (2026-08-08 audit): the muOS base carries a whole 32-bit userland + a full app
# toolkit we never run. VERIFIED offline: arch-scanned EVERY retained ELF (all aarch64, ZERO 32-bit)
# and grepped the retained boot scripts (no refs to anything below). Takes the rootfs 669MB -> ~230MB.
# 281MB: the orphaned 32-bit (armhf) compat tree — muOS ships it for 32-bit RetroArch cores we omit.
rm -rf "$R/usr/lib32" "$R/lib32" "$R/lib/ld-linux-armhf.so.3" 2>/dev/null || true
# ~120MB of assets nothing on our boot/wifi/audio/display/minui path uses:
# (2>/dev/null || true on each: a few vim test files carry the immutable attr and EPERM on rm would
#  otherwise abort the whole build under `set -e`; the tiny remnant is harmless.)
rm -rf "$R/usr/share/fonts/truetype/noto" 2>/dev/null || true   # 63MB CJK fonts (minui ships its own font)
rm -rf "$R/usr/share/soundfonts" 2>/dev/null || true            # 31MB MIDI soundfont
rm -rf "$R/usr/share/vim" "$R/usr/bin/vim" 2>/dev/null || true  # 18MB editor
rm -f  "$R/usr/share/misc/magic.mgc" 2>/dev/null || true        # 8MB file(1) magic db
# dev/util binaries + their app-only libs (none on the boot path; grep-verified):
rm -f "$R/usr/bin/btop" "$R/usr/bin/htop" "$R/usr/bin/dust" "$R/usr/bin/mpv" "$R/usr/bin/7zr" \
      "$R/usr/bin/ld" "$R/usr/bin/ld.bfd" "$R/usr/bin/as" "$R/usr/bin/readelf" "$R/usr/bin/objdump" \
      "$R/usr/bin/bsdunzip" "$R/usr/bin/get_disto" \
      "$R/usr/bin/img2webp" "$R/usr/bin/cwebp" "$R/usr/bin/dwebp" "$R/usr/bin/gif2webp" "$R/usr/bin/webpmux" \
      "$R/usr/bin/webpinfo" "$R/usr/bin/webp_quality" "$R/usr/bin/vwebp" "$R/usr/bin/anim_dump" "$R/usr/bin/anim_diff" 2>/dev/null || true
rm -f "$R/usr/lib/libmpv.so."* "$R/usr/lib/libtcl8.6.so."* "$R/usr/lib/libvpx.so."* \
      "$R/usr/lib/libjanet.so."* "$R/usr/lib/libzmusiclite.so."* 2>/dev/null || true
rm -rf "$R/usr/share/tcltk" 2>/dev/null || true
# Orphaned 64-bit app libraries (dependency-closure analysis 2026-08-08: seeded with all 458
# retained binaries + our minui/minarch/cores; these are referenced by NONE — leftovers from the
# muOS app suite). ~20MB. Names are explicit for the SDL families so the SDL2 core/image/ttf we use
# survive; we also KEEP the dlopen-prone glibc name-resolution libs (nss/resolv/nsl), the image
# codecs SDL2_image may dlopen (jpeg/tiff/webp), and libsamplerate/libopenal/libarchive.
rm -f "$R/usr/lib/libboost_"* 2>/dev/null || true
rm -f "$R/usr/lib/libSDL-1.2.so."* "$R/usr/lib/libSDL_mixer-1.2.so."* "$R/usr/lib/libSDL_image-1.2.so."* "$R/usr/lib/libSDL_gfx.so."* "$R/usr/lib/libSDL_net-1.2.so."* "$R/usr/lib/libSDL_ttf-2.0.so."* 2>/dev/null || true
rm -f "$R/usr/lib/libSDL2_mixer-2.0.so."* "$R/usr/lib/libSDL2_net-2.0.so."* "$R/usr/lib/libSDL2_gfx-1.0.so."* 2>/dev/null || true
rm -f "$R/usr/lib/libpulse"* "$R/usr/lib/libasyncns.so."* 2>/dev/null || true
rm -f "$R/usr/lib/libgnutls.so."* "$R/usr/lib/libp11-kit.so."* "$R/usr/lib/libtasn1.so."* "$R/usr/lib/libidn2.so."* "$R/usr/lib/libgmp.so."* 2>/dev/null || true
rm -f "$R/usr/lib/libdrm_intel.so."* "$R/usr/lib/libdrm_radeon.so."* "$R/usr/lib/libdrm_nouveau.so."* "$R/usr/lib/libdrm_freedreno.so."* 2>/dev/null || true
rm -f "$R/usr/lib/libtheora"* "$R/usr/lib/libFLAC"* "$R/usr/lib/libfaad"* "$R/usr/lib/libass.so."* "$R/usr/lib/libWildMidi.so."* "$R/usr/lib/libxmp.so."* "$R/usr/lib/libsidplay2.so."* "$R/usr/lib/libresid"* "$R/usr/lib/libsidutils.so."* "$R/usr/lib/libhardsid"* "$R/usr/lib/libsbc.so."* "$R/usr/lib/libportmidi.so."* 2>/dev/null || true
rm -f "$R/usr/lib/libmoonlight-common.so."* "$R/usr/lib/libgamestream.so."* "$R/usr/lib/libenet.so."* "$R/usr/lib/libcec.so."* "$R/usr/lib/libcapsimage.so."* "$R/usr/lib/libm3g.so" "$R/usr/lib/libjq.so."* 2>/dev/null || true
rm -f "$R/usr/lib/libopcodes-"* "$R/usr/lib/libctf"* "$R/usr/lib/libdw-"* "$R/usr/lib/libelf-"* "$R/usr/lib/libasm-"* 2>/dev/null || true
rm -f "$R/usr/lib/libevent"* "$R/usr/lib/libwrap.so."* "$R/usr/lib/libwpa_client.so" "$R/usr/lib/libnghttp2.so."* "$R/usr/lib/libfmt.so."* "$R/usr/lib/libatopology.so."* "$R/usr/lib/libnl-xfrm-3.so."* "$R/usr/lib/libapparmor.so."* "$R/usr/lib/libbluetooth.so."* "$R/usr/lib/libserial"* "$R/usr/lib/libusb-1.0.so."* "$R/usr/lib/libparted-fs-resize.so."* 2>/dev/null || true
# Leftover app-tool BINARIES + their libs (ffmpeg/flac/lame/ogg encoders, avahi mDNS, expect, the
# icu/lcf/pipewire tools). Several were ALREADY broken by the base strip; the orphaned-lib cut above
# finished the rest. None are on the boot/wifi/audio/display/minui path. Removing the binaries too
# clears the dangling deps and trims a bit more.
rm -f "$R/usr/bin/ffmpeg" "$R/usr/bin/ffplay" "$R/usr/bin/ffprobe" \
      "$R/usr/bin/flac" "$R/usr/bin/metaflac" "$R/usr/bin/lame" "$R/usr/bin/ogg123" "$R/usr/bin/oggenc" "$R/usr/bin/opusenc" "$R/usr/bin/playsound" "$R/usr/bin/playsound_simple" \
      "$R/usr/bin/avahi-"* "$R/usr/bin/expect" "$R/usr/bin/icuexportdata" "$R/usr/bin/pkgdata" "$R/usr/bin/lcf2xml" "$R/usr/bin/lcfstrings" "$R/usr/bin/wpctl" "$R/usr/bin/wpexec" 2>/dev/null || true
rm -f "$R/usr/lib/libSDL_sound-1.0.so."* "$R/usr/lib/libsndfile.so."* "$R/usr/lib/libpostproc.so."* "$R/usr/lib/libswresample.so."* "$R/usr/lib/libswscale.so."* "$R/usr/lib/libexslt.so."* "$R/usr/lib/libxslt.so."* "$R/usr/lib/libavahi-"* 2>/dev/null || true
# openssh (32MB): we ship dropbearmulti instead (~250KB aarch64, launched by the frontend) — the
# same ssh path the Smart Pro uses. And /etc/udev/hwdb.bin (9.3MB): udev autoloads modules by
# MODALIAS (not hwdb) and h700 input is raw evdev, so the hardware database has no consumer here.
rm -rf "$R/opt/openssh" 2>/dev/null || true
rm -f "$R/etc/udev/hwdb.bin" 2>/dev/null || true
# KEEP: /opt/muos/script (init + network.sh + halt.sh + func.sh), /opt/muos/device + config,
#       udev, wifi stack, dropbearmulti (ssh), SDL, kernel modules, alsa-utils (amixer/alsactl),
#       gl4es/libopenal/libsamplerate/embiggen-disk (possible SDL/runtime deps — not worth the risk)

echo "  swapping FRONTEND -> minui in startup.sh..."
SU="$R/opt/muos/script/system/startup.sh"
# replace the frontend launch with our loop; skip the hotkey daemon (minui reads input directly)
sed -i "s|^FRONTEND start|sh /opt/minui-zero/minui-frontend.sh \&|" "$SU"
sed -i "s|^HOTKEY start|true # hotkey daemon disabled (minui owns input)|" "$SU"
mkdir -p "$R/opt/minui-zero"
cp /a/minui-frontend.sh "$R/opt/minui-zero/minui-frontend.sh"
chmod +x "$R/opt/minui-zero/minui-frontend.sh"
# NO wifi credentials baked into the image (privacy): WIPE muOS saved creds from the rdumped rootfs.
# It bakes the builder network at /opt/muos/config/network/{ssid,pass} (net.sh no-ops on empty SSID),
# so after this wifi is driven SOLELY by the USER-SUPPLIED wifi.txt (SSID:password) at the SD-card
# root at boot, MinUI-style. Ship a commented example.
: > "$R/opt/muos/config/network/ssid" 2>/dev/null || true
: > "$R/opt/muos/config/network/pass" 2>/dev/null || true
rm -f "$R/etc/wpa_supplicant.conf" 2>/dev/null || true
# ssh access: dropbear (the frontend launches dropbearmulti) reads /root/.ssh/authorized_keys for
# key auth. openssh is stripped, so its host-key perm dance is gone with it.
mkdir -p "$R/root/.ssh"
cp /a/authorized_keys "$R/root/.ssh/authorized_keys" 2>/dev/null || true
chmod 700 "$R/root/.ssh" 2>/dev/null || true

echo "  wiring ALSA-direct audio (pipewire removed)..."
# Route the default ALSA PCM straight to the codec (plug = auto rate/format/channel convert),
# replacing the muOS default->pipewire routing. Survives boot: pipewire.sh RESTORE_CONF copies
# from /opt/muos/share/conf (stripped), finds no source, and leaves this file untouched.
cat > "$R/etc/asound.conf" <<"ASOUND"
pcm.!default {
    type plug
    slave.pcm "hw:0,0"
}
ctl.!default {
    type hw
    card 0
}
ASOUND
# Trim pipewire.sh (startup.sh still runs it at boot) to the ONE thing still needed: restoring the
# codec mixer state (unmute + output routing) from asound.state under /opt/muos/device (kept).
# Drops the pipewire daemon start and the wpctl finalise, whose ~6-9s of timeouts would otherwise
# stall every boot now. minui volume then drives the digital-volume mixer directly (libmsettings).
cat > "$R/opt/muos/script/system/pipewire.sh" <<"PWSH"
#!/bin/sh
# MinUI Zero: pipewire removed; keep only the codec init startup.sh needs at boot.
case "$1" in
start) alsactl -U -f /opt/muos/device/control/asound.state restore 2>/dev/null ;;
esac
exit 0
PWSH
chmod +x "$R/opt/muos/script/system/pipewire.sh"

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
cp "$REPO/skeleton/SYSTEM/tg5040/bin/dropbearmulti"     "$STAGE/.system/h700/bin/dropbearmulti"  # ssh (openssh stripped); aarch64, shared with tg5040
chmod +x "$STAGE/.system/h700/bin/dropbearmulti"
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

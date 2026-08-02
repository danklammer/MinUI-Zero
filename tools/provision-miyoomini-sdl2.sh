#!/bin/sh
# Provision SDL2 into the miyoomini toolchain image -> tag `miyoomini-toolchain-sdl2`.
#
# WHY: union-miyoomini-toolchain ships SDL 1.2 ONLY. Our miyoomini platform must move to SDL2
# because SDL 1.2 audio on this device goes through /dev/dsp -> audioserver via the libpadsp
# preload, and libpadsp SEGFAULTS binaries built by this toolchain (verified 2026-07-24 with a
# 10-line open("/dev/dsp") test). SDL2 talks to MI_AO directly through its MMIYOO driver:
#   MEASURED on-device: "audio drivers available: 1 / - MMIYOO ... SDL_OpenAudio OK driver=MMIYOO"
# with audioserver killed and no LD_PRELOAD.
#
# HEADERS come from upstream SDL2 (pinned below). The RUNTIME LIB is the vendor build that ships
# on the device (it is the only one containing the MMIYOO driver) — it is NOT vendored into this
# repo; pull it off a device once and pass the path in.
#
#   scp root@<device>:/mnt/SDCARD/.system/miyoomini/lib/libSDL2-2.0.so.0 .notes/mmp-build/
#   sh tools/provision-miyoomini-sdl2.sh [path-to-libSDL2-2.0.so.0]
set -e

SDL2_TAG=release-2.26.5
IMG_TAG=release-2.6.3             # SDL2_image headers
TTF_TAG=release-2.20.2            # SDL2_ttf headers           # header pin; ABI-compatible with the device's libSDL2-2.0.so.0
BASE_IMAGE=miyoomini-toolchain
OUT_IMAGE=miyoomini-toolchain-sdl2
LIB="${1:-.notes/mmp-build/libSDL2-2.0.so.0}"

[ -f "$LIB" ] || { echo "ERROR: device libSDL2 not found at $LIB"; echo "See header comment for how to fetch it."; exit 1; }
docker image inspect "$BASE_IMAGE" >/dev/null 2>&1 || { echo "ERROR: build $BASE_IMAGE first (make shell PLATFORM=miyoomini)"; exit 1; }

# mount the lib's own directory (macOS: mktemp dirs are not Docker-mountable by default)
LIBDIR=$(cd "$(dirname "$LIB")" && pwd)
LIBNAME=$(basename "$LIB")
CNAME=mmp-sdl2-provision-$$

docker run --name "$CNAME" --platform linux/amd64 -v "$LIBDIR:/tmp/in:ro" -e LIBNAME="$LIBNAME" "$BASE_IMAGE" /bin/bash -lc "
  set -e
  SR=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot/usr
  cd /tmp
  wget -q https://github.com/libsdl-org/SDL/archive/refs/tags/${SDL2_TAG}.tar.gz -O sdl2.tgz
  tar xzf sdl2.tgz
  mkdir -p \$SR/include/SDL2
  cp SDL-${SDL2_TAG}/include/*.h \$SR/include/SDL2/
  wget -q https://github.com/libsdl-org/SDL_image/archive/refs/tags/${IMG_TAG}.tar.gz -O img.tgz
  tar xzf img.tgz && find SDL_image-${IMG_TAG} -maxdepth 2 -name SDL_image.h -exec cp {} \$SR/include/SDL2/ \;
  wget -q https://github.com/libsdl-org/SDL_ttf/archive/refs/tags/${TTF_TAG}.tar.gz -O ttf.tgz
  tar xzf ttf.tgz && find SDL_ttf-${TTF_TAG} -maxdepth 2 -name SDL_ttf.h -exec cp {} \$SR/include/SDL2/ \;
  cp /tmp/in/\$LIBNAME \$SR/lib/libSDL2-2.0.so.0
  cp /tmp/in/libSDL2_image-2.0.so.0 \$SR/lib/ || true
  cp /tmp/in/libSDL2_ttf-2.0.so.0 \$SR/lib/ || true
  ln -sf libSDL2-2.0.so.0 \$SR/lib/libSDL2.so
  ln -sf libSDL2_image-2.0.so.0 \$SR/lib/libSDL2_image.so
  ln -sf libSDL2_ttf-2.0.so.0 \$SR/lib/libSDL2_ttf.so
  rm -rf /tmp/SDL-${SDL2_TAG} /tmp/sdl2.tgz
  echo \"provisioned: \$(ls \$SR/include/SDL2 | wc -l) headers\"
"
docker commit "$CNAME" "$OUT_IMAGE" >/dev/null
docker rm "$CNAME" >/dev/null


echo "tagged $OUT_IMAGE"
echo "NOTE: link with -Wl,--allow-shlib-undefined -Wl,--unresolved-symbols=ignore-in-shared-libs"
echo "      (the vendor libSDL2 pulls in libEGL/libGLESv2/libSDL2_image/... which live on-device)"

#!/bin/sh
# Rebuild the miyoomini libSDL2 that ships in skeleton/SYSTEM/miyoomini/lib/.
#
# WHY THIS EXISTS
# The runtime libs for this platform are vendored as tracked binaries (the convention already in
# place for libfreetype, libpng, libjson-c, ...). That is fine for stock upstream libraries, but our
# libSDL2 is NOT stock: it is built with SDL2's OSS backend enabled so audio can be routed through
# the vendor audioserver (see MinUI.pak/launch.sh for why that removes the game-boundary pops).
# A vendored binary nobody can regenerate is exactly how six foreign Buildroot cores shipped
# undetected, so the build that produces it lives in the repo, pinned, next to the artifact.
#
# This is deliberately NOT wired into `make`: it needs Docker + an apt install + a json-c cross
# build, and the output changes roughly never. Run it when the SDL2 pin or audio config changes;
# `make` verifies the result (the OSS-marker guard in the makefile) rather than rebuilding it.
#
#   usage: sh tools/build-miyoomini-sdl2.sh
#   needs: Docker running, image `miyoomini-toolchain-sdl2` (amd64)
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDL2_REPO=https://github.com/XK9274/sdl2_miyoo.git
# Pin the COMMIT, not the branch. `--branch pico8` is a moving target: it silently changes what we
# ship between builds, which is the same trap the unpinned cores fell into.
SDL2_PIN=2caea4749b7ecff1df5cb5285da8f892235cef97
DEST=$ROOT/skeleton/SYSTEM/miyoomini/lib/libSDL2-2.0.so.0
LOG=$ROOT/.notes/mmp-build/sdl2-full.log
mkdir -p "$(dirname "$LOG")"

# Record WHICH toolchain produced the binary. The image is built locally and is not itself pinned,
# so without this the artifact is unattributable — the exact gap that let six foreign Buildroot
# cores ship undetected.
IMAGE_ID=$(docker image inspect miyoomini-toolchain-sdl2 --format '{{.Id}}' 2>/dev/null)
[ -n "$IMAGE_ID" ] || { echo "toolchain image miyoomini-toolchain-sdl2 not found"; exit 1; }
{
	echo "build-miyoomini-sdl2.sh"
	echo "  sdl2 pin : $SDL2_PIN"
	echo "  sdl2 repo: $SDL2_REPO"
	echo "  toolchain: $IMAGE_ID"
} > "$LOG"

# Everything happens in ONE `docker run` on purpose. The image is used with --rm, so anything
# installed into its sysroot (autotools, cmake, json-c) evaporates when the container exits.
# Splitting this across runs means the SDL2 build cannot see the json-c it needs.
docker run --rm --platform linux/amd64 \
  -v "$ROOT/workspace:/root/workspace" \
  -v "$ROOT/.notes:/root/notes" \
  -v "$ROOT/skeleton:/root/skel:ro" \
  -e SDL2_REPO="$SDL2_REPO" -e SDL2_PIN="$SDL2_PIN" \
  miyoomini-toolchain-sdl2 /bin/bash -lc '
  set -e
  SYSROOT=/opt/miyoomini-toolchain/arm-linux-gnueabihf/libc
  export CROSS=/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-
  export CC=${CROSS}gcc AR=${CROSS}ar AS=${CROSS}as LD=${CROSS}ld CXX=${CROSS}g++
  # HOST must be exported as an ENV VAR, not just passed as --host: configure.ac:2407 gates
  #   if test x$HOST = xarm-linux; then EXTRA_CFLAGS="... -DMMIYOO ..."
  # on the environment variable. Without it the mmiyoo driver compiles WITHOUT -DMMIYOO, so
  # MI_U32 / MI_AO_ChnState_t / AoDevId are all undeclared and the driver fails to build.
  export MOD=mmiyoo HOST=arm-linux

  echo "=== [1/4] build deps ==="
  # json-c is pinned to 0.16 because that series has soname libjson-c.so.5 -- matching the runtime
  # lib already on the card. A mismatched soname builds fine and then fails at load.
  (apt-get update -qq && apt-get install -y -qq autoconf automake libtool m4 pkg-config cmake) \
    >/tmp/apt.log 2>&1 || { echo "apt failed"; tail -15 /tmp/apt.log; exit 1; }
  echo "  autoconf=$(command -v autoconf) cmake=$(command -v cmake)"

  echo "=== [2/4] json-c -> sysroot ==="
  # Pinned to the COMMIT the json-c-0.16 tag resolves to, not the json-c-0.16 BRANCH, which is a
  # maintenance branch that moves. The 0.16 series is required because its soname is
  # libjson-c.so.5, matching the runtime lib already on the card; a mismatched soname builds fine
  # and then fails at load, so it is asserted below rather than assumed.
  JSONC_PIN=2f2ddc1f2dbca56c874e8f9c31b5b963202d80e7
  cd /tmp && rm -rf json-c && mkdir -p json-c && cd json-c
  git init -q
  git remote add origin https://github.com/json-c/json-c.git
  git fetch -q --depth 1 origin $JSONC_PIN
  git checkout -q FETCH_HEAD
  mkdir -p b && cd b
  cmake .. -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
    -DCMAKE_C_COMPILER=${CROSS}gcc -DCMAKE_INSTALL_PREFIX=$SYSROOT/usr \
    -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DDISABLE_WERROR=ON >/tmp/jc1.log 2>&1 \
      || { tail -20 /tmp/jc1.log; exit 1; }
  make -j4 >/tmp/jc2.log 2>&1 || { grep -iE "error" /tmp/jc2.log | head -10; exit 1; }
  make install >/tmp/jc3.log 2>&1
  JC_SONAME=$(${CROSS}readelf -d $SYSROOT/usr/lib/libjson-c.so 2>/dev/null | grep -o "libjson-c.so.[0-9]*" | head -1)
  echo "  soname: $JC_SONAME"
  [ "$JC_SONAME" = "libjson-c.so.5" ] \
    || { echo "FATAL: json-c soname is $JC_SONAME, card needs libjson-c.so.5 -- would fail at load"; exit 1; }

  echo "=== [3/4] SDL2 source @ pin ==="
  # FRESH clone into a container-local directory every run. The previous version reused
  # workspace/miyoomini/other/sdl2 and cleaned it with `git checkout -- .` + `make clean`, which
  # does NOT remove untracked files -- that tree had accumulated 13 modified, 1 deleted and 4
  # untracked generated files, so "reproducible" was not reproducible. The container is --rm, so
  # /tmp here is guaranteed pristine and cannot leak back into the repo.
  SRC=/tmp/sdl2-src
  rm -rf "$SRC" && mkdir -p "$SRC" && cd "$SRC"
  git init -q
  git remote add origin "$SDL2_REPO"
  # fetch the exact commit -- shallow, no branch involved, so a moved branch cannot change it
  git fetch -q --depth 1 origin "$SDL2_PIN" || { echo "cannot fetch pin $SDL2_PIN"; exit 1; }
  git checkout -q FETCH_HEAD
  echo "  at $(git rev-parse HEAD) (pinned, fresh clone)"
  echo "  tree clean: $(git status --porcelain | wc -l) modified/untracked files"

  # configure.ac hardcodes -lneonarmmiyoo for the NEON scaler helpers. The card ships that exact
  # library as libneon.so, and the VENDOR libSDL2 records libneon.so as its NEEDED entry. Staging
  # our copy as "libneonarmmiyoo.so" made the linker record that name, and the result failed to
  # load on device with:
  #   minui.elf: error while loading shared libraries: libneonarmmiyoo.so: cannot open shared object
  # Point the link at the real name so our NEEDED list matches the vendor one exactly.
  # (NOTE: no apostrophes in this block -- it lives inside bash -lc SINGLE QUOTES, and an
  #  apostrophe silently terminates the whole command. That has now broken this script twice.)
  # sed -i cannot create its temp file on this bind mount (Permission denied) -- the same
  # limitation already documented in tools/gen-miyoomini-core-patches.sh. Filter through /tmp.
  sed "s/-lneonarmmiyoo/-lneon/g" configure.ac > /tmp/cfgac && cat /tmp/cfgac > configure.ac
  cp -f /root/skel/SYSTEM/miyoomini/lib/libneon.so ./libneon.so

  # NO audio driver patch. MyMinUI does not patch the audio driver in their build -- their
  # other/sdl2.patch is dead reference code, and applying it here broke game launch. We use SDL2
  # STOCK OSS backend, selected at runtime via SDL_AUDIODRIVER=dsp, with the vendor libpadsp
  # redirecting /dev/dsp into audioserver.

  echo "=== [4/4] configure + make ==="
  # REMOVE the installed SDL2 headers before self-building SDL2.
  # src/cfg/SDL_picocfg_mmiyoo.c does `#include <SDL2/SDL.h>` -- angle brackets with the SDL2/
  # prefix, which always resolves to $SYSROOT/usr/include/SDL2 (installed for building
  # minui/minarch), never to this source tree. Those headers carry their own SDL_config.h, which
  # self-guards with "#error Wrong SDL_config.h, check your include path?". The sysroot is on the
  # default search path, so no -I juggling avoids it. The container is --rm, so this cannot leak.
  # Rescue the SATELLITE headers first: the mmiyoo video driver includes <SDL_ttf.h>
  # (src/video/mmiyoo/SDL_video_mmiyoo.h:36) and the SDL2_image/SDL2_ttf headers live in the same
  # directory as the core ones, so deleting wholesale also removes headers the drivers need.
  mkdir -p /tmp/sdlsat
  for h in SDL_ttf.h SDL_image.h; do
    [ -f "$SYSROOT/usr/include/SDL2/$h" ] && cp "$SYSROOT/usr/include/SDL2/$h" /tmp/sdlsat/
  done
  rm -rf "$SYSROOT/usr/include/SDL2"
  cp -f /tmp/sdlsat/*.h include/ 2>/dev/null || true

  ./autogen.sh >/tmp/ag.log 2>&1 || { tail -10 /tmp/ag.log; exit 1; }
  # OSS is deliberately NOT disabled here -- that is the whole point of this build. Every other
  # backend is off so SDL cannot silently pick a different one.
  ./configure --host=arm-linux \
    --disable-joystick-virtual --disable-power --disable-alsa --disable-diskaudio \
    --disable-video-x11 --disable-video-wayland --disable-video-kmsdrm --disable-video-vulkan \
    --disable-dbus --disable-ime --disable-fcitx --disable-hidapi --disable-pulseaudio \
    --disable-sndio --disable-libudev --disable-jack --disable-video-opengl \
    --disable-video-opengles --disable-video-opengles2 --disable-dummyaudio \
    --disable-video-dummy >/tmp/conf.log 2>&1 || { tail -25 /tmp/conf.log; exit 1; }
  grep -q "define SDL_AUDIO_DRIVER_OSS 1" include/SDL_config.h \
    || { echo "FATAL: OSS backend NOT enabled -- this build would reintroduce the audio pop"; exit 1; }
  echo "  configure OK, OSS backend enabled"

  # Make the SDL2/-prefixed system-style include resolve to THIS tree (see picocfg note above).
  ln -sfn . include/SDL2

  # Always copy the build log OUT of the container: it lives in /tmp and the container is --rm, so
  # a failure previously destroyed the only record of WHY it failed.
  make -j4 V=0 >/tmp/make.log 2>&1 || {
    cp /tmp/make.log /root/notes/mmp-build/sdl2-make.log 2>/dev/null
    echo "--- last 40 lines of make ---"; tail -40 /tmp/make.log; exit 1; }
  cp /tmp/make.log /root/notes/mmp-build/sdl2-make.log 2>/dev/null
  cp -L build/.libs/libSDL2-2.0.so.0 /root/notes/mmp-build/libSDL2-patched.so.0
  echo SDL2_BUILD_DONE
' >> "$LOG" 2>&1 || { echo "build FAILED — see $LOG"; tail -20 "$LOG"; exit 1; }

grep -q SDL2_BUILD_DONE "$LOG" || { echo "build did not complete — see $LOG"; tail -20 "$LOG"; exit 1; }

# Verify BEFORE overwriting the shipped lib. A build that lost the OSS backend must not be allowed
# to quietly replace a good one -- that failure is silent on device (audio still "works", the pop
# just comes back) and would be near-impossible to attribute later.
NEW=$ROOT/.notes/mmp-build/libSDL2-patched.so.0
if ! strings "$NEW" | grep -q "OSS /dev/dsp standard audio"; then
	echo "REFUSING to install: fresh build has no OSS backend"; exit 1
fi
cp "$NEW" "$DEST"
echo "installed $(md5 -q "$DEST" 2>/dev/null || md5sum "$DEST" | cut -d" " -f1) -> skeleton/SYSTEM/miyoomini/lib/libSDL2-2.0.so.0"

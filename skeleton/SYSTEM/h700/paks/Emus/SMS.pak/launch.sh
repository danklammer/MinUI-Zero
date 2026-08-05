#!/bin/sh
# h700 SMS pak — hosted-dev. minarch + gambatte, measured panel rate, pipewire audio route.
EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"

export PLATFORM="h700"
export SDCARD_PATH="/mnt/mmc"
export SYSTEM_PATH="$SDCARD_PATH/.system/$PLATFORM"
export USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
export LOGS_PATH="$USERDATA_PATH/logs"
export SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/shared"
export SAVES_PATH="$SDCARD_PATH/Saves"
export BIOS_PATH="$SDCARD_PATH/Bios"
export CORES_PATH="$SYSTEM_PATH/cores"
# make-built binaries link libmsettings.so (shipped in .system/h700/lib)
export LD_LIBRARY_PATH="$SYSTEM_PATH/lib:$LD_LIBRARY_PATH"
# Panel measured 59.9777 Hz (panelprobe 2026-08-04)
export MINARCH_PANEL_FPS=59.9777
# muOS's pipewire owns the audio hardware; ALSA "default" routes to it via the pipewire plugin,
# which needs the socket dir. Without these SDL_OpenAudio fails "Host is down". Verified working
# 2026-08-05 (32768Hz stream opened).
export XDG_RUNTIME_DIR=/run
export PIPEWIRE_RUNTIME_DIR=/run
export SDL_AUDIODRIVER=alsa

mkdir -p "$LOGS_PATH" "$SAVES_PATH/$EMU_TAG" "$SHARED_USERDATA_PATH/.minui"
"$SYSTEM_PATH/bin/minarch.elf" "$CORES_PATH/picodrive_libretro.so" "$ROM" > "$LOGS_PATH/$EMU_TAG.txt" 2>&1

#!/bin/sh
# h700 SMS pak — hosted-dev. minarch + gambatte, measured panel rate, ALSA-direct audio.
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
# ALSA-direct audio (pipewire removed): SDL's alsa backend opens "default", which asound.conf
# routes straight to the codec (plug -> hw:0,0). The codec is unmuted/routed at boot by the
# trimmed pipewire.sh (alsactl restore); minui drives the digital-volume mixer.
export SDL_AUDIODRIVER=alsa

mkdir -p "$LOGS_PATH" "$SAVES_PATH/$EMU_TAG" "$SHARED_USERDATA_PATH/.minui"
"$SYSTEM_PATH/bin/minarch.elf" "$CORES_PATH/picodrive_libretro.so" "$ROM" > "$LOGS_PATH/$EMU_TAG.txt" 2>&1

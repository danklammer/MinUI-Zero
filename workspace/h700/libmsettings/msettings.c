// h700 msettings — device settings (moved verbatim from platform.c when the makefile wiring
// landed; the shared minui/minarch makefiles link -lmsettings on every platform).
//
// On the MinUI Zero image we OWN the codec and panel: volume drives the ALSA 'digital volume'
// mixer directly (amixer, see SetRawVolume — pipewire is removed), brightness the Allwinner
// dispdbg debugfs. This library is standalone by design (no utils.c), so it carries its own
// tiny sysfs/mixer helpers.
#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>

#include "msettings.h"

// Brightness: this device has NO /sys/class/backlight. muOS drives the panel through the
// Allwinner dispdbg debugfs (func.sh DISPLAY_WRITE): name=disp0, command=setbl/getbl,
// param=0-255 (max from /opt/muos/device/config/screen/bright), then start=1. getbl answers
// in /sys/kernel/debug/dispdbg/info, so brightness is READABLE — no persistence file needed.
#define DISPDBG "/sys/kernel/debug/dispdbg/"
#define BRIGHT_RAW_MAX 255
#define BRIGHT_RAW_MIN 8 // UI 0 stays faintly visible, never a black screen

static void putStr(const char* path, const char* s) {
	FILE* file = fopen(path, "w");
	if (file != NULL) {
		fputs(s, file);
		fclose(file);
	}
}
static void dispdbg_cmd(const char* cmd, const char* param) {
	putStr(DISPDBG "name", "disp0");
	putStr(DISPDBG "command", cmd);
	if (param) putStr(DISPDBG "param", param);
	putStr(DISPDBG "start", "1");
}

// Volume drives the codec's 'digital volume' mixer via libasound in-process (snd_mixer), NOT a
// forked amixer. MinUI Zero removes pipewire — a headless rootfs can't autolaunch its D-Bus session
// bus so it never starts, and it is 9.5MB of idle weight against the thesis — so the old
// wpctl-on-the-sink path is gone. We OWN the codec now. An earlier version shelled out to `amixer &`
// per keypress; that forked ~9x/s on a held ramp with no ordering guarantee (Codex-confirmed race:
// a late-completing process could leave the level a step off). In-process is synchronous, ordered,
// and fork-free — no race and no audio-ring stall. This control's dB TLV is garbage (D33: reports
// tens of thousands of dB), so we map the RAW integer over the control's queried range, which is
// linear and higher=louder (MEASURED 2026-08-06 on this H700 codec: raw 0-63, 40 = 63% — NOT
// reversed like the A133P Brick).
#define VOL_CTL "digital volume"

static int cur_vol = -1; // 0-20 UI scale; -1 = not yet read
static snd_mixer_t*      vol_mixer = NULL;
static snd_mixer_elem_t* vol_elem  = NULL;
static long vol_min = 0, vol_max = 63; // 'digital volume' raw range, queried at open

// Open the codec mixer once and cache the 'digital volume' element for the process lifetime.
static void vol_open(void) {
	if (vol_mixer) return;
	if (snd_mixer_open(&vol_mixer, 0) < 0) { vol_mixer = NULL; return; }
	snd_mixer_selem_id_t* sid;
	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, VOL_CTL);
	if (snd_mixer_attach(vol_mixer, "hw:0") < 0 ||
	    snd_mixer_selem_register(vol_mixer, NULL, NULL) < 0 ||
	    snd_mixer_load(vol_mixer) < 0 ||
	    !(vol_elem = snd_mixer_find_selem(vol_mixer, sid))) {
		snd_mixer_close(vol_mixer);
		vol_mixer = NULL; vol_elem = NULL;
		return;
	}
	if (snd_mixer_selem_get_playback_volume_range(vol_elem, &vol_min, &vol_max) < 0 || vol_max <= vol_min) {
		vol_min = 0; vol_max = 63;
	}
}

void InitSettings(void) {
	// Read the codec's current raw level (set at boot by the frontend's alsactl restore) back to UI.
	vol_open();
	if (vol_elem) {
		long v = vol_min, range = vol_max - vol_min;
		if (snd_mixer_selem_get_playback_volume(vol_elem, SND_MIXER_SCHN_MONO, &v) >= 0)
			cur_vol = (int)(((v - vol_min) * 20 + range / 2) / range);
	}
	if (cur_vol < 0) cur_vol = 10;
	if (cur_vol > 20) cur_vol = 20;
}
void QuitSettings(void) {
	if (vol_mixer) { snd_mixer_close(vol_mixer); vol_mixer = NULL; vol_elem = NULL; }
}

int GetBrightness(void) { // 0-10 UI, read back from the panel itself
	int raw = -1;
	dispdbg_cmd("getbl", NULL);
	FILE* f = fopen(DISPDBG "info", "r");
	if (f) {
		if (fscanf(f, "%d", &raw) != 1) raw = -1;
		fclose(f);
	}
	if (raw < 0) return 5;
	int ui = (raw * 10 + BRIGHT_RAW_MAX / 2) / BRIGHT_RAW_MAX;
	return ui > 10 ? 10 : ui;
}
int GetVolume(void) { return cur_vol; }

void SetRawBrightness(int value) { // 0-255
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", value);
	dispdbg_cmd("setbl", buf);
}
void SetRawVolume(int value) { // 0-100 (SetVolume passes UI*5); MUTE_VOLUME_RAW (0) mutes
	// In-process snd_mixer write: a single mixer ioctl (microseconds), synchronous and ordered.
	// SetVolume runs on the emulation thread (the input hook) and this fires ~9x/s on a held ramp —
	// no fork means no ordering race and no fork+wait stall of the audio ring (the old `amixer &`
	// path had both). value 0 -> raw vol_min = mute; value 100 -> raw vol_max.
	if (value < 0) value = 0;
	if (value > 100) value = 100;
	if (!vol_elem) vol_open();
	if (!vol_elem) return;
	long raw = vol_min + ((long)value * (vol_max - vol_min) + 50) / 100;
	(void)snd_mixer_selem_set_playback_volume_all(vol_elem, raw);
}

void SetBrightness(int value) { // 0-10 UI
	if (value < 0) value = 0;
	if (value > 10) value = 10;
	int raw = value ? value * BRIGHT_RAW_MAX / 10 : BRIGHT_RAW_MIN;
	SetRawBrightness(raw);
}
void SetVolume(int value) { // 0-20 UI
	if (value < 0) value = 0;
	if (value > 20) value = 20;
	cur_vol = value;
	SetRawVolume(value * 5);
}

int GetJack(void) { return 0; }
void SetJack(int value) {}

int GetHDMI(void) { return 0; }
void SetHDMI(int value) {}

int GetMute(void) { return 0; }

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <string.h>

#include <mi_ao.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "msettings.h"

///////////////////////////////////////

//	mmplus axp223 (via eggs)
#define	AXPDEV	"/dev/i2c-1"
#define	AXPID	(0x34)

//
//	AXP223 write (plus)
//		32 .. bit7: Shutdown Control
//
int axp_write(unsigned char address, unsigned char val) {
	struct i2c_msg msg[1];
	struct i2c_rdwr_ioctl_data packets;
	unsigned char buf[2];
	int ret;
	int fd = open(AXPDEV, O_RDWR);
	ioctl(fd, I2C_TIMEOUT, 5);
	ioctl(fd, I2C_RETRIES, 1);

	buf[0] = address;
	buf[1] = val;
	msg[0].addr = AXPID;
	msg[0].flags = 0;
	msg[0].len = 2;
	msg[0].buf = buf;

	packets.nmsgs = 1;
	packets.msgs = &msg[0];
	ret = ioctl(fd, I2C_RDWR, &packets);

	close(fd);
	if (ret < 0) return -1;
	return 0;
}

//
//	AXP223 read (plus)
//		00 .. C4/C5(USBDC connected) 00(discharging)
//			bit7: ACIN presence indication 0:ACIN not exist, 1:ACIN exists
//			bit6: Indicating whether ACIN is usable (used by axp_test)
//			bit4: Indicating whether VBUS is usable (used by axp_test)
//			bit2: Indicating the Battery current direction 0: discharging, 1: charging
//			bit0: Indicating whether the boot source is ACIN or VBUS
//		01 .. 70(charging) 30(non-charging)
//			bit6: Charge indication 0:not charge or charge finished, 1: in charging
//		B9 .. (& 0x7F) battery percentage
//
int axp_read(unsigned char address) {
	struct i2c_msg msg[2];
	struct i2c_rdwr_ioctl_data packets;
	unsigned char val;
	int ret;
	int fd = open(AXPDEV, O_RDWR);
	ioctl(fd, I2C_TIMEOUT, 5);
	ioctl(fd, I2C_RETRIES, 1);

	msg[0].addr = AXPID;
	msg[0].flags = 0;
	msg[0].len = 1;
	msg[0].buf = &address;
	msg[1].addr = AXPID;
	msg[1].flags = I2C_M_RD;
	msg[1].len = 1;
	msg[1].buf = &val;

	packets.nmsgs = 2;
	packets.msgs = &msg[0];
	ret = ioctl(fd, I2C_RDWR, &packets);

	close(fd);
	if(ret < 0) return -1;
	return val;
}

///////////////////////////////////////

#define SETTINGS_VERSION 2
typedef struct Settings {
	int version; // future proofing
	int brightness;
	int headphones;
	int speaker;
	int jack; 
	int unused[3]; // for future use
} Settings;
static Settings DefaultSettings = {
	.version = SETTINGS_VERSION,
	.brightness = 3,
	.headphones = 20,
	.speaker = 20,
	.jack = 0,
};
static Settings* settings;

#define SHM_KEY "/SharedSettings"
static char SettingsPath[256];
static int shm_fd = -1;
static int is_host = 0;
static int shm_size = sizeof(Settings);
static int is_plus = 0;

static void setMute(int flag); // defined below; used by InitSettings to avoid the power-on pop

void InitSettings(void) {
	is_plus = access("/customer/app/axp_test", F_OK)==0;
	
	sprintf(SettingsPath, "%s/msettings.bin", getenv("USERDATA_PATH"));
	
	shm_fd = shm_open(SHM_KEY, O_RDWR | O_CREAT | O_EXCL, 0644); // see if it exists
	if (shm_fd==-1 && errno==EEXIST) { // already exists
		puts("Settings client");
		shm_fd = shm_open(SHM_KEY, O_RDWR, 0644);
		settings = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	}
	else { // host
		puts("Settings host");
		is_host = 1;
		// we created it so set initial size and populate
		ftruncate(shm_fd, shm_size);
		settings = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
		
		int fd = open(SettingsPath, O_RDONLY);
		if (fd>=0) {
			read(fd, settings, shm_size);
			if (settings->version==1) {
				settings->version = 2;
				settings->headphones = DefaultSettings.headphones;
				settings->jack = DefaultSettings.jack;
			}
			close(fd);
		}
		else {
			// load defaults
			memcpy(settings, &DefaultSettings, shm_size);
		}
	}
	printf("brightness: %i\nspeaker: %i\nheadphones: %i\njack: %i\n", settings->brightness, settings->speaker, settings->headphones, settings->jack);

	// Audio pop on power-on/resume: the DAC channel used to be enabled BEFORE the volume was
	// applied, so the output snapped from off to its default level and that step hit the speaker
	// as a pop. Mute first, bring the channel up, set the real level while still muted, let the
	// rail settle, then unmute. Same mute ioctl the volume path already uses (MI_AO_SETMUTE).
	// DO NOT enable MI_AO here. `audioserver` owns the codec for the lifetime of the system and
	// brings it up exactly once; anything else touching MI_AO reintroduces a power transition, and
	// a power transition IS the pop (proven on device: muting never helped, removing MI_AO_Disable
	// did). MyMinUI comments these same two calls out for the same reason.
	// SDL2 reaches the daemon through /dev/dsp: its STOCK OSS backend, selected per-game with
	// SDL_AUDIODRIVER=dsp, with the vendor libpadsp.so redirecting /dev/dsp into audioserver.
	// No SDL audio patch is involved (an earlier draft that rewrote the MMIYOO driver was dead
	// reference code and black-screened every game).
	//
	// NOTE: on models where the daemon is unavailable the launcher falls back to direct MMIYOO,
	// and that path DOES own the codec — see PLAT_resetAudio in platform.c, which is a no-op in
	// daemon mode and performs the full teardown in direct mode.
	SetVolume(GetVolume());
	SetBrightness(GetBrightness());
}
void QuitSettings(void) {
	munmap(settings, shm_size);
	if (is_host) shm_unlink(SHM_KEY);
}
static inline void SaveSettings(void) {
	int fd = open(SettingsPath, O_CREAT|O_WRONLY, 0644);
	if (fd>=0) {
		write(fd, settings, shm_size);
		// Targeted fsync, NOT a global sync(). This runs on EVERY brightness and volume step, on
		// every wake (PWR_exitSleep -> SetVolume), and twice per process launch from InitSettings.
		// sync() flushes the entire filesystem — on a slow FAT32 SD that is a multi-hundred-ms
		// stall triggered by a keypress, and it is exactly the global-sync antipattern this fork
		// already removed on tg5040. fsync() commits this file and nothing else.
		fsync(fd);
		close(fd);
	}
}

int GetBrightness(void) { // 0-10
	return settings->brightness;
}
// REVERTED to the linear mapping. I had swapped this for OnionOS's geometric ramp
// (duty = round(3*exp(0.350656*value)), i.e. {3,4,6,9,12,17,25,35,50,70,100}) on the theory that
// it is perceptually more even. That was an unprompted change to working behaviour and it was a
// regression: it silently redefined what every EXISTING brightness setting means, making levels
// 0-7 roughly 3x dimmer (level 5 went 50 -> 17, level 3 went 30 -> 9) without the user touching
// anything. A device already set near the bottom of the range read as "the screen is broken".
// If a perceptual curve is ever wanted, it needs to migrate the stored setting at the same time,
// and it should be an explicit decision — not a silent remap.
void SetBrightness(int value) {
	SetRawBrightness(value==0?6:value*10);
	settings->brightness = value;
	SaveSettings();
}

int GetVolume(void) { // 0-20
	return settings->jack ? settings->headphones : settings->speaker;
}
void SetVolume(int value) {
	if (settings->jack) settings->headphones = value;
	else settings->speaker = value;
	
	int raw = -60 + value * 3;
	SetRawVolume(raw);
	SaveSettings();
}

void SetRawBrightness(int val) {
	int fd = open("/sys/class/pwm/pwmchip0/pwm0/duty_cycle", O_WRONLY);
	if (fd>=0) {
		dprintf(fd,"%d",val);
		close(fd);
	}
}

#define MI_AO_SETMUTE	0x4008690d
static void setMute(int flag) {
	int fd = open("/dev/mi_ao", O_RDWR); // TODO: can this be kept open?
	if (fd >= 0) {
		int buf2[] = {0, flag};
		uint64_t buf1[] = {sizeof(buf2), (uintptr_t)buf2};
		ioctl(fd, MI_AO_SETMUTE, buf1);
		close(fd);
	}
}

// Raw ioctls, same shape as setMute above. MEASURED on device: the libmi_ao API calls fail with
//   [MI ERR] MI_AO_SetVolume[4481]: Dev0 failed to set valume!!! error number:0xa0052017
//   [MI ERR] MI_AO_GetVolume[4518]: Dev0 failed to get valume!!! error number:0xa0052017
// whenever this process is not the one that brought the AO device up — which is most of the time
// now that the codec is deliberately left enabled across process boundaries. When that happened on
// the wake path (PWR_exitSleep -> SetVolume(GetVolume())) the volume was never restored from the
// -60 mute floor, and the device came back SILENT.
// The raw ioctl path works regardless of ownership (setMute already relies on it), so use it as a
// fallback. Constants confirmed independently by OnionOS (volume.h), spruceOS (mm_set_volume.py)
// and the SDL2 miyoo backend.
#define MI_AO_SETVOLUME 0x4008690b
#define MI_AO_GETVOLUME 0xc008690c
static int aoVolIoctl(unsigned long req, int* val) {
	int fd = open("/dev/mi_ao", O_RDWR);
	if (fd < 0) return 0;
	int buf2[] = {0, *val};
	uint64_t buf1[] = {sizeof(buf2), (uintptr_t)buf2};
	int r = ioctl(fd, req, buf1);
	if (r >= 0) *val = buf2[1];
	close(fd);
	return r >= 0;
}

void SetRawVolume(int val) {
	int old = 0;
	if (MI_AO_GetVolume(0, &old) != 0) {       // API path failed — try the ioctl
		int v = 0;
		if (aoVolIoctl(MI_AO_GETVOLUME, &v)) old = v;
		else old = val;                        // unknown: skip the mute-boundary transition
	}
	if (old!=val) {
			 if (val==-60) setMute(1);
		else if (old==-60) setMute(0);
	}
	if (MI_AO_SetVolume(0,val) != 0) {         // API path failed — the ioctl still works
		int v = val;
		aoVolIoctl(MI_AO_SETVOLUME, &v);
	}
}

int GetJack(void) {
	return settings->jack;
}
void SetJack(int value) {
	printf("SetJack(%i)\n", value); fflush(stdout);
	
	settings->jack = value;
	SetVolume(GetVolume());
}

int GetHDMI(void) {
	return 0;
}
void SetHDMI(int value) {
	// buh
}

int GetMute(void) { return 0; }
void SetMute(int value) {}

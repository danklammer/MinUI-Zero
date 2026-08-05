// h700 — Anbernic RG35XX Plus / RG35XX-H (Allwinner sun50iw9). V0: HOSTED-DEV platform layer.
//
// This version runs as a GUEST inside muOS (launched over ssh with muxfrontend stopped), which
// shapes several choices on purpose:
//   * present = raw fbdev (mmap + FBIOPAN_DISPLAY), the path panelprobe/hellofb proved on-device.
//     The DESTINATION present is the Allwinner DE layer API (/dev/disp, MyMinUI's h700 reference);
//     see README-BRINGUP.md. Do not let this v0 ossify.
//   * CPU/governor = LOG ONLY. A guest must not fight the host OS's governor. The schedutil
//     ceiling model ports when we own the image.
//   * powerOff = exit(0), never poweroff(2) — this is someone's running muOS session.
//   * function surface intentionally mirrors workspace/macos/platform/platform.c, the smallest
//     set proven to link against the shared api.c.
//
// Panel: 640x480 MEASURED 59.9777 Hz. fb is ARGB8888 (stride 2560), virtual 640x960 = 2 pages.
// Render surface is RGB565 like every platform; flip converts 565->XRGB while copying to the
// back page, then pans. 640x480x2 conversions are ~1.2MB of writes — fine for the launcher; games
// revisit this (NEON or the DE layer does the conversion for free).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "msettings.h"

#include "defines.h"
#include "platform.h"
#include "api.h"
#include "utils.h"

#include "scaler.h"

///////////////////////////////
// msettings: muOS owns audio/brightness while we are a guest. Brightness passes through the
// backlight sysfs when present; volume is a stub until the ALSA path lands (minui is silent).

void InitSettings(void){}
void QuitSettings(void){}

int GetBrightness(void) { return getInt("/sys/class/backlight/backlight/brightness"); }
int GetVolume(void) { return 0; }

void SetRawBrightness(int value) { putInt("/sys/class/backlight/backlight/brightness", value); }
void SetRawVolume(int value){}

void SetBrightness(int value) { SetRawBrightness(value * 10); } // 0-10 UI -> 0-100ish raw; TODO: verify range
void SetVolume(int value) {}

int GetJack(void) { return 0; }
void SetJack(int value) {}

int GetHDMI(void) { return 0; }
void SetHDMI(int value) {}

int GetMute(void) { return 0; }

///////////////////////////////

// RAW EVDEV INPUT. muOS's SDL2 delivered zero keyboard events to us (live-tested: every button
// dead), so this platform reads the hardware nodes directly and overrides the weak PLAT_pollInput
// in api.c. Minimal evdev ABI declared by hand — <linux/input.h> defines BTN_* macros that collide
// with api.h's button names (the miyoomini port hit the same wall).
//
// The code table below is CANDIDATE mappings (standard gpio-keys arrows + standard gamepad BTN_*
// codes, Anbernic A-right/B-down orientation). Every event from an UNKNOWN code is logged once, so
// the first hands-on session corrects this table from its own log. That turns the capture problem
// into normal use.
struct raw_input_event { // struct input_event, aarch64 layout
	uint64_t tv_sec;
	uint64_t tv_usec;
	uint16_t type;   // EV_KEY=1, EV_ABS=3
	uint16_t code;
	int32_t  value;  // 1=down, 0=up, 2=autorepeat
};
#define EVDEV_COUNT 3
static int ev_fds[EVDEV_COUNT] = { -1, -1, -1 };
static const char* ev_paths[EVDEV_COUNT] = {
	"/dev/input/event0", // axp2202-pek: power button
	"/dev/input/event1", // muOS-Keys (gpio-keys-polled)
	"/dev/input/event2", // dierct-keys-polled (sic)
};

static SDL_Joystick *joystick;
void PLAT_initInput(void) {
	SDL_InitSubSystem(SDL_INIT_JOYSTICK);
	joystick = SDL_JoystickOpen(0);
	for (int i = 0; i < EVDEV_COUNT; i++) {
		ev_fds[i] = open(ev_paths[i], O_RDONLY | O_NONBLOCK);
		LOG_info("evdev: %s -> fd %d\n", ev_paths[i], ev_fds[i]);
	}
}
void PLAT_quitInput(void) {
	for (int i = 0; i < EVDEV_COUNT; i++) if (ev_fds[i] >= 0) close(ev_fds[i]);
	if (joystick) SDL_JoystickClose(joystick);
	SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

// code -> (btn, id). Candidates; unknowns are logged and this table is corrected from the log.
static void ev_translate(uint16_t code, int* btn, int* id) {
	switch (code) {
	case 103: *btn = BTN_DPAD_UP;    *id = BTN_ID_DPAD_UP;    break; // KEY_UP
	case 108: *btn = BTN_DPAD_DOWN;  *id = BTN_ID_DPAD_DOWN;  break; // KEY_DOWN
	case 105: *btn = BTN_DPAD_LEFT;  *id = BTN_ID_DPAD_LEFT;  break; // KEY_LEFT
	case 106: *btn = BTN_DPAD_RIGHT; *id = BTN_ID_DPAD_RIGHT; break; // KEY_RIGHT
	case 305: *btn = BTN_A;      *id = BTN_ID_A;      break; // BTN_EAST  (right = A, Anbernic)
	case 304: *btn = BTN_B;      *id = BTN_ID_B;      break; // BTN_SOUTH (bottom = B)
	case 307: *btn = BTN_X;      *id = BTN_ID_X;      break; // BTN_NORTH
	case 308: *btn = BTN_Y;      *id = BTN_ID_Y;      break; // BTN_WEST
	case 310: *btn = BTN_L1;     *id = BTN_ID_L1;     break; // BTN_TL
	case 311: *btn = BTN_R1;     *id = BTN_ID_R1;     break; // BTN_TR
	case 312: *btn = BTN_L2;     *id = BTN_ID_L2;     break; // BTN_TL2
	case 313: *btn = BTN_R2;     *id = BTN_ID_R2;     break; // BTN_TR2
	case 314: *btn = BTN_SELECT; *id = BTN_ID_SELECT; break; // BTN_SELECT
	case 315: *btn = BTN_START;  *id = BTN_ID_START;  break; // BTN_START
	case 316: *btn = BTN_MENU;   *id = BTN_ID_MENU;   break; // BTN_MODE
	case 116: *btn = BTN_POWER;  *id = BTN_ID_POWER;  break; // KEY_POWER
	case 115: *btn = BTN_PLUS;   *id = BTN_ID_PLUS;   break; // KEY_VOLUMEUP
	case 114: *btn = BTN_MINUS;  *id = BTN_ID_MINUS;  break; // KEY_VOLUMEDOWN
	default:  *btn = BTN_NONE;   *id = -1;            break;
	}
}

void PLAT_pollInput(void) {
	pad.just_pressed  = BTN_NONE;
	pad.just_released = BTN_NONE;
	pad.just_repeated = BTN_NONE;

	uint32_t tick = SDL_GetTicks();
	for (int i = 0; i < BTN_ID_COUNT; i++) {
		int btn = 1 << i;
		if ((pad.is_pressed & btn) && (tick >= pad.repeat_at[i])) {
			pad.just_repeated |= btn;
			pad.repeat_at[i] += PAD_REPEAT_INTERVAL;
		}
	}

	SDL_PumpEvents();
	SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT); // SDL is not our input source here

	static uint32_t seen_unknown[8]; // log each unknown code once, not per event
	struct raw_input_event ev;
	for (int i = 0; i < EVDEV_COUNT; i++) {
		if (ev_fds[i] < 0) continue;
		while (read(ev_fds[i], &ev, sizeof(ev)) == sizeof(ev)) {
			if (ev.type != 1) continue;      // EV_KEY only (EV_ABS sticks come later)
			if (ev.value == 2) continue;     // autorepeat: we do our own
			int btn, id;
			ev_translate(ev.code, &btn, &id);
			if (btn == BTN_NONE) {
				int slot = ev.code & 7;
				if (seen_unknown[slot] != ev.code) {
					seen_unknown[slot] = ev.code;
					LOG_info("evdev: UNKNOWN key code=%u value=%d (%s) — add to ev_translate\n",
						ev.code, ev.value, ev_paths[i]);
				}
				continue;
			}
			if (ev.value) {
				pad.is_pressed   |= btn;
				pad.just_pressed |= btn;
				pad.repeat_at[id] = tick + PAD_REPEAT_DELAY;
			}
			else {
				pad.is_pressed    &= ~btn;
				pad.just_released |= btn;
			}
		}
	}
}

///////////////////////////////

static struct VID_Context {
	SDL_Surface* screen;   // RGB565 render target handed to the frontend
	GFX_Renderer* blit;

	int fdfb;
	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;
	uint8_t* fbmmap;
	size_t page_bytes;
	int pages;
	int page;              // page we will draw the NEXT frame into

	int width;
	int height;
	int pitch;
} vid;

SDL_Surface* PLAT_initVideo(void) {
	// SDL video first, for its input plumbing (keyboard events ride the video subsystem; the MMP
	// taught us buttons die without it). muOS's SDL2 is Mali-fbdev; if it refuses because we are
	// not the fb owner of record, keep going — we present through fbdev ourselves regardless.
	if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_EVENTS | SDL_INIT_VIDEO) < 0) {
		LOG_info("SDL video init failed (%s) — input may be joystick-only\n", SDL_GetError());
		SDL_Init(SDL_INIT_TIMER | SDL_INIT_EVENTS);
	}

	vid.fdfb = open("/dev/fb0", O_RDWR);
	if (vid.fdfb < 0) { LOG_error("fb0: %s\n", strerror(errno)); return NULL; }
	ioctl(vid.fdfb, FBIOGET_VSCREENINFO, &vid.vinfo);
	ioctl(vid.fdfb, FBIOGET_FSCREENINFO, &vid.finfo);

	// Trust what the driver reports (recon: 640x480, virtual x960, 32bpp, stride 2560).
	vid.pages = vid.vinfo.yres ? (int)(vid.vinfo.yres_virtual / vid.vinfo.yres) : 1;
	if (vid.pages < 1) vid.pages = 1;
	if (vid.pages > 2) vid.pages = 2;
	vid.page_bytes = (size_t)vid.finfo.line_length * vid.vinfo.yres;
	vid.fbmmap = mmap(NULL, vid.page_bytes * vid.pages, PROT_READ|PROT_WRITE, MAP_SHARED, vid.fdfb, 0);
	if (vid.fbmmap == MAP_FAILED) { LOG_error("fb mmap: %s\n", strerror(errno)); return NULL; }
	LOG_info("fb: %ux%u %ubpp stride=%u pages=%d\n", vid.vinfo.xres, vid.vinfo.yres,
		vid.vinfo.bits_per_pixel, vid.finfo.line_length, vid.pages);

	vid.screen = SDL_CreateRGBSurface(SDL_SWSURFACE, FIXED_WIDTH, FIXED_HEIGHT, FIXED_DEPTH, RGBA_MASK_565);
	vid.width  = FIXED_WIDTH;
	vid.height = FIXED_HEIGHT;
	vid.pitch  = FIXED_PITCH;

	// Draw into the page the panel is NOT scanning. The MMP's whole tearing saga came from
	// nothing tracking the front page; with 2 pages and a SYNCHRONOUS pan (blocks ~16.7ms,
	// measured) the alternation IS the ownership invariant — MyMinUI's model, safe by structure.
	int front = (vid.vinfo.yres && vid.vinfo.yoffset % vid.vinfo.yres == 0)
		? (int)(vid.vinfo.yoffset / vid.vinfo.yres) : 0;
	vid.page = (vid.pages > 1) ? !front : 0;

	return vid.screen;
}

// RGB565 -> XRGB8888 while copying into a fb page. Plain C: the launcher redraws on input, not
// per-frame, so this is nowhere near hot. Games get NEON or the DE layer.
static void fb_blit565(int page) {
	uint16_t* src = (uint16_t*)vid.screen->pixels;
	int src_stride = vid.screen->pitch / 2;
	uint32_t* dst = (uint32_t*)(vid.fbmmap + (size_t)page * vid.page_bytes);
	int dst_stride = vid.finfo.line_length / 4;
	int w = vid.vinfo.xres < (unsigned)vid.screen->w ? vid.vinfo.xres : vid.screen->w;
	int h = vid.vinfo.yres < (unsigned)vid.screen->h ? vid.vinfo.yres : vid.screen->h;
	for (int y = 0; y < h; y++) {
		uint16_t* s = src + y * src_stride;
		uint32_t* d = dst + y * dst_stride;
		for (int x = 0; x < w; x++) {
			uint16_t p = s[x];
			uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
			d[x] = 0xFF000000 | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
		}
	}
}

static void fb_pan(int page) {
	vid.vinfo.yoffset = vid.vinfo.yres * page;
	vid.vinfo.activate = FB_ACTIVATE_VBL;
	ioctl(vid.fdfb, FBIOPAN_DISPLAY, &vid.vinfo); // blocks ~16.7ms (measured): our vsync
}

void PLAT_quitVideo(void) {
	if (vid.screen) SDL_FreeSurface(vid.screen);
	if (vid.fbmmap && vid.fbmmap != MAP_FAILED) munmap(vid.fbmmap, vid.page_bytes * vid.pages);
	if (vid.fdfb >= 0) close(vid.fdfb);
	SDL_Quit();
}

void PLAT_clearVideo(SDL_Surface* screen) {
	SDL_FillRect(screen, NULL, 0);
}
void PLAT_clearAll(void) {
	PLAT_clearVideo(vid.screen);
	// Zero only the page WE own next; the pan in the next flip retires the other. Never memset
	// the whole mmap — that writes into live scanout (the MMP's PLAT_clearAll lesson).
	if (vid.fbmmap && vid.fbmmap != MAP_FAILED)
		memset(vid.fbmmap + (size_t)vid.page * vid.page_bytes, 0, vid.page_bytes);
}

void PLAT_setVsync(int vsync) {
	// the synchronous pan is the vsync; nothing to configure in v0
}

SDL_Surface* PLAT_resizeVideo(int w, int h, int p) {
	// v0 launcher never resizes (fixed 640x480). Games will need this; see README order-of-work.
	return vid.screen;
}

void PLAT_setVideoScaleClip(int x, int y, int width, int height) {}
void PLAT_setNearestNeighbor(int enabled) {}
void PLAT_setSharpness(int sharpness) {}

void PLAT_vsync(int remaining) {
	// The pan blocks a full refresh already; only burn what pacing asks for beyond that.
	if (remaining > 0) SDL_Delay(remaining);
}

scaler_t PLAT_getScaler(GFX_Renderer* renderer) {
	return scale1x1_c16;
}

void PLAT_blitRenderer(GFX_Renderer* renderer) {
	vid.blit = renderer;
	scale1x1_c16(
		renderer->src, renderer->dst,
		renderer->true_w, renderer->true_h, renderer->src_p,
		vid.screen->w, vid.screen->h, vid.screen->pitch
	);
}

void PLAT_flip(SDL_Surface* IGNORED, int ignored) {
	fb_blit565(vid.page);
	fb_pan(vid.page);                        // synchronous: returns with the page on its way out
	if (vid.pages > 1) vid.page = !vid.page; // strict alternation = ownership by structure
	vid.blit = NULL;
}

///////////////////////////////

#define OVERLAY_WIDTH PILL_SIZE
#define OVERLAY_HEIGHT PILL_SIZE
#define OVERLAY_BPP 4
#define OVERLAY_DEPTH 16
#define OVERLAY_PITCH (OVERLAY_WIDTH * OVERLAY_BPP)
#define OVERLAY_RGBA_MASK 0x00ff0000,0x0000ff00,0x000000ff,0xff000000
static struct OVL_Context {
	SDL_Surface* overlay;
} ovl;

SDL_Surface* PLAT_initOverlay(void) {
	ovl.overlay = SDL_CreateRGBSurface(SDL_SWSURFACE, SCALE2(OVERLAY_WIDTH,OVERLAY_HEIGHT),OVERLAY_DEPTH,OVERLAY_RGBA_MASK);
	return ovl.overlay;
}
void PLAT_quitOverlay(void) {
	if (ovl.overlay) SDL_FreeSurface(ovl.overlay);
}
void PLAT_enableOverlay(int enable) {}

///////////////////////////////
// AXP2202 — the Brick's exact PMIC, verified on-device 2026-08-04 (identical sysfs paths).

static int online = 0;
void PLAT_getBatteryStatus(int* is_charging, int* charge) {
	*is_charging = getInt("/sys/class/power_supply/axp2202-usb/online");

	int i = getInt("/sys/class/power_supply/axp2202-battery/capacity");
	// worry less about battery and more about the game you're playing
	     if (i>80) *charge = 100;
	else if (i>60) *charge =  80;
	else if (i>40) *charge =  60;
	else if (i>20) *charge =  40;
	else if (i>10) *charge =  20;
	else           *charge =  10;
}

void PLAT_enableBacklight(int enable) {
	// TODO: find the true backlight node on this device; guarded so a wrong path is a no-op
	if (exists("/sys/class/backlight/backlight/bl_power"))
		putInt("/sys/class/backlight/backlight/bl_power", enable ? 0 : 4);
}

void PLAT_powerOff(void) {
	// GUEST MODE: we are a visitor inside muOS. Exiting hands the console back to the host
	// frontend; powering off the host from a dev harness would be hostile.
	SND_quit();
	VIB_quit();
	PWR_quit();
	GFX_quit();
	exit(0);
}

///////////////////////////////

void PLAT_setCPUSpeed(int speed) {
	// GUEST MODE: log intent, touch nothing. muOS owns the governor while it hosts us. The
	// schedutil ceiling model (tg5040) ports when we own the image — schedutil is confirmed
	// available on this SoC.
	LOG_info("PLAT_setCPUSpeed(%d) — guest mode, not applied\n", speed);
}

void PLAT_setCPUMaxFreq(int khz) {
	LOG_info("PLAT_setCPUMaxFreq: %d kHz — guest mode, not applied\n", khz);
}

void PLAT_setRumble(int strength) {
	// no rumble hardware reported in recon
}

int PLAT_pickSampleRate(int requested, int max) {
	return MIN(requested, max);
}

// minarch-only surface, v0 stubs
void PLAT_setEffect(int effect) {} // no scanline/DMG effects on the fbdev path yet
void PLAT_setDebugOverlay(uint16_t* top, uint16_t* bottom, int w, int h, int stride) {} // HUD later
void PLAT_getGameRect(int* x, int* y, int* w, int* h) {
	// v0 presents full-screen 640x480; refine when scaling modes land
	if (x) *x = 0; if (y) *y = 0; if (w) *w = FIXED_WIDTH; if (h) *h = FIXED_HEIGHT;
}

char* PLAT_getModel(void) {
	// TODO: distinguish RG35XX Plus vs H (near-twins; likely a DT compatible string or a key count)
	return "Anbernic RG35XX";
}

int PLAT_isOnline(void) {
	return online;
}

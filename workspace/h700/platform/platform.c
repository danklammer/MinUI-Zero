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

// code -> (btn, id). MEASURED 2026-08-05: counted-press session on the RG35XX Plus (each button
// pressed a distinct number of times, decoded from count + temporal order). The canonical evdev
// names are WRONG on this hardware — 315 "BTN_START" is R2, 314 "BTN_SELECT" is L2, 310 "BTN_TL"
// is Select — so this table is receipts, not <input-event-codes.h>. The MENU key emits 312 AND 354
// together; both map to BTN_MENU (same bit, harmless).
// The dpad is an ANALOG HAT (EV_ABS codes 16/17), handled separately in PLAT_pollInput.
static void ev_translate(uint16_t code, int* btn, int* id) {
	switch (code) {
	case 304: *btn = BTN_A;      *id = BTN_ID_A;      break;
	case 305: *btn = BTN_B;      *id = BTN_ID_B;      break;
	case 307: *btn = BTN_X;      *id = BTN_ID_X;      break;
	case 306: *btn = BTN_Y;      *id = BTN_ID_Y;      break;
	case 308: *btn = BTN_L1;     *id = BTN_ID_L1;     break;
	case 309: *btn = BTN_R1;     *id = BTN_ID_R1;     break;
	case 314: *btn = BTN_L2;     *id = BTN_ID_L2;     break;
	case 315: *btn = BTN_R2;     *id = BTN_ID_R2;     break;
	case 311: *btn = BTN_START;  *id = BTN_ID_START;  break;
	case 310: *btn = BTN_SELECT; *id = BTN_ID_SELECT; break;
	case 354: *btn = BTN_MENU;   *id = BTN_ID_MENU;   break;
	case 312: *btn = BTN_MENU;   *id = BTN_ID_MENU;   break; // MENU companion code
	case 116: *btn = BTN_POWER;  *id = BTN_ID_POWER;  break; // KEY_POWER (event0, axp2202-pek)
	case 115: *btn = BTN_PLUS;   *id = BTN_ID_PLUS;   break; // KEY_VOLUMEUP
	case 114: *btn = BTN_MINUS;  *id = BTN_ID_MINUS;  break; // KEY_VOLUMEDOWN
	default:  *btn = BTN_NONE;   *id = -1;            break;
	}
}

// The dpad hat: EV_ABS code 16 (X: -1 left / +1 right) and 17 (Y: -1 up / +1 down), 0 = released.
static void ev_hat(uint16_t code, int32_t value, uint32_t tick) {
	int neg_btn, neg_id, pos_btn, pos_id;
	if (code == 16) { neg_btn = BTN_DPAD_LEFT; neg_id = BTN_ID_DPAD_LEFT; pos_btn = BTN_DPAD_RIGHT; pos_id = BTN_ID_DPAD_RIGHT; }
	else if (code == 17) { neg_btn = BTN_DPAD_UP; neg_id = BTN_ID_DPAD_UP; pos_btn = BTN_DPAD_DOWN; pos_id = BTN_ID_DPAD_DOWN; }
	else { LOG_info("hat: ignored ABS code=%u value=%d\n", code, value); return; }
	LOG_info("hat: code=%u value=%d\n", code, value);
	// release whichever direction is no longer held, press whichever is
	if (value <= 0 && (pad.is_pressed & pos_btn)) { pad.is_pressed &= ~pos_btn; pad.just_released |= pos_btn; }
	if (value >= 0 && (pad.is_pressed & neg_btn)) { pad.is_pressed &= ~neg_btn; pad.just_released |= neg_btn; }
	if (value < 0 && !(pad.is_pressed & neg_btn)) { pad.is_pressed |= neg_btn; pad.just_pressed |= neg_btn; pad.repeat_at[neg_id] = tick + PAD_REPEAT_DELAY; }
	if (value > 0 && !(pad.is_pressed & pos_btn)) { pad.is_pressed |= pos_btn; pad.just_pressed |= pos_btn; pad.repeat_at[pos_id] = tick + PAD_REPEAT_DELAY; }
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

	// Log EVERY key-down once per distinct code — mapped or not — so a single press-everything
	// session yields the complete map. (The first version deduped by code&7 and codes silenced
	// each other; a press session came back with 2 of ~15 codes. Receipts require a real set.)
	struct raw_input_event ev;
	for (int i = 0; i < EVDEV_COUNT; i++) {
		if (ev_fds[i] < 0) continue;
		while (read(ev_fds[i], &ev, sizeof(ev)) == sizeof(ev)) {
			if (ev.type == 3) { ev_hat(ev.code, ev.value, tick); continue; } // dpad hat
			if (ev.type != 1) continue;      // EV_KEY otherwise
			if (ev.value == 2) continue;     // autorepeat: we do our own
			int btn, id;
			ev_translate(ev.code, &btn, &id);
			if (btn == BTN_NONE) continue;
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
	// center a smaller-than-panel surface (scaled game); borders were cleared at resize
	int ox = ((int)vid.vinfo.xres - w) / 2; if (ox < 0) ox = 0;
	int oy = ((int)vid.vinfo.yres - h) / 2; if (oy < 0) oy = 0;
	for (int y = 0; y < h; y++) {
		uint16_t* s = src + y * src_stride;
		uint32_t* d = dst + (y + oy) * dst_stride + ox;
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
	// minarch resizes the render surface to the SCALED game size (e.g. 480x432 for GBC at 3x) and
	// derives its row pitch from it. Ignoring this was the shear-and-repeat glitch: minarch wrote
	// 480-wide rows into a 640-wide surface. Recreate the surface at the requested size; the flip
	// centers it on the panel.
	if (w == vid.width && h == vid.height && p == vid.pitch) return vid.screen;
	LOG_info("resizeVideo(%d,%d,%d)\n", w, h, p);
	if (vid.screen) SDL_FreeSurface(vid.screen);
	vid.screen = SDL_CreateRGBSurface(SDL_SWSURFACE, w, h, FIXED_DEPTH, RGBA_MASK_565);
	vid.width = w; vid.height = h; vid.pitch = vid.screen->pitch;
	// geometry changed: scrub both fb pages so old borders do not linger (one brief artifact
	// beats permanent letterbox garbage; the proper deferred scrub comes with the DE-layer work)
	if (vid.fbmmap && vid.fbmmap != MAP_FAILED) memset(vid.fbmmap, 0, vid.page_bytes * vid.pages);
	return vid.screen;
}

void PLAT_setVideoScaleClip(int x, int y, int width, int height) {}
void PLAT_setNearestNeighbor(int enabled) {}
void PLAT_setSharpness(int sharpness) {}

void PLAT_vsync(int remaining) {
	// The pan blocks a full refresh already; only burn what pacing asks for beyond that.
	if (remaining > 0) SDL_Delay(remaining);
}

// Integer scaling + centering, the MMP pattern exactly: minarch computes the scale and dst rect;
// the platform honors both. (v0 ignored them — the first game ran 1:1 in the corner.)
scaler_t PLAT_getScaler(GFX_Renderer* renderer) {
	switch (renderer->scale) {
		case 6:  return scale6x6_c16;
		case 5:  return scale5x5_c16;
		case 4:  return scale4x4_c16;
		case 3:  return scale3x3_c16;
		case 2:  return scale2x2_c16;
		default: return scale1x1_c16;
	}
}

void PLAT_blitRenderer(GFX_Renderer* renderer) {
	vid.blit = renderer;
	void* dst = renderer->dst + (renderer->dst_y * renderer->dst_p) + (renderer->dst_x * FIXED_BPP);
	((scaler_t)renderer->blit)(renderer->src, dst,
		renderer->src_w, renderer->src_h, renderer->src_p,
		renderer->dst_w, renderer->dst_h, renderer->dst_p);
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

// Charging inhibits autosleep (same behaviour as the Brick). Crucial in hosted dev: the 30s
// autosleep was ending input sessions before anyone pressed a button.
int PLAT_isToppingUp(void) {
	// HOSTED-DEV: report always-topping-up so autosleep never fires. Twice tonight the 30s
	// autosleep ate an input session (screen slept, the wake tap fell into the power-off flow).
	// The real charging read returns with the image build, where sleep policy matters.
	return 1;
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

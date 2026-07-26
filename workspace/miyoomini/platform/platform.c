// miyoomini
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <linux/fb.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <msettings.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "defines.h"
#include "platform.h"
#include "api.h"
#include "utils.h"
#include "scaler.h"

///////////////////////////////
// based on eggs GFXSample_rev15

#include <mi_sys.h>
#include <mi_gfx.h>
#include <mi_ao.h>   // MI_AO_Disable/DisableChn for PLAT_resetAudio

int is_560p = 0;
int is_plus = 0;
// has_axp: battery/charge come from the AXP PMIC over i2c (Plus AND Flip) rather than the
// original Mini's gpio59. Allium ships all three models off one card and maps the Flip to the
// SAME battery implementation as the Plus (Miyoo354Battery); only the original Mini gets its
// own. Keying this off is_plus alone would put a Flip on the original Mini path.
int has_axp = 0;

// Which of the TWO MMA render buffers (PAGE_COUNT==2) the frontend is currently drawing into.
// Deliberately independent of vid.page, which indexes the up-to-THREE scanout pages: indexing
// this 2-entry buffer with vid.page walks off the end of the allocation when vid.page==2.
static int mma_page = 0;

#define ALIGN4K(val)	((val+4095)&(~4095))

// NOTE: these 16bpp render targets use RGBA_MASK_565 — SDL2 requires real channel
// masks; RGBA_MASK_AUTO (0,0,0,0) was an SDL 1.2 convenience and fails there.
// SDL 1.2 let the upstream code stash the MMA physical address in SDL_Surface::unused1
// (`#define pixelsPa unused1`). SDL2 removed that field, so keep a tiny side table instead —
// only the render target and the framebuffer-page wrapper ever need a physical address.
#define PA_SLOTS 4
static struct { SDL_Surface* s; MI_PHY pa; } pa_tbl[PA_SLOTS];
static void surf_setPa(SDL_Surface* s, MI_PHY pa) {
	if (!s) return;
	for (int i=0;i<PA_SLOTS;i++) if (pa_tbl[i].s==s) { pa_tbl[i].pa=pa; return; }
	for (int i=0;i<PA_SLOTS;i++) if (!pa_tbl[i].s)   { pa_tbl[i].s=s; pa_tbl[i].pa=pa; return; }
}
static MI_PHY surf_getPa(SDL_Surface* s) {
	if (!s) return 0;
	for (int i=0;i<PA_SLOTS;i++) if (pa_tbl[i].s==s) return pa_tbl[i].pa;
	return 0;
}
static void surf_clearPa(SDL_Surface* s) {
	for (int i=0;i<PA_SLOTS;i++) if (pa_tbl[i].s==s) { pa_tbl[i].s=NULL; pa_tbl[i].pa=0; }
}

//
//	Get GFX_ColorFmt from SDL_Surface
//
static inline MI_GFX_ColorFmt_e	GFX_ColorFmt(SDL_Surface *surface) {
	if (surface) {
		if (surface->format->BytesPerPixel == 2) {
			if (surface->format->Amask == 0x0000) return E_MI_GFX_FMT_RGB565;
			if (surface->format->Amask == 0x8000) return E_MI_GFX_FMT_ARGB1555;
			if (surface->format->Amask == 0xF000) return E_MI_GFX_FMT_ARGB4444;
			if (surface->format->Amask == 0x0001) return E_MI_GFX_FMT_RGBA5551;
			if (surface->format->Amask == 0x000F) return E_MI_GFX_FMT_RGBA4444;
			return E_MI_GFX_FMT_RGB565;
		}
		if (surface->format->Bmask == 0x000000FF) return E_MI_GFX_FMT_ARGB8888;
		if (surface->format->Rmask == 0x000000FF) return E_MI_GFX_FMT_ABGR8888;
	}
	return E_MI_GFX_FMT_ARGB8888;
}

//
//	Flush write cache of needed segments
//		x and w are not considered since 4K units
//
// Range of the fbdev mmap. MI_SYS_FlushInvCache is only valid for MMA-allocated memory, and the
// framebuffer is not: flushing it takes down the process on the first present.
static void*  g_fb_base = NULL;
static size_t g_fb_len  = 0;

static inline void FlushCacheNeeded(void* pixels, uint32_t pitch, uint32_t y, uint32_t h) {
	if (g_fb_base && (uintptr_t)pixels >= (uintptr_t)g_fb_base
	              && (uintptr_t)pixels <  (uintptr_t)g_fb_base + g_fb_len) return; // fb: not MMA
	uintptr_t pixptr = (uintptr_t)pixels;
	uintptr_t startaddress = (pixptr + pitch*y)&(~4095);
	uint32_t size = ALIGN4K(pixptr + pitch*(y+h)) - startaddress;
	if (size) MI_SYS_FlushInvCache((void*)startaddress, size);
}

//
//	GFX BlitSurface (MI_GFX ver) / in place of SDL_BlitSurface
//		with scale/bpp convert and rotate/mirror
//		rotate : 1 = 90 / 2 = 180 / 3 = 270
//		mirror : 1 = Horizontal / 2 = Vertical / 3 = Both
//		nowait : 0 = wait until done / 1 = no wait
//
static inline void GFX_BlitSurfaceExec(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect, uint32_t rotate, uint32_t mirror, uint32_t nowait) {
	MI_PHY srcPa = surf_getPa(src), dstPa = surf_getPa(dst);
	if ((src)&&(dst)&&(srcPa)&&(dstPa)) {
		MI_GFX_Surface_t Src;
		MI_GFX_Surface_t Dst;
		MI_GFX_Rect_t SrcRect;
		MI_GFX_Rect_t DstRect;
		MI_GFX_Opt_t Opt;
		MI_U16 Fence;

		Src.phyAddr = srcPa;
		Src.u32Width = src->w;
		Src.u32Height = src->h;
		Src.u32Stride = src->pitch;
		Src.eColorFmt = GFX_ColorFmt(src);
		if (srcrect) {
			SrcRect.s32Xpos = srcrect->x;
			SrcRect.s32Ypos = srcrect->y;
			SrcRect.u32Width = srcrect->w;
			SrcRect.u32Height = srcrect->h;
		} else {
			SrcRect.s32Xpos = 0;
			SrcRect.s32Ypos = 0;
			SrcRect.u32Width = Src.u32Width;
			SrcRect.u32Height = Src.u32Height;
		}
		FlushCacheNeeded(src->pixels, src->pitch, SrcRect.s32Ypos, SrcRect.u32Height);

		Dst.phyAddr = dstPa;
		Dst.u32Width = dst->w;
		Dst.u32Height = dst->h;
		Dst.u32Stride = dst->pitch;
		Dst.eColorFmt = GFX_ColorFmt(dst);
		if (dstrect) {
			DstRect.s32Xpos = dstrect->x;
			DstRect.s32Ypos = dstrect->y;
			if (dstrect->w|dstrect->h) {
				DstRect.u32Width = dstrect->w;
				DstRect.u32Height = dstrect->h;
			} else {
				DstRect.u32Width = SrcRect.u32Width;
				DstRect.u32Height = SrcRect.u32Height;
			}
		} else {
			DstRect.s32Xpos = 0;
			DstRect.s32Ypos = 0;
			DstRect.u32Width = Dst.u32Width;
			DstRect.u32Height = Dst.u32Height;
		}
		if (rotate & 1) FlushCacheNeeded(dst->pixels, dst->pitch, DstRect.s32Ypos, DstRect.u32Width);
		else FlushCacheNeeded(dst->pixels, dst->pitch, DstRect.s32Ypos, DstRect.u32Height);

		memset(&Opt, 0, sizeof(Opt));
		// SDL2: blend/colorkey state moved off SDL_Surface::flags and format->{alpha,colorkey}
		// onto accessors. The present path is an opaque blit, so both are usually inactive.
		SDL_BlendMode bmode = SDL_BLENDMODE_NONE;
		SDL_GetSurfaceBlendMode(src, &bmode);
		if (bmode != SDL_BLENDMODE_NONE) {
			Uint8 amod = SDL_ALPHA_OPAQUE;
			SDL_GetSurfaceAlphaMod(src, &amod);
			Opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_INVSRCALPHA;
			if (amod != SDL_ALPHA_OPAQUE && src->format->Amask) {
				Opt.u32GlobalSrcConstColor = (amod << (src->format->Ashift - src->format->Aloss)) & src->format->Amask;
				Opt.eDFBBlendFlag = (MI_Gfx_DfbBlendFlags_e)
						   (E_MI_GFX_DFB_BLEND_SRC_PREMULTIPLY | E_MI_GFX_DFB_BLEND_COLORALPHA | E_MI_GFX_DFB_BLEND_ALPHACHANNEL);
			} else	Opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_SRC_PREMULTIPLY;
		}
		{
			Uint32 ckey;
			if (SDL_HasColorKey(src) && SDL_GetColorKey(src, &ckey) == 0) {
				Opt.stSrcColorKeyInfo.bEnColorKey = TRUE;
				Opt.stSrcColorKeyInfo.eCKeyFmt = Src.eColorFmt;
				Opt.stSrcColorKeyInfo.eCKeyOp = E_MI_GFX_RGB_OP_EQUAL;
				Opt.stSrcColorKeyInfo.stCKeyVal.u32ColorStart =
				Opt.stSrcColorKeyInfo.stCKeyVal.u32ColorEnd = ckey;
			}
		}
		Opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
		Opt.eRotate = (MI_GFX_Rotate_e)rotate;
		Opt.eMirror = (MI_GFX_Mirror_e)mirror;
		Opt.stClipRect.s32Xpos = dst->clip_rect.x;
		Opt.stClipRect.s32Ypos = dst->clip_rect.y;
		Opt.stClipRect.u32Width = dst->clip_rect.w;
		Opt.stClipRect.u32Height = dst->clip_rect.h;

		MI_GFX_BitBlit(&Src, &SrcRect, &Dst, &DstRect, &Opt, &Fence);
		if (!nowait) MI_GFX_WaitAllDone(FALSE, Fence);
	} else SDL_BlitSurface(src, srcrect, dst, dstrect);
}

///////////////////////////////

#define LID_PATH "/sys/devices/soc0/soc/soc:hall-mh248/hallvalue"
void PLAT_initLid(void) {
	lid.has_lid = exists(LID_PATH);
}
int PLAT_lidChanged(int* state) {
	if (lid.has_lid) {
		int lid_open = getInt(LID_PATH);
		if (lid_open!=lid.is_open) {
			lid.is_open = lid_open;
			if (state) *state = lid_open;
			return 1;
		}
	}
	return 0;
}

// Minimal evdev ABI. We deliberately do NOT include <linux/input.h>: it defines BTN_* macros that
// collide with MinUI's own BTN_* button names in api.h.
struct zero_input_event { struct timeval time; unsigned short type; unsigned short code; int value; };
#define ZERO_EV_KEY 0x01

static pthread_t input_pt;
static int input_run = 0;
static void* input_thread(void* arg) {
	int fd = open("/dev/input/event0", O_RDONLY);
	if (fd < 0) { LOG_info("input: cannot open /dev/input/event0\n"); return NULL; }
	struct zero_input_event ev;
	while (input_run) {
		int r = read(fd, &ev, sizeof(ev));
		if (r != sizeof(ev)) { if (r < 0 && errno == EINTR) continue; break; }
		if (ev.type != ZERO_EV_KEY) continue;
		if (ev.value != 0 && ev.value != 1) continue; // ignore key repeat (value 2)
		SDL_Event e;
		memset(&e, 0, sizeof(e));
		e.type = ev.value ? SDL_KEYDOWN : SDL_KEYUP;
		e.key.state = ev.value ? SDL_PRESSED : SDL_RELEASED;
		// api.c compares keysym.sym against platform.h's CODE_* — which are EVDEV codes.
		e.key.keysym.sym = (SDL_Keycode)ev.code;
		e.key.keysym.scancode = (SDL_Scancode)ev.code;
		SDL_PushEvent(&e);
	}
	close(fd);
	return NULL;
}

// MI_AO hardware mute. Same ioctl libmsettings uses for volume 0. Unmute is deferred slightly:
// the DAC needs a moment after being enabled before it is safe to let signal through, otherwise
// we just move the pop rather than remove it.
//
// keepAudioOpen is 0. It WAS 1: SDL2's MMIYOO driver calls MI_AO_Disable/DisableChn on close, that
// power-down is the exit pop, and leaving the codec enabled did genuinely remove it (confirmed on
// device). But it breaks the NEXT open outright — MEASURED:
//     MI_AO_SetPubAttr[3364]: Dev0 failed to set pub attr!!! error number:0xa0052009
//     SDL_OpenAudio error:  — audio disabled
//     MI_AO_Enable[3413]: Dev0 has not be set pub attribute.
// MI_AO_SetPubAttr cannot reconfigure a device that is still ENABLED, and the SDL driver calls it
// unconditionally on every open. So the trade was: no exit pop, but no audio at all in the next
// game. Silence is far worse than a click, so the codec closes normally again.
//
// The pop is still worth solving, but not this way. The remaining levers are the mute discipline
// around open/close (kept, see SND_init/SND_pause) and — if that is not enough — a persistent
// holder process that owns MI_AO for the whole session, which is how every other CFW on this SoC
// does it. Do not simply flip this back to 1 without handling SetPubAttr.
// 0 = close the codec when a game exits (i.e. MI_AO_Disable runs).
//
// This costs an audible pop on exit, and we know exactly why: the pop IS the analog power-down,
// CONFIRMED BY EAR — holding the codec open made the exit pop vanish completely. Muting and
// rail-settling around the disable does nothing, because the data path was never the problem.
//
// It is still 0, because holding it open breaks the NEXT launch: game 1 has sound, game 2 is
// silent (confirmed by ear, and the same failure that forced an earlier revert). The SigmaStar
// SDL driver calls MI_AO_SetPubAttr on every open, and that cannot reconfigure a device which is
// still enabled — so the second process opens "successfully" and produces nothing.
//
// Silence is worse than a click, so this stays 0 until the second-launch path is actually solved.
// Solving it means one of:
//   * patch the SDL MMIYOO audio driver to skip reconfiguration when the attrs already match, or
//   * own MI_AO in a long-lived process (which is precisely what the stock audioserver does, and
//     why stock never pops; we cannot reuse audioserver because libpadsp segfaults binaries built
//     by this toolchain).
// AND it means building a way to verify AUDIBILITY automatically. The bench only asserts that
// SDL_OpenAudio returned 0, which is not the same claim as "sound reaches the speaker" — that gap
// is what let this regression ship twice.
int PLAT_keepAudioOpen(void) { return 0; }

// Force the AO device back to a closed state so a fresh SDL_OpenAudio can succeed.
//
// MEASURED (automated bench, 5 systems back to back): the FIRST game gets audio, every one after
// it fails with
//     MI_AO_SetPubAttr[3364]: Dev0 failed to set pub attr!!! error number:0xa0052009
//     MI_AO_DisableChn[3667]: Dev0 has not been enabled.
//     SDL_OpenAudio error: — audio disabled
// MI_AO_SetPubAttr cannot reconfigure a device that is still ENABLED. Any abnormal exit — a crash,
// a SIGKILL, a force-quit — skips SND_quit, so the codec stays enabled and EVERY later game is
// silent until reboot. Nothing in the normal path repairs it.
// Disabling is safe when the device is already disabled (the driver just logs "has not been
// enabled" and returns), so this is idempotent and cheap to call on the failure path.
void PLAT_resetAudio(void) {
	// Mute and let the rail settle BEFORE removing power. Disabling a live output stage is a step
	// discontinuity — the pop. The enable path mutes first for the same reason; this is its mirror.
	// PLAT_muteAudio already carries the settle delay, so no extra sleep here.
	PLAT_muteAudio(1);
	MI_AO_DisableChn(0, 0);
	MI_AO_Disable(0);
}

#define MI_AO_SETMUTE_IOCTL 0x4008690d
// Rail-settle window, both directions. The enable path in libmsettings already waits ~40ms
// between bringing the channel up and unmuting, for exactly this reason: the analog stage does
// not follow the digital gate instantly.
#define MI_AO_SETTLE_US 40000
void PLAT_muteAudio(int mute) {
	int fd = open("/dev/mi_ao", O_RDWR);
	if (fd < 0) return;
	if (!mute) usleep(60000); // let the rail settle before unmuting
	int buf2[] = {0, mute ? 1 : 0};
	uint64_t buf1[] = {sizeof(buf2), (uintptr_t)buf2};
	ioctl(fd, MI_AO_SETMUTE_IOCTL, buf1);
	close(fd);
	// SETTLE AFTER MUTING TOO. Every caller that mutes is about to tear the codec down
	// (SND_quit -> SDL_CloseAudio -> MI_AO_Disable, PWR_enterSleep, PLAT_resetAudio). Returning
	// the instant the ioctl lands meant the power-down hit an output stage that was still
	// settling, which is audible as the exit pop. Symmetric with the unmute side above.
	if (mute) usleep(MI_AO_SETTLE_US);
}

void PLAT_initInput(void) {
	// api.c PAD_poll also handles SDL_JOYBUTTON*; harmless when no joystick exists.
	SDL_InitSubSystem(SDL_INIT_JOYSTICK);
	if (SDL_NumJoysticks() > 0) SDL_JoystickOpen(0);
	input_run = 1;
	if (pthread_create(&input_pt, NULL, input_thread, NULL) != 0) {
		input_run = 0;
		LOG_info("input: pump thread failed to start\n");
	}
}
void PLAT_quitInput(void) {
	input_run = 0; // thread exits on its next read (or when the fd closes at process exit)
	SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

///////////////////////////////

// MMA render-buffer page size.
// The frontend calls GFX_resize(dst_w, dst_h, dst_p) with the SCALED GAME buffer size
// (minarch.c: `screen = GFX_resize(dst_w,dst_h,dst_p)`, `dst_p = dst_w * FIXED_BPP`), and then
// writes straight into screen->pixels. So this page must hold the LARGEST scaled buffer, not just
// one panel. Upstream reserved PAGE_SCALE(=3) squared (5.5MB/page) for exactly that reason.
//
// I originally shrank this to one panel (614KB) because upstream's 10.5MB request failed against
// the ~6MB free in mma_heap_name0 — and that was the bug behind the garbage band: NES at 3x is
// 768x672 (~1MB), so the frontend wrote past the end of the page into the next one, and we
// presented the overflow. GBC stayed under a panel's worth, which is why it looked clean.
//
// 4x panel area per page (2.4MB for both pages) covers every integer scale the frontend picks
// here while still fitting the heap. resizeVideo hard-checks the request against it.
#define MMA_PAGE ALIGN4K(FIXED_PITCH * FIXED_HEIGHT * 4)

typedef struct HWBuffer {
	MI_PHY padd;
	void* vadd;
} HWBuffer;

static struct VID_Context {
	SDL_Surface* video;   // presentation target: a wrapper around the CURRENT fb page
	SDL_Surface* screen;  // render target handed to the frontend (MMA-backed, MI_GFX-blittable)
	HWBuffer buffer;

	int page;
	int width;
	int height;
	int pitch;

	int direct;
	int cleared;

	// fbdev presentation (SDL2 has no SDL_SetVideoMode/SDL_Flip, and this SoC's SDL2 video
	// driver refuses to init while another process owns the panel — MEASURED: "No available
	// video device". MyMinUI uses raw fbdev here too, so this is the proven path.)
	int fdfb;
	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;
	void* fbmmap;
	size_t fbsize;
	int page_bytes;       // bytes per visible page (line_length * yres)
	int pages;            // scanout pages the driver actually gave us (2 or 3)
} vid;

// Point vid.video at framebuffer page `page`. pixelsPa carries the PHYSICAL address so
// GFX_BlitSurfaceExec can hand it to MI_GFX and let the 2D engine scale straight into scanout.
static void fb_bindPage(int page) {
	vid.video->pixels   = (uint8_t*)vid.fbmmap + (size_t)page * vid.page_bytes;
	surf_setPa(vid.video, vid.finfo.smem_start + (size_t)page * vid.page_bytes);
}
static void fb_pan(int page) {
	vid.vinfo.yoffset = vid.vinfo.yres * page;
	// activate is REQUIRED: without it some fbdev drivers accept the ioctl but never latch the
	// new yoffset, so the panel keeps scanning the old page while memory updates invisibly.
	vid.vinfo.activate = FB_ACTIVATE_VBL;
	ioctl(vid.fdfb, FBIOPAN_DISPLAY, &vid.vinfo);
}

#define MODES_PATH "/sys/class/graphics/fb0/modes"
static int hasMode(const char *path, const char *mode) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char s[128];
    while (fgets(s, sizeof s, f)) if (strstr(s, mode)) return fclose(f), 1;
    fclose(f); return 0;
}

// Reclaim MMA chunks orphaned by a process that died without freeing them.
//
// The SigmaStar MMA heap does no owner tracking across process death: if minarch is SIGKILLed or
// crashes, its render buffer stays allocated until reboot. Two or three of those exhaust the heap
// and then EVERY game fails to start with "MMA_Alloc failed" — the classic "black screen until I
// reboot" report. Parsing /proc and freeing by physical address is how eggs' freemma() solves it
// (OnionOS src/clock/gfx.c); the exclusion list below is copied from there and is load-bearing:
//   fb_device    — the framebuffer itself; freeing it kills the display
//   ao-Dev0-tmp  — the audio buffer
//   daemon*      — anything a long-lived helper declared as persistent
// The proc table gives heap-relative offsets, so the physical base is recovered from the known
// physical address of the fb_device chunk.
// Returns the number of chunks freed.
static int mmp_reclaimMMA(void) {
	FILE* fp = fopen("/proc/mi_modules/mi_sys_mma/mma_heap_name0", "r");
	if (!fp) return 0;
	char str[256];
	// skip the header; the chunk table starts after the "sys-logConfig" row
	do { if (fscanf(fp, "%255s", str) == EOF) { fclose(fp); return 0; } }
	while (strcmp(str, "sys-logConfig"));

	uint32_t offset, length, used;
	MI_PHY base = 0;
	int freed = 0;
	// first pass would be ideal, but the fb row appears before the app rows in practice and we
	// need its base before freeing anything, so bail out of freeing until we have it.
	while (fscanf(fp, "%x %x %x %255s", &offset, &length, &used, str) != EOF) {
		if (!used) continue;
		if (!strcmp(str, "fb_device")) { base = vid.finfo.smem_start - offset; continue; }
		if (!strcmp(str, "ao-Dev0-tmp")) continue;   // audio
		if (!strncmp(str, "daemon", 6)) continue;    // declared persistent
		if (!base) continue;                          // no physical base yet — cannot free safely
		if (MI_SYS_MMA_Free(base + offset) == MI_SUCCESS) {
			LOG_info("mi_sys: reclaimed stale chunk %s (%u bytes)\n", str, length);
			freed++;
		}
	}
	fclose(fp);
	return freed;
}

static void mmpFlipStart(void); // async flip thread; defined with the flip machinery below
static void mmpFlipStop(void);

SDL_Surface* PLAT_initVideo(void) {
	is_plus = exists("/customer/app/axp_test");
	has_axp = is_plus || exists(LID_PATH); // Plus, or Flip (clamshell hall sensor)
	is_560p = hasMode(MODES_PATH, "752x560p") && exists(USERDATA_PATH "/enable-560p");
	LOG_info("is 560p: %i\n", is_560p);
	
	// SDL2: timers only. We do NOT init SDL video — this SoC's only SDL2 video driver (mmiyoo)
	// fails with "No available video device" when anything else owns the panel, and we present
	// through fbdev anyway. Audio is initialized separately by SND_init (driver: MMIYOO).
	// SDL2 delivers keyboard events through the VIDEO subsystem's input backend, and this device's
	// buttons are gpio_keys on the kbd handler. Without VIDEO, SDL_PollEvent never reports a button
	// and the game is uncontrollable. We still present through fbdev ourselves; VIDEO is initialized
	// for its input plumbing. If the mmiyoo video driver refuses (it does while another process owns
	// the panel), fall back so we at least keep rendering.
	if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_EVENTS | SDL_INIT_VIDEO) < 0) {
		LOG_info("SDL: video init failed (%s) - input may not work; continuing\n", SDL_GetError());
		SDL_Init(SDL_INIT_TIMER | SDL_INIT_EVENTS);
	}

	// --- fbdev: 32bpp, TRIPLE-buffered where the reservation allows ---
	// FBIOPAN_DISPLAY BLOCKS ~16.76ms on this driver (measured, both activate flags). With only
	// two pages the render thread pays that stall every frame, so ONE late frame costs a whole
	// extra frame and the rate halves. MEASURED on GBC — the lightest core we ship, ~8% load —
	// dropping to exactly 29.8fps (59.7/2) with the audio ring backing up. A third page plus an
	// async flip thread lets the producer hand the pan off and keep working.
	// Room check: the fb reservation is 0x400000 (4MB); 3 x 640x480x4 = 3.69MB fits. We still
	// verify what the driver actually accepted and fall back to 2 pages if it refused.
	vid.fdfb = open("/dev/fb0", O_RDWR);
	ioctl(vid.fdfb, FBIOGET_FSCREENINFO, &vid.finfo);
	ioctl(vid.fdfb, FBIOGET_VSCREENINFO, &vid.vinfo);
	vid.vinfo.xres = FIXED_WIDTH;
	vid.vinfo.yres = FIXED_HEIGHT;
	vid.vinfo.xres_virtual = FIXED_WIDTH;
	vid.vinfo.yres_virtual = FIXED_HEIGHT * 3;
	vid.vinfo.bits_per_pixel = 32;
	vid.vinfo.yoffset = 0;
	vid.vinfo.activate = FB_ACTIVATE_NOW;
	if (ioctl(vid.fdfb, FBIOPUT_VSCREENINFO, &vid.vinfo) < 0) {
		// driver refused 3 pages — ask for the 2 we know work
		vid.vinfo.yres_virtual = FIXED_HEIGHT * 2;
		ioctl(vid.fdfb, FBIOPUT_VSCREENINFO, &vid.vinfo);
	}
	ioctl(vid.fdfb, FBIOBLANK, FB_BLANK_UNBLANK); // previous owner may have blanked the panel
	ioctl(vid.fdfb, FBIOGET_FSCREENINFO, &vid.finfo);
	ioctl(vid.fdfb, FBIOGET_VSCREENINFO, &vid.vinfo);

	// Trust what the driver reports, never what we asked for.
	vid.pages = vid.vinfo.yres ? (int)(vid.vinfo.yres_virtual / vid.vinfo.yres) : 1;
	if (vid.pages < 1) vid.pages = 1;
	if (vid.pages > 3) vid.pages = 3;

	vid.page_bytes = vid.finfo.line_length * vid.vinfo.yres;
	vid.fbsize     = (size_t)vid.page_bytes * vid.pages;
	if (vid.finfo.smem_len && vid.fbsize > vid.finfo.smem_len) {
		// reservation smaller than the geometry implies — clamp rather than map past the end
		vid.pages  = vid.finfo.smem_len / vid.page_bytes;
		vid.fbsize = (size_t)vid.page_bytes * vid.pages;
	}
	LOG_info("fb: %d page(s), yres_virtual=%d smem_len=%u\n",
		vid.pages, vid.vinfo.yres_virtual, (unsigned)vid.finfo.smem_len);
	vid.fbmmap     = mmap(NULL, vid.fbsize, PROT_READ|PROT_WRITE, MAP_SHARED, vid.fdfb, 0);
	g_fb_base = vid.fbmmap; g_fb_len = vid.fbsize;
	memset(vid.fbmmap, 0, vid.fbsize);
	LOG_info("fb: %dx%d %dbpp line=%d phys=%p\n", vid.vinfo.xres, vid.vinfo.yres,
		vid.vinfo.bits_per_pixel, vid.finfo.line_length, (void*)vid.finfo.smem_start);

	// Wrapper surface over a page; pixels + physical address are re-pointed per flip by
	// fb_bindPage(). NOTE: must be created with a REAL pointer — SDL2's CreateRGBSurfaceFrom
	// dereferences it (SDL 1.2 tolerated NULL, which is what upstream passed).
	vid.video = SDL_CreateRGBSurfaceFrom(vid.fbmmap, FIXED_WIDTH, FIXED_HEIGHT, 32,
		vid.finfo.line_length, 0x00FF0000,0x0000FF00,0x000000FF,0xFF000000); // ARGB (fbset: r@16 g@8 b@0 a@24)
	if (!vid.video) LOG_info("fb: SDL_CreateRGBSurfaceFrom failed: %s\n", SDL_GetError());
	fb_bindPage(0);

	// The MI (SigmaStar) layer used to be initialized for us by the custom SDL 1.2 inside
	// SDL_Init(SDL_INIT_VIDEO). We no longer init SDL video, so bring MI_SYS/MI_GFX up here.
	// Without this MI_SYS_MMA_Alloc silently returns NULL (MEASURED: vadd=(nil) padd=0) and
	// the first memset on the render buffer segfaults.
	if (MI_SYS_Init() != MI_SUCCESS) LOG_info("mi_sys: MI_SYS_Init FAILED\n");
	if (MI_GFX_Open() != MI_SUCCESS) LOG_info("mi_gfx: MI_GFX_Open FAILED\n");

	int buffer_size = ALIGN4K(MMA_PAGE) * PAGE_COUNT;
	// Name the heap explicitly. Passing NULL relied on the custom SDL having already selected a
	// default heap; on its own it returns NULL here. The device exposes exactly one:
	// /proc/mi_modules/mi_sys_mma/mma_heap_name0 (21MB total, ~6MB free at the menu).
	MI_S32 mma_rc = MI_SYS_MMA_Alloc((MI_U8*)"mma_heap_name0", ALIGN4K(buffer_size), &vid.buffer.padd);
	if (mma_rc != MI_SUCCESS) {
		LOG_info("mi_sys: MMA_Alloc(mma_heap_name0, %d) failed rc=%d, retrying default heap\n", ALIGN4K(buffer_size), mma_rc);
		mma_rc = MI_SYS_MMA_Alloc(NULL, ALIGN4K(buffer_size), &vid.buffer.padd);
	}
	if (mma_rc != MI_SUCCESS) {
		// Heap exhausted — almost always because a PREVIOUS minarch was killed or crashed. The
		// SigmaStar MMA heap has no owner tracking across process death: a SIGKILL leaks the whole
		// render buffer (4.9MB here) permanently, and after two or three of those NOTHING can
		// allocate and every game fails to start until reboot.
		// MEASURED: the automated bench force-killed each core and the heap went 1 -> 2 -> 3 stale
		// "app-mmaAlloc" chunks, after which MMA_Alloc returned failure for everyone.
		//
		// Reclaim stale chunks and retry once. Unlike Onion's freemma (which sweeps
		// unconditionally at every init) this only runs when we are ALREADY stuck, so a chunk
		// belonging to a live process is never freed out from under it.
		// Exclusions are load-bearing and come from eggs' original: never free the framebuffer,
		// the audio buffer, or anything a daemon declared.
		LOG_info("mi_sys: MMA_Alloc failed (rc=%d) — reclaiming stale chunks\n", mma_rc);
		int reclaimed = mmp_reclaimMMA();
		LOG_info("mi_sys: reclaimed %d stale chunk(s)\n", reclaimed);
		if (reclaimed > 0) {
			mma_rc = MI_SYS_MMA_Alloc((MI_U8*)"mma_heap_name0", ALIGN4K(buffer_size), &vid.buffer.padd);
			if (mma_rc != MI_SUCCESS)
				mma_rc = MI_SYS_MMA_Alloc(NULL, ALIGN4K(buffer_size), &vid.buffer.padd);
		}
	}
	if (mma_rc != MI_SUCCESS) {
		// Still nothing. Everything downstream assumes vadd is valid — the memset below
		// dereferences it directly — so bail loudly instead of segfaulting at startup with no
		// explanation.
		LOG_error("mi_sys: MMA_Alloc failed on every heap (rc=%d) — cannot bring up video\n", mma_rc);
		vid.buffer.vadd = NULL;
		return NULL;
	}
	if (MI_SYS_Mmap(vid.buffer.padd, ALIGN4K(buffer_size), &vid.buffer.vadd, true) != MI_SUCCESS
	    || !vid.buffer.vadd) {
		LOG_error("mi_sys: MMA_Mmap failed for padd=%llx — cannot bring up video\n",
			(unsigned long long)vid.buffer.padd);
		MI_SYS_MMA_Free(vid.buffer.padd);
		vid.buffer.vadd = NULL;
		return NULL;
	}

	vid.page = 1;
	// The frontend renders RGB565 (FIXED_DEPTH) everywhere. Upstream could return vid.video
	// because SDL 1.2's SDL_SetVideoMode produced a 16bpp surface and the custom SDL converted
	// on flip. Our vid.video now wraps the RAW 32bpp framebuffer, so returning it is a depth
	// mismatch (observed: 'Unknown pixel format' + 'SDL_UpperBlit: passed a NULL surface').
	// Always render into the MMA-backed RGB565 screen and let MI_GFX convert 565->ARGB8888
	// during the present blit; format conversion is free in the 2D engine.
	vid.direct = 0;
	vid.width = FIXED_WIDTH;
	vid.height = FIXED_HEIGHT;
	vid.pitch = FIXED_PITCH;
	vid.cleared = 0;
	
	LOG_info("mi_sys: MMA vadd=%p padd=%llx\n", vid.buffer.vadd, (unsigned long long)vid.buffer.padd);
	vid.screen = SDL_CreateRGBSurfaceFrom(vid.buffer.vadd + ALIGN4K(mma_page*MMA_PAGE),vid.width,vid.height,FIXED_DEPTH,vid.pitch,RGBA_MASK_565);
	surf_setPa(vid.screen, vid.buffer.padd + ALIGN4K(mma_page*MMA_PAGE));
	memset(vid.screen->pixels, 0, vid.pitch * vid.height);

	mmpFlipStart(); // async pan, if the driver gave us 3 pages

	return vid.direct ? vid.video : vid.screen;
}

void PLAT_quitVideo(void) {
	mmpFlipStop(); // must be first: the thread pans through vid.fdfb/vid.vinfo

	// Free the PA side-table slots too. It only has PA_SLOTS entries; leaking two per
	// init->quit cycle eventually exhausts it, after which surf_setPa silently no-ops,
	// surf_getPa returns 0, and GFX_BlitSurfaceExec falls back to SDL_BlitSurface — which does
	// NO rotation. Symptom would be an upside-down screen with no diagnostic whatsoever.
	surf_clearPa(vid.screen);
	surf_clearPa(vid.video);

	SDL_FreeSurface(vid.screen);

	if (vid.video) { vid.video->pixels = NULL; SDL_FreeSurface(vid.video); vid.video = NULL; }

	// Hand the panel back on PAGE 0 before we go. We page-flip, so we may well be scanning out
	// page 1 at exit — while show.elf / batmon.elf / blank.elf all write page 0 and never touch
	// yoffset. Exiting on page 1 makes their images invisible (the charging screen and the
	// transition art simply never appear).
	if (vid.fdfb > 0) {
		vid.vinfo.yoffset = 0;
		vid.vinfo.activate = FB_ACTIVATE_NOW;
		ioctl(vid.fdfb, FBIOPAN_DISPLAY, &vid.vinfo);
	}

	if (vid.fbmmap && vid.fbmmap != MAP_FAILED) {
		memset(vid.fbmmap, 0, vid.fbsize);   // leave a black panel, not the last frame
		munmap(vid.fbmmap, vid.fbsize);
		vid.fbmmap = NULL;
	}
	// Deliberately `> 0`, not `>= 0`: vid is zero-initialised, so fdfb==0 means "never opened".
	// Treating 0 as valid here would close STDIN on a teardown that ran without a successful init.
	if (vid.fdfb > 0) { close(vid.fdfb); vid.fdfb = 0; }

	// Unmap what we actually mapped: initVideo maps ALIGN4K(MMA_PAGE) * PAGE_COUNT, not one page.
	MI_SYS_Munmap(vid.buffer.vadd, ALIGN4K(ALIGN4K(MMA_PAGE) * PAGE_COUNT));
	MI_SYS_MMA_Free(vid.buffer.padd);
	MI_GFX_Close();
	// Pair MI_SYS_Init() from PLAT_initVideo. This used to be masked by accident: SDL2's
	// MMIYOO_VideoQuit calls MI_SYS_Exit — but that backend never initializes here
	// (SDL_VIDEODRIVER is unset, so MMIYOO_Available() returns 0 and video init fails by design),
	// so nothing was ever unwinding MI_SYS.
	MI_SYS_Exit();

	// SDL_Quit() tears down EVERY initialized subsystem — including audio, which SDL_OpenAudio
	// brought up implicitly. SDL_AudioQuit closes the device, and the MMIYOO driver's CloseDevice
	// fires MI_AO_Disable/MI_AO_DisableChn: exactly the codec power-down that PLAT_keepAudioOpen()
	// exists to prevent. So SND_quit's careful "don't close the audio device" was undone two calls
	// later, and the exit pop survived. Quit only the subsystems we actually initialized and leave
	// the codec enabled; the kernel reclaims our fds at process exit without a disable ioctl, and
	// keymon.elf keeps /dev/mi_ao open regardless.
	if (PLAT_keepAudioOpen()) SDL_QuitSubSystem(SDL_INIT_TIMER | SDL_INIT_EVENTS | SDL_INIT_VIDEO);
	else SDL_Quit();
}

void PLAT_clearVideo(SDL_Surface* screen) {
	MI_SYS_FlushInvCache(vid.buffer.vadd + ALIGN4K(mma_page*MMA_PAGE), ALIGN4K(MMA_PAGE));
	MI_SYS_MemsetPa(vid.buffer.padd + ALIGN4K(mma_page*MMA_PAGE), 0, MMA_PAGE);
	SDL_FillRect(screen, NULL, 0);
	// memset(screen->pixels, 0, MMA_PAGE); // this causes crashing
}
void PLAT_clearAll(void) {
	// Clear the FRAMEBUFFER too, not just the render buffer. The present blit only covers the
	// game rect, so whatever sits in the letterbox/pillarbox region persists across a geometry
	// change (previous system's frame, or anything else that wrote to fb0). initVideo zeroes the
	// fb once at startup; this keeps it clean when the geometry changes mid-session.
	if (vid.fbmmap && vid.fbmmap != MAP_FAILED) memset(vid.fbmmap, 0, vid.fbsize);

	PLAT_clearVideo(vid.screen); // clear backbuffer
	vid.cleared = 1; // defer clearing frontbuffer until offscreen
}

// "Prevent Tearing" is REAL on this platform now. It used to be a no-op — the option existed in
// the menu and did nothing, which is worse than not offering it.
//
// With the async flip thread the producer hands a page over and carries on. If it produces another
// frame before the panner has scanned the previous one out, the pending page is replaced and the
// older frame is never displayed (MEASURED: ~1.4% of frames). That is the right default — the
// alternative is the render thread eating the full 16.76ms pan, which is what used to halve GBC to
// 29.8fps. But it IS a dropped frame, so STRICT lets the user choose the other side of the trade:
//   STRICT  -> producer waits for the pan to complete: nothing is ever dropped, at the cost of the
//              stall (classic vsync).
//   LENIENT -> coalesce; newest frame wins. Default.
//   OFF     -> same as lenient here; there is no un-synced pan on this driver (FBIOPAN_DISPLAY
//              blocks regardless of the activate flag — measured on both).
static int flip_block = 0; // 1 = STRICT: never coalesce
void PLAT_setVsync(int vsync) {
	flip_block = (vsync == VSYNC_STRICT);
	LOG_info("fb: vsync=%d (%s)\n", vsync, flip_block ? "strict: wait for pan, drop nothing"
	                                                   : "lenient: coalesce, newest frame wins");
}

SDL_Surface* PLAT_resizeVideo(int w, int h, int pitch) {
	// QUIESCE THE PANNER FIRST. Everything below is hostile to a pan in flight: it memsets every
	// framebuffer page, frees vid.screen, and re-points it at a different MMA offset. The async
	// flip thread meanwhile may be inside fb_pan() on a page we are about to zero, or holding a
	// flip_pending index for one.
	//
	// This is a real, user-visible bug, not theory: entering the in-game menu calls GFX_resize
	// (minarch Menu_loop), so pressing MENU raced the panner and INTERMITTENTLY presented a
	// half-zeroed page — reported from device as "sometimes when I hit the menu button" with a
	// screen of blue garbage instead of the menu.
	//
	// mmpFlipStop() joins the thread (it does not cancel it), so once it returns no pan can be in
	// flight and no page index is held. Restarted at the bottom. Resizes are rare — menu open and
	// close, plus core geometry changes — so a thread teardown here costs nothing measurable.
	mmpFlipStop();

	// Geometry change => the region the present blit covers changes too. Anything outside the new
	// game rect (letterbox/pillarbox) would otherwise keep showing the PREVIOUS content, which is
	// the glitchy band. Clear BOTH framebuffer pages here; per-frame clearing would be wasteful.
	if (vid.fbmmap && vid.fbmmap != MAP_FAILED) memset(vid.fbmmap, 0, vid.fbsize);

	vid.direct = 0; // see PLAT_initVideo: the frontend always gets the RGB565 surface
	vid.width = w;
	vid.height = h;
	vid.pitch = pitch;
	
	if (vid.direct) memset(vid.video->pixels, 0, vid.pitch * vid.height);
	else {
		vid.screen->pixels = NULL;
		surf_clearPa(vid.screen);
		SDL_FreeSurface(vid.screen);
		
		if (vid.pitch <= 0) {
			// Guard the division below: a pitch of 0 would SIGFPE rather than clamp.
			LOG_info("resizeVideo: refusing pitch=%d (%dx%d)\n", vid.pitch, vid.width, vid.height);
			vid.pitch = vid.width * FIXED_BPP;
		}
		if ((size_t)vid.pitch * vid.height > (size_t)MMA_PAGE) {
			// Clamp the surface height so we never advertise more page than we own.
			// NOTE: this is only ACTUALLY protective because minarch re-reads screen->h after
			// GFX_resize and lowers renderer.dst_h to match. The scalers take dst_h as a
			// parameter and never consult the surface, so clamping here alone would have hidden
			// the garbage band while the overflow continued — which is what it did before.
			int max_h = (int)((size_t)MMA_PAGE / (size_t)vid.pitch);
			LOG_info("resizeVideo: %dx%d p=%d needs %zu > MMA_PAGE %d — clamping height to %d\n",
				vid.width, vid.height, vid.pitch, (size_t)vid.pitch * vid.height, (int)MMA_PAGE, max_h);
			vid.height = max_h > 0 ? max_h : 1;
		}
		vid.screen = SDL_CreateRGBSurfaceFrom(vid.buffer.vadd + ALIGN4K(mma_page*MMA_PAGE),vid.width,vid.height,FIXED_DEPTH,vid.pitch,RGBA_MASK_565);
		surf_setPa(vid.screen, vid.buffer.padd + ALIGN4K(mma_page*MMA_PAGE));
		memset(vid.screen->pixels, 0, vid.pitch * vid.height);
	}

	mmpFlipStart(); // buffers are stable again; resume async panning

	return vid.direct ? vid.video : vid.screen;
}

void PLAT_setVideoScaleClip(int x, int y, int width, int height) {
	// buh
}
void PLAT_setNearestNeighbor(int enabled) {
	// buh
}
static int next_effect = EFFECT_NONE;
static int effect_type = EFFECT_NONE;
void PLAT_setSharpness(int sharpness) {
	// force effect to reload
	// on scaling change
	if (effect_type>=EFFECT_NONE) next_effect = effect_type;
	effect_type = -1;
}

void PLAT_setEffect(int effect) {
	next_effect = effect;
}

void PLAT_vsync(int remaining) {
	if (remaining>0) SDL_Delay(remaining);
}

scaler_t PLAT_getScaler(GFX_Renderer* renderer) {
	if (effect_type==EFFECT_LINE) {
		switch (renderer->scale) {
			case 4:  return scale4x_line;
			case 3:  return scale3x_line;
			case 2:  return scale2x_line;
			default: return scale1x_line;
		}
	}
	else if (effect_type==EFFECT_GRID) {
		switch (renderer->scale) {
			case 3:  return scale3x_grid;
			case 2:  return scale2x_grid;
		}
	}
	
	switch (renderer->scale) {
		case 6:  return scale6x6_n16;
		case 5:  return scale5x5_n16;
		case 4:  return scale4x4_n16;
		case 3:  return scale3x3_n16;
		case 2:  return scale2x2_n16;
		default: return scale1x1_n16;
	}
}

void PLAT_blitRenderer(GFX_Renderer* renderer) {
	if (effect_type!=next_effect) {
		effect_type = next_effect;
		renderer->blit = PLAT_getScaler(renderer); // refresh the scaler
	}
	void* dst = renderer->dst + (renderer->dst_y * renderer->dst_p) + (renderer->dst_x * FIXED_BPP);
	((scaler_t)renderer->blit)(renderer->src,dst,renderer->src_w,renderer->src_h,renderer->src_p,renderer->dst_w,renderer->dst_h,renderer->dst_p);
}


static void drawDebugOverlay(void); // defined with the rest of the HUD code below

///////////////////////////////
// Async page flip.
//
// FBIOPAN_DISPLAY blocks ~16.76ms on this driver (MEASURED). Doing that on the render thread means
// one late frame costs a whole extra frame and the rate halves — observed on GBC at exactly
// 29.8fps (= 59.7/2) while the core itself used only ~8% CPU. Handing the pan to a thread lets the
// producer start the next frame immediately.
//
// Only enabled with 3 scanout pages: with 2 the producer would have to blit into the page the
// panner is still handing to scanout. With 3 there is always a free page (one on screen, one
// pending, one being drawn).
// The pending slot is 1-deep and COALESCING — a newer frame replaces an unpanned older one rather
// than queueing, so we never build latency.
static pthread_t       flip_pt;
static pthread_mutex_t flip_mx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  flip_req = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  flip_ack = PTHREAD_COND_INITIALIZER;
static int flip_pending = -1;  // page awaiting pan, -1 = none
static unsigned flip_frames  = 0; // frames handed to the flip thread
static unsigned flip_dropped = 0; // frames COALESCED AWAY: a newer frame replaced an unpanned
                                  // one, so the older frame was never scanned out. This is the
                                  // cost of not blocking the producer, and it must be measured,
                                  // not assumed — dropped frames are invisible to an fps counter.
static int flip_active  = -1;  // page currently being panned, -1 = none
static int flip_run     = 0;

static void* flip_thread(void* arg) {
	(void)arg;
	pthread_mutex_lock(&flip_mx);
	while (flip_run) {
		while (flip_run && flip_pending < 0) pthread_cond_wait(&flip_req, &flip_mx);
		if (!flip_run) break;
		flip_active  = flip_pending;
		flip_pending = -1;
		pthread_cond_broadcast(&flip_ack); // the pending slot is free now — release a STRICT producer
		pthread_mutex_unlock(&flip_mx);

		fb_pan(flip_active);            // the ~16.76ms block, off the render thread

		pthread_mutex_lock(&flip_mx);
		flip_active = -1;
		pthread_cond_broadcast(&flip_ack); // a producer may be waiting for this page to free
	}
	pthread_mutex_unlock(&flip_mx);
	return NULL;
}

static void mmpFlipStart(void) {
	if (vid.pages < 3) return;          // stay synchronous on 2 pages
	flip_run = 1;
	if (pthread_create(&flip_pt, NULL, flip_thread, NULL) != 0) {
		flip_run = 0;
		LOG_info("fb: flip thread failed to start — falling back to synchronous pan\n");
	}
	else LOG_info("fb: async flip thread started (%d pages)\n", vid.pages);
}

static void mmpFlipStop(void) {
	if (!flip_run) return;
	LOG_info("fb: flip stats — %u handed to panner, %u coalesced away (%.1f%%)\n",
		flip_frames, flip_dropped,
		flip_frames ? (100.0 * flip_dropped / flip_frames) : 0.0);
	pthread_mutex_lock(&flip_mx);
	flip_run = 0;
	pthread_cond_broadcast(&flip_req);
	pthread_mutex_unlock(&flip_mx);
	pthread_join(flip_pt, NULL);        // join, don't cancel: a cancelled thread can die holding
	flip_pending = flip_active = -1;    // flip_mx and every later lock would deadlock
}

void PLAT_flip(SDL_Surface* IGNORED, int sync) {
	// Present: MI_GFX blits (and scales, when the render target is core-sized) from the MMA
	// buffer straight into the framebuffer page's PHYSICAL address, then we page-flip. The
	// 2D engine does the scale, so no CPU scaling happens here — that is the whole point of
	// using this SoC's hardware blitter instead of a software present.
	// Composite the debug HUD into the render surface BEFORE the present blit, so it goes through
	// the same rotate=2 as the game frame. No-op when the HUD is off.
	drawDebugOverlay();

	int back = (vid.pages > 1) ? (vid.page + 1) % vid.pages : 0;

	if (flip_run) {
		// Never draw into the page that is on screen or about to be. With 3 pages one is always
		// free, so this normally does not wait at all.
		pthread_mutex_lock(&flip_mx);
		while (flip_run && (back == flip_active || back == flip_pending))
			pthread_cond_wait(&flip_ack, &flip_mx);
		pthread_mutex_unlock(&flip_mx);
	}

	fb_bindPage(back);
	GFX_BlitSurfaceExec(vid.screen, NULL, vid.video, NULL, 2,0,0); // rotate=2 (180, panel is mounted inverted), nowait=0

	if (flip_run) {
		pthread_mutex_lock(&flip_mx);
		// STRICT: wait until the panner has taken the previous frame, so nothing is ever
		// coalesced away. This reintroduces the vsync stall by design — that is what the user
		// asked for by choosing it.
		while (flip_block && flip_run && flip_pending >= 0)
			pthread_cond_wait(&flip_ack, &flip_mx);
		if (flip_pending >= 0) flip_dropped++; // overwriting an unpanned frame = that frame is lost
		flip_frames++;
		// Report periodically rather than at teardown: a SIGTERM/crash exit never reaches
		// PLAT_quitVideo, which is exactly when we most want this number.
		if ((flip_frames % 600) == 0)
			LOG_info("fb: flip %u handed, %u coalesced away (%.1f%%)\n", flip_frames, flip_dropped,
				100.0 * flip_dropped / flip_frames);
		flip_pending = back;              // coalescing: a newer frame replaces an unpanned one
		pthread_cond_signal(&flip_req);
		pthread_mutex_unlock(&flip_mx);
	}
	else fb_pan(back);                    // 2-page fallback: pan inline as before

	vid.page = back;

	// Swap the render-side backbuffer. This is INDEPENDENT of the scanout page count: there are
	// exactly PAGE_COUNT(2) MMA render buffers, so it needs its own toggle. Keying it off
	// (vid.page&1) was correct for 2 scanout pages but yields 0,1,0,0,1,0 for 3.
	if (!vid.direct) {
		mma_page ^= 1;
		vid.screen->pixels = vid.buffer.vadd + ALIGN4K(mma_page*MMA_PAGE);
		surf_setPa(vid.screen, vid.buffer.padd + ALIGN4K(mma_page*MMA_PAGE));
	}

	if (vid.cleared) {
		PLAT_clearVideo(vid.screen);
		vid.cleared = 0;
	}
}

///////////////////////////////

// TODO:
#define OVERLAY_WIDTH PILL_SIZE // unscaled
#define OVERLAY_HEIGHT PILL_SIZE // unscaled
#define OVERLAY_BPP 4
#define OVERLAY_DEPTH 16
#define OVERLAY_PITCH (OVERLAY_WIDTH * OVERLAY_BPP) // unscaled
#define OVERLAY_RGBA_MASK 0x00ff0000,0x0000ff00,0x000000ff,0xff000000 // ARGB
static struct OVL_Context {
	SDL_Surface* overlay;
} ovl;

SDL_Surface* PLAT_initOverlay(void) {
	// setup surface
	ovl.overlay = SDL_CreateRGBSurface(SDL_SWSURFACE, SCALE2(OVERLAY_WIDTH,OVERLAY_HEIGHT),OVERLAY_DEPTH,OVERLAY_RGBA_MASK);
	return ovl.overlay;
}
void PLAT_quitOverlay(void) {
	if (ovl.overlay) SDL_FreeSurface(ovl.overlay);
}
void PLAT_enableOverlay(int enable) {

}

///////////////////////////////

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

///////////////////////////////

static int online = 0;
void PLAT_getBatteryStatus(int* is_charging, int* charge) {
	*is_charging = has_axp ? (axp_read(0x00) & 0x4) > 0 : getInt("/sys/devices/gpiochip0/gpio/gpio59/value");
	
	int i = getInt("/tmp/battery"); // 0-100?

	// worry less about battery and more about the game you're playing
	     if (i>80) *charge = 100;
	else if (i>60) *charge =  80;
	else if (i>40) *charge =  60;
	else if (i>20) *charge =  40;
	else if (i>10) *charge =  20;
	else           *charge =  10;

	// TODO: tmp
	// *is_charging = 0;
	// *charge = PWR_LOW_CHARGE;
	
	// getFile() leaves the buffer UNTOUCHED when the open fails, and the base Mini (and any Plus
	// with the wifi module not inserted) has no wlan0 — so this was reading uninitialised stack
	// and re-rolling `online` from garbage every poll. Initialise it.
	char status[16] = {0};
	getFile("/sys/class/net/wlan0/operstate", status,16);
	online = prefixMatch("up", status);
}

#define PWM_DUTY_PATH "/sys/class/pwm/pwmchip0/pwm0/duty_cycle"
#define FB_CTRL_PATH  "/proc/mi_modules/fb/mi_fb0"

// Screen off/on. NO GPIO4 — deliberately.
//
// GPIO4 is PAD_GPIO4, and on this board PAD_GPIO4 *is the PWM0 output pad* (the stock MY354 DTB's
// pwm node carries `pad-ctrl = <0x4>`, and the vendor PWM driver maps pad id 4 to PWM0_MODE_3 in
// CHIPTOP reg 0x07 bits[2:0]). It is NOT a separate panel-power rail, which is what upstream MinUI
// and Onion's comments imply.
//
// Consequences, MEASURED on this device:
//  * Exporting gpio4 STEALS the pad from PWM0 via padmux, and `unexport` does NOT give it back —
//    only a pwm `enable` 0->1 bounce re-muxes it. We found gpio4 left exported from an earlier
//    sleep cycle, which had silently killed brightness control outright: duty writes went nowhere
//    because the pad was GPIO-owned.
//  * Reading /sys/class/gpio/gpio4/value returns the pad's INPUT receiver, not the output latch,
//    so "write 1, read 0" is expected and proves nothing. An earlier fix here was built on that
//    readback and its stated root cause was wrong.
//  * Driving the pad low is in any case REDUNDANT with setting duty to 0 — it is the same pin.
//
// So we blank the way Allium and spruceOS do: ask the framebuffer to hide its layer and zero the
// backlight duty. No padmux games, nothing to leak.
//
// HONEST CAVEAT, measured: the `GUI_SHOW 0 off` write SUCCEEDS (rc=0) but does NOT change the
// layer state on this firmware — /proc/mi_modules/fb/mi_fb0 still reports `Visible State=1`
// afterwards. So the blanking that actually happens here is duty=0, and the GUI_SHOW call is
// currently decorative. It is kept because it is a single harmless write, it is the documented
// SigmaStar interface, and it does work on the firmware revisions Allium/spruce target — but do
// NOT rely on it hiding anything on this device.
// Ordering follows Allium: dim BEFORE hiding, show BEFORE brightening, so no frame is ever
// displayed at the wrong level.
void PLAT_enableBacklight(int enable) {
	static int saved_duty = -1;

	if (enable) {
		putFile(FB_CTRL_PATH, "GUI_SHOW 0 on");
		if (saved_duty > 0) { SetRawBrightness(saved_duty); saved_duty = -1; }
		else {
			// Nothing saved (e.g. enable without a preceding disable): fall back to the user's
			// stored level rather than leaving the panel dark.
			int b = GetBrightness();
			SetRawBrightness(b==0 ? 6 : b*10);
		}
	}
	else {
		int d = getInt(PWM_DUTY_PATH);
		if (d > 0) saved_duty = d; // the user's live level; keep any earlier save otherwise
		SetRawBrightness(0);
		putFile(FB_CTRL_PATH, "GUI_SHOW 0 off");
	}
}
void PLAT_powerOff(void) {
	sleep(2);

	SetRawVolume(MUTE_VOLUME_RAW);
	PLAT_enableBacklight(0);
	SND_quit();
	VIB_quit();
	PWR_quit();
	GFX_quit();
	
	system("shutdown");
	while (1) pause(); // lolwat
}

///////////////////////////////

// copy/paste of 35XX version now that we have our own overclock.elf
void PLAT_setCPUMaxFreq(int khz); // defined below, next to the OPP table

void PLAT_setCPUSpeed(int speed) {
	// WAS: upstream's verbatim `system("overclock.elf <freq>")` with a 504/1104/1296/1488 MHz
	// table. That OVERCLOCKS — 1488 MHz is 24% above this SoC's top stock OPP (1200 MHz, which
	// Onion's own shipped docs confirm as stock in two places) — and overclock.elf reaches it by
	// poking the SigmaStar MPLL registers through /dev/mem, bypassing cpufreq entirely, so
	// scaling_setspeed cannot undo it. The in-game "CPU Speed" menu made that reachable at will,
	// on a fork whose north star is NEVER overclock. 504000 was not a real OPP either, so the
	// menu left cpufreq and the actual MPLL disagreeing.
	//
	// Now: map to REAL measured OPPs and go through the same snapped, clamped actuator the
	// governor uses, so only one mechanism ever owns the clock (see the note above the OPP table).
	int khz = 0;
	switch (speed) {
		case CPU_SPEED_MENU:		khz =  600000; break; // matches MinUI.pak/launch.sh's boot clock
		case CPU_SPEED_POWERSAVE:	khz = 1000000; break;
		case CPU_SPEED_NORMAL:		khz = 1100000; break;
		case CPU_SPEED_PERFORMANCE:	khz = 1200000; break; // top STOCK OPP — never above
		default:			khz = 1000000; break;
	}
	PLAT_setCPUMaxFreq(khz);
}

void PLAT_setRumble(int strength) {
    static char lastvalue = 0;
    const char str_export[2] = "48";
    const char str_direction[3] = "out";
    char value[1];
    int fd;

    value[0] = (strength == 0 ? 0x31 : 0x30); // '0' : '1'
    if (lastvalue != value[0]) {
       fd = open("/sys/class/gpio/export", O_WRONLY);
       if (fd > 0) { write(fd, str_export, 2); close(fd); }
       fd = open("/sys/class/gpio/gpio48/direction", O_WRONLY);
       if (fd > 0) { write(fd, str_direction, 3); close(fd); }
       fd = open("/sys/class/gpio/gpio48/value", O_WRONLY);
       if (fd > 0) { write(fd, value, 1); close(fd); }
       lastvalue = value[0];
    }
}

int PLAT_pickSampleRate(int requested, int max) {
	return max;
}

char* PLAT_getModel(void) {
	char* model = getenv("MY_MODEL");
	if (exactMatch(model,"MY285")) return "Miyoo Mini Flip";
	else if (is_plus) return "Miyoo Mini Plus";
	else return "Miyoo Mini";
}

int PLAT_isOnline(void) {
	return online;
}

///////////////////////////////
// Closed-loop governor actuation (MinUI Zero).
//
// This SoC has NO schedutil — `scaling_available_governors` is exactly
// "userspace powersave ondemand performance" (MEASURED on-device 2026-07-24). So the tg5040
// model (write a scaling_max_freq ceiling and let schedutil pick beneath it) does not apply.
// Here the commanded ceiling IS the clock: userspace governor + scaling_setspeed, which is
// also MinUI's own historical model on this platform.
//
// NOTE: MinUI/MyMinUI otherwise drive the clock with overclock.elf, which pokes the SigmaStar
// MPLL registers directly and bypasses cpufreq entirely. Whoever writes last wins, so the
// governor owns the clock for the duration of a game; do not mix the two.
//
// MEASURED OPP table (scaling_available_frequencies, 2026-07-24):
//   400000 600000 800000 1000000 1100000 1200000 kHz.
// Requests are snapped DOWN to a real OPP (never up — never silently overclock) and clamped
// to the table. 1200000 is the top *stock* step; overclock.elf can exceed it, we do not.
#define MMP_CPUF_DIR "/sys/devices/system/cpu/cpufreq/policy0"
static const int mmp_opp_khz[] = { 400000, 600000, 800000, 1000000, 1100000, 1200000 };
#define MMP_OPP_COUNT ((int)(sizeof(mmp_opp_khz)/sizeof(mmp_opp_khz[0])))

static int mmp_writeStr(const char* path, const char* val) {
	int fd = open(path, O_WRONLY);
	if (fd < 0) return 0;
	ssize_t n = write(fd, val, strlen(val));
	close(fd);
	return n == (ssize_t)strlen(val);
}

void PLAT_setCPUMaxFreq(int khz) {
	static int last_khz = -1;
	static int gov_set = 0;

	// snap DOWN to a real OPP, clamp into the table
	int target = mmp_opp_khz[0];
	for (int i = 0; i < MMP_OPP_COUNT; i++) {
		if (mmp_opp_khz[i] <= khz) target = mmp_opp_khz[i];
	}
	if (khz >= mmp_opp_khz[MMP_OPP_COUNT-1]) target = mmp_opp_khz[MMP_OPP_COUNT-1];

	if (!gov_set) { // take ownership of the clock once per session
		// Only latch this if it actually succeeded — otherwise we would stop trying to claim
		// the governor after a single transient failure and every later setspeed write would be
		// applied under whatever governor happens to be active.
		gov_set = mmp_writeStr(MMP_CPUF_DIR "/scaling_governor", "userspace");
	}
	if (target == last_khz) return; // avoid pointless sysfs writes every tick

	char buf[16];
	snprintf(buf, sizeof(buf), "%d", target);
	// Cache ONLY on success. This used to assign last_khz before the write, so one failed write
	// convinced the governor it had already applied that clock and it never retried — the closed
	// loop silently went open and the ceiling stuck wherever it was.
	if (mmp_writeStr(MMP_CPUF_DIR "/scaling_setspeed", buf)) last_khz = target;
	else LOG_info("gov: scaling_setspeed write FAILED for %d kHz (will retry)\n", target);
}

///////////////////////////////
// Debug-HUD hooks. (This block used to say the HUD was "not implemented on this platform yet,
// so these are honest no-ops" — that is stale: the code below draws real pixels and the HUD is
// verified working on device. PLAT_getGameRect must still report a sane rect — callers divide
// by w/h.)

// Debug HUD. minarch hands us two RGB565 strips (top/bottom) generated at screen->w /
// DBG_OVERLAY_SCALE and expects them presented SCALED so they span the panel — see minarch.c:3809
// ("presented scaled -> strips span the panel"). 0xF81F (magenta) is the transparency key.
//
// tg5040 uploads these as ARGB textures and lets SDL's renderer scale them. We have no renderer,
// and MI_GFX exposes no filter or convenient sub-rect compositing (its Opt struct has no filter
// field at all — verified against three SigmaStar SDK drops). But our render surface is ALREADY
// RGB565, so we can skip the Brick's ARGB conversion entirely and composite straight into
// vid.screen with an integer nearest upscale, BEFORE the present blit. The HUD then rides the
// same MI_GFX rotate+scale as the game, which is what keeps it right-side up on this inverted
// panel — anything drawn directly into the framebuffer instead would come out upside-down.
#define DBG_KEY 0xF81F
static struct { uint16_t *top, *bottom; int w, h, stride; } dbg = {0};

void PLAT_setDebugOverlay(uint16_t* top, uint16_t* bottom, int w, int h, int stride) {
	dbg.top = top; dbg.bottom = bottom; dbg.w = w; dbg.h = h; dbg.stride = stride;
}

static void dbgBlitStrip(uint16_t* strip, int dst_y) {
	const int S = DBG_OVERLAY_SCALE;
	uint16_t* base = (uint16_t*)vid.screen->pixels;
	int dpitch = vid.pitch / 2; // vid.pitch is bytes; RGB565 => 2 bytes/px
	for (int sy = 0; sy < dbg.h; sy++) {
		uint16_t* srow = strip + sy * dbg.stride;
		for (int ry = 0; ry < S; ry++) {
			int y = dst_y + sy * S + ry;
			if (y < 0 || y >= vid.height) continue;
			uint16_t* drow = base + (size_t)y * dpitch;
			for (int sx = 0; sx < dbg.w; sx++) {
				uint16_t p = srow[sx];
				if (p == DBG_KEY) continue; // transparent
				int x0 = sx * S;
				for (int rx = 0; rx < S; rx++) {
					int x = x0 + rx;
					if (x >= vid.width) break;
					drow[x] = p;
				}
			}
		}
	}
}

static void drawDebugOverlay(void) {
	// minarch calls PLAT_setDebugOverlay(NULL,...) when the HUD is off, so this is a null check
	// on the hot path and nothing more.
	if (!dbg.top || !dbg.bottom || dbg.w <= 0 || dbg.h <= 0) return;
	if (dbg.stride < dbg.w) return;                       // malformed: refuse rather than read OOB
	if (!vid.screen || !vid.screen->pixels) return;
	const int S = DBG_OVERLAY_SCALE;
	int strip_h = dbg.h * S;
	if (strip_h * 2 + S * 2 > vid.height) return;         // no room; skip rather than overlap
	dbgBlitStrip(dbg.top, S);
	dbgBlitStrip(dbg.bottom, vid.height - strip_h - S);
}

void PLAT_getGameRect(int* x, int* y, int* w, int* h) {
	*x = 0; *y = 0;
	*w = vid.video ? vid.video->w : FIXED_WIDTH;
	*h = vid.video ? vid.video->h : FIXED_HEIGHT;
}
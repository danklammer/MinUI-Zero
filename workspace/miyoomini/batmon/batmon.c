// batmon — charging screen for the Miyoo Mini Plus.
//
// Shown when the device is powered on while plugged in. Draws a battery with a REAL fill level and
// a percentage, refreshed once a second.
//
// Rewritten from the original, which had four problems worth naming because each was reported as
// a fault:
//   1. It displayed a STATIC png (charging-640-480.png) — no level, no progress, no feedback.
//   2. It blanked the panel after 3s and then blocked forever, so booting on the charger looked
//      exactly like a hung device.
//   3. Unplugging the charger ran `shutdown` — pulling the cable POWERED THE DEVICE OFF.
//   4. Its wait loop had no sleep, pinning a Cortex-A7 core at 100% for the whole charging
//      session: slower charging, more heat, on a fork whose entire thesis is the opposite.
//
// It also dropped the SDL 1.2 + SDL_image dependency (which this fork does not ship — it resolved
// only against the stock rootfs). Everything here is libc + the framebuffer + i2c.
//
// Behaviour now:
//   POWER pressed  -> exit 0, boot continues normally
//   charger pulled -> exit 0, boot continues normally  (NEVER powers off)
//   idle           -> dims the backlight, any key restores it; never blocks input
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <linux/fb.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define AXPDEV "/dev/i2c-1"
#define AXPID  0x34

// Minimal evdev ABI (see platform.c: <linux/input.h> BTN_* collide with MinUI's own names)
struct bm_event { struct timeval time; unsigned short type; unsigned short code; int value; };
#define BM_EV_KEY   0x01
#define KEY_POWER_  116

#define DUTY_PATH "/sys/class/pwm/pwmchip0/pwm0/duty_cycle"

static int axp_read(unsigned char address) {
	struct i2c_msg msg[2];
	struct i2c_rdwr_ioctl_data packets;
	unsigned char val = 0;
	int fd = open(AXPDEV, O_RDWR);
	if (fd < 0) return -1;
	ioctl(fd, I2C_TIMEOUT, 5);
	ioctl(fd, I2C_RETRIES, 1);
	msg[0].addr = AXPID; msg[0].flags = 0;        msg[0].len = 1; msg[0].buf = &address;
	msg[1].addr = AXPID; msg[1].flags = I2C_M_RD; msg[1].len = 1; msg[1].buf = &val;
	packets.nmsgs = 2; packets.msgs = &msg[0];
	int ret = ioctl(fd, I2C_RDWR, &packets);
	close(fd);
	return ret < 0 ? -1 : val;
}

// reg 0x00: bit7 = ACIN present, bit4 = VBUS usable, bit2 = battery current direction.
// Returns 1 externally powered, 0 running on battery, -1 READ FAILED.
// The -1 matters: minui and keymon poll the same AXP over the same i2c bus, so a contended
// read fails transiently and must NOT be read as "unplugged".
// We gate on POWER PRESENT rather than bit2, because bit2 clears once the battery is full
// (no current flows into a charged cell) even though the cable is still attached.
static int isPowered(void) {
	int v = axp_read(0x00);
	if (v < 0) return -1;
	return (v & 0x80) || (v & 0x10) ? 1 : 0;   // ACIN present or VBUS usable
}
// Actively pushing current into the battery (vs. plugged in and already full).
static int isCharging(void) { int v = axp_read(0x00); return v < 0 ? -1 : ((v & 0x04) != 0); }
// reg 0xB9 bits[6:0] = fuel gauge percentage. VERIFIED: matches /customer/app/axp_test exactly.
static int battPercent(void) {
	int v = axp_read(0xB9);
	if (v < 0) return -1;
	v &= 0x7F;
	return v > 100 ? -1 : v;   // the gauge is known to return garbage occasionally — reject it
}

// ---- framebuffer ----
static int fb_fd = -1;
static uint32_t* fb = NULL;
static int FBW = 640, FBH = 480, FBP = 640; // pixels per row

// The panel is mounted INVERTED. Anything written straight into the framebuffer bypasses the
// MI_GFX rotate the rest of the stack uses, so plot 180-rotated here or it renders upside down.
static inline void px(int x, int y, uint32_t c) {
	if (x < 0 || y < 0 || x >= FBW || y >= FBH) return;
	fb[(FBH - 1 - y) * FBP + (FBW - 1 - x)] = c;
}
static void fillRect(int x, int y, int w, int h, uint32_t c) {
	for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(x+i, y+j, c);
}
static void frameRect(int x, int y, int w, int h, int t, uint32_t c) {
	fillRect(x, y, w, t, c); fillRect(x, y+h-t, w, t, c);
	fillRect(x, y, t, h, c); fillRect(x+w-t, y, t, h, c);
}

// 5x7 digits 0-9 plus '%', one bit per column-row. Enough for "100%".
static const unsigned char FONT[11][7] = {
	{0x1E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
	{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
	{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
	{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
	{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
	{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
	{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
	{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
	{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
	{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
	{0x11,0x12,0x04,0x08,0x11,0x01,0x01}, // %
};
static void drawGlyph(int gi, int x, int y, int s, uint32_t c) {
	for (int r = 0; r < 7; r++)
		for (int b = 0; b < 5; b++)
			if (FONT[gi][r] & (1 << (4 - b))) fillRect(x + b*s, y + r*s, s, s, c);
}
static void drawText(const char* t, int x, int y, int s, uint32_t c) {
	for (const char* p = t; *p; p++) {
		int gi = (*p >= '0' && *p <= '9') ? *p - '0' : (*p == '%' ? 10 : -1);
		if (gi >= 0) drawGlyph(gi, x, y, s, c);
		x += 6 * s;
	}
}

static void setDuty(int v) {
	int fd = open(DUTY_PATH, O_WRONLY);
	if (fd < 0) return;
	char b[8]; int n = snprintf(b, sizeof b, "%d", v);
	if (write(fd, b, n) < 0) { /* best effort */ }
	close(fd);
}
static int getDuty(void) {
	int fd = open(DUTY_PATH, O_RDONLY);
	if (fd < 0) return -1;
	char b[16] = {0};
	int n = read(fd, b, sizeof b - 1);
	close(fd);
	return n > 0 ? atoi(b) : -1;
}

static void render(int pct) {
	memset(fb, 0, (size_t)FBP * FBH * 4);

	const uint32_t WHITE = 0x00FFFFFF, DIM = 0x00404040, GREEN = 0x0040D060;

	int bw = 260, bh = 120;                 // battery body
	int bx = (FBW - bw) / 2, by = (FBH - bh) / 2 - 20;
	frameRect(bx, by, bw, bh, 5, WHITE);
	fillRect(bx + bw, by + bh/2 - 18, 14, 36, WHITE);   // terminal nub

	int iw = bw - 26, ih = bh - 26;         // inner area
	int ix = bx + 13, iy = by + 13;
	if (pct < 0) {
		fillRect(ix, iy, iw, ih, DIM);      // unknown level
	} else {
		int fwid = iw * pct / 100;
		if (fwid > 0) fillRect(ix, iy, fwid, ih, GREEN);
		if (fwid < iw) fillRect(ix + fwid, iy, iw - fwid, ih, DIM);
	}

	char buf[8];
	if (pct < 0) snprintf(buf, sizeof buf, "%%");
	else snprintf(buf, sizeof buf, "%d%%", pct);
	int s = 5, tw = (int)strlen(buf) * 6 * s;
	drawText(buf, (FBW - tw) / 2, by + bh + 34, s, WHITE);
}

int main(void) {
	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0) return 0;                // no framebuffer: just let the boot continue
	struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
	ioctl(fb_fd, FBIOGET_VSCREENINFO, &v);
	ioctl(fb_fd, FBIOGET_FSCREENINFO, &f);
	FBW = v.xres; FBH = v.yres; FBP = f.line_length / 4;

	// Draw into the page that is actually being scanned out. A previous owner may have left the
	// panel panned to page 1 or 2 — the original wrote page 0 unconditionally, so on those boots
	// its image was invisible and the screen just looked dead.
	size_t page = (size_t)f.line_length * v.yres;
	size_t off  = (size_t)v.yoffset * f.line_length;
	fb = mmap(NULL, page * 3 > f.smem_len ? f.smem_len : page * 3,
	          PROT_READ|PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb == MAP_FAILED) { fprintf(stderr,"batmon: mmap failed\n"); close(fb_fd); return 0; }
	fb = (uint32_t*)((char*)fb + off);

	fprintf(stderr,"batmon: fb %dx%d pitch=%d yoffset=%d smem=%u\n",
		FBW, FBH, FBP, v.yoffset, (unsigned)f.smem_len);
	int input_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	fprintf(stderr,"batmon: input_fd=%d\n", input_fd);
	struct pollfd pfd = { .fd = input_fd, .events = POLLIN };

	int duty0 = getDuty();
	if (duty0 <= 0) duty0 = 50;
	int dimmed = 0, idle = 0, last = -2;

	int miss = 0;
	while (1) {
		int chg = isPowered();
		if (chg < 0) {                      // transient i2c failure — do NOT treat as unplugged
			fprintf(stderr,"batmon: i2c read failed (%d)\n", miss+1);
			if (++miss >= 5) { fprintf(stderr,"batmon: exit - PMIC unreadable\n"); break; }
		}
		else {
			miss = 0;
			if (!chg) { fprintf(stderr,"batmon: exit - unplugged\n"); break; }
		}

		int pct = battPercent();
		if (pct != last) { render(pct); last = pct; }

		// Blocking poll — no busy-wait. 1s cadence keeps the readout live and costs nothing.
		int r = input_fd >= 0 ? poll(&pfd, 1, 1000) : (usleep(1000000), 0);
		if (r > 0) {
			struct bm_event ev;
			while (read(input_fd, &ev, sizeof ev) == sizeof ev) {
				if (ev.type != BM_EV_KEY) continue;
				if (ev.code == KEY_POWER_ && ev.value == 0) {
					fprintf(stderr,"batmon: exit - POWER\n"); goto done; }
				if (ev.value == 1) { idle = 0; if (dimmed) { setDuty(duty0); dimmed = 0; } }
			}
		}
		else if (++idle >= 20 && !dimmed) { // ~20s idle: dim, but stay responsive and keep drawing
			setDuty(1);
			dimmed = 1;
		}
	}
done:
	if (dimmed) setDuty(duty0);
	memset(fb, 0, (size_t)FBP * FBH * 4);   // hand a clean screen to the menu
	if (input_fd >= 0) close(input_fd);
	close(fb_fd);
	return 0;
}

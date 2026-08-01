// panelprobe — time the Miyoo Mini Plus panel directly, with nothing else competing.
//
// Measuring the panel through a running emulator is confounded: under Strict present the flip rate
// is min(core generation, panel rate), so in any scene where the core is the limit you measure the
// CORE and cannot tell which regime you were in. Two such runs disagreed by 4% (60.382 vs 58.058
// Hz), which is what prompted this.
//
// This does the only unambiguous thing: page-flip in a tight loop with no emulation, no audio and
// no scaler, and time it with CLOCK_MONOTONIC in-process. FBIOPAN_DISPLAY blocks until the pan is
// latched, so the loop rate IS the panel rate.
//
// Build (from repo root):
//   docker run --rm --platform linux/amd64 -v "$PWD/.notes/mmp-build:/w" miyoomini-toolchain-sdl2 \
//     /bin/bash -c '/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-gcc -O2 -o /w/panelprobe /w/panelprobe.c'
//
// Run on device with NO game running (it takes the framebuffer):
//   /tmp/panelprobe 600
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
	int n = (argc > 1) ? atoi(argv[1]) : 600;
	if (n < 60) n = 60;

	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) { perror("open /dev/fb0"); return 1; }

	struct fb_var_screeninfo v;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0) { perror("FBIOGET_VSCREENINFO"); return 1; }
	printf("panel %ux%u, virtual %ux%u, %ubpp\n", v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.bits_per_pixel);
	if (v.yres_virtual < v.yres * 2) {
		printf("only one page (yres_virtual=%u) — cannot page-flip; measuring pan-in-place\n", v.yres_virtual);
	}

	// Warm up: the first pans after a mode/owner change are not representative.
	for (int i = 0; i < 60; i++) {
		v.yoffset = (i % 2 && v.yres_virtual >= v.yres * 2) ? v.yres : 0;
		v.activate = FB_ACTIVATE_VBL;
		ioctl(fd, FBIOPAN_DISPLAY, &v);
	}

	double t0 = now_s(), prev = t0, worst = 0, best = 1e9;
	for (int i = 0; i < n; i++) {
		v.yoffset = (i % 2 && v.yres_virtual >= v.yres * 2) ? v.yres : 0;
		v.activate = FB_ACTIVATE_VBL;
		if (ioctl(fd, FBIOPAN_DISPLAY, &v) < 0) { perror("FBIOPAN_DISPLAY"); return 1; }
		double t = now_s(), dt = t - prev; prev = t;
		if (dt > worst) worst = dt;
		if (dt < best)  best  = dt;
	}
	double total = now_s() - t0;

	printf("%d pans in %.4fs\n", n, total);
	printf("  mean interval %.4f ms  ->  %.4f Hz\n", total * 1000.0 / n, n / total);
	printf("  fastest %.4f ms, slowest %.4f ms\n", best * 1000.0, worst * 1000.0);
	close(fd);
	return 0;
}

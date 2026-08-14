// layerclean — disable the MinUI DE layer (channel 1, layer 0) so muOS's screen is visible
// again. FAILSAFE for the hosted-dev session: a crashed minui/minarch leaks its layer config,
// which sits above the framebuffer and covers the restored muOS frontend. Runs in every
// session.sh restore path. Raw 220-byte config ABI per MyMinUI's h700 finding.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define DISP_LAYER_SET_CONFIG2 0x49
int main(void) {
	int fd = open("/dev/disp", O_RDWR);
	if (fd < 0) { perror("disp"); return 1; }
	uint32_t raw[55];
	memset(raw, 0, sizeof(raw));
	raw[52] = 0; // enable = 0
	raw[53] = 1; // channel
	raw[54] = 0; // layer_id
	unsigned long args[4] = { 0, (unsigned long)(uintptr_t)raw, 1, 0 };
	int ret = ioctl(fd, DISP_LAYER_SET_CONFIG2, &args);
	close(fd);
	printf("layerclean: %d\n", ret);
	return 0;
}

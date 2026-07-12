// uvtool — TCS4838 (FAN53555-family) CPU-rail voltage tool for the undervolt harness.
// SAFETY MODEL: "read" always allowed; "set" first validates that the register decode
// matches the kernel regulator's reported voltage — wrong chip/map => refuse to write.
// Voltage is written to BOTH VSEL registers (covers either VSEL pin state). Changes are
// RAM-only: any reboot/crash restores the kernel's stock table on the next OPP change.
//
//   uvtool read                      dump regs + decoded voltages
//   uvtool set <microvolts>          e.g. uvtool set 862500 (12.5mV steps, floor-guarded)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>

#define I2C_DEV   "/dev/i2c-6"
#define ADDR      0x41
#define REG_VSEL0 0x00
#define REG_VSEL1 0x01
#define BASE_UV   712500
#define STEP_UV   12500
#define FLOOR_UV  762500  // campaign hard floor: never command below this
#define CEIL_UV   1187500
#define KERNEL_UV "/sys/class/regulator/regulator.0/microvolts" // resolved at runtime instead

static int i2c_open(void) {
	int fd = open(I2C_DEV, O_RDWR);
	if (fd < 0) { perror("open i2c"); exit(1); }
	if (ioctl(fd, I2C_SLAVE_FORCE, ADDR) < 0) { perror("slave"); exit(1); }
	return fd;
}
static int reg_read(int fd, int reg) {
	unsigned char r = reg, v;
	if (write(fd, &r, 1) != 1 || read(fd, &v, 1) != 1) { perror("i2c rd"); exit(1); }
	return v;
}
static void reg_write(int fd, int reg, int val) {
	unsigned char b[2] = { (unsigned char)reg, (unsigned char)val };
	if (write(fd, b, 2) != 2) { perror("i2c wr"); exit(1); }
}
static long kernel_uv(void) {
	// find the tcs4838-dcdc0 regulator's reported voltage
	char path[128], name[64];
	for (int i = 0; i < 32; i++) {
		snprintf(path, sizeof path, "/sys/class/regulator/regulator.%d/name", i);
		FILE* f = fopen(path, "r");
		if (!f) continue;
		if (fgets(name, sizeof name, f) && !strncmp(name, "tcs4838-dcdc0", 13)) {
			fclose(f);
			snprintf(path, sizeof path, "/sys/class/regulator/regulator.%d/microvolts", i);
			f = fopen(path, "r");
			if (!f) return -1;
			long uv; if (fscanf(f, "%ld", &uv) != 1) uv = -1;
			fclose(f);
			return uv;
		}
		fclose(f);
	}
	return -1;
}
static long decode(int v) { return BASE_UV + (long)(v & 0x3F) * STEP_UV; }

int main(int argc, char** argv) {
	if (argc < 2) { fprintf(stderr, "usage: uvtool read | set <uV>\n"); return 1; }
	int fd = i2c_open();
	int v0 = reg_read(fd, REG_VSEL0), v1 = reg_read(fd, REG_VSEL1);
	long kuv = kernel_uv();
	if (!strcmp(argv[1], "read")) {
		printf("VSEL0=0x%02x (%lduV en=%d)\n", v0, decode(v0), (v0>>7)&1);
		printf("VSEL1=0x%02x (%lduV en=%d)\n", v1, decode(v1), (v1>>7)&1);
		for (int r = 2; r <= 5; r++) printf("reg[0x%02x]=0x%02x\n", r, reg_read(fd, r));
		printf("kernel says: %lduV\n", kuv);
		printf("decode match: %s\n",
			(kuv == decode(v0) || kuv == decode(v1)) ? "YES" : "NO — DO NOT WRITE");
		return 0;
	}
	if (!strcmp(argv[1], "set") && argc == 3) {
		char* end = NULL;
		errno = 0;
		long uv = strtol(argv[2], &end, 10);
		if (errno || !end || *end) { fprintf(stderr, "refuse: invalid voltage '%s'\n", argv[2]); return 2; }
		if (uv < FLOOR_UV || uv > CEIL_UV) { fprintf(stderr, "refuse: %ld outside [%d..%d]\n", uv, FLOOR_UV, CEIL_UV); return 2; }
		if ((uv - BASE_UV) % STEP_UV) { fprintf(stderr, "refuse: not a %duV step\n", STEP_UV); return 2; }
		if (kuv != decode(v0) && kuv != decode(v1)) {
			fprintf(stderr, "refuse: register decode (%ld/%ld) != kernel (%ld) — wrong map?\n",
				decode(v0), decode(v1), kuv);
			return 3;
		}
		int vsel = (int)((uv - BASE_UV) / STEP_UV);
		reg_write(fd, REG_VSEL0, (v0 & 0xC0) | vsel);
		reg_write(fd, REG_VSEL1, (v1 & 0xC0) | vsel);
		int n0 = reg_read(fd, REG_VSEL0), n1 = reg_read(fd, REG_VSEL1);
		printf("set %lduV: VSEL0=0x%02x VSEL1=0x%02x (readback %ld/%ld)\n",
			uv, n0, n1, decode(n0), decode(n1));
		if (decode(n0) != uv || decode(n1) != uv) {
			fprintf(stderr, "refuse: VSEL readback mismatch after write\n");
			return 4;
		}
		return 0;
	}
	fprintf(stderr, "usage: uvtool read | set <uV>\n");
	return 1;
}

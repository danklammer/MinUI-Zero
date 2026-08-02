ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif

TARGET = batmon
PRODUCT = $(TARGET).elf

CC = $(CROSS_COMPILE)gcc
CFLAGS   = -marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -march=armv7ve 
# No SDL: batmon draws straight to the framebuffer now. It previously linked libSDL-1.2 and
# libSDL_image-1.2, NEITHER of which this fork ships — it resolved only against the stock rootfs,
# so it was one firmware change away from failing at the loader on the charging path.
LDFLAGS	 = 

all:
	$(CC) $(TARGET).c -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
clean:
	rm -f $(PRODUCT)
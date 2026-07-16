###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
	$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

ifeq (,$(CROSS_COMPILE))
	$(error missing CROSS_COMPILE for this toolchain)
endif

###########################################################

include ../../$(PLATFORM)/platform/makefile.env
SDL?=SDL

###########################################################

TARGET = minarch
LIBRETRO_COMMON_HASH = a59514233de7ea31c6b78d7c8dbc438aee81c8e8
LIBRETRO_COMMON_PIN = libretro-common/.minui-zero-$(LIBRETRO_COMMON_HASH)
.PHONY: $(LIBRETRO_COMMON_PIN)
INCDIR = -I. -I./libretro-common/include/ -I../common/ -I../../$(PLATFORM)/platform/
SOURCE = $(TARGET).c ../common/scaler.c ../common/utils.c ../common/api.c ../common/governor.c ../common/telemetry.c ../../$(PLATFORM)/platform/platform.c

CC = $(CROSS_COMPILE)gcc
CFLAGS   = $(ARCH) -fomit-frame-pointer -DZERO_UV_ENGINE
ifeq ($(PLATFORM),tg5040)
CFLAGS  += -DZERO_DISABLE_FRONTEND_THREADING
endif
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -DUSE_$(SDL) -O3 -std=gnu99
CFLAGS	+= -flto
LDFLAGS	 = -ldl $(LIBS) -lmsettings -l$(SDL) -l$(SDL)_image -l$(SDL)_ttf -lpthread -lm -lz
# CFLAGS  += -Wall -Wno-unused-variable -Wno-unused-function -Wno-format-overflow
# CFLAGS  += -fsanitize=address -fno-common
# LDFLAGS += -lasan

BUILD_DATE!=date +%Y.%m.%d
BUILD_HASH!=cat ../../hash.txt
CFLAGS += -DBUILD_DATE=\"${BUILD_DATE}\" -DBUILD_HASH=\"${BUILD_HASH}\"

# Threading v2 (phase 2) — default ON for tg5040 (the shipping platform). The engine (framering +
# frontend_core) is compiled in but USED ONLY at depth-2 (the pipelined energy win, opted in per-pak
# via ZERO_FTV2_DEPTH — PS.pak sets it). At depth<2 (every other system) minarch runs the plain
# serial path with ZERO engine overhead: validated on-device 2026-07-15 — guard-on-depth-1 matches
# guard-off serial exactly (Dr. Mario 21-22% use both ways; the old always-engine depth-1 cost ~8pp
# more). DEPTH-2 proven on BR2 (holds 60 @ stock where serial drops to 51; DECISIONS D52-D61).
# Build the pure-serial binary with: make ... ZERO_FRONTEND_THREADING_V2=0
ifeq ($(PLATFORM),tg5040)
ZERO_FRONTEND_THREADING_V2 ?= 1
endif
ifeq ($(ZERO_FRONTEND_THREADING_V2),1)
SOURCE += ../common/framering.c ../common/frontend_core.c
CFLAGS += -DZERO_FRONTEND_THREADING_V2
endif

# CFLAGS += -DBUILD_DATE=\"20230326\" -DBUILD_HASH=\"d34db33f\"

PRODUCT= build/$(PLATFORM)/$(TARGET).elf

all: $(LIBRETRO_COMMON_PIN) $(PREFIX)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
clean:
	rm -f $(PRODUCT)

libretro-common:
	git clone https://github.com/libretro/libretro-common

$(LIBRETRO_COMMON_PIN): libretro-common
	cd libretro-common && (git cat-file -e $(LIBRETRO_COMMON_HASH)^{commit} || git fetch origin $(LIBRETRO_COMMON_HASH))
	cd libretro-common && git checkout --detach $(LIBRETRO_COMMON_HASH)
	cd libretro-common && git diff --quiet && git diff --cached --quiet
	touch $(LIBRETRO_COMMON_PIN)
	
$(PREFIX)/include/msettings.h:
	cd /root/workspace/$(PLATFORM)/libmsettings && make

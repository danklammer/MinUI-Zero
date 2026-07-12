# workspace (all)

###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

###########################################################

.PHONY: all

all:
	cd ./$(PLATFORM)/libmsettings && make
	cd ./$(PLATFORM) && make early # eg. other libs
	cd ./$(PLATFORM)/keymon && make
	cd ./all/minui/ && make clean && make
	cd ./all/minarch/ && make clean && make
	cd ./all/clock/ && make
	cd ./all/minput/ && make
	cd ./all/syncsettings/ && make
	cd ./all/say/ && make
	cd ./all/confirm/ && make
	# Tune Voltage harness binaries (undervolt calibration) — build inside the mount;
	# the top makefile copies them into the pak host-side (build/ is outside the container).
	mkdir -p ./tg5040/undervolt/build
	/opt/aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc -O2 -o ./tg5040/undervolt/build/uvtool ./tg5040/undervolt/uvtool.c
	/opt/aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc -O2 -o ./tg5040/undervolt/build/stress ./tg5040/undervolt/stress.c -lpthread
	# A release must not reuse an output .so built from an older checkout, patch, or flags.
	# Pin/patch verification runs inside the core makefile before this clean rebuild.
	cd ./$(PLATFORM)/cores && make verify && make clean && make
	cd ./$(PLATFORM) && make

clean:
	cd ./$(PLATFORM)/libmsettings && make clean
	cd ./$(PLATFORM)/keymon && make clean
	cd ./all/minui/ && make clean
	cd ./all/minarch/ && make clean
	cd ./all/clock/ && make clean
	cd ./all/minput/ && make clean
	cd ./all/syncsettings/ && make clean
	cd ./all/say/ && make clean
	cd ./all/confirm/ && make clean
	cd ./$(PLATFORM) && make clean

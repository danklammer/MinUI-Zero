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
	# tg5040-only: the TCS4838 PMIC engine and its aarch64 compiler path don't exist elsewhere.
ifeq (tg5040,$(PLATFORM))
	mkdir -p ./tg5040/undervolt/build
	/opt/aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc -O2 -o ./tg5040/undervolt/build/uvtool ./tg5040/undervolt/uvtool.c
	/opt/aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc -O2 -o ./tg5040/undervolt/build/stress ./tg5040/undervolt/stress.c -lpthread
	/opt/aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc -O2 -o ./tg5040/undervolt/build/deadman ./tg5040/undervolt/deadman.c
endif
	# A release must not reuse an output .so built from an older checkout, patch, or flags.
	# Pin/patch verification runs inside the core makefile before this clean rebuild.
ifneq (miyoomini,$(PLATFORM))
	cd ./$(PLATFORM)/cores && make verify && make clean && make
else
	# TRACKED DEBT: the miyoomini port ships PREBUILT cores committed under
	# skeleton/SYSTEM/miyoomini/cores/ instead of building them from pinned source the way
	# tg5040 does. Its patches under miyoomini/cores/patches/ are tg5040-shaped and do not
	# apply to the miyoomini clones (fceumm fails first). The 6 shipped cores are the ones
	# validated by the on-device bench harness, so the artifact is CORRECT but its cores are
	# NOT yet reproducible from source. Say so loudly rather than quietly shipping.
	@echo ""
	@echo "*** miyoomini: SKIPPING core build ***"
	@echo "    Shipping prebuilt cores from skeleton/SYSTEM/miyoomini/cores/."
	@echo "    These are NOT built from pinned source — the miyoomini artifact is not yet"
	@echo "    reproducible. Frontend binaries in this build ARE built from source."
	@echo ""
endif
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

###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

###########################################################

REQUIRES_SDL=other/sdl
REQUIRES_COMMANDER=other/DinguxCommander

all: readmes
	cd blank && make
	cd batmon && make
	cd lumon && make
	cd show && make
	# overclock.elf: pokes the SigmaStar MPLL via /dev/mem, beyond cpufreq's 1200 top step.
	# Excluded while never-overclock was a non-negotiable; that rule was amended 2026-07-28
	# (CLAUDE.md — quality gameplay outranks it, earned by receipts). The receipt: PS1 p95
	# needs ~1.5GHz (autotest 2026-07-27), and upstream MinUI ran this very tool at 1296/1488
	# as its NORMAL/PERFORMANCE clocks. The governor stays the only writer of cpufreq; the
	# ONLY caller of this tool is PLAT_setCPUMaxFreq's MINARCH_OC_KHZ translation, so the
	# "two writers fighting" hazard that motivated the exclusion cannot recur.
	cd overclock && make

early: $(REQUIRES_SDL) $(REQUIRES_COMMANDER)
	cd other/sdl && ./make.sh
	cd other/logotweak && make
	cd other/latency_reduction && make
	cd other/DinguxCommander && make -j

clean:
	cd blank && make clean
	cd batmon && make clean
	cd lumon && make clean
	cd show && make clean
	cd other/sdl && make distclean
	cd other/latency_reduction && make clean
	cd other/DinguxCommander && make clean

###########################################################

$(REQUIRES_SDL):
	git clone --depth 1 --branch miniui-miyoomini https://github.com/shauninman/SDL-1.2.git $(REQUIRES_SDL)
$(REQUIRES_COMMANDER):
	git clone --depth 1 --branch miniui-miyoomini https://github.com/shauninman/DinguxCommander.git $(REQUIRES_COMMANDER)

include ../all/readmes/makefile
